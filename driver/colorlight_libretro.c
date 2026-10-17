/*
 * colorlight_libretro.c — Raspberry Pi 5 → Colorlight receiver card, direct.
 *
 * A minimal libretro frontend that loads an emulator core (mGBA, Gambatte, ...)
 * with dlopen(), takes each frame straight from the core's video callback and
 * streams it to a Colorlight 5A-75B / E80 receiver card as raw Layer-2 Ethernet
 * frames. No HDMI, no sending card (X2S etc.), no X server, no screen capture.
 *
 *   core pixels -> scale/center -> BGR rows -> sendmmsg() -> receiver card -> HUB75
 *
 * Built for the 5ft 9in "Pokémon Yellow edition" giant Game Boy Color:
 * 3x3 grid of 64x64 P3 panels = 192x192 pixels.
 *
 * Build:
 *   gcc -O2 -o colorlight_libretro colorlight_libretro.c -ldl -lasound -lrt -lm -lpthread
 *
 * Run (needs CAP_NET_RAW + CAP_SYS_NICE, or sudo):
 *   ./colorlight_libretro -i eth0 \
 *       -c /usr/lib/aarch64-linux-gnu/libretro/mgba_libretro.so \
 *       -r ~/roms/gbc/game.gbc
 *
 * Copyright (c) 2026 Nick Alvarez — MIT License (see LICENSE).
 * Colorlight packet layout based on community reverse-engineering of the
 * Colorlight 5A-75B/E80 "PC/netcard" protocol (see README for credits).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <alsa/asoundlib.h>

/* ========================================================================
 * Minimal libretro API (subset of libretro.h)
 * ======================================================================== */
#define RETRO_ENVIRONMENT_GET_CAN_DUPE          3
#define RETRO_ENVIRONMENT_SET_MESSAGE           6
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY  9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT     10
#define RETRO_ENVIRONMENT_GET_VARIABLE         15
#define RETRO_ENVIRONMENT_SET_VARIABLES        16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE  17
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE    27
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY   31
#define RETRO_ENVIRONMENT_SET_GEOMETRY         37

#define RETRO_PIXEL_FORMAT_0RGB1555 0
#define RETRO_PIXEL_FORMAT_XRGB8888 1
#define RETRO_PIXEL_FORMAT_RGB565   2

#define RETRO_MEMORY_SAVE_RAM 0

#define RETRO_DEVICE_JOYPAD 1
enum {
    JP_B = 0, JP_Y, JP_SELECT, JP_START, JP_UP, JP_DOWN, JP_LEFT, JP_RIGHT,
    JP_A, JP_X, JP_L, JP_R, JP_COUNT = 16
};

typedef bool    (*retro_environment_t)(unsigned cmd, void *data);
typedef void    (*retro_video_refresh_t)(const void *data, unsigned w, unsigned h, size_t pitch);
typedef void    (*retro_audio_sample_t)(int16_t l, int16_t r);
typedef size_t  (*retro_audio_sample_batch_t)(const int16_t *data, size_t frames);
typedef void    (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);

struct retro_system_info {
    const char *library_name, *library_version, *valid_extensions;
    bool need_fullpath, block_extract;
};
struct retro_system_av_info {
    struct { unsigned base_width, base_height, max_width, max_height; float aspect_ratio; } geometry;
    struct { double fps, sample_rate; } timing;
};
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_variable  { const char *key; const char *value; };

