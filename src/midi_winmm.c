/*
 * midi_winmm.c — Windows winmm MIDI backend for the Octopus/Nemo port.
 *
 * Replaces midi_alsa.c. Uses the classic winmm API (midiOutShortMsg)
 * which is universal on all Windows versions.
 *
 * Two output devices (out_A, out_B) map to physical/virtual MIDI out ports.
 * Channel routing matches the ALSA port: channels 1-16 -> out_A, 17-32 -> out_B,
 * 33-48 -> out_A (virtual), 49-64 -> out_B (virtual).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include "hal_linux.h"

/* MIDI message type constants — must match midi_alsa.c values */
#define ALSA_MIDI_NOTE      0
#define ALSA_MIDI_PGMCH     1
#define ALSA_MIDI_CC        2
#define ALSA_MIDI_CLOCK     3
#define ALSA_MIDI_BENDER    4
#define ALSA_MIDI_PRESSURE  6

#define ALSA_MIDICLOCK_CLOCK     0xF8
#define ALSA_MIDICLOCK_START     0xFA
#define ALSA_MIDICLOCK_CONTINUE  0xFB
#define ALSA_MIDICLOCK_STOP      0xFC

static HMIDIOUT hMidiOut_A = NULL;
static HMIDIOUT hMidiOut_B = NULL;
static HMIDIIN  hMidiIn    = NULL;
static int winmm_initialized = 0;

/* Device IDs. -1 = MIDI_MAPPER (system default), >=0 = specific device index.
 * Set before midi_init() via command-line args, or changed at runtime via OSC. */
static int g_dev_out_a = -1;  /* MIDI_MAPPER */
static int g_dev_out_b = -1;  /* MIDI_MAPPER (falls back to sharing A) */
static int g_dev_in    = 0;   /* first input device */

/* Input callback thread state */
static pthread_t input_pthread;
static volatile int input_running = 0;

/* Firmware functions for MIDI input interpretation (same as midi_alsa.c) */
extern void G_midi_interpret_REALTIME(unsigned char byte);
extern void G_midi_interpret_NOTE_ON(unsigned char channel, unsigned char pitch, unsigned char velocity);
extern void G_midi_interpret_CONTROL(unsigned char channel, unsigned char controller, unsigned char value);
extern void G_midi_interpret_BENDER(unsigned char channel, unsigned char lsb, unsigned char msb);
extern void G_midi_interpret_PRESSURE(unsigned char channel, unsigned char pressure);
extern unsigned char G_clock_source;
extern unsigned char G_run_bit;

/* ============================================================ */
/* Device enumeration                                           */
/* ============================================================ */

/* Forward declaration — defined below */
static void CALLBACK midi_in_callback(HMIDIIN hMidiIn, UINT wMsg,
                                       DWORD_PTR dwInstance, DWORD_PTR dwParam1,
                                       DWORD_PTR dwParam2);

static void winmm_list_devices(void) {
    unsigned int i;
    MIDIOUTCAPSA caps;

    fprintf(stderr, "midi_winmm: %u MIDI output devices:\n", midiOutGetNumDevs());
    fprintf(stderr, "  [-1] <MIDI_MAPPER> (system default)\n");
    for (i = 0; i < midiOutGetNumDevs(); i++) {
        if (midiOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            fprintf(stderr, "  [%u] %s  (mid=%u pid=%u)\n",
                    i, caps.szPname, caps.wMid, caps.wPid);
        }
    }

    fprintf(stderr, "midi_winmm: %u MIDI input devices:\n", midiInGetNumDevs());
    MIDIINCAPSA icaps;
    for (i = 0; i < midiInGetNumDevs(); i++) {
        if (midiInGetDevCapsA(i, &icaps, sizeof(icaps)) == MMSYSERR_NOERROR) {
            fprintf(stderr, "  [%u] %s\n", i, icaps.szPname);
        }
    }
}

/* Get the device name for a given output device ID (for logging).
 * Returns "MIDI_MAPPER" for -1, or the name from midiOutGetDevCaps. */
