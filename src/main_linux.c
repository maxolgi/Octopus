/*
 * main_linux.c — Linux entry point for the Octopus/Nemo port.
 *
 * Phase 0: memory init (done)
 * Phase 1: ALSA MIDI out + sequencer thread (done)
 * Phase 2: OSC input server (keys + transport)
 */

/* Pull in the entire firmware — same include chain as original main.c. */
#include "_OCT_global/includes.h"
#include "_OCT_global/flash-block.h"
#include "_OCT_objects/PersistentV2.h"

/* Include the .c files into this single TU to match the original architecture */
#include "_OCT_objects/PersistentV1.c"
#include "_OCT_objects/PersistentV2.c"
#include "_OCT_objects/Persistent.c"
#include "_OCT_objects/Phrase.c"
#include "_OCT_objects/Phrase-presets.c"
#include "_OCT_global/flash-block.c"
#include "_OCT_interrupts/cpu-load.c"

/* ============================================================ */
/* Sequencer thread — replaces Timer1 ISR + driveSequencer      */
/* ============================================================ */
static volatile int sequencer_running = 0;
static pthread_t sequencer_pthread;
volatile int main_running = 1;

/* Precomputed nanoseconds per 48-PPQN tick. Updated by
 * G_TIMER_REFILL_update() whenever G_master_tempo changes.
 * Read by the sequencer thread hot loop to avoid FP division. */
volatile long g_tick_ns = 0;

#include <signal.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

static void *sequencer_thread_func(void *arg) {
    (void)arg;

#ifdef _WIN32
    /* Windows: use MMCSS (Multimedia Class Scheduler) for pro-audio priority,
     * plus high thread priority. This is the Windows equivalent of SCHED_FIFO.
     *
     * Note: we use void* instead of HANDLE because the firmware defines
     * HANDLE as 5 (a display mode constant in defs_general.h:198). */
    DWORD taskIndex = 0;
    void* hMmTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (hMmTask) {
        fprintf(stderr, "sequencer: MMCSS \"Pro Audio\" task activated\n");
    } else {
        fprintf(stderr, "sequencer: MMCSS unavailable, using THREAD_PRIORITY_TIME_CRITICAL\n");
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    /* Windows does not have mlockall. VirtualLock could lock pages but needs
     * privilege. Skip for now — jitter is bounded by the busy-wait spin. */
#else
    struct sched_param sp;
    sp.sched_priority = 80;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "sequencer: SCHED_FIFO not available, using default scheduler\n");
    }

    /* Lock all memory pages to prevent page-fault jitter.
     * Requires CAP_IPC_LOCK or root; non-fatal if unavailable. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "sequencer: mlockall failed (jitter may increase): %s\n", strerror(errno));
    }
#endif

    /* Absolute-deadline timing with hybrid sleep + busy-wait.
     *
     * On Linux: clock_nanosleep(TIMER_ABSTIME) wakes with 50-300 μs of
     * variable latency on a non-RT kernel. We sleep until ~300 μs before
     * the deadline, then busy-wait on clock_gettime.
     *
     * On Windows: Sleep() has ~1ms granularity (after timeBeginPeriod(1)).
     * We Sleep until ~2ms before the deadline, then busy-wait on
     * QueryPerformanceCounter for sub-microsecond precision.
     *
     * The busy-wait uses the vDSO TSC / QPC read (~20 ns resolution). */
#ifdef _WIN32
    #define BUSY_WAIT_NS  2000000   /* 2 ms (Sleep granularity margin) */
#else
    #define BUSY_WAIT_NS  300000    /* 300 μs */
#endif

