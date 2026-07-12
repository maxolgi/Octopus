/*
 * osc_render.c — MIR → OSC output bridge.
 *
 * Replaces VIEWER_show_MIR() from show_hwdriver.h. After the firmware's
 * fill/show pipeline populates the MIR (Matrix Intermediate Representation),
 * this module broadcasts the LED state over OSC to a control surface.
 *
 * Also provides a 60 Hz render thread that calls Page_full_refresh() and
 * a blinker timer for blink/shine LED animation.
 */

#include "hal_linux.h"
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

/* ============================================================ */
/* Minimal OSC sender (UDP)                                     */
/* ============================================================ */

#ifdef _WIN32
#define RENDER_CLOSE(fd) closesocket(fd)
#else
#define RENDER_CLOSE(fd) close(fd)
#endif

static int render_sockfd = -1;
static struct sockaddr_in render_addr;

/* Big-endian int32 writer */
static void osc_write_int32(unsigned char *buf, int val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >>  8) & 0xFF;
    buf[3] = val & 0xFF;
}

/* Send an OSC message with a single blob argument */
static void osc_send_blob(const char *address, const void *data, int datalen) {
    unsigned char buf[1024];
    int pos = 0;

    /* Address string (null-terminated, padded to 4 bytes) */
    int alen = strlen(address);
    strcpy((char *)buf + pos, address);
    pos += alen + 1;
    while (pos % 4 != 0) buf[pos++] = 0;

    /* Type tag string: ",b" for one blob */
    buf[pos++] = ',';
    buf[pos++] = 'b';
    buf[pos++] = 0;
    while (pos % 4 != 0) buf[pos++] = 0;

    /* Blob: 4-byte size + data padded to 4 bytes */
    osc_write_int32(buf + pos, datalen);
    pos += 4;
    memcpy(buf + pos, data, datalen);
    pos += datalen;
    while (pos % 4 != 0) buf[pos++] = 0;

    if (render_sockfd >= 0) {
        sendto(render_sockfd, buf, pos, 0,
               (struct sockaddr *)&render_addr, sizeof(render_addr));
    }
}

/* Send an OSC message with one int argument */
static void osc_send_int(const char *address, int val) {
    unsigned char buf[256];
    int pos = 0;

    int alen = strlen(address);
    strcpy((char *)buf + pos, address);
    pos += alen + 1;
    while (pos % 4 != 0) buf[pos++] = 0;

    buf[pos++] = ',';
    buf[pos++] = 'i';
    buf[pos++] = 0;
    while (pos % 4 != 0) buf[pos++] = 0;

    osc_write_int32(buf + pos, val);
    pos += 4;

    if (render_sockfd >= 0) {
        sendto(render_sockfd, buf, pos, 0,
               (struct sockaddr *)&render_addr, sizeof(render_addr));
    }
}

/* ============================================================ */
/* MIR → OSC bridge                                             */
/* ============================================================ */

/* The MIR array is defined in the firmware's variables.h */
extern unsigned char MIR[2][17][5];

/* Previous MIR for diffing */
static unsigned char prev_MIR[2][17][5];

/* Check if MIR changed since last frame */
static int mir_changed(void) {
    return memcmp(MIR, prev_MIR, sizeof(MIR)) != 0;
}

/* The main OSC output function — called instead of VIEWER_show_MIR() */
void VIEWER_show_MIR(void) {
    extern unsigned char G_master_blinker;

    /* Apply blink to a copy of MIR: when blinker is off, clear color bits
     * where the blink bit is set (matches hardware VIEWER_show_MIR behavior) */
    unsigned char mir_out[170];
    memcpy(mir_out, MIR, sizeof(MIR));

    if (G_master_blinker == 0) {
        int set, row;
        for (set = 0; set < 2; set++) {
            for (row = 0; row < 17; row++) {
                int base = set * 85 + row * 5;
                mir_out[base + 1] &= ~mir_out[base + 0]; /* red */
                mir_out[base + 2] &= ~mir_out[base + 0]; /* green */
            }
        }
    }

    /* Send the processed MIR if changed */
    if (memcmp(mir_out, prev_MIR, sizeof(mir_out)) != 0) {
        osc_send_blob("/mir", mir_out, sizeof(mir_out));
        memcpy(prev_MIR, mir_out, sizeof(mir_out));
    }
}

