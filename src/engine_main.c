/*
 * engine_main.c — Standalone entry point for the Octopus engine.
 *
 * Thin wrapper around the engine API in engine.c: parses command-line
 * arguments, runs the engine, waits for /quit or Ctrl+C, shuts down.
 */

#include "engine_api.h"
#include "hal_linux.h"
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
/* Windows: timeBeginPeriod for --list-midi */
#include <timeapi.h>
#endif

/* main_running and save_state live in engine.c */
extern volatile int main_running;
extern void save_state(const char *filepath);

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

int main(int argc, char **argv) {
    oct_engine_opts_t opts;
    int list_and_exit = 0;
    int autosave = 0;
    int i;

    opts.osc_udp_port = 8000;
    opts.out_a = -1;
    opts.out_b = -1;
    opts.midi_in = -1;
    opts.state_file = NULL;
    opts.hosted = 0;

    /* Parse command-line arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list-midi") == 0 || strcmp(argv[i], "-l") == 0) {
            list_and_exit = 1;
        } else if (strcmp(argv[i], "--autosave") == 0) {
            autosave = 1;
        } else if ((strcmp(argv[i], "--out-a") == 0 || strcmp(argv[i], "--out_a") == 0) && i + 1 < argc) {
            opts.out_a = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--out-b") == 0 || strcmp(argv[i], "--out_b") == 0) && i + 1 < argc) {
            opts.out_b = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--in") == 0 || strcmp(argv[i], "--in-midi") == 0 || strcmp(argv[i], "--in_midi") == 0) && i + 1 < argc) {
            opts.midi_in = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--osc-port") == 0 || strcmp(argv[i], "--osc_port") == 0) && i + 1 < argc) {
            opts.osc_udp_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            opts.state_file = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage: %s [options]\n"
                "\n"
                "Options:\n"
                "  --out-a <id>   Output port A device (default: -1=MIDI_MAPPER)\n"
                "  --out-b <id>   Output port B device (default: -1=MIDI_MAPPER)\n"
                "  --in <id>      Input device (default: 0)\n"
                "  --osc-port <n> OSC server port (default: 8000)\n"
                "  --state <path> State file (default: octopus_state.bin in cwd)\n"
                "  --autosave     Save state on exit (default: off)\n"
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

    if (oct_engine_start(&opts) != 0) {
        fprintf(stderr, "main: engine failed to start\n");
        return 1;
    }

    fprintf(stderr, "main: running. Send /quit or Ctrl+C to exit.\n");

    /* Main loop — wait for /quit or Ctrl+C.
     * main_running is set to 0 by /quit via OSC, or by the
     * console control handler (Windows) / signal handler (Linux). */
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

    oct_engine_stop();

    /* Auto-save state (only if --autosave flag was passed) */
    if (autosave) {
        save_state(opts.state_file ? opts.state_file : "octopus_state.bin");
    }

    fprintf(stderr, "main: done.\n");
    fflush(stderr);
    return 0;
}
