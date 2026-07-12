/*
 * osc_server.c — Minimal OSC server over UDP.
 *
 * Implements just enough of the OSC protocol to receive control messages
 * from a control surface (Open Stage Control, TouchOSC, etc.). No external
 * library dependency — raw UDP sockets + binary parsing.
 *
 * Supported argument types: int32 ('i'), float32 ('f'), string ('s').
 * Address matching is exact (no globbing).
 */

#include "hal_linux.h"
#ifndef _WIN32
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

/* ============================================================ */
/* Minimal OSC protocol implementation                          */
/* ============================================================ */

#ifdef _WIN32
#define OSC_CLOSE(fd) closesocket(fd)
#define OSC_ERRNO WSAGetLastError()
#define OSC_EINTR WSAEINTR
#else
#define OSC_CLOSE(fd) close(fd)
#define OSC_ERRNO errno
#define OSC_EINTR EINTR
#endif

/* Round up to 4-byte boundary (OSC padding rule) */
static int osc_padded(int len) {
    return (len + 3) & ~3;
}

/* Read a 4-byte big-endian int32 from buffer */
static int osc_read_int32(const unsigned char *buf) {
    return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

/* Read a 4-byte big-endian float32 from buffer */
static float osc_read_float32(const unsigned char *buf) {
    union { int i; float f; } u;
    u.i = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    return u.f;
}

/* Parsed OSC message */
typedef struct {
    char address[256];
    char types[64];
    int nargs;
    int iargs[8];
    float fargs[8];
    char sargs[8][256];
} osc_message_t;

/* Parse a raw UDP packet into an OSC message.
 * Returns 0 on success, -1 on error. */
static int osc_parse(const unsigned char *data, int len, osc_message_t *msg) {
    int pos = 0;
    int i;

    /* Read address string */
    int addr_len = strlen((const char *)data);
    if (addr_len >= sizeof(msg->address) || addr_len >= len) return -1;
    strcpy(msg->address, (const char *)data);
    pos = osc_padded(addr_len + 1);

    if (pos >= len) return -1;

    /* Read type tag string (must start with ',') */
    if (data[pos] != ',') return -1;
    int type_len = strlen((const char *)(data + pos));
    if (type_len >= sizeof(msg->types)) return -1;
    strcpy(msg->types, (const char *)(data + pos));
    pos += osc_padded(type_len + 1);

    if (pos > len) return -1;

    /* Parse arguments */
    msg->nargs = 0;
    for (i = 1; i < type_len && msg->nargs < 8; i++) {
        char t = msg->types[i];
        if (pos + 4 > len && t != 's') return -1;

        switch (t) {
            case 'i':
                msg->iargs[msg->nargs] = osc_read_int32(data + pos);
                msg->fargs[msg->nargs] = 0;
                msg->nargs++;
                pos += 4;
                break;
            case 'f':
                msg->fargs[msg->nargs] = osc_read_float32(data + pos);
                msg->iargs[msg->nargs] = 0;
                msg->nargs++;
                pos += 4;
                break;
            case 's': {
                int slen = strlen((const char *)(data + pos));                if (pos + slen >= len) return -1;
                strncpy(msg->sargs[msg->nargs], (const char *)(data + pos), 255);
                msg->sargs[msg->nargs][255] = '\0';
                msg->iargs[msg->nargs] = 0;
                msg->fargs[msg->nargs] = 0;
                msg->nargs++;
                pos += osc_padded(slen + 1);
                break;
            }
            default:
                /* Skip unsupported types */
                msg->nargs++;
                break;
        }
        if (pos > len) break;
    }

    return 0;
}

/* ============================================================ */
/* OSC dispatch — calls into the Octopus firmware               */
/* ============================================================ */

/* These are defined in the firmware (pulled in via main_linux.c includes) */
extern unsigned char G_run_bit;
extern unsigned char G_pause_bit;
extern unsigned char G_master_tempo;
extern unsigned char G_zoom_level;
extern unsigned char G_clock_source;
extern void executeKey(unsigned int keyNdx);
extern void executeRot(unsigned int in_rotNdx);
extern void sequencer_START(void);
extern void sequencer_STOP(int midi_send_stop);
extern void sequencer_HALT(void);
extern void sequencer_UNHALT(void);
extern void G_TIMER_REFILL_update(void);
extern void Page_requestRefresh(void);

/* G_pressed_keys — used by the firmware for chord/modifier logic */
extern unsigned int G_pressed_keys[];
extern unsigned char G_key_pressed;

/* page_preview_step — cleared when all keys released (normally done in Intr_KEY.h) */
extern void* page_preview_step;

/* main_running — set to 0 by /octopus/quit to exit the main loop */
extern volatile int main_running;

/* Flash file persistence */
extern void flash_file_save(const char *filepath);
extern void flash_file_load(const char *filepath);
/* Firmware flash flush/recall functions */
extern void Flash_write_all_pages(void);
extern void Flash_read_all_pages(void);
extern void Flash_write_grid(void);
extern void Flash_read_grid(void);
extern void Octopus_recall_flash(void);

#define KEY_PRESS   1
#define KEY_RELEASE 0

static volatile int osc_running = 0;
static int osc_sockfd = -1;
static pthread_t osc_thread;

/* Simple helpers to get args */
static int osc_arg_int(const osc_message_t *msg, int idx, int def) {
    if (idx >= msg->nargs) return def;
    char t = msg->types[idx + 1]; /* +1 because types[0] is ',' */
    if (t == 'i') return msg->iargs[idx];
    if (t == 'f') return (int)msg->fargs[idx];
    return def;
}

static const char *osc_arg_string(const osc_message_t *msg, int idx, const char *def) {
    if (idx >= msg->nargs) return def;
    return msg->sargs[idx];
}

/* ============================================================ */
/* Name → index lookup tables                                   */
/* ============================================================ */

typedef struct { const char *name; int index; } name_entry_t;

static const name_entry_t key_name_table[] = {
    /* Mix strip */
    {"MIX",21},{"SEL",32},{"ATR",43},{"VOL",54},{"PAN",65},{"MOD",76},{"EXP",87},
    {"U0",98},{"U1",109},{"U2",120},{"U3",131},{"U4",142},{"U5",153},
    {"MUT",164},{"EDT",175},{"ESC",186},
    /* Mutators */
    {"TGGL",187},{"SOLO",188},{"CLR",189},{"RND",190},{"FLT",191},
    {"RMX",192},{"EFF",193},{"ZOOM",194},{"CPY",195},{"PST",196},
    /* Attributes */
    {"VEL",1},{"PIT",2},{"LEN",3},{"STR",4},{"POS",5},{"DIR",6},{"AMT",7},{"GRV",8},{"MCC",9},{"MCH",10},
    /* Big Knob */
    {"BK1",201},{"BK2",200},{"BK3",199},{"BK4",198},{"BK5",197},
    {"BK6",207},{"BK7",206},{"BK8",216},{"BK9",215},{"BK100",224},{"BK200",233},
    /* Chain modes */
    {"CH1",205},{"CH2",204},{"CH3",203},{"CH4",202},
    /* Channel */
    {"CHN",213},{"FLW",214},
    /* Scale (outer ring) */
    {"MY",243},{"PEN",244},{"WHL",245},{"MAJ",246},{"MIN",247},{"DIM",248},{"CHR",249},
    /* Scale (inner ring) */
    {"SSEL",222},{"SMOD",221},{"CAD",230},
    /* Program / Tempo */
    {"PGM",242},{"TPO",234},
    /* Transport */
    {"REC",223},{"STP",231},{"PSE",232},{"P1",241},{"P2",240},{"P4",250},
    /* Zoom (center) */
    {"GRID",218},{"PAGE",219},{"TRK",220},{"STEP",227},{"MAP",228},{"PLAY",229},
    /* Notes (inner ring) */
    {"C",212},{"C#",211},{"D",210},{"D#",209},{"E",208},{"F",217},
    {"F#",225},{"G",226},{"G#",235},{"A",236},{"A#",237},{"B",238},{"CUP",239},
    /* Side bow */
    {"ALN",251},
    {"CHORD0",258},{"CHORD1",257},{"CHORD2",256},{"CHORD3",255},
    {"CHORD4",254},{"CHORD5",253},{"CHORD6",252},
    {NULL,0}
};

static const name_entry_t rot_name_table[] = {
    {"TPO",0},{"VEL",1},{"PIT",2},{"LEN",3},{"STA",4},{"POS",5},{"DIR",6},
    {"AMT",7},{"GRV",8},{"MCC",9},{"MCH",10},
    {"R0",20},{"R1",19},{"R2",18},{"R3",17},{"R4",16},
    {"R5",15},{"R6",14},{"R7",13},{"R8",12},{"R9",11},
    {NULL,0}
};

static int lookup_name(const name_entry_t *table, const char *name) {
    int i;
    for (i = 0; table[i].name; i++) {
        if (strcasecmp(table[i].name, name) == 0) return table[i].index;
    }
    return -1;
}

/* Dispatch a parsed OSC message to the appropriate firmware function */
static void osc_dispatch(const osc_message_t *msg) {
    /* /key/<NAME> <press> — name-based key press */
    if (strncmp(msg->address, "/key/", 5) == 0) {
        int keyNdx = lookup_name(key_name_table, msg->address + 5);
        if (keyNdx < 0) return;
        int press = osc_arg_int(msg, 0, 1);
        if (press) {
            G_pressed_keys[keyNdx] = keyNdx;
            G_key_pressed = 1;
            executeKey(keyNdx);
        } else {
            G_pressed_keys[keyNdx] = 0;
            int any_pressed = 0, i;
            for (i = 0; i < 261; i++) {
                if (G_pressed_keys[i]) { any_pressed = 1; break; }
            }
            if (!any_pressed) {
                G_key_pressed = 0;
                page_preview_step = NULL;
            }
        }
        return;
    }

    /* /rotary/<NAME> <INC=2|DEC=1> — name-based rotary */
    if (strncmp(msg->address, "/rotary/", 8) == 0) {
        int rotNdx = lookup_name(rot_name_table, msg->address + 8);
        if (rotNdx < 0) return;
        int dir = osc_arg_int(msg, 0, 2);
        executeRot((rotNdx << 2) | dir);
        return;
    }

    /* /key <index> <press|release> */
    if (strcmp(msg->address, "/key") == 0) {
        int keyNdx = osc_arg_int(msg, 0, 0);
        int press = osc_arg_int(msg, 1, 1);

        if (press) {
            G_pressed_keys[keyNdx] = keyNdx;
            G_key_pressed = 1;
            executeKey(keyNdx);
        } else {
            G_pressed_keys[keyNdx] = 0;
            int any_pressed = 0, i;
            for (i = 0; i < 261; i++) {
                if (G_pressed_keys[i]) { any_pressed = 1; break; }
            }
            if (!any_pressed) {
                G_key_pressed = 0;
                page_preview_step = NULL;
            }
        }
        return;
    }

    /* /octopus/rotary <rotNdx> <INC=2|DEC=1> */
    if (strcmp(msg->address, "/rotary") == 0) {
        int rotNdx = osc_arg_int(msg, 0, 0);
        int dir = osc_arg_int(msg, 1, 2);
        executeRot((rotNdx << 2) | dir);
        return;
    }

    /* /octopus/transport <"start"|"stop"|"pause"|"continue"> */
    if (strcmp(msg->address, "/transport") == 0) {
        const char *cmd = osc_arg_string(msg, 0, "");
        if (strcmp(cmd, "start") == 0) {
            sequencer_START();
        } else if (strcmp(cmd, "stop") == 0) {
            sequencer_STOP(true);
        } else if (strcmp(cmd, "pause") == 0) {
            if (G_run_bit) sequencer_HALT();
            else sequencer_UNHALT();
        } else if (strcmp(cmd, "continue") == 0) {
            sequencer_UNHALT();
        }
        return;
    }

    /* /octopus/tempo <bpm> */
    if (strcmp(msg->address, "/tempo") == 0) {
        int bpm = osc_arg_int(msg, 0, 120);
        if (bpm >= 10 && bpm <= 199) {
            G_master_tempo = bpm;
            G_TIMER_REFILL_update();
        }
        return;
    }

    /* /octopus/zoom <level> */
    if (strcmp(msg->address, "/zoom") == 0) {
        int level = osc_arg_int(msg, 0, 3);
        G_zoom_level = level;
        Page_requestRefresh();
        return;
    }

    /* /octopus/clocksource <0=OFF|1=INT|2=EXT> */
    if (strcmp(msg->address, "/clocksource") == 0) {
        G_clock_source = osc_arg_int(msg, 0, 1);
        return;
    }

    /* /midi/out_a <device_id>  — switch output port A at runtime */
    if (strcmp(msg->address, "/midi/out_a") == 0) {
        int dev = osc_arg_int(msg, 0, -1);
        midi_open_out(0, dev);
        return;
    }

    /* /midi/out_b <device_id>  — switch output port B at runtime */
    if (strcmp(msg->address, "/midi/out_b") == 0) {
        int dev = osc_arg_int(msg, 0, -1);
        midi_open_out(1, dev);
        return;
    }

    /* /midi/in <device_id>  — switch input device at runtime */
    if (strcmp(msg->address, "/midi/in") == 0) {
        int dev = osc_arg_int(msg, 0, 0);
        midi_open_in(dev);
        return;
    }

    /* /midi/list  — print available devices to stderr */
    if (strcmp(msg->address, "/midi/list") == 0) {
        midi_list_devices();
        return;
    }

    /* /octopus/save — save state to octopus_state.bin (engine keeps running) */
    if (strcmp(msg->address, "/save") == 0) {
        save_state("octopus_state.bin");
        return;
    }

    /* /octopus/load <filepath> — not supported at runtime (use auto-load) */
    if (strcmp(msg->address, "/load") == 0) {
        fprintf(stderr, "osc: load not supported at runtime, restart to reload\n");
        return;
    }

    /* /octopus/quit */
    if (strcmp(msg->address, "/quit") == 0) {
        osc_running = 0;
        main_running = 0;
#ifdef _WIN32
        /* Windows: no signal needed, main loop polls main_running */
#else
        kill(getpid(), SIGINT);  /* wake up sigwait in main thread */
#endif
        return;
    }
}

/* OSC server thread — receives UDP packets and dispatches */
static void *osc_thread_func(void *arg) {
    int port = *(int *)arg;
    struct sockaddr_in addr;
    unsigned char buf[4096];

#ifdef _WIN32
    /* Initialize Winsock */
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "osc_server: WSAStartup failed: %d\n", WSAGetLastError());
        return NULL;
    }