static struct {
    void     (*init)(void);
    void     (*deinit)(void);
    void     (*get_system_info)(struct retro_system_info *);
    void     (*get_system_av_info)(struct retro_system_av_info *);
    void     (*set_environment)(retro_environment_t);
    void     (*set_video_refresh)(retro_video_refresh_t);
    void     (*set_audio_sample)(retro_audio_sample_t);
    void     (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void     (*set_input_poll)(retro_input_poll_t);
    void     (*set_input_state)(retro_input_state_t);
    void     (*set_controller_port_device)(unsigned, unsigned);
    bool     (*load_game)(const struct retro_game_info *);
    void     (*unload_game)(void);
    void     (*run)(void);
    void    *(*get_memory_data)(unsigned);
    size_t   (*get_memory_size)(unsigned);
} core;

/* ========================================================================
 * Colorlight protocol
 * The receiver card accepts raw Ethernet frames with fixed MACs. The
 * "EtherType" bytes are reused: byte 12 = packet type, byte 13 = data[0].
 * ======================================================================== */
static const uint8_t DST_MAC[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const uint8_t SRC_MAC[6] = {0x22, 0x22, 0x33, 0x44, 0x55, 0x66};

#define CL_SYNC_TYPE    0x01   /* "display frame" / latch            */
#define CL_BRIGHT_TYPE  0x0A   /* brightness                          */
#define CL_PIXEL_TYPE   0x55   /* one row (or part of a row) of BGR   */
#define ETH_HDR         14
#define ROW_HDR         7      /* row hi/lo, offset hi/lo, count hi/lo, 0x08 */
#define SYNC_LEN        (ETH_HDR + 98)
#define BRIGHT_LEN      (ETH_HDR + 63)
#define MAX_ROWS        512

/* ========================================================================
 * Configuration (command line)
 * ======================================================================== */
static char   g_ifname[IFNAMSIZ] = "eth0";
static int    g_out_w       = 192;
static int    g_out_h       = 192;
static int    g_brightness  = 255;     /* receiver card brightness 0-255   */
static float  g_dim         = 1.0f;    /* software dim, 0.0-1.0            */
static int    g_offset_y    = 0;       /* fine vertical nudge in rows       */
static int    g_offset_x    = 0;
static int    g_integer_scale = 0;     /* 1 = 1:1 pixels, centered          */
static int    g_batch_rows  = 32;      /* rows per sendmmsg (1/32 scan)     */
static int    g_sync_delay_us = 500;   /* wait before sync: fixes last-row flicker */
static int    g_cpu         = 3;       /* pin to core (-1 = off)            */
static int    g_rt          = 1;
static int    g_audio       = 1;
static int    g_pot_volume  = 0;       /* --pot-volume: scale audio by Pico pot (js axis 5) */
static char   g_joy_hint[128] = "";
static char   g_system_dir[512] = "";
static char   g_save_dir[512]   = "";

/* Core options passed with --opt key=value (e.g. Gambatte settings) */
#define MAX_OPTS 32
static char  *g_opt_keys[MAX_OPTS], *g_opt_vals[MAX_OPTS];
static int    g_opt_count = 0;
static int    g_log_vars  = 0;          /* --log-vars: print every option the core asks for */

/* ========================================================================
 * Runtime state
 * ======================================================================== */
static volatile sig_atomic_t g_running = 1;
static int      g_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static double   g_fps = 59.7275, g_sample_rate = 32768.0;
static void    *g_core_handle;

static int      g_sock = -1;
static uint8_t *g_rows[MAX_ROWS];
static int      g_row_len;
static uint8_t  g_sync[SYNC_LEN];
static struct mmsghdr g_msgs[MAX_ROWS];
static struct iovec   g_iov[MAX_ROWS];
static uint8_t *g_fb;                    /* BGR framebuffer, out_w*out_h*3 */

static int      g_joy_fd = -1, g_pot_fd = -1;
static int16_t  g_pad[JP_COUNT];
static int      g_axis_lo[2] = {64, 64}, g_axis_hi[2] = {192, 192};   /* X, Y */
static float    g_volume = 1.0f;

static snd_pcm_t *g_pcm;
static char     g_srm_path[1024];
static uint64_t g_frames;

static void on_signal(int s) { (void)s; g_running = 0; }

/* ========================================================================
 * Network
 * ======================================================================== */
static int open_raw_socket(const char *ifname) {
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { perror("socket (need CAP_NET_RAW or sudo)"); return -1; }

    struct ifreq ifr = {0};
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); close(fd); return -1; }

    struct sockaddr_ll addr = {0};
    addr.sll_family  = AF_PACKET;
    addr.sll_ifindex = ifr.ifr_ifindex;
    addr.sll_halen   = ETH_ALEN;
    memcpy(addr.sll_addr, DST_MAC, 6);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); close(fd); return -1; }

    int sndbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    return fd;
}

static void eth_header(uint8_t *p, uint8_t type) {
    memcpy(p, DST_MAC, 6);
    memcpy(p + 6, SRC_MAC, 6);
    p[12] = type;
}