static const char *winmm_out_device_name(int device_id) {
    static char name[MAXPNAMELEN];
    if (device_id < 0) return "MIDI_MAPPER";
    MIDIOUTCAPSA caps;
    if (midiOutGetDevCapsA(device_id, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
        strncpy(name, caps.szPname, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        return name;
    }
    return "(unknown)";
}

static const char *winmm_in_device_name(int device_id) {
    static char name[MAXPNAMELEN];
    MIDIINCAPSA caps;
    if (midiInGetDevCapsA(device_id, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
        strncpy(name, caps.szPname, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        return name;
    }
    return "(unknown)";
}

/* Enumerate available devices for the in-process GUI picker.
 * idx 0,1,2,...; returns the device id (0..N-1) and fills name,
 * or -1 when idx runs past the end. */
int midi_enum_devices(int is_input, int idx, char *name, int name_len) {
    if (is_input) {
        unsigned int n = midiInGetNumDevs();
        MIDIINCAPSA caps;
        if (idx < 0 || (unsigned int)idx >= n) return -1;
        if (midiInGetDevCapsA(idx, &caps, sizeof(caps)) != MMSYSERR_NOERROR) return -1;
        strncpy(name, caps.szPname, name_len - 1);
        name[name_len - 1] = '\0';
        return idx;
    } else {
        unsigned int n = midiOutGetNumDevs();
        MIDIOUTCAPSA caps;
        if (idx < 0 || (unsigned int)idx >= n) return -1;
        if (midiOutGetDevCapsA(idx, &caps, sizeof(caps)) != MMSYSERR_NOERROR) return -1;
        strncpy(name, caps.szPname, name_len - 1);
        name[name_len - 1] = '\0';
        return idx;
    }
}

/* ============================================================ */
/* Runtime device selection (callable from OSC commands)        */
/* ============================================================ */

/* Close and reopen output port: which=0 for out_A, which=1 for out_B.
 * device_id: -1=MIDI_MAPPER, >=0=specific device index.
 * Returns 0 on success, -1 on failure. */
int midi_open_out(int which, int device_id) {
    HMIDIOUT *phOut = (which == 0) ? &hMidiOut_A : &hMidiOut_B;
    int *pdev = (which == 0) ? &g_dev_out_a : &g_dev_out_b;

    /* Close existing handle */
    if (*phOut) {
        midiOutClose(*phOut);
        *phOut = NULL;
    }

    /* Validate device ID */
    if (device_id >= 0) {
        unsigned int n = midiOutGetNumDevs();
        if ((unsigned int)device_id >= n) {
            fprintf(stderr, "midi_winmm: out_%c device %d out of range (0-%u)\n",
                    which == 0 ? 'A' : 'B', device_id, n - 1);
            return -1;
        }
    }

    /* Open new device */
    UINT_PTR dev = (device_id < 0) ? MIDI_MAPPER : (UINT_PTR)device_id;
    MMRESULT err = midiOutOpen(phOut, dev, 0, 0, CALLBACK_NULL);
    if (err != MMSYSERR_NOERROR) {
        char errmsg[MAXERRORLENGTH];
        midiOutGetErrorTextA(err, errmsg, sizeof(errmsg));
        fprintf(stderr, "midi_winmm: out_%c device %d (%s) open failed: %s\n",
                which == 0 ? 'A' : 'B', device_id, winmm_out_device_name(device_id), errmsg);
        *phOut = NULL;
        return -1;
    }

    *pdev = device_id;
    fprintf(stderr, "midi_winmm: out_%c -> [%d] %s\n",
            which == 0 ? 'A' : 'B', device_id, winmm_out_device_name(device_id));
    return 0;
}

/* Close and reopen input port.
 * device_id: >=0=specific device index.
 * Returns 0 on success, -1 on failure. */
int midi_open_in(int device_id) {
    /* Close existing */
    if (hMidiIn) {
        midiInStop(hMidiIn);
        midiInClose(hMidiIn);
        hMidiIn = NULL;
    }

    /* Validate */
    unsigned int n = midiInGetNumDevs();
    if (device_id < 0 || (unsigned int)device_id >= n) {
        fprintf(stderr, "midi_winmm: input device %d out of range (0-%u)\n", device_id, n - 1);
        return -1;
    }

    MMRESULT err = midiInOpen(&hMidiIn, device_id, (DWORD_PTR)midi_in_callback, 0, CALLBACK_FUNCTION);
    if (err != MMSYSERR_NOERROR) {
        fprintf(stderr, "midi_winmm: input device %d (%s) open failed\n",
                device_id, winmm_in_device_name(device_id));
        hMidiIn = NULL;
        return -1;
    }

    midiInStart(hMidiIn);
    g_dev_in = device_id;
    fprintf(stderr, "midi_winmm: in -> [%d] %s\n", device_id, winmm_in_device_name(device_id));
    return 0;
}

/* Print the device list to stderr (callable from OSC /midi/list) */
void midi_list_devices(void) {
    winmm_list_devices();
    fprintf(stderr, "midi_winmm: current: out_A=[%d] %s, out_B=[%d] %s, in=[%d] %s\n",
            g_dev_out_a, winmm_out_device_name(g_dev_out_a),
            g_dev_out_b, winmm_out_device_name(g_dev_out_b),
            g_dev_in, hMidiIn ? winmm_in_device_name(g_dev_in) : "(none)");
}

/* ============================================================ */
/* MIDI Input callback                                          */
/* ============================================================ */

/* winmm calls us from its own thread via the callback function.
 * We translate to the same G_midi_interpret_* calls as midi_alsa.c. */

static void CALLBACK midi_in_callback(HMIDIIN hMidiIn, UINT wMsg,
                                       DWORD_PTR dwInstance, DWORD_PTR dwParam1,
                                       DWORD_PTR dwParam2) {
    (void)hMidiIn;
    (void)dwInstance;
    (void)dwParam2;

    if (wMsg != MIM_DATA) return;

    DWORD msg = (DWORD)dwParam1;
    unsigned char status = msg & 0xFF;
    unsigned char data1  = (msg >> 8) & 0xFF;
    unsigned char data2  = (msg >> 16) & 0xFF;

    cyg_scheduler_lock();

    /* Realtime messages */
    if (status >= 0xF8) {
        G_midi_interpret_REALTIME(status);
        cyg_scheduler_unlock();
        return;
    }

    unsigned char channel = (status & 0x0F) + 1;  /* firmware uses 1-based channels */
    unsigned char msg_type = status & 0xF0;

    switch (msg_type) {
        case 0x90:  /* Note On */
            G_midi_interpret_NOTE_ON(channel, data1, data2);
            break;
        case 0x80:  /* Note Off */
            G_midi_interpret_NOTE_ON(channel, data1, 0);
            break;
        case 0xB0:  /* Control Change */
            G_midi_interpret_CONTROL(channel, data1, data2);
            break;
        case 0xE0:  /* Pitch Bend */
            G_midi_interpret_BENDER(channel, data1, data2);
            break;
        case 0xD0:  /* Channel Pressure */
            G_midi_interpret_PRESSURE(channel, data1);
            break;
        default:
            break;
    }

    cyg_scheduler_unlock();
}

/* ============================================================ */
/* Public API — matches the neutral interface in hal_linux.h    */
/* ============================================================ */

void midi_init(int queue_ppqn) {
    (void)queue_ppqn;  /* winmm has no queue; timing is driven by sequencer thread */

    if (winmm_initialized) return;

    /* Raise timer resolution to 1ms for more accurate Sleep() in the
     * sequencer/render threads. Restore on cleanup. */
    timeBeginPeriod(1);

    winmm_list_devices();

    /* Open output device A using the configured device ID.
     * Default is MIDI_MAPPER (-1) = system default synth. */
    midi_open_out(0, g_dev_out_a);

    /* Open output device B. If only 1 device exists, B stays NULL
     * and send_event falls back to A. Default is MIDI_MAPPER. */
    if (midiOutGetNumDevs() > 1) {
        midi_open_out(1, g_dev_out_b);
    } else {
        /* Single device: open it for B too (same device, both ports work) */
        midi_open_out(1, g_dev_out_a >= 0 ? g_dev_out_a : 0);
    }

    /* Open MIDI input using configured device ID */
    if (midiInGetNumDevs() > 0) {
        midi_open_in(g_dev_in);
    }

    winmm_initialized = 1;
    input_running = 1;

    fprintf(stderr, "midi_winmm: initialized\n");
}

/* Set device IDs before calling midi_init().
 * which: 0=out_A, 1=out_B, 2=in.
 * device_id: -1=MIDI_MAPPER (out only), >=0=specific device index. */
void midi_set_device(int which, int device_id) {
    switch (which) {
        case 0: g_dev_out_a = device_id; break;
        case 1: g_dev_out_b = device_id; break;
        case 2: g_dev_in    = device_id; break;
    }
}

void midi_set_tempo(int bpm) {
    /* winmm has no queue tempo; the sequencer thread handles timing */
    (void)bpm;
}

void midi_start_queue(void) {
    /* No-op — sequencer thread drives timing */
}

void midi_stop_queue(void) {
    /* No-op */
}

void midi_continue_queue(void) {
    /* No-op */
}

void midi_send_event(int type, int val0, int val1, int val2, unsigned int timestamp) {
    (void)timestamp;  /* winmm sends immediately; sequencer thread handles scheduling */

    if (!hMidiOut_A && !hMidiOut_B) return;

    /* Route by channel: 1-16 -> A, 17-32 -> B, 33-48 -> A, 49-64 -> B */
    HMIDIOUT hOut = hMidiOut_A;
    int channel = val0;

    if (type == ALSA_MIDI_NOTE || type == ALSA_MIDI_PGMCH || type == ALSA_MIDI_CC ||
        type == ALSA_MIDI_BENDER || type == ALSA_MIDI_PRESSURE) {

        if (val0 > 16 && val0 <= 32) {
            hOut = hMidiOut_B ? hMidiOut_B : hMidiOut_A;
            channel = val0 - 16;
        } else if (val0 > 32 && val0 <= 48) {
            channel = val0 - 32;
        } else if (val0 > 48 && val0 <= 64) {
            hOut = hMidiOut_B ? hMidiOut_B : hMidiOut_A;
            channel = val0 - 48;
        }
        /* Convert to 0-based channel for MIDI protocol */
        if (channel > 0) channel--;
        else channel = 0;
    }

    DWORD msg;

    switch (type) {
        case ALSA_MIDI_NOTE: {
            unsigned char status = (val2 == 0) ? 0x80 : 0x90;  /* Note Off/On */
            msg = ((DWORD)val2 << 16) | ((DWORD)val1 << 8) | (status | (channel & 0x0F));
            midiOutShortMsg(hOut, msg);
            break;
        }
        case ALSA_MIDI_PGMCH:
            msg = ((DWORD)val1 << 8) | (0xC0 | (channel & 0x0F));
            midiOutShortMsg(hOut, msg);
            break;
        case ALSA_MIDI_CC:
            msg = ((DWORD)val2 << 16) | ((DWORD)val1 << 8) | (0xB0 | (channel & 0x0F));
            midiOutShortMsg(hOut, msg);
            break;
        case ALSA_MIDI_BENDER: {
            int bend = (val2 << 7 | val1) - 8192;
            unsigned int ubend = (unsigned int)(bend + 8192);  /* 0-16383 */
            msg = ((DWORD)(ubend & 0x3FFF) << 0) | (0xE0 | (channel & 0x0F));
            /* winmm pitch bend: bits 0-6 = LSB, bits 8-14 = MSB */
            msg = (((ubend >> 7) & 0x7F) << 16) | ((ubend & 0x7F) << 8) | (0xE0 | (channel & 0x0F));
            midiOutShortMsg(hOut, msg);
            break;
        }
        case ALSA_MIDI_PRESSURE:
            msg = ((DWORD)val1 << 8) | (0xD0 | (channel & 0x0F));
            midiOutShortMsg(hOut, msg);
            break;
        case ALSA_MIDI_CLOCK: {
            /* System Real-Time messages — single byte */
            unsigned char rt = 0;
            switch (val0) {
                case ALSA_MIDICLOCK_CLOCK:  rt = 0xF8; break;
                case ALSA_MIDICLOCK_START:  rt = 0xFA; break;
                case ALSA_MIDICLOCK_CONTINUE: rt = 0xFB; break;
                case ALSA_MIDICLOCK_STOP:   rt = 0xFC; break;
            }
            if (rt) {
                /* Send clock to both outputs if available */
                if (hMidiOut_A) midiOutShortMsg(hMidiOut_A, rt);
                if (hMidiOut_B) midiOutShortMsg(hMidiOut_B, rt);
            }
            break;
        }
        default:
            break;
    }
}

void midi_flush_queue(unsigned int current_timestamp) {
    (void)current_timestamp;
    /* No-op — winmm sends are immediate */
}

int midi_get_client_id(void) {
    /* Windows has no client ID concept. Return a fixed value for display. */
    return 0;
}

void midi_cleanup(void) {
    input_running = 0;

    if (hMidiIn) {
        midiInStop(hMidiIn);
        midiInClose(hMidiIn);
        hMidiIn = NULL;
    }
    if (hMidiOut_A) {
        midiOutClose(hMidiOut_A);
        hMidiOut_A = NULL;
    }
    if (hMidiOut_B) {
        midiOutClose(hMidiOut_B);
        hMidiOut_B = NULL;
    }

    timeEndPeriod(1);
    winmm_initialized = 0;
    fprintf(stderr, "midi_winmm: cleanup complete\n");
}

/* Input thread function — on Windows, the winmm callback runs in its own
 * thread so we don't need a separate polling thread. This stub exists for
 * API compatibility (called from main_linux.c on Linux but not on Windows). */
void *midi_input_thread(void *arg) {
    (void)arg;
    return NULL;
}