#endif

    osc_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (osc_sockfd < 0) {
        fprintf(stderr, "osc_server: socket() failed: %s\n", strerror(errno));
        return NULL;
    }

    /* Allow rebinding immediately after restart (Windows TIME_WAIT) */
    {
        int reuse = 1;
        setsockopt(osc_sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(osc_sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "osc_server: bind(%d) failed: %s\n", port,
#ifdef _WIN32
                "port in use"
#else
                strerror(errno)
#endif
        );
        OSC_CLOSE(osc_sockfd);
        return NULL;
    }

    fprintf(stderr, "osc_server: listening on port %d\n", port);

    struct sockaddr_in sender;
    socklen_t sender_len;

    while (osc_running) {
        sender_len = sizeof(sender);
        ssize_t n = recvfrom(osc_sockfd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&sender, &sender_len);
        if (n < 0) {
            if (OSC_ERRNO == OSC_EINTR) continue;
            break;
        }

        if (n > 0) {
            osc_render_update_target(&sender);
        }

        osc_message_t msg;
        if (osc_parse(buf, n, &msg) == 0) {
            /* Lock the scheduler to protect firmware globals */
            cyg_scheduler_lock();
            osc_dispatch(&msg);
            cyg_scheduler_unlock();
        }
    }

    OSC_CLOSE(osc_sockfd);
    osc_sockfd = -1;
    return NULL;
}

/* ============================================================ */
/* Public API                                                   */
/* ============================================================ */

void osc_server_init(int port) {
    static int osc_port;
    osc_port = port;
    osc_running = 1;
    pthread_create(&osc_thread, NULL, osc_thread_func, &osc_port);
}

void osc_server_stop(void) {
    osc_running = 0;
    if (osc_sockfd >= 0) {
        shutdown(osc_sockfd,
#ifdef _WIN32
                  SD_BOTH
#else
                  SHUT_RDWR
#endif
        );
        OSC_CLOSE(osc_sockfd);
        osc_sockfd = -1;
    }
    pthread_join(osc_thread, NULL);
#ifdef _WIN32
    WSACleanup();
#endif
}
