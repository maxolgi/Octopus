/*
 * hal_linux.c — Linux implementation of the eCos compatibility shim.
 *
 * Backs all cyg_* APIs with pthreads, pipes, POSIX semaphores, and timerfd.
 * This allows ~50k lines of original Octopus/Nemo firmware code to compile
 * and run on Linux with minimal modification.
 */

#include "hal_linux.h"

/* Global state */
int hal_debug_enabled = 0;
unsigned long long hal_clock_counter = 0;

/* CPU load timer start value — used by HAL_READ_UINT32 to simulate a
 * countdown timer that hasn't counted down much (0% CPU load) */
unsigned int hal_cpu_load_timer_start = 0;

/* In-memory flash buffer — replaces raw memory-mapped flash.
 * MY_FLASH_BASE is 0x01900000 in the firmware. We allocate a flat buffer
 * large enough to hold all page blocks + grid.
 * Flash layout: 15 blocks × 64KB = ~960KB. We allocate 1MB to be safe.
 */
#define HAL_FLASH_SIZE  (1024 * 1024)
unsigned char *hal_flash_base = NULL;

/* ============================================================ */
/* Flash API — backed by in-memory buffer                       */
/* The firmware treats MY_FLASH_BASE (0x01900000) as a raw      */
/* memory pointer. We replace it with a malloc'd buffer and     */
/* implement flash_read/erase/program as memory operations.     */
/* ============================================================ */

static void hal_flash_ensure_init(void) {
    if (hal_flash_base == NULL) {
        hal_flash_base = (unsigned char *)calloc(1, HAL_FLASH_SIZE);
        if (hal_flash_base == NULL) {
            fprintf(stderr, "hal_linux: FATAL: cannot allocate flash buffer\n");
            abort();
        }
    }
}

/* Translate the firmware's source/dest pointers (which are MY_FLASH_BASE + offset)
 * into our in-memory buffer offsets.
 */
static unsigned char *hal_flash_xlate(void *firmware_ptr) {
    hal_flash_ensure_init();
    unsigned long long fw_addr = (unsigned long long)(unsigned char *)firmware_ptr;
    unsigned long long base_addr = 0x01900000ULL;
    unsigned long long offset;

    if (fw_addr >= base_addr) {
        offset = fw_addr - base_addr;
    } else {
        /* Already a relative pointer? */
        offset = fw_addr;
    }

    if (offset >= HAL_FLASH_SIZE) {
        fprintf(stderr, "hal_linux: WARNING: flash access out of bounds: offset=%llu\n", offset);
        return hal_flash_base;
    }
    return hal_flash_base + offset;
}

int flash_read(void *src, void *dest, unsigned int len, void **err_addr) {
    unsigned char *s = hal_flash_xlate(src);
    memcpy(dest, s, len);
    if (err_addr) *err_addr = (void *)0;
    return 0;
}

int flash_erase(void *dest, unsigned int len, void **err_addr) {
    unsigned char *d = hal_flash_xlate(dest);
    memset(d, 0xFF, len);  /* erased flash reads as 0xFF */
    if (err_addr) *err_addr = (void *)0;
    return 0;
}

int flash_program(void *dest, void *src, unsigned int len, void **err_addr) {
    unsigned char *d = hal_flash_xlate(dest);
    memcpy(d, src, len);
    if (err_addr) *err_addr = (void *)0;
    return 0;
}

/* ============================================================ */
/* Thread API — backed by pthreads                              */
/* ============================================================ */

/* Internal structure to pass entry+data to the pthread trampoline */
typedef struct {
    void (*entry)(cyg_addrword_t);
    cyg_addrword_t data;
} hal_thread_arg_t;

static void *hal_thread_trampoline(void *arg) {
    hal_thread_arg_t *targ = (hal_thread_arg_t *)arg;
    void (*entry)(cyg_addrword_t) = targ->entry;
    cyg_addrword_t data = targ->data;
    free(targ);
    entry(data);
    return NULL;
}