static void init_packets(void) {
    /* Sync / display-frame packet. data[N] lives at byte 13+N. */
    memset(g_sync, 0, sizeof(g_sync));
    eth_header(g_sync, CL_SYNC_TYPE);
    g_sync[13] = 0x07;                 /* data[0]                     */
    g_sync[35] = (uint8_t)g_brightness;/* data[22] brightness         */
    g_sync[36] = 0x05;                 /* data[23]                    */
    g_sync[38] = (uint8_t)g_brightness;/* data[25..27] R/G/B          */
    g_sync[39] = (uint8_t)g_brightness;
    g_sync[40] = (uint8_t)g_brightness;

    /* One pre-built packet per row; only the pixel payload changes. */
    g_row_len = ETH_HDR + ROW_HDR + g_out_w * 3;
    for (int r = 0; r < g_out_h; r++) {
        uint8_t *p = calloc(1, g_row_len);
        eth_header(p, CL_PIXEL_TYPE);
        p[13] = (r >> 8) & 0xFF;          /* row number               */
        p[14] = r & 0xFF;
        p[15] = 0; p[16] = 0;             /* pixel offset in row      */
        p[17] = (g_out_w >> 8) & 0xFF;    /* pixel count              */
        p[18] = g_out_w & 0xFF;
        p[19] = 0x08;
        p[20] = 0x88;
        g_rows[r] = p;

        g_iov[r].iov_base = p;
        g_iov[r].iov_len  = g_row_len;
        g_msgs[r].msg_hdr.msg_iov    = &g_iov[r];
        g_msgs[r].msg_hdr.msg_iovlen = 1;
    }
}

static void send_brightness(void) {
    uint8_t p[BRIGHT_LEN] = {0};
    eth_header(p, CL_BRIGHT_TYPE);
    p[13] = p[14] = p[15] = (uint8_t)g_brightness;
    p[16] = 0xFF;
    send(g_sock, p, sizeof(p), 0);
    send(g_sock, p, sizeof(p), 0);     /* v13 firmware likes it twice */
}

static void send_batch(struct mmsghdr *m, int n) {
    int sent = 0;
    while (sent < n) {
        int r = sendmmsg(g_sock, m + sent, n - sent, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == ENOBUFS || errno == EINTR) { usleep(20); continue; }
            perror("sendmmsg");
            return;
        }
        sent += r;
    }
}

static void send_frame(void) {
    const int row_bytes = g_out_w * 3;
    for (int r = 0; r < g_out_h; r++)
        memcpy(g_rows[r] + ETH_HDR + ROW_HDR, g_fb + r * row_bytes, row_bytes);

    for (int r = 0; r < g_out_h; r += g_batch_rows) {
        int n = g_out_h - r < g_batch_rows ? g_out_h - r : g_batch_rows;
        send_batch(&g_msgs[r], n);
    }

    /* Give the card time to finish the last rows before latching,
     * otherwise the bottom row of panels flickers. */
    if (g_sync_delay_us > 0) usleep(g_sync_delay_us);
    send(g_sock, g_sync, sizeof(g_sync), 0);
    send(g_sock, g_sync, sizeof(g_sync), 0);
}

/* ========================================================================
 * Video: scale (aspect-preserving) and center into the BGR framebuffer
 * ======================================================================== */
static inline void put_bgr(uint8_t *d, uint8_t r, uint8_t g, uint8_t b) {
    if (g_dim < 1.0f) { r = (uint8_t)(r * g_dim); g = (uint8_t)(g * g_dim); b = (uint8_t)(b * g_dim); }
    d[0] = b; d[1] = g; d[2] = r;
}

