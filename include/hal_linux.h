#ifndef HAL_LINUX_H_
#define HAL_LINUX_H_

/*
 * hal_linux.h — eCos compatibility shim for the Octopus/Nemo firmware.
 *
 * Provides all eCos types, constants, HAL macros, and function declarations
 * that the original firmware expects from <cyg/kernel/kapi.h>, <cyg/hal/hal_io.h>,
 * <cyg/io/flash.h>, and <cyg/infra/diag.h>.
 *
 * Implementations are in hal_linux.c, backed by pthreads, pipes, timerfd, etc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>  /* C99 bool, true, false */

#ifdef _WIN32
/* ============================================================ */
/* Windows includes (MinGW-w64)                                 */
/* ============================================================ */
/* winsock2.h MUST come before windows.h to avoid winsock.h conflict */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX        /* prevent windows.h from defining min/max macros */
#define _WIN32_WINNT 0x0601  /* Win7+ for CreateWaitableTimerExW */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>   /* winmm MIDI */
#include <avrt.h>       /* AvSetMmThreadCharacteristics (pro audio scheduling) */

/* pthreads (MinGW winpthreads) */
#include <pthread.h>
#include <semaphore.h>

/* POSIX-ish helpers provided by MinGW */
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <process.h>    /* _exit, _getpid */
#include <io.h>         /* _open, _read, _write, _close, _pipe */

/* ---- Windows replacements for Linux primitives ---- */

/* ssize_t is not defined by MinGW's io.h by default in all configs */
#ifndef _SSIZE_T_DEFINED
  #include <basetsd.h>
  typedef SSIZE_T ssize_t;
  #define _SSIZE_T_DEFINED
#endif

/* We use a Windows HANDLE for the timer fd surrogate */
typedef void* hal_fd_t;

/* MinGW provides pipe()/read()/write()/close() via unistd.h + io.h.
 * For the non-blocking write to a pipe, we avoid fcntl O_NONBLOCK
 * (unreliable on Windows pipes) and use a ring-buffer mailbox instead
 * — see hal_linux.c mbox implementation. */

#else /* __linux__ and other POSIX */
/* ============================================================ */
/* POSIX/Linux includes                                         */
/* ============================================================ */
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/timerfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

/* ============================================================ */
/* eCos type definitions                                        */
/* ============================================================ */

typedef unsigned int    cyg_handle_t;
typedef unsigned int    cyg_addrword_t;
typedef int             cyg_vector_t;
typedef unsigned long long cyg_tick_count_t;
typedef unsigned int    cyg_ucount;
typedef unsigned int    cyg_uint;
typedef unsigned int    cyg_ucount32;
typedef unsigned int    cyg_uint32;
typedef unsigned short  cyg_uint16;
typedef unsigned char   cyg_uint8;
typedef int             cyg_count32;
typedef int             cyg_int32;
typedef unsigned char   cyg_bool;

/* Thread control block — wraps a pthread */
typedef struct {
    pthread_t       tid;
    pthread_attr_t  attr;
    void          (*entry)(cyg_addrword_t);
    cyg_addrword_t  data;
    int             started;
    int             priority;
    char            name[64];
} cyg_thread;

/* Thread info for cyg_thread_get_info */
typedef struct {
    unsigned short id;
    char          *name;
    int            set_pri;
    unsigned int   stack_used;
    unsigned int   stack_size;
} cyg_thread_info;

/* Mailbox — backed by a pipe (Linux) or CRITICAL_SECTION + ring
 * buffer (Windows, since Windows pipe fcntl O_NONBLOCK is unreliable) */
typedef struct {
#ifdef _WIN32
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    void           *items[64];  /* ring buffer */
    int             head;
    int             tail;
    int             count;
#else
    int fd_read;
    int fd_write;
#endif
} cyg_mbox;

/* Mutex */
typedef pthread_mutex_t cyg_mutex_t;

/* Counting semaphore */
typedef sem_t cyg_sem_t;

/* Interrupt — we don't use real interrupts; this is a placeholder */
typedef struct {
    cyg_vector_t vector;
    int          priority;
    cyg_addrword_t data;
    unsigned int  (*isr)(cyg_vector_t, cyg_addrword_t);
    void          (*dsr)(cyg_vector_t, cyg_ucount32, cyg_addrword_t);
} cyg_interrupt;

/* Alarm — backed by timerfd */
/* In eCos, cyg_alarm_t is a function TYPE (not a pointer): typedef void cyg_alarm_t(...) */
typedef void cyg_alarm_t(cyg_handle_t, cyg_addrword_t);

typedef struct {
#ifdef _WIN32
    void           *htimer;     /* HANDLE from CreateWaitableTimer */
#else
    int             tfd;        /* timerfd file descriptor */
#endif
    pthread_t       watcher_tid;
    int             active;
    cyg_alarm_t    *handler;
    cyg_addrword_t  data;
    cyg_handle_t    handle;
} cyg_alarm;