void cyg_thread_create(
    unsigned int        priority,
    void              (*entry)(cyg_addrword_t),
    cyg_addrword_t      data,
    const char         *name,
    void               *stack_base,
    unsigned int        stack_size,
    cyg_handle_t       *handle,
    cyg_thread         *thread_obj
) {
    (void)priority;
    (void)stack_base;
    (void)stack_size;

    /* Store info in the thread object */
    thread_obj->entry = entry;
    thread_obj->data = data;
    thread_obj->priority = (int)priority;
    thread_obj->started = 1;
    strncpy(thread_obj->name, name ? name : "unnamed", sizeof(thread_obj->name) - 1);
    thread_obj->name[sizeof(thread_obj->name) - 1] = '\0';

    /* Set handle to the index into our thread object (the handle IS the pointer cast to unsigned int) */
    *handle = (cyg_handle_t)(unsigned long long)thread_obj;

    /* We do NOT create the pthread yet — that happens on cyg_thread_resume */
}

/* We need to store the thread args until resume. Simplest: use a small registry */
#define HAL_MAX_THREADS 32
static hal_thread_arg_t *hal_pending_args[HAL_MAX_THREADS];

void cyg_thread_resume(cyg_handle_t handle) {
    cyg_thread *thread_obj = (cyg_thread *)(unsigned long long)handle;
    if (!thread_obj->started) return;

    /* Allocate the trampoline arg */
    hal_thread_arg_t *targ = (hal_thread_arg_t *)malloc(sizeof(hal_thread_arg_t));
    targ->entry = thread_obj->entry;
    targ->data = thread_obj->data;

    /* Create the pthread */
    if (pthread_create(&thread_obj->tid, NULL, hal_thread_trampoline, targ) != 0) {
        fprintf(stderr, "hal_linux: FATAL: pthread_create failed for '%s'\n", thread_obj->name);
        abort();
    }
}

void cyg_thread_delay(unsigned int ticks) {
    /* eCos ticks are ~10ms each */
    usleep(ticks * 10000);
}

cyg_bool cyg_thread_get_next(cyg_handle_t *thread, unsigned short *id) {
    (void)thread;
    (void)id;
    return 0;  /* no more threads */
}

cyg_bool cyg_thread_get_info(cyg_handle_t thread, unsigned short id, cyg_thread_info *info) {
    (void)thread;
    (void)id;
    (void)info;
    return 0;
}

/* ============================================================ */
/* Mailbox API — backed by pipes (Linux) or ring buffer (Win)   */
/* ============================================================ */

/* Mailbox handles map to our cyg_mbox struct via the handle value.
 * In eCos, the handle is set by cyg_mbox_create and used in subsequent calls.
 * We use the handle as an index into a registry of mailbox structs.
 */
#define HAL_MAX_MBOXS 16
static cyg_mbox *hal_mbox_registry[HAL_MAX_MBOXS];
static int hal_mbox_count = 0;

void cyg_mbox_create(cyg_handle_t *handle, cyg_mbox *mbox) {
#ifdef _WIN32
    InitializeCriticalSection(&mbox->cs);
    InitializeConditionVariable(&mbox->cv);
    mbox->head = 0;
    mbox->tail = 0;
    mbox->count = 0;
    memset(mbox->items, 0, sizeof(mbox->items));
#else
    int fds[2];
    if (pipe(fds) != 0) {
        fprintf(stderr, "hal_linux: FATAL: pipe() failed for mbox\n");
        abort();
    }
    mbox->fd_read = fds[0];
    mbox->fd_write = fds[1];

    /* Set the write end to non-blocking for tryput */
    int flags = fcntl(mbox->fd_write, F_GETFL, 0);
    fcntl(mbox->fd_write, F_SETFL, flags | O_NONBLOCK);
#endif

    int idx = hal_mbox_count++;
    if (idx >= HAL_MAX_MBOXS) {
        fprintf(stderr, "hal_linux: FATAL: too many mailboxes\n");
        abort();
    }
    hal_mbox_registry[idx] = mbox;
    *handle = (cyg_handle_t)idx;
}