static void cb_video(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;                      /* dupe frame: keep last image */

    int sw, sh;
    if (g_integer_scale) {
        sw = (int)w; sh = (int)h;
    } else {
        float sx = (float)g_out_w / w, sy = (float)g_out_h / h;
        float s = sx < sy ? sx : sy;        /* 160x144 -> 192x172 on 192x192 */
        sw = (int)(w * s); sh = (int)(h * s);
    }
    int ox = (g_out_w - sw) / 2 + g_offset_x;
    int oy = (g_out_h - sh) / 2 + g_offset_y;

    memset(g_fb, 0, (size_t)g_out_w * g_out_h * 3);
    const uint8_t *src = data;

    for (int dy = 0; dy < sh; dy++) {
        int y = dy + oy;
        if (y < 0 || y >= g_out_h) continue;
        int syi = dy * (int)h / sh;
        uint8_t *drow = g_fb + (size_t)y * g_out_w * 3;
        for (int dx = 0; dx < sw; dx++) {
            int x = dx + ox;
            if (x < 0 || x >= g_out_w) continue;
            int sxi = dx * (int)w / sw;
            uint8_t r, g, b;
            if (g_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p = ((const uint32_t *)(src + syi * pitch))[sxi];
                r = (p >> 16) & 0xFF; g = (p >> 8) & 0xFF; b = p & 0xFF;
            } else if (g_pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t p = ((const uint16_t *)(src + syi * pitch))[sxi];
                r = ((p >> 11) & 0x1F) << 3; g = ((p >> 5) & 0x3F) << 2; b = (p & 0x1F) << 3;
            } else { /* 0RGB1555 */
                uint16_t p = ((const uint16_t *)(src + syi * pitch))[sxi];
                r = ((p >> 10) & 0x1F) << 3; g = ((p >> 5) & 0x1F) << 3; b = (p & 0x1F) << 3;
            }
            put_bgr(drow + x * 3, r, g, b);
        }
    }

    send_frame();
    g_frames++;
}

/* ========================================================================
 * Audio (ALSA, non-blocking, ~100 ms buffer, software volume)
 * ======================================================================== */