#ifdef _WIN32
    /* Windows high-resolution timer */
    LARGE_INTEGER qpc_freq;
    QueryPerformanceFrequency(&qpc_freq);
    LARGE_INTEGER qpc_next;
    QueryPerformanceCounter(&qpc_next);

    while (sequencer_running) {
        long add_ns = g_tick_ns;

        /* Advance to the next absolute deadline (in QPC ticks) */
        long long add_qpc = (long long)((double)add_ns * qpc_freq.QuadPart / 1e9 + 0.5);
        qpc_next.QuadPart += add_qpc;

        /* Sleep until ~BUSY_WAIT_NS before the deadline */
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        long long remaining_ns = (long long)((double)(qpc_next.QuadPart - qpc_now.QuadPart) * 1e9 / qpc_freq.QuadPart);

        if (remaining_ns > BUSY_WAIT_NS) {
            DWORD sleep_ms = (DWORD)((remaining_ns - BUSY_WAIT_NS) / 1000000);
            if (sleep_ms > 0) Sleep(sleep_ms);
        }

        /* Busy-wait the final stretch for precision */
        do {
            QueryPerformanceCounter(&qpc_now);
        } while (qpc_now.QuadPart < qpc_next.QuadPart);
#else
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (sequencer_running) {
        /* Read precomputed tick interval (avoids FP division in hot loop). */
        long add_ns = g_tick_ns;

        /* Advance to the next absolute deadline */
        next.tv_sec  += (time_t)(add_ns / 1000000000L);
        next.tv_nsec += add_ns % 1000000000L;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_sec++;
            next.tv_nsec -= 1000000000L;
        }

        /* Sleep until just before the deadline */
        struct timespec sleep_target = next;
        sleep_target.tv_nsec -= BUSY_WAIT_NS;
        if (sleep_target.tv_nsec < 0) {
            sleep_target.tv_sec--;
            sleep_target.tv_nsec += 1000000000L;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleep_target, NULL);

        /* Busy-wait the final stretch for precision */
        struct timespec now;
        do {
            clock_gettime(CLOCK_MONOTONIC, &now);
        } while (now.tv_sec < next.tv_sec ||
                 (now.tv_sec == next.tv_sec && now.tv_nsec < next.tv_nsec));
#endif

        /* Send MIDI clock BEFORE acquiring the scheduler lock.
         *
         * The clock byte (0xF8) is the most timing-critical output.
         * Sending it here — immediately after the busy-wait completes —
         * eliminates jitter from mutex contention with the render thread
         * (60 Hz Page_full_refresh) and OSC input thread, both of which
         * acquire the same scheduler mutex.
         *
         * G_TTC_abs_value and G_clock_source are single-value reads
         * (atomic on x86_64). driveSequencer() will update G_TTC_abs_value
         * to the same next_ttc value, so they stay in sync.
         *
         * The firmware's own clock send (Intr_TMR.h) is skipped on Linux
         * via #ifndef __linux__. */
        {
            unsigned char next_ttc = (G_TTC_abs_value % 12) + 1;
            if (next_ttc % 2 == 1) {
                if (G_clock_source == INT ||
                    (G_clock_source == EXT && MIDICLOCK_PASSTHROUGH == TRUE)) {
                    MIDI_send(MIDI_CLOCK, MIDICLOCK_CLOCK, 0, 0);
                }
            }
        }

        cyg_scheduler_lock();
        driveSequencer();
        cyg_scheduler_unlock();
    }

#ifdef _WIN32
    if (hMmTask) AvRevertMmThreadCharacteristics(hMmTask);
#endif
    return NULL;
}

static void start_sequencer_thread(void) {    sequencer_running = 1;
    pthread_create(&sequencer_pthread, NULL, sequencer_thread_func, NULL);
}

#ifdef _WIN32
/* Windows console control handler — catches Ctrl+C and close events.
 * Sets main_running = 0 to break the main loop for clean shutdown. */
static BOOL WINAPI console_ctrl_handler(DWORD ctrl) {
    if (ctrl == CTRL_C_EVENT || ctrl == CTRL_CLOSE_EVENT ||
        ctrl == CTRL_BREAK_EVENT) {
        fprintf(stderr, "\nmain: received console ctrl event %lu, shutting down...\n", ctrl);
        main_running = 0;
        return TRUE;
    }
    return FALSE;
}
#endif

/* ============================================================ */
/* State save/load (PersistentV2 format)                         */
/* ============================================================ */

void save_state(const char *filepath) {
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "save_state: cannot write '%s'\n", filepath);
        return;
    }

    extern void PersPageExport(const Pagestruct*, card8*, size_t);
    extern void PersGridExport(card8*, size_t);

    /* Write grid */
    card8 gridBuf[sizeof(GridPersistentV2)];
    memset(gridBuf, 0, sizeof(gridBuf));
    PersGridExport(gridBuf, sizeof(GridPersistentV2));
    fwrite("GRID", 1, 4, f);
    unsigned sz = sizeof(GridPersistentV2);
    fwrite(&sz, 4, 1, f);
    fwrite(gridBuf, sz, 1, f);

    /* Write each page */
    unsigned int p;
    for (p = 0; p < MAX_NROF_PAGES; p++) {
        card8 pageBuf[sizeof(PagePersistentV2)];
        memset(pageBuf, 0, sizeof(pageBuf));
        PersPageExport(&Page_repository[p], pageBuf, sizeof(PagePersistentV2));
        fwrite("PAGE", 1, 4, f);
        sz = sizeof(PagePersistentV2);
        fwrite(&sz, 4, 1, f);
        fwrite(pageBuf, sz, 1, f);
    }

    fclose(f);
    fprintf(stderr, "save_state: saved %u pages + grid to '%s'\n", MAX_NROF_PAGES, filepath);
}