/* ============================================================ */
/* Render thread — 60 Hz refresh + blinker toggle               */
/* ============================================================ */

extern void Page_full_refresh(void);
extern void Page_requestRefresh(void);
extern unsigned char G_master_blinker;
extern unsigned char G_run_bit;

static volatile int render_running = 0;
static pthread_t render_pthread;

/* Blink phase counter. At 60 Hz, toggling every 4 frames ≈ 7.5 Hz blink */
#define BLINK_FRAMES 10

static void *render_thread_func(void *arg) {
    (void)arg;
    int frame = 0;
    struct timespec ts_60hz = { .tv_sec = 0, .tv_nsec = 16666667 }; /* ~60 Hz */

    fprintf(stderr, "render: thread started (60 Hz)\n");

    while (render_running) {
        /* Toggle blinker every BLINK_FRAMES */
        frame++;
        if (frame % BLINK_FRAMES == 0) {
            G_master_blinker ^= 1;
        }

        /* Refresh the page display (fills MIR via firmware fill/show pipeline,
         * then calls VIEWER_show_MIR() which sends OSC).
         *
         * No scheduler lock — the original firmware's showPage_thread
         * (show__master.h:74-80) deliberately ran Page_full_refresh()
         * WITHOUT the scheduler lock to avoid blocking the sequencer.
         * Re-enabling it here caused MIDI clock jitter because the
         * sequencer thread blocks on the same global mutex. */
        Page_full_refresh();

        /* Send transport/zoom state */
        osc_send_int("/transport", G_run_bit ? 1 : 0);

#ifdef _WIN32
        Sleep(16);  /* ~60 Hz (Sleep granularity is 1ms after timeBeginPeriod) */
#else
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts_60hz, NULL);
#endif
    }

    return NULL;
}

/* ============================================================ */
/* Public API                                                   */
/* ============================================================ */

void osc_render_init(const char *host, int port) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    render_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (render_sockfd < 0) {
        fprintf(stderr, "osc_render: socket() failed: %s\n", strerror(errno));
        return;
    }

#ifdef _WIN32
    /* CRITICAL Windows UDP fix: when sending to a port with no listener,
     * Windows receives an ICMP "port unreachable" and then permanently
     * fails all subsequent sendto() calls on the same socket with
     * WSAECONNRESET (10054). This SIO_UDP_CONNRESET ioctl disables that
     * behavior so sendto() silently drops packets instead. Without this,
     * the render thread stops sending the moment web_gui.py restarts. */
    #ifndef SIO_UDP_CONNRESET
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    #endif
    DWORD dwBytesReturned = 0;
    BOOL bNewBehavior = FALSE;
    WSAIoctl((SOCKET)render_sockfd, SIO_UDP_CONNRESET,
             &bNewBehavior, sizeof(bNewBehavior),
             NULL, 0, &dwBytesReturned, NULL, NULL);
#endif

    memset(&render_addr, 0, sizeof(render_addr));
    render_addr.sin_family = AF_INET;
    render_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host ? host : "127.0.0.1", &render_addr.sin_addr) != 1) {
        fprintf(stderr, "osc_render: invalid host '%s', using 127.0.0.1\n", host);
        inet_pton(AF_INET, "127.0.0.1", &render_addr.sin_addr);
    }

    fprintf(stderr, "osc_render: sending to %s:%d\n",
            host ? host : "127.0.0.1", port);

    /* Initialize prev_MIR to all-FF so first frame is detected as changed */
    memset(prev_MIR, 0xFF, sizeof(prev_MIR));
}

void osc_render_update_target(const struct sockaddr_in *new_addr) {
    if (new_addr && render_sockfd >= 0) {
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &new_addr->sin_addr, ipstr, sizeof(ipstr));
        fprintf(stderr, "osc_render: target updated to %s:%d\n",
                ipstr, ntohs(new_addr->sin_port));
        render_addr = *new_addr;
    }
}

void osc_render_start(void) {
    render_running = 1;
    pthread_create(&render_pthread, NULL, render_thread_func, NULL);
}

void osc_render_stop(void) {
    render_running = 0;
    pthread_join(render_pthread, NULL);
    if (render_sockfd >= 0) {
        RENDER_CLOSE(render_sockfd);
        render_sockfd = -1;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}