static int init_alsa(double rate_hz) {
    unsigned rate = (unsigned)rate_hz;
    int err = snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) { fprintf(stderr, "  Audio disabled: %s\n", snd_strerror(err)); g_pcm = NULL; return -1; }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(g_pcm, hw);
    snd_pcm_hw_params_set_access(g_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(g_pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(g_pcm, hw, 2);
    snd_pcm_hw_params_set_rate_near(g_pcm, hw, &rate, 0);
    snd_pcm_uframes_t buf = rate / 10, per = rate / 60;
    snd_pcm_hw_params_set_buffer_size_near(g_pcm, hw, &buf);
    snd_pcm_hw_params_set_period_size_near(g_pcm, hw, &per, 0);
    if ((err = snd_pcm_hw_params(g_pcm, hw)) < 0) {
        fprintf(stderr, "  Audio disabled: %s\n", snd_strerror(err));
        snd_pcm_close(g_pcm); g_pcm = NULL; return -1;
    }
    fprintf(stderr, "  Audio: ALSA %u Hz, buffer %lu frames\n", rate, (unsigned long)buf);
    return 0;
}

static size_t cb_audio_batch(const int16_t *data, size_t frames) {
    if (!g_pcm || !frames) return frames;
    static int16_t tmp[8192];
    const int16_t *out = data;
    if (g_volume < 0.999f && frames * 2 <= sizeof(tmp) / sizeof(tmp[0])) {
        for (size_t i = 0; i < frames * 2; i++) tmp[i] = (int16_t)(data[i] * g_volume);
        out = tmp;
    }
    snd_pcm_sframes_t n = snd_pcm_writei(g_pcm, out, frames);
    if (n < 0 && n != -EAGAIN) snd_pcm_recover(g_pcm, (int)n, 1);
    return frames;                          /* never block the emulator */
}

static void cb_audio_sample(int16_t l, int16_t r) {
    int16_t s[2] = {l, r};
    cb_audio_batch(s, 1);
}

/* ========================================================================
 * Input (evdev)
 *
 * The giant Game Boy's controller is a Raspberry Pi Pico running the
 * firmware in /controller. Its 11 buttons arrive as BTN_SOUTH (bit 0)
 * through BTN_SELECT (bit 10). Bit 0 is the bistable power switch, which
 * is handled by power_switch.py, so it is ignored here.
 * ======================================================================== */
static int pico_button_to_retro(uint16_t code) {
    switch (code) {
        case BTN_SOUTH:  return -1;        /* bit0  GP0  power switch  */
        case BTN_EAST:   return JP_LEFT;   /* bit1  GP1                 */
        case BTN_C:      return JP_SELECT; /* bit2  GP2                 */
        case BTN_NORTH:  return JP_DOWN;   /* bit3  GP3                 */
        case BTN_WEST:   return JP_RIGHT;  /* bit4  GP4                 */
        case BTN_Z:      return JP_START;  /* bit5  GP5                 */
        case BTN_TL:     return JP_B;      /* bit6  GP6                 */
        case BTN_TR:     return -1;        /* bit7  GP7  menu (spare)   */
        case BTN_TL2:    return JP_A;      /* bit8  GP8                 */
        case BTN_TR2:    return JP_UP;     /* bit9  GP9                 */
        case BTN_SELECT: return JP_R;      /* bit10 GP10 right bumper   */
        default:         return -1;
    }
}

static int has_bit(const unsigned long *bits, int bit) {
    return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

static void open_gamepad(void) {
    char path[128] = "";
    if (g_joy_hint[0]) {
        strncpy(path, g_joy_hint, sizeof(path) - 1);
    } else {
        /* Auto-detect: first event device that reports gamepad buttons */
        for (int i = 0; i < 32 && !path[0]; i++) {
            char p[64];
            snprintf(p, sizeof(p), "/dev/input/event%d", i);
            int fd = open(p, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
            if (has_bit(keys, BTN_SOUTH) || has_bit(keys, BTN_TRIGGER))
                strncpy(path, p, sizeof(path) - 1);
            close(fd);
        }
    }
    if (!path[0]) { fprintf(stderr, "  Gamepad: none found\n"); return; }
    g_joy_fd = open(path, O_RDONLY | O_NONBLOCK);
    if (g_joy_fd < 0) { fprintf(stderr, "  Gamepad: cannot open %s\n", path); return; }
    char name[128] = "?";
    ioctl(g_joy_fd, EVIOCGNAME(sizeof(name)), name);
    fprintf(stderr, "  Gamepad: %s (%s)\n", name, path);

    /* D-pad axis thresholds from the device's own range: 0..255 on generic
     * pads like the iBuffalo, -127..127 on the Pico. Press = outer quarter. */
    for (int a = 0; a < 2; a++) {
        struct input_absinfo ai;
        if (ioctl(g_joy_fd, EVIOCGABS(a == 0 ? ABS_X : ABS_Y), &ai) == 0 && ai.maximum > ai.minimum) {
            int q = (ai.maximum - ai.minimum) / 4;
            g_axis_lo[a] = ai.minimum + q;
            g_axis_hi[a] = ai.maximum - q;
        }
    }
}

static void poll_gamepad(void) {
    if (g_joy_fd < 0) return;
    struct input_event ev;
    while (read(g_joy_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_KEY) {
            int id = pico_button_to_retro(ev.code);
            if (id >= 0) g_pad[id] = ev.value ? 1 : 0;
        } else if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) { g_pad[JP_LEFT] = ev.value < g_axis_lo[0]; g_pad[JP_RIGHT] = ev.value > g_axis_hi[0]; }
            if (ev.code == ABS_Y) { g_pad[JP_UP]   = ev.value < g_axis_lo[1]; g_pad[JP_DOWN]  = ev.value > g_axis_hi[1]; }
            if (ev.code == ABS_HAT0X) { g_pad[JP_LEFT] = ev.value < 0; g_pad[JP_RIGHT] = ev.value > 0; }
            if (ev.code == ABS_HAT0Y) { g_pad[JP_UP]   = ev.value < 0; g_pad[JP_DOWN]  = ev.value > 0; }
        }
    }
}

/* Volume knob: Pico reports the pot on the Rz axis; the joystick API
 * exposes it as axis 5 scaled to -32767..32767. */
static void poll_volume_pot(void) {
    if (g_pot_fd < 0) return;
    struct js_event ev;
    while (read(g_pot_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if ((ev.type & ~JS_EVENT_INIT) == JS_EVENT_AXIS && ev.number == 5)
            g_volume = (float)(ev.value + 32767) / 65534.0f;
    }
}

static void cb_input_poll(void) { poll_gamepad(); poll_volume_pot(); }

static int16_t cb_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port || device != RETRO_DEVICE_JOYPAD || id >= JP_COUNT) return 0;
    return g_pad[id];
}

/* ========================================================================
 * Environment
 * ======================================================================== */
static bool cb_environment(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            g_pixel_format = *(const unsigned *)data;
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char **)data = g_system_dir;      /* gbc_bios.bin etc. */
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **)data = g_save_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool *)data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *v = data;
            v->value = NULL;
            for (int i = g_opt_count - 1; i >= 0; i--)     /* later --opt wins */
                if (v->key && !strcmp(v->key, g_opt_keys[i])) { v->value = g_opt_vals[i]; break; }
            if (g_log_vars)
                fprintf(stderr, "  [core asks] %s -> %s\n", v->key ? v->key : "?", v->value ? v->value : "(default)");
            return v->value != NULL;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool *)data = false;
            return true;
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
        case RETRO_ENVIRONMENT_SET_MESSAGE:
            return true;
        default:
            return false;
    }
}

