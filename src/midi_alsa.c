/*
 * midi_alsa.c — ALSA sequencer binding for the Octopus/Nemo port.
 *
 * Two output ports (out_A, out_B) + one input port for clock slave / recording.
 */

#include <alsa/asoundlib.h>
#include "hal_linux.h"

static snd_seq_t *seq_handle = NULL;
static int out_port_a = -1;
static int out_port_b = -1;
static int in_port = -1;
static int queue_id = -1;
static pthread_t input_thread;
static volatile int input_running = 0;

static int g_dev_out_a = -1;
static int g_dev_out_b = -1;
static int g_dev_in = -1;

void midi_set_device(int which, int device_id) {
    switch (which) {
        case 0: g_dev_out_a = device_id; break;
        case 1: g_dev_out_b = device_id; break;
        case 2: g_dev_in    = device_id; break;
    }
}

void midi_list_devices(void) {
    snd_seq_t *tmp;
    if (snd_seq_open(&tmp, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        fprintf(stderr, "midi_alsa: cannot open sequencer for listing\n");
        return;
    }

    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    int self_client = snd_seq_client_id(tmp);

    fprintf(stderr, "midi_alsa: MIDI output devices:\n");
    fprintf(stderr, "  [-1] <none> (no connection)\n");
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(tmp, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == self_client || client == 0) continue;
        const char *cname = snd_seq_client_info_get_name(cinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(tmp, pinfo) >= 0) {
            int port = snd_seq_port_info_get_port(pinfo);
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & SND_SEQ_PORT_CAP_SUBS_WRITE) && !(caps & SND_SEQ_PORT_CAP_NO_EXPORT)) {
                int id = client * 256 + port;
                fprintf(stderr, "  [%d] %s:%d %s\n", id, cname, port,
                        snd_seq_port_info_get_name(pinfo));
            }
        }
    }

    fprintf(stderr, "midi_alsa: MIDI input devices:\n");
    fprintf(stderr, "  [-1] <none> (no connection)\n");
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(tmp, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == self_client || client == 0) continue;
        const char *cname = snd_seq_client_info_get_name(cinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(tmp, pinfo) >= 0) {
            int port = snd_seq_port_info_get_port(pinfo);
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & SND_SEQ_PORT_CAP_SUBS_READ) && !(caps & SND_SEQ_PORT_CAP_NO_EXPORT)) {
                int id = client * 256 + port;
                fprintf(stderr, "  [%d] %s:%d %s\n", id, cname, port,
                        snd_seq_port_info_get_name(pinfo));
            }
        }
    }

    snd_seq_close(tmp);
}

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

