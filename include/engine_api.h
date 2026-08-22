/*
 * engine_api.h — Public API for the Octopus engine.
 *
 * Used by the standalone binary (engine_main.c) and by the hosted Rust
 * GUI/CLI binaries via FFI.
 */

#ifndef ENGINE_API_H
#define ENGINE_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* Engine startup options. Zero/default fields get sensible defaults. */
typedef struct {
    int osc_udp_port;   /* UDP OSC input port (0 = disable; host injects via oct_send_osc) */
    int out_a;          /* MIDI output A device id (-1 = none/default) */
    int out_b;          /* MIDI output B device id (-1 = none/default) */
    int midi_in;        /* MIDI input device id (-1 = none/default) */
    const char *state_file; /* State file path (NULL = "octopus_state.bin" in cwd) */
    int hosted;         /* 1 = running inside another process: /quit won't signal, no UDP render */
} oct_engine_opts_t;

/* Start the engine (init firmware, load state, spawn threads).
 * Returns 0 on success, -1 if already started. One start per process. */
int  oct_engine_start(const oct_engine_opts_t *opts);

/* Stop all engine threads and close MIDI devices. Final — the engine
 * cannot be restarted in the same process. */
void oct_engine_stop(void);

int  oct_engine_running(void);
int  oct_engine_wants_quit(void); /* main_running == 0 (set by /quit) */

/* Inject a raw OSC packet into the dispatcher (same path as the UDP
 * server). Thread-safe: takes the scheduler lock. */
void oct_send_osc(const unsigned char *buf, int len);

/* Save engine state (NULL = default path) */
void oct_save_state(const char *filepath);

/* MIDI device enumeration: iterate idx = 0,1,2,... until -1.
 * Returns the device id and fills name; -1 when idx is out of range. */
int  midi_enum_devices(int is_input, int idx, char *name, int name_len);

/* Frame callback mode (hosted): register BEFORE oct_engine_start to
 * receive packed OSC output frames (MIR blobs, /transport) instead of
 * them being sent over UDP to port 9000. */
typedef void (*oct_frame_cb_t)(const unsigned char *buf, int len);
void oct_set_frame_callback(oct_frame_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_API_H */