/* ========================================================================
 * Battery saves (.srm next to the ROM)
 * ======================================================================== */
static void make_srm_path(const char *rom) {
    snprintf(g_srm_path, sizeof(g_srm_path), "%s", rom);
    char *dot = strrchr(g_srm_path, '.'), *slash = strrchr(g_srm_path, '/');
    if (dot && (!slash || dot > slash)) *dot = 0;
    strncat(g_srm_path, ".srm", sizeof(g_srm_path) - strlen(g_srm_path) - 1);
}

static void load_sram(void) {
    if (!core.get_memory_data) return;
    uint8_t *mem = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t   sz  = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !sz) return;
    FILE *f = fopen(g_srm_path, "rb");
    if (!f) return;
    size_t n = fread(mem, 1, sz, f);
    fclose(f);
    fprintf(stderr, "  Save: loaded %zu bytes from %s\n", n, g_srm_path);
}

static void save_sram(void) {
    if (!core.get_memory_data) return;
    uint8_t *mem = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t   sz  = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !sz) return;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_srm_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(mem, 1, sz, f);
    fflush(f); fsync(fileno(f)); fclose(f);
    rename(tmp, g_srm_path);               /* atomic: survives power-off */
}

/* ========================================================================
 * Core loading
 * ======================================================================== */
#define SYM(field, name, required) do { \
    *(void **)&core.field = dlsym(g_core_handle, name); \
    if (!core.field && required) { fprintf(stderr, "Core is missing %s\n", name); return -1; } \
} while (0)

static int load_core(const char *path) {
    g_core_handle = dlopen(path, RTLD_LAZY);
    if (!g_core_handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return -1; }
    SYM(init, "retro_init", 1);
    SYM(deinit, "retro_deinit", 1);
    SYM(get_system_info, "retro_get_system_info", 1);
    SYM(get_system_av_info, "retro_get_system_av_info", 1);
    SYM(set_environment, "retro_set_environment", 1);
    SYM(set_video_refresh, "retro_set_video_refresh", 1);
    SYM(set_audio_sample, "retro_set_audio_sample", 1);
    SYM(set_audio_sample_batch, "retro_set_audio_sample_batch", 1);
    SYM(set_input_poll, "retro_set_input_poll", 1);
    SYM(set_input_state, "retro_set_input_state", 1);
    SYM(set_controller_port_device, "retro_set_controller_port_device", 0);
    SYM(load_game, "retro_load_game", 1);
    SYM(unload_game, "retro_unload_game", 1);
    SYM(run, "retro_run", 1);
    SYM(get_memory_data, "retro_get_memory_data", 0);
    SYM(get_memory_size, "retro_get_memory_size", 0);
    return 0;
}

static void *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = malloc(n > 0 ? n : 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

/* ========================================================================
 * Timing helpers
 * ======================================================================== */
static void ts_add_ns(struct timespec *t, long ns) {
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Load core options from a RetroArch-style .opt file:  key = "value"  */
static void load_opts_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "  Warning: cannot open options file %s\n", path); return; }
    char line[512];
    while (fgets(line, sizeof(line), f) && g_opt_count < MAX_OPTS) {
        char key[256], val[256];
        if (line[0] == '#' || line[0] == ';') continue;
        if (sscanf(line, " %255[^= \t] = \"%255[^\"]\"", key, val) == 2 ||
            sscanf(line, " %255[^= \t] = %255s", key, val) == 2) {
            g_opt_keys[g_opt_count] = strdup(key);
            g_opt_vals[g_opt_count++] = strdup(val);
        }
    }
    fclose(f);
    fprintf(stderr, "  Options: %d loaded from %s\n", g_opt_count, path);
}

/* ========================================================================
 * main
 * ======================================================================== */