/* ============================================================ */
/* Main entry point                                             */
/* ============================================================ */

int main(int argc, char **argv) {
    unsigned int pvalue = 0;
    int osc_port = 8000;
    int list_and_exit = 0;
    int autosave = 0;
    int i;

    /* Parse command-line arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list-midi") == 0 || strcmp(argv[i], "-l") == 0) {
            list_and_exit = 1;
        } else if (strcmp(argv[i], "--autosave") == 0) {
            autosave = 1;
        } else if ((strcmp(argv[i], "--out-a") == 0 || strcmp(argv[i], "--out_a") == 0) && i + 1 < argc) {
            midi_set_device(0, atoi(argv[++i]));
        } else if ((strcmp(argv[i], "--out-b") == 0 || strcmp(argv[i], "--out_b") == 0) && i + 1 < argc) {
            midi_set_device(1, atoi(argv[++i]));
        } else if ((strcmp(argv[i], "--in") == 0 || strcmp(argv[i], "--in-midi") == 0 || strcmp(argv[i], "--in_midi") == 0) && i + 1 < argc) {
            midi_set_device(2, atoi(argv[++i]));
        } else if ((strcmp(argv[i], "--osc-port") == 0 || strcmp(argv[i], "--osc_port") == 0) && i + 1 < argc) {
            osc_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage: %s [options]\n"
                "\n"
                "Options:\n"
                "  --out-a <id>   Output port A device (default: -1=MIDI_MAPPER)\n"
                "  --out-b <id>   Output port B device (default: -1=MIDI_MAPPER)\n"
                "  --in <id>      Input device (default: 0)\n"
                "  --osc-port <n> OSC server port (default: 8000)\n"
                "  --autosave    Save state to octopus_state.bin on exit (default: off)\n"
                "  --list-midi    List MIDI devices and exit\n"
                "  --help         Show this help\n"
                "\n"
                "Device IDs: -1 = MIDI_MAPPER (system default), 0..N = device index.\n"
                "Use --list-midi to see available devices and their IDs.\n"
                "Underscores are accepted: --out_a, --out_b, --osc_port\n"
                "\n", argv[0]);
            return 0;
        }
    }

    /* --list-midi: list devices and exit (cross-platform) */
    if (list_and_exit) {
#ifdef _WIN32
        timeBeginPeriod(1);
        midi_list_devices();
        timeEndPeriod(1);
#else
        midi_list_devices();
#endif
        return 0;
    }

#ifdef _WIN32
    /* Windows: use SetConsoleCtrlHandler for clean shutdown.
     * The handler sets main_running = 0 to break the main loop. */
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    /* Linux: Block SIGINT/SIGTERM in this thread. All subsequently created
     * threads inherit this mask, so signals never interrupt worker
     * threads. The main loop uses sigwait() to catch them synchronously. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
#endif

    fprintf(stderr, "octopus-linux: starting (Phase 2: OSC input)\n");

    /* Flash init */
    flash_init(diag_printf);

    /* Initialize mailboxes and mutexes */
    init_mailboxes();
    init_muxes();

    /* Initialize sem_readKeys before init_alarms (TV alarm posts to it) */
    cyg_semaphore_init(&sem_readKeys, 0);

    /* Create all alarms (double-click, quickturn, timeout, TV, overload) */
    init_alarms();

    /* Seed the randomizer */
    HAL_CLOCK_READ(&pvalue);
    srand(pvalue);

    /* Initialize MIDI */
    midi_init(24);

    /* Initialize all memory, repositories, defaults */
    fprintf(stderr, "main: calling Octopus_memory_init()...\n");
    Octopus_memory_init();
    fprintf(stderr, "main: Octopus_memory_init() completed\n");

    /* Auto-load saved state (AFTER init) */
    {
        FILE *f = fopen("octopus_state.bin", "rb");
        if (f) {
            extern void PersPageImport(const card8*, size_t, Pagestruct*);
            extern void PersGridImport(const card8*, size_t);

            char tag[4];
            unsigned sz;
            int pages_loaded = 0, grid_loaded = 0;

            while (fread(tag, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
                card8 buf[65536];
                if (sz > sizeof(buf)) break;
                if (fread(buf, 1, sz, f) != sz) break;

                if (memcmp(tag, "GRID", 4) == 0) {
                    PersGridImport(buf, sz);
                    grid_loaded = 1;
                } else if (memcmp(tag, "PAGE", 4) == 0) {
                    PagePersistentV2 *pp = (PagePersistentV2 *)buf;
                    if (pp->pageNdx < MAX_NROF_PAGES) {
                        PersPageImport(buf, sz, &Page_repository[pp->pageNdx]);
                        pages_loaded++;
                    }
                }
            }

            /* Re-assign Step and Track pointers */
            extern void Page_repository_assign_Steps(void);
            extern void Page_repository_assign_Tracks(void);
            Page_repository_assign_Steps();
            Page_repository_assign_Tracks();

            fclose(f);
            fprintf(stderr, "main: state loaded (%d pages, grid=%d)\n", pages_loaded, grid_loaded);
        }
    }

    /* Set clock to internal */
    G_clock_source = INT;

    /* Update timer refill (tempo) */
    G_TIMER_REFILL_update();

    /* Start OSC server (input from control surface) */
    osc_server_init(osc_port);

    /* Start OSC renderer (output to control surface) */
    osc_render_init("127.0.0.1", 9000);
    osc_render_start();

    /* Print connection info */
    int client_id = midi_get_client_id();
    fprintf(stderr, "\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, " Octopus Linux Port - Phase 2\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, " ALSA client: %d (ports %d:0, %d:1)\n", client_id, client_id, client_id);
    fprintf(stderr, " OSC server:  port %d\n", osc_port);
    fprintf(stderr, " Tempo:       %d BPM\n", G_master_tempo);
    fprintf(stderr, " Clock:       %s\n", G_clock_source == INT ? "Internal" : "External");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "OSC commands:\n");
    fprintf(stderr, "  /octopus/transport \"start\"\n");
    fprintf(stderr, "  /octopus/transport \"stop\"\n");
    fprintf(stderr, "  /octopus/key <index> <1=press|0=release>\n");
    fprintf(stderr, "  /octopus/rotary <0-20> <1=DEC|2=INC>\n");
    fprintf(stderr, "  /octopus/tempo <bpm>\n");
    fprintf(stderr, "  /octopus/quit\n");
    fprintf(stderr, "\n");

    /* Start the sequencer */
    sequencer_START();
    start_sequencer_thread();

    /* Main loop — wait for /octopus/quit or Ctrl+C.
     * main_running is set to 0 by /octopus/quit via OSC, or by the
     * console control handler (Windows) / signal handler (Linux). */
    fprintf(stderr, "main: running. Send /octopus/quit or Ctrl+C to exit.\n");
    while (main_running) {
#ifdef _WIN32
        Sleep(100);
#else
        int sig;
        if (sigwait(&mask, &sig) == 0) {
            fprintf(stderr, "\nmain: received signal %d, shutting down...\n", sig);
            main_running = 0;
        }
#endif
    }

    /* Cleanup — order matters on Windows to avoid zombie processes:
     * 1. Stop all thread running flags
     * 2. Close MIDI devices (prevents winmm DLL cleanup hang)
     * 3. Save state
     * 4. TerminateProcess (skips DllMain that would hang on open handles)
     */
    fprintf(stderr, "main: shutting down...\n");
    fflush(stderr);
    sequencer_STOP(true);
    sequencer_running = 0;

#ifdef _WIN32
    /* Give the sequencer thread a moment to notice sequencer_running=0
     * and exit its loop. Don't pthread_join — the MMCSS thread may
     * take too long to clean up. */
    Sleep(50);

    /* Close MIDI devices BEFORE exiting. If winmm handles are still
     * open when TerminateProcess runs, the DLL detach handler can
     * hang, creating an unkillable zombie process. */
    midi_cleanup();
#else
    pthread_join(sequencer_pthread, NULL);
#endif

    /* Auto-save state (only if --autosave flag was passed) */
    if (autosave) {
        save_state("octopus_state.bin");
    }

    fprintf(stderr, "main: done.\n");
    fflush(stderr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0);
#else
    _exit(0);
#endif
}