static cyg_mbox *hal_mbox_lookup(cyg_handle_t handle) {
    if (handle >= (cyg_handle_t)hal_mbox_count) return NULL;
    return hal_mbox_registry[handle];
}

void *cyg_mbox_get(cyg_handle_t handle) {
    cyg_mbox *mbox = hal_mbox_lookup(handle);
    if (!mbox) return NULL;
#ifdef _WIN32
    void *item = NULL;
    EnterCriticalSection(&mbox->cs);
    while (mbox->count == 0) {
        SleepConditionVariableCS(&mbox->cv, &mbox->cs, INFINITE);
    }
    item = mbox->items[mbox->head];
    mbox->head = (mbox->head + 1) % 64;
    mbox->count--;
    LeaveCriticalSection(&mbox->cs);
    return item;
#else
    void *item = NULL;
    if (read(mbox->fd_read, &item, sizeof(void *)) != (ssize_t)sizeof(void *))
        return NULL;
    return item;
#endif
}

cyg_bool cyg_mbox_tryput(cyg_handle_t handle, void *item) {
    cyg_mbox *mbox = hal_mbox_lookup(handle);
    if (!mbox) return 0;
#ifdef _WIN32
    int ok = 0;
    EnterCriticalSection(&mbox->cs);
    if (mbox->count < 64) {
        mbox->items[mbox->tail] = item;
        mbox->tail = (mbox->tail + 1) % 64;
        mbox->count++;
        ok = 1;
    }
    LeaveCriticalSection(&mbox->cs);
    if (ok) WakeConditionVariable(&mbox->cv);
    return ok;
#else
    ssize_t n = write(mbox->fd_write, &item, sizeof(void *));
    return (n == (ssize_t)sizeof(void *)) ? 1 : 0;
#endif
}

int cyg_mbox_peek_item(cyg_handle_t handle) {
    cyg_mbox *mbox = hal_mbox_lookup(handle);
    if (!mbox) return 0;
#ifdef _WIN32
    return mbox->count;
#else
    int count = 0;
    ioctl(mbox->fd_read, FIONREAD, &count);
    return count / (int)sizeof(void *);
#endif
}

/* ============================================================ */
/* Mutex API                                                    */
/* ============================================================ */

void cyg_mutex_init(cyg_mutex_t *mutex) {
    pthread_mutex_init(mutex, NULL);
}

void cyg_mutex_lock(cyg_mutex_t *mutex) {
    pthread_mutex_lock(mutex);
}

void cyg_mutex_unlock(cyg_mutex_t *mutex) {
    pthread_mutex_unlock(mutex);
}

/* ============================================================ */
/* Semaphore API                                                */
/* ============================================================ */

void cyg_semaphore_init(cyg_sem_t *sem, unsigned int val) {
    sem_init(sem, 0, val);
}

void cyg_semaphore_post(cyg_sem_t *sem) {
    sem_post(sem);
}

void cyg_semaphore_wait(cyg_sem_t *sem) {
    sem_wait(sem);
}

int cyg_semaphore_trywait(cyg_sem_t *sem) {
    return sem_trywait(sem);
}

void cyg_semaphore_peek(cyg_sem_t *sem, int *count) {
    int val = 0;
    sem_getvalue(sem, &val);
    if (count) *count = val;
}

/* ============================================================ */
/* Alarm API — backed by timerfd                                */
/* ============================================================ */

#define HAL_MAX_ALARMS 16
static cyg_alarm *hal_alarm_registry[HAL_MAX_ALARMS];
static int hal_alarm_count = 0;

/* Counter/clock handles — dummy */
static cyg_handle_t hal_counter_handle = 1;

cyg_handle_t cyg_real_time_clock(void) {
    return hal_counter_handle;
}

void cyg_clock_to_counter(cyg_handle_t clock, cyg_handle_t *counter) {
    (void)clock;
    *counter = hal_counter_handle;
}