/* ============================================================ */
/* eCos constants                                               */
/* ============================================================ */

#define CYG_ISR_CALL_DSR    1
#define CYG_ISR_HANDLED     1

/* Interrupt vectors — dummy values, never used on Linux */
#define CYGNUM_HAL_INTERRUPT_TIMER1      1
#define CYGNUM_HAL_INTERRUPT_EXT1        2
#define CYGNUM_HAL_INTERRUPT_UART0_RX    3
#define CYGNUM_HAL_INTERRUPT_UART1_RX    4

/* ============================================================ */
/* E7T hardware register defines — dummy values for compilation */
/* (the HAL macros are no-ops, so these values are never used)  */
/* ============================================================ */

#define E7T_INTMOD          0x01
#define E7T_INTPND          0x02
#define E7T_INTPRI0         0x03
#define E7T_INTPRI1         0x04
#define E7T_INTPRI2         0x05
#define E7T_INTPRI3         0x06
#define E7T_INTPRI4         0x07
#define E7T_INTPRI5         0x08
#define E7T_IOPCON          0x09
#define E7T_IOPDATA         0x0A
#define E7T_IOPMOD          0x0B
#define E7T_TCNT1           0x0C
#define E7T_TDATA1          0x0D
#define E7T_TMOD            0x0E
#define E7T_TMOD_TE1        0x01
#define E7T_TMOD_TMD1       0x02
#define E7T_UART0_BASE      0x10
#define E7T_UART1_BASE      0x20
#define E7T_UART_BRDIV      0x00
#define E7T_UART_CON        0x04
#define E7T_UART_LCON       0x08
#define E7T_UART_RXBUF      0x0C
#define E7T_UART_TXBUF      0x10
#define E7T_UART_STAT       0x14
#define E7T_UART_CON_TXM_INT  0x01
#define E7T_UART_CON_RXM_INT  0x02
#define E7T_UART_LCON_8_DBITS  0x03
#define E7T_UART_LCON_NO_PARITY 0x00
#define E7T_UART_LCON_1_SBITS  0x00

/* ============================================================ */
/* HAL macros — no-ops on Linux                                 */
/* ============================================================ */

/* HAL_WRITE_UINT32 — no-op (hardware register write) */
#define HAL_WRITE_UINT32(addr, val)   do { (void)(addr); (void)(val); } while(0)

/* HAL_READ_UINT32 — for the timer countdown register (E7T_TCNT1), return the
 * start value so that cpu_load_measure() computes 0% load.
 * For all other addresses, return 0. */
extern unsigned int hal_cpu_load_timer_start;
#define HAL_READ_UINT32(addr, val)    do { \
    (void)(addr); \
    (val) = ((addr) == 0x0C /*E7T_TCNT1*/) ? hal_cpu_load_timer_start : 0; \
} while(0)

/* HAL_CLOCK_READ — returns monotonic nanoseconds / 100 (rough tick count) */
extern unsigned long long hal_clock_counter;
#define HAL_CLOCK_READ(pval)  do { \
    struct timespec _ts; \
    clock_gettime(CLOCK_MONOTONIC, &_ts); \
    *(pval) = (unsigned int)(_ts.tv_sec * 10000000ULL + _ts.tv_nsec / 100); \
} while(0)

/* ============================================================ */
/* diag_printf — debug output                                   */
/* ============================================================ */

extern int hal_debug_enabled;

static inline void diag_printf(const char *fmt, ...) {
    if (hal_debug_enabled) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fflush(stderr);
    }
}

/* d_iag_printf — same as diag_printf (used throughout codebase) */
#define d_iag_printf(...)  do { if (hal_debug_enabled) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while(0)

/* ============================================================ */
/* Flash API — backed by in-memory buffer / file               */
/* ============================================================ */

/* In-memory flash buffer (replaces raw memory-mapped flash at 0x01900000) */
extern unsigned char *hal_flash_base;

typedef void (*flash_printf_fn)(const char *, ...);

static inline void flash_init(void *printfn) {
    (void)printfn;
}

int flash_read(void *src, void *dest, unsigned int len, void **err_addr);
int flash_erase(void *dest, unsigned int len, void **err_addr);
int flash_program(void *dest, void *src, unsigned int len, void **err_addr);

/* ============================================================ */
/* eCos thread API — backed by pthreads                         */
/* ============================================================ */

void cyg_thread_create(
    unsigned int        priority,
    void              (*entry)(cyg_addrword_t),
    cyg_addrword_t      data,
    const char         *name,
    void               *stack_base,
    unsigned int        stack_size,
    cyg_handle_t       *handle,
    cyg_thread         *thread_obj
);

void cyg_thread_resume(cyg_handle_t handle);
void cyg_thread_delay(unsigned int ticks);
cyg_bool cyg_thread_get_next(cyg_handle_t *thread, unsigned short *id);
cyg_bool cyg_thread_get_info(cyg_handle_t thread, unsigned short id, cyg_thread_info *info);