void midi_init(int queue_ppqn) {
    int err;

    err = snd_seq_open(&seq_handle, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (err < 0) {
        fprintf(stderr, "midi_alsa: Failed to open ALSA sequencer: %s\n", snd_strerror(err));
        return;
    }

    snd_seq_set_client_name(seq_handle, "Octopus");

    out_port_a = snd_seq_create_simple_port(seq_handle, "out_A",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    out_port_b = snd_seq_create_simple_port(seq_handle, "out_B",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    in_port = snd_seq_create_simple_port(seq_handle, "in",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    if (out_port_a < 0 || out_port_b < 0 || in_port < 0) {
        fprintf(stderr, "midi_alsa: Failed to create ports\n");
    }

    queue_id = snd_seq_alloc_queue(seq_handle);

    snd_seq_queue_tempo_t *tempo;
    snd_seq_queue_tempo_alloca(&tempo);
    snd_seq_queue_tempo_set_tempo(tempo, 60000000 / 120);
    snd_seq_queue_tempo_set_ppq(tempo, queue_ppqn);
    snd_seq_set_queue_tempo(seq_handle, queue_id, tempo);

    /* Auto-connect to selected MIDI devices */
    if (g_dev_out_a >= 0) {
        int c = g_dev_out_a / 256, p = g_dev_out_a % 256;
        if (snd_seq_connect_to(seq_handle, out_port_a, c, p) < 0)
            fprintf(stderr, "midi_alsa: failed to connect out_A to %d:%d\n", c, p);
        else
            fprintf(stderr, "midi_alsa: out_A -> %d:%d\n", c, p);
    }
    if (g_dev_out_b >= 0) {
        int c = g_dev_out_b / 256, p = g_dev_out_b % 256;
        if (snd_seq_connect_to(seq_handle, out_port_b, c, p) < 0)
            fprintf(stderr, "midi_alsa: failed to connect out_B to %d:%d\n", c, p);
        else
            fprintf(stderr, "midi_alsa: out_B -> %d:%d\n", c, p);
    }
    if (g_dev_in >= 0) {
        int c = g_dev_in / 256, p = g_dev_in % 256;
        if (snd_seq_connect_from(seq_handle, in_port, c, p) < 0)
            fprintf(stderr, "midi_alsa: failed to connect in from %d:%d\n", c, p);
        else
            fprintf(stderr, "midi_alsa: in <- %d:%d\n", c, p);
    }

    fprintf(stderr, "midi_alsa: Initialized (client=%d, out_A=%d, out_B=%d, in=%d, queue=%d)\n",
            snd_seq_client_id(seq_handle), out_port_a, out_port_b, in_port, queue_id);

    /* Start MIDI input thread */
    input_running = 1;
    pthread_create(&input_thread, NULL, midi_input_thread, NULL);
}

/* ============================================================ */
/* MIDI Input — feeds firmware's MIDI interpreter              */
/* ============================================================ */

/* Firmware functions for MIDI input interpretation */
extern void G_midi_interpret_REALTIME(unsigned char byte);
extern void G_midi_interpret_NOTE_ON(unsigned char channel, unsigned char pitch, unsigned char velocity);
extern void G_midi_interpret_CONTROL(unsigned char channel, unsigned char controller, unsigned char value);
extern void G_midi_interpret_BENDER(unsigned char channel, unsigned char lsb, unsigned char msb);
extern void G_midi_interpret_PRESSURE(unsigned char channel, unsigned char pressure);
extern unsigned char G_clock_source;
extern unsigned char G_run_bit;

/* External clock tempo smoothing state */
static unsigned int clock_pulse_intervals[8];
static int clock_interval_idx = 0;
static int clock_pulse_count = 0;
static struct timespec last_clock_time;

void *midi_input_thread(void *arg) {
    (void)arg;
    snd_seq_event_t *ev;

    fprintf(stderr, "midi_alsa: input thread started\n");

    while (input_running) {
        int r = snd_seq_event_input(seq_handle, &ev);

        if (r < 0) {
            if (r == -ENODEV) break;
            continue;
        }

        cyg_scheduler_lock();

        switch (ev->type) {
            case SND_SEQ_EVENT_CLOCK:
                /* MIDI clock pulse — feed to firmware realtime interpreter */
                G_midi_interpret_REALTIME(ALSA_MIDICLOCK_CLOCK);
                break;

            case SND_SEQ_EVENT_START:
                G_midi_interpret_REALTIME(ALSA_MIDICLOCK_START);
                break;

            case SND_SEQ_EVENT_CONTINUE:
                G_midi_interpret_REALTIME(ALSA_MIDICLOCK_CONTINUE);
                break;

            case SND_SEQ_EVENT_STOP:
                G_midi_interpret_REALTIME(ALSA_MIDICLOCK_STOP);
                break;

            case SND_SEQ_EVENT_NOTEON:
                G_midi_interpret_NOTE_ON(
                    ev->data.note.channel + 1,
                    ev->data.note.note,
                    ev->data.note.velocity);
                break;

            case SND_SEQ_EVENT_NOTEOFF:
                G_midi_interpret_NOTE_ON(
                    ev->data.note.channel + 1,
                    ev->data.note.note,
                    0);
                break;

            case SND_SEQ_EVENT_CONTROLLER:
                G_midi_interpret_CONTROL(
                    ev->data.control.channel + 1,
                    ev->data.control.param,
                    ev->data.control.value);
                break;

            case SND_SEQ_EVENT_PITCHBEND:
                {   int pb = ev->data.control.value + 8192;
                    G_midi_interpret_BENDER(
                        ev->data.control.channel + 1,
                        pb & 0x7F,
                        (pb >> 7) & 0x7F);
                }
                break;

            case SND_SEQ_EVENT_CHANPRESS:
                G_midi_interpret_PRESSURE(
                    ev->data.control.channel + 1,
                    ev->data.control.value);
                break;

            default:
                break;
        }

        snd_seq_free_event(ev);
        cyg_scheduler_unlock();
    }

    return NULL;
}

/* ============================================================ */
/* MIDI Output                                                  */
/* ============================================================ */

void midi_set_tempo(int bpm) {
    if (!seq_handle || queue_id < 0) return;
    snd_seq_queue_tempo_t *tempo;
    snd_seq_queue_tempo_alloca(&tempo);
    snd_seq_queue_tempo_set_tempo(tempo, 60000000 / bpm);
    snd_seq_queue_tempo_set_ppq(tempo, 24);
    snd_seq_set_queue_tempo(seq_handle, queue_id, tempo);
}

void midi_start_queue(void) {
    if (!seq_handle || queue_id < 0) return;
    snd_seq_start_queue(seq_handle, queue_id, NULL);
    snd_seq_drain_output(seq_handle);
}

void midi_stop_queue(void) {
    if (!seq_handle || queue_id < 0) return;
    snd_seq_stop_queue(seq_handle, queue_id, NULL);
    snd_seq_drain_output(seq_handle);
}

void midi_continue_queue(void) {
    if (!seq_handle || queue_id < 0) return;
    snd_seq_continue_queue(seq_handle, queue_id, NULL);
    snd_seq_drain_output(seq_handle);
}

void midi_send_event(int type, int val0, int val1, int val2, unsigned int timestamp) {
    if (!seq_handle) return;

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);

    int port = out_port_a;
    int channel = val0;

    if (type == ALSA_MIDI_NOTE || type == ALSA_MIDI_PGMCH || type == ALSA_MIDI_CC ||
        type == ALSA_MIDI_BENDER || type == ALSA_MIDI_PRESSURE) {

        if (val0 > 16 && val0 <= 32) {
            port = out_port_b;
            channel = val0 - 16;
        } else if (val0 > 32 && val0 <= 48) {
            port = out_port_a;
            channel = val0 - 32;
        } else if (val0 > 48 && val0 <= 64) {
            port = out_port_b;
            channel = val0 - 48;
        }
        if (channel > 0) channel--;
        else channel = 0;
    }

    snd_seq_ev_set_source(&ev, port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);

    switch (type) {
        case ALSA_MIDI_NOTE:
            if (val2 == 0) snd_seq_ev_set_noteoff(&ev, channel, val1, 0);
            else snd_seq_ev_set_noteon(&ev, channel, val1, val2);
            break;
        case ALSA_MIDI_PGMCH:
            snd_seq_ev_set_pgmchange(&ev, channel, val1);
            break;
        case ALSA_MIDI_CC:
            snd_seq_ev_set_controller(&ev, channel, val1, val2);
            break;
        case ALSA_MIDI_BENDER:
            snd_seq_ev_set_pitchbend(&ev, channel, (val2 << 7 | val1) - 8192);
            break;
        case ALSA_MIDI_PRESSURE:
            snd_seq_ev_set_chanpress(&ev, channel, val1);
            break;
        case ALSA_MIDI_CLOCK:
            switch (val0) {
                case ALSA_MIDICLOCK_CLOCK:  ev.type = SND_SEQ_EVENT_CLOCK; break;
                case ALSA_MIDICLOCK_START:  ev.type = SND_SEQ_EVENT_START; break;
                case ALSA_MIDICLOCK_CONTINUE: ev.type = SND_SEQ_EVENT_CONTINUE; break;
                case ALSA_MIDICLOCK_STOP:   ev.type = SND_SEQ_EVENT_STOP; break;
            }
            /* Send MIDI clock out immediately. */
            snd_seq_event_output_direct(seq_handle, &ev);
            return;
    }

    snd_seq_event_output_direct(seq_handle, &ev);
}

void midi_flush_queue(unsigned int current_timestamp) {
    if (!seq_handle) return;
    snd_seq_drain_output(seq_handle);
}

int midi_get_client_id(void) {
    if (!seq_handle) return -1;
    return snd_seq_client_id(seq_handle);
}

void midi_cleanup(void) {
    if (!seq_handle) return;

    /* Stop the input thread — make snd_seq_event_input non-blocking so
     * the thread sees input_running=0 and exits. */
    input_running = 0;
    snd_seq_nonblock(seq_handle, 1);
    pthread_join(input_thread, NULL);

    snd_seq_close(seq_handle);
    seq_handle = NULL;
}

int midi_open_out(int which, int device_id) {
    if (!seq_handle) return -1;
    int port = (which == 0) ? out_port_a : out_port_b;
    if (port < 0) return -1;

    /* Disconnect all existing subscriptions on this port */
    snd_seq_unsubscribe_port(seq_handle, port);

    if (device_id < 0) return 0;

    int c = device_id / 256, p = device_id % 256;
    if (snd_seq_connect_to(seq_handle, port, c, p) < 0) {
        fprintf(stderr, "midi_alsa: failed to connect out_%c to %d:%d\n",
                which == 0 ? 'A' : 'B', c, p);
        return -1;
    }
    fprintf(stderr, "midi_alsa: out_%c -> %d:%d\n", which == 0 ? 'A' : 'B', c, p);
    if (which == 0) g_dev_out_a = device_id;
    else g_dev_out_b = device_id;
    return 0;
}

int midi_open_in(int device_id) {
    if (!seq_handle) return -1;
    if (in_port < 0) return -1;

    snd_seq_unsubscribe_port(seq_handle, in_port);

    if (device_id < 0) return 0;

    int c = device_id / 256, p = device_id % 256;
    if (snd_seq_connect_from(seq_handle, in_port, c, p) < 0) {
        fprintf(stderr, "midi_alsa: failed to connect in from %d:%d\n", c, p);
        return -1;
    }
    fprintf(stderr, "midi_alsa: in <- %d:%d\n", c, p);
    g_dev_in = device_id;
    return 0;
}