/* Alarm watcher thread function */
static void *hal_alarm_watcher(void *arg) {
    cyg_alarm *alarm = (cyg_alarm *)arg;

#ifdef _WIN32
    /* Windows: waitable timer approach */
    while (alarm->active) {
        if (WaitForSingleObject(alarm->htimer, INFINITE) != WAIT_OBJECT_0) {
            break;
        }
        if (alarm->handler && alarm->active) {
            alarm->handler(alarm->handle, alarm->data);
        }
    }
#else
    /* Linux: timerfd approach */
    uint64_t expirations;

    while (alarm->active) {
        ssize_t s = read(alarm->tfd, &expirations, sizeof(expirations));
        if (s != sizeof(uint64_t)) {
            if (errno == EINTR) continue;
            break;
        }
        if (alarm->handler && alarm->active) {
            alarm->handler(alarm->handle, alarm->data);
        }
    }
#endif
    return NULL;
}

void cyg_alarm_create(
    cyg_handle_t    counter,
    cyg_alarm_t     alarm_fn,
    cyg_addrword_t  data,
    cyg_handle_t   *handle,
    cyg_alarm      *alarm_obj
) {
    (void)counter;

    alarm_obj->handler = alarm_fn;
    alarm_obj->data = data;
    alarm_obj->active = 0;

#ifdef _WIN32
    /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION requires Win10 1803+ */
    alarm_obj->htimer = CreateWaitableTimerExW(NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!alarm_obj->htimer) {
        /* Fall back to standard waitable timer */
        alarm_obj->htimer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    if (!alarm_obj->htimer) {
        fprintf(stderr, "hal_linux: WARNING: CreateWaitableTimer failed\n");
        return;
    }
#else
    alarm_obj->tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (alarm_obj->tfd < 0) {
        fprintf(stderr, "hal_linux: WARNING: timerfd_create failed\n");
        return;
    }
#endif

    int idx = hal_alarm_count++;
    if (idx >= HAL_MAX_ALARMS) {
        fprintf(stderr, "hal_linux: FATAL: too many alarms\n");
        abort();
    }
    hal_alarm_registry[idx] = alarm_obj;
    alarm_obj->handle = (cyg_handle_t)idx;
    *handle = (cyg_handle_t)idx;
}

void cyg_alarm_initialize(cyg_handle_t handle, cyg_tick_count_t trigger, cyg_tick_count_t interval) {
    if (handle >= (cyg_handle_t)hal_alarm_count) return;
    cyg_alarm *alarm = hal_alarm_registry[handle];
    if (!alarm) return;

    /* eCos ticks are ~10ms. Convert to nanoseconds/milliseconds. */
    cyg_tick_count_t trigger_ms = trigger * 10;
    cyg_tick_count_t interval_ms = interval * 10;

    /* Compute relative delay. In eCos, trigger is ABSOLUTE time
     * (callers pass cyg_current_time() + timeout). When interval==0
     * (one-shot), we must compute trigger - now to get the actual delay.
     * When interval>0 (repeating), interval is already a relative period. */
    cyg_tick_count_t delay_ms;
    if (interval > 0) {
        delay_ms = interval_ms;
    } else if (trigger > 0) {
        cyg_tick_count_t now = cyg_current_time();
        if (trigger > now) {
            delay_ms = (trigger - now) * 10;
        } else {
            delay_ms = 1;
        }
    } else {
        delay_ms = 0;
    }

#ifdef _WIN32
    if (!alarm->htimer) return;

    LARGE_INTEGER due;
    /* Negative value = relative time in 100ns units */
    due.QuadPart = -(LONGLONG)(delay_ms * 10000);

    LONG period_ms = 0;
    if (interval > 0) {
        period_ms = (LONG)interval_ms;
    }

    SetWaitableTimer(alarm->htimer, &due, period_ms, NULL, NULL, FALSE);
#else
    if (alarm->tfd < 0) return;

    struct itimerspec its;
    memset(&its, 0, sizeof(its));

    if (interval > 0) {
        its.it_interval.tv_sec = interval_ms / 1000;
        its.it_interval.tv_nsec = (interval_ms % 1000) * 1000000;
    }

    if (delay_ms > 0) {
        its.it_value.tv_sec = delay_ms / 1000;
        its.it_value.tv_nsec = (delay_ms % 1000) * 1000000;
    }

    timerfd_settime(alarm->tfd, 0, &its, NULL);
#endif

    if (!alarm->active) {
        alarm->active = 1;
        pthread_create(&alarm->watcher_tid, NULL, hal_alarm_watcher, alarm);
    }
}

void cyg_alarm_disable(cyg_handle_t handle) {
    if (handle >= (cyg_handle_t)hal_alarm_count) return;
    cyg_alarm *alarm = hal_alarm_registry[handle];
    if (!alarm) return;
    alarm->active = 0;
#ifdef _WIN32
    if (alarm->htimer) {
        CancelWaitableTimer(alarm->htimer);
    }
#else
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    if (alarm->tfd >= 0)
        timerfd_settime(alarm->tfd, 0, &its, NULL);
#endif
}

/* ============================================================ */
/* Interrupt API — no-ops (Linux uses threads, not ISRs)        */
/* ============================================================ */

void cyg_interrupt_create(
    cyg_vector_t    vector,
    int             priority,
    cyg_addrword_t  data,
    unsigned int  (*isr)(cyg_vector_t, cyg_addrword_t),
    void          (*dsr)(cyg_vector_t, cyg_ucount32, cyg_addrword_t),
    cyg_handle_t  *handle,
    cyg_interrupt *intr_obj
) {
    (void)vector;
    (void)priority;
    (void)data;
    (void)isr;
    (void)dsr;
    intr_obj->vector = vector;
    *handle = (cyg_handle_t)(unsigned long long)intr_obj;
}

void cyg_interrupt_attach(cyg_handle_t handle) {
    (void)handle;
}

void cyg_interrupt_unmask(cyg_vector_t vector) {
    (void)vector;
}

void cyg_interrupt_mask(cyg_vector_t vector) {
    (void)vector;
}

/* Interrupt enable/disable — use a global counter.
 * In eCos, cyg_interrupt_disable() returns a saved state that is passed to
 * cyg_interrupt_enable(). Some code uses the pattern:
 *   old = cyg_interrupt_disable(); ...; cyg_interrupt_enable(old);
 * But the firmware code (OS_infrastructure.h, flash-block.c) uses the void
 * pattern: cyg_interrupt_disable(); ...; cyg_interrupt_enable();
 * So we support the void form.
 */
static volatile int hal_intr_disable_count = 0;

void cyg_interrupt_enable(void) {
    if (hal_intr_disable_count > 0)
        hal_intr_disable_count--;
}

void cyg_interrupt_disable(void) {
    hal_intr_disable_count++;
}

void cyg_interrupt_acknowledge(cyg_vector_t vector) {
    (void)vector;
}

/* ============================================================ */
/* Scheduler lock — backed by a global recursive mutex           */
/* ============================================================ */

static pthread_mutex_t hal_scheduler_mutex;
static int hal_scheduler_initialized = 0;

static void hal_scheduler_init(void) {
    if (!hal_scheduler_initialized) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        /* Priority inheritance prevents the SCHED_FIFO sequencer thread from
         * being blocked indefinitely by a lower-priority thread holding the
         * scheduler lock (priority inversion). */
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
        if (pthread_mutex_init(&hal_scheduler_mutex, &attr) != 0) {
            /* PRIO_INHERIT may be unsupported on some kernels; fall back */
            pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_NONE);
            pthread_mutex_init(&hal_scheduler_mutex, &attr);
        }
        pthread_mutexattr_destroy(&attr);
        hal_scheduler_initialized = 1;
    }
}

void cyg_scheduler_lock(void) {
    hal_scheduler_init();
    pthread_mutex_lock(&hal_scheduler_mutex);
}

void cyg_scheduler_unlock(void) {
    hal_scheduler_init();
    pthread_mutex_unlock(&hal_scheduler_mutex);
}

/* ============================================================ */
/* Time functions                                               */
/* ============================================================ */

cyg_tick_count_t cyg_current_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* eCos ticks are ~10ms (100 Hz) */
    return (cyg_tick_count_t)(ts.tv_sec * 100 + ts.tv_nsec / 10000000);
}