/* ============================================================ */
/* eCos mailbox API — backed by pipes                           */
/* ============================================================ */

void cyg_mbox_create(cyg_handle_t *handle, cyg_mbox *mbox);
void *cyg_mbox_get(cyg_handle_t handle);
cyg_bool cyg_mbox_tryput(cyg_handle_t handle, void *item);
int cyg_mbox_peek_item(cyg_handle_t handle);

/* ============================================================ */
/* eCos mutex API — backed by pthread_mutex                     */
/* ============================================================ */

void cyg_mutex_init(cyg_mutex_t *mutex);
void cyg_mutex_lock(cyg_mutex_t *mutex);
void cyg_mutex_unlock(cyg_mutex_t *mutex);

/* ============================================================ */
/* eCos semaphore API — backed by POSIX semaphores              */
/* ============================================================ */

void cyg_semaphore_init(cyg_sem_t *sem, unsigned int val);
void cyg_semaphore_post(cyg_sem_t *sem);
void cyg_semaphore_wait(cyg_sem_t *sem);
int cyg_semaphore_trywait(cyg_sem_t *sem);
void cyg_semaphore_peek(cyg_sem_t *sem, int *count);

/* ============================================================ */
/* eCos alarm API — backed by timerfd                           */
/* ============================================================ */

void cyg_alarm_create(
    cyg_handle_t    counter,
    cyg_alarm_t    *alarm_fn,
    cyg_addrword_t  data,
    cyg_handle_t   *handle,
    cyg_alarm      *alarm_obj
);

void cyg_alarm_initialize(cyg_handle_t handle, cyg_tick_count_t trigger, cyg_tick_count_t interval);
void cyg_alarm_disable(cyg_handle_t handle);

/* Counter/clock functions */
cyg_handle_t cyg_real_time_clock(void);
void cyg_clock_to_counter(cyg_handle_t clock, cyg_handle_t *counter);

/* ============================================================ */
/* eCos interrupt API — stubs (no real interrupts on Linux)     */
/* ============================================================ */

void cyg_interrupt_create(
    cyg_vector_t    vector,
    int             priority,
    cyg_addrword_t  data,
    unsigned int  (*isr)(cyg_vector_t, cyg_addrword_t),
    void          (*dsr)(cyg_vector_t, cyg_ucount32, cyg_addrword_t),
    cyg_handle_t  *handle,
    cyg_interrupt *intr_obj
);

void cyg_interrupt_attach(cyg_handle_t handle);
void cyg_interrupt_unmask(cyg_vector_t vector);
void cyg_interrupt_mask(cyg_vector_t vector);
void cyg_interrupt_enable(void);
void cyg_interrupt_disable(void);
void cyg_interrupt_acknowledge(cyg_vector_t vector);

/* ============================================================ */
/* eCos scheduler lock — backed by a global mutex               */
/* ============================================================ */

void cyg_scheduler_lock(void);
void cyg_scheduler_unlock(void);

/* ============================================================ */
/* Time functions                                               */
/* ============================================================ */

cyg_tick_count_t cyg_current_time(void);

/* cyg_user_start — declared here, defined in main_linux.c */
extern void cyg_user_start(void);

/* ============================================================ */
/* MIDI backend — implemented in midi_alsa.c (Linux) or         */
/*               midi_winmm.c (Windows)                         */
/* ============================================================ */
extern void midi_init(int queue_ppqn);
extern void midi_set_tempo(int bpm);
extern void midi_start_queue(void);
extern void midi_stop_queue(void);
extern void midi_continue_queue(void);
extern void midi_send_event(int type, int channel, int val1, int val2, unsigned int timestamp);
extern void midi_flush_queue(unsigned int current_timestamp);
extern int  midi_get_client_id(void);
extern void midi_cleanup(void);
extern void *midi_input_thread(void *arg);
/* Windows MIDI device selection (midi_winmm.c) */
extern void midi_set_device(int which, int device_id); /* which: 0=A,1=B,2=in */
extern int  midi_open_out(int which, int device_id);   /* runtime switch */
extern int  midi_open_in(int device_id);               /* runtime switch */
extern void midi_list_devices(void);

/* ============================================================ */
/* OSC server — implemented in osc_server.c                     */
/* ============================================================ */
extern void osc_server_init(int port);
extern void osc_server_stop(void);

/* ============================================================ */
/* OSC render — implemented in osc_render.c                     */
/* ============================================================ */
extern void osc_render_init(const char *host, int port);
extern void osc_render_start(void);
extern void osc_render_stop(void);
extern void osc_render_update_target(const struct sockaddr_in *new_addr);

/* ============================================================ */
/* State persistence — implemented in main_linux.c              */
/* ============================================================ */
extern void save_state(const char *filepath);

#endif /* HAL_LINUX_H_ */