static void usage(const char *p) {
    fprintf(stderr,
        "Raspberry Pi 5 -> Colorlight receiver card libretro frontend\n\n"
        "Usage: %s -c CORE.so -r ROM [options]\n\n"
        "  -i, --interface NAME   Ethernet interface wired to the card (eth0)\n"
        "  -c, --core PATH        libretro core (.so)\n"
        "  -r, --rom PATH         ROM file\n"
        "  -W, --width N          output width in pixels (192)\n"
        "  -H, --height N         output height in pixels (192)\n"
        "  -b, --brightness N     card brightness 0-255 (255)\n"
        "  -d, --dim F            software dim 0.0-1.0 (1.0)\n"
        "  -y, --offset-y N       nudge image vertically (rows)\n"
        "  -x, --offset-x N       nudge image horizontally (pixels)\n"
        "      --integer          1:1 pixels instead of aspect-fit scaling\n"
        "  -j, --joystick PATH    evdev gamepad (default: auto-detect)\n"
        "      --batch N          rows per sendmmsg call (32)\n"
        "      --sync-delay US    pause before sync packet (500)\n"
        "      --cpu N            pin to CPU core, -1 to disable (3)\n"
        "      --system-dir PATH  BIOS directory (default: ROM folder)\n"
        "      --opt KEY=VALUE    core option, repeatable\n"
        "      --opts-file PATH   load core options from a .opt file\n"
        "      --log-vars         print every core option the core asks for\n"
        "      --pot-volume       scale game audio by the Pico pot (instead of volume_daemon.py)\n"
        "      --no-audio  --no-rt\n", p);
}

int main(int argc, char **argv) {
    char core_path[512] = "", rom_path[512] = "";

    static const struct option opts[] = {
        {"interface", 1, 0, 'i'}, {"core", 1, 0, 'c'}, {"rom", 1, 0, 'r'},
        {"width", 1, 0, 'W'}, {"height", 1, 0, 'H'}, {"brightness", 1, 0, 'b'},
        {"dim", 1, 0, 'd'}, {"offset-y", 1, 0, 'y'}, {"offset-x", 1, 0, 'x'},
        {"joystick", 1, 0, 'j'}, {"integer", 0, 0, 1}, {"batch", 1, 0, 2},
        {"sync-delay", 1, 0, 3}, {"cpu", 1, 0, 4}, {"system-dir", 1, 0, 5},
        {"opt", 1, 0, 6}, {"no-audio", 0, 0, 7}, {"pot-volume", 0, 0, 8},
        {"no-rt", 0, 0, 9}, {"opts-file", 1, 0, 10}, {"log-vars", 0, 0, 11},
        {"help", 0, 0, 'h'}, {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:c:r:W:H:b:d:y:x:j:h", opts, NULL)) != -1) {
        switch (c) {
            case 'i': strncpy(g_ifname, optarg, sizeof(g_ifname) - 1); break;
            case 'c': strncpy(core_path, optarg, sizeof(core_path) - 1); break;
            case 'r': strncpy(rom_path, optarg, sizeof(rom_path) - 1); break;
            case 'W': g_out_w = atoi(optarg); break;
            case 'H': g_out_h = atoi(optarg); break;
            case 'b': g_brightness = atoi(optarg); break;
            case 'd': g_dim = (float)atof(optarg); break;
            case 'y': g_offset_y = atoi(optarg); break;
            case 'x': g_offset_x = atoi(optarg); break;
            case 'j': strncpy(g_joy_hint, optarg, sizeof(g_joy_hint) - 1); break;
            case 1: g_integer_scale = 1; break;
            case 2: g_batch_rows = atoi(optarg); break;
            case 3: g_sync_delay_us = atoi(optarg); break;
            case 4: g_cpu = atoi(optarg); break;
            case 5: strncpy(g_system_dir, optarg, sizeof(g_system_dir) - 1); break;
            case 6: {
                char *eq = strchr(optarg, '=');
                if (eq && g_opt_count < MAX_OPTS) {
                    *eq = 0;
                    g_opt_keys[g_opt_count] = optarg;
                    g_opt_vals[g_opt_count++] = eq + 1;
                }
                break;
            }
            case 7: g_audio = 0; break;
            case 8: g_pot_volume = 1; break;
            case 9: g_rt = 0; break;
            case 10: load_opts_file(optarg); break;
            case 11: g_log_vars = 1; break;
            default: usage(argv[0]); return c == 'h' ? 0 : 1;
        }
    }
    if (!core_path[0] || !rom_path[0]) { usage(argv[0]); return 1; }
    if (g_out_h > MAX_ROWS) { fprintf(stderr, "height > %d not supported\n", MAX_ROWS); return 1; }
    if (g_batch_rows < 1) g_batch_rows = 1;
    if (g_dim < 0) g_dim = 0;
    if (g_dim > 1) g_dim = 1;

    /* Default BIOS/save dirs: the ROM's folder */
    char rom_dir[512];
    snprintf(rom_dir, sizeof(rom_dir), "%s", rom_path);
    char *sl = strrchr(rom_dir, '/');
    if (sl) *sl = 0; else strcpy(rom_dir, ".");
    if (!g_system_dir[0]) snprintf(g_system_dir, sizeof(g_system_dir), "%s", rom_dir);
    snprintf(g_save_dir, sizeof(g_save_dir), "%s", rom_dir);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* Real-time setup: FIFO priority, locked memory, dedicated core */
    if (g_rt) {
        struct sched_param sp = { .sched_priority = 50 };
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
            fprintf(stderr, "  Warning: SCHED_FIFO failed (%s)\n", strerror(errno));
        mlockall(MCL_CURRENT | MCL_FUTURE);
    }
    if (g_cpu >= 0) {
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(g_cpu, &set);
        sched_setaffinity(0, sizeof(set), &set);
    }

    if ((g_sock = open_raw_socket(g_ifname)) < 0) return 1;
    g_fb = calloc(1, (size_t)g_out_w * g_out_h * 3);
    init_packets();
    send_brightness();

    fprintf(stderr, "Colorlight libretro frontend\n  Output: %dx%d on %s\n", g_out_w, g_out_h, g_ifname);

    if (load_core(core_path)) return 1;
    core.set_environment(cb_environment);
    core.set_video_refresh(cb_video);
    core.set_audio_sample(cb_audio_sample);
    core.set_audio_sample_batch(cb_audio_batch);
    core.set_input_poll(cb_input_poll);
    core.set_input_state(cb_input_state);
    core.init();

    struct retro_system_info si = {0};
    core.get_system_info(&si);
    fprintf(stderr, "  Core: %s %s\n", si.library_name, si.library_version);

    struct retro_game_info game = { .path = rom_path };
    void *rom_data = NULL;
    if (!si.need_fullpath) {
        rom_data = read_file(rom_path, &game.size);
        if (!rom_data) { perror(rom_path); return 1; }
        game.data = rom_data;
    }
    if (!core.load_game(&game)) { fprintf(stderr, "Core failed to load %s\n", rom_path); return 1; }
    fprintf(stderr, "  ROM: %s\n", rom_path);

    struct retro_system_av_info av = {0};
    core.get_system_av_info(&av);
    if (av.timing.fps > 1) g_fps = av.timing.fps;
    if (av.timing.sample_rate > 1) g_sample_rate = av.timing.sample_rate;

    make_srm_path(rom_path);
    load_sram();

    if (g_audio) init_alsa(g_sample_rate);
    open_gamepad();
    if (g_pot_volume) g_pot_fd = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
    if (core.set_controller_port_device) core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    /* Main loop: absolute-deadline pacing at the core's native rate */
    const long frame_ns = (long)(1e9 / g_fps);
    const uint64_t autosave_every = (uint64_t)(g_fps * 5);   /* every ~5 s */
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    double t0 = now_s();

    while (g_running) {
        core.run();
        if (g_frames && g_frames % autosave_every == 0) save_sram();

        ts_add_ns(&next, frame_ns);
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec + 1) next = now;          /* resync after a stall */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    double dt = now_s() - t0;
    fprintf(stderr, "\n  %llu frames in %.1fs (%.2f fps)\n",
            (unsigned long long)g_frames, dt, dt > 0 ? g_frames / dt : 0);

    save_sram();
    memset(g_fb, 0, (size_t)g_out_w * g_out_h * 3);         /* blank the panels */
    send_frame();

    core.unload_game();
    core.deinit();
    dlclose(g_core_handle);
    if (g_pcm) snd_pcm_close(g_pcm);
    if (g_joy_fd >= 0) close(g_joy_fd);
    if (g_pot_fd >= 0) close(g_pot_fd);
    close(g_sock);
    for (int r = 0; r < g_out_h; r++) free(g_rows[r]);
    free(g_fb);
    free(rom_data);
    return 0;
}
