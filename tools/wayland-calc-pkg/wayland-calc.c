/*
 * wayland-calc.c — Brook-native Wayland "calculator" client.
 *
 * Renders a static calculator-face UI (display + 4x5 button grid) into
 * a 240x320 wl_shm buffer, attaches it to an xdg_toplevel, and stays
 * mapped for a few seconds so it's visible on the framebuffer.
 *
 * No font/cairo/pango dependencies — buttons are coloured rects, digits
 * are simple 7-segment style polylines hand-drawn from filled rectangles.
 *
 * Pure C, only depends on libwayland-client + xdg-shell-protocol. This
 * is the "any Wayland app rendering visually with low deps" target.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "xdg-shell-client-protocol.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001
#endif

static int memfd_create_shim(const char *name, unsigned int flags) {
    return (int)syscall(319 /* SYS_memfd_create */, name, flags);
}

/* ------------------------- Wayland plumbing ------------------------- */

static struct wl_shm        *g_shm  = NULL;
static struct wl_compositor *g_comp = NULL;
static struct xdg_wm_base   *g_wm   = NULL;
static int g_got_configure = 0;
static uint32_t g_configure_serial = 0;

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version) {
    (void)data;
    if (!strcmp(iface, "wl_shm"))
        g_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "wl_compositor"))
        g_comp = wl_registry_bind(reg, name, &wl_compositor_interface,
                                   version < 4 ? version : 4);
    else if (!strcmp(iface, "xdg_wm_base"))
        g_wm = wl_registry_bind(reg, name, &xdg_wm_base_interface,
                                 version < 3 ? version : 3);
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) {
    (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_lis = {
    .global = on_global, .global_remove = on_global_remove,
};

static void on_wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    (void)data; xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_lis = { .ping = on_wm_ping };

static void on_xdg_surface_configure(void *data, struct xdg_surface *xs,
                                        uint32_t serial) {
    (void)data;
    g_got_configure = 1;
    g_configure_serial = serial;
    xdg_surface_ack_configure(xs, serial);
    fprintf(stderr, "[calc] configure serial=%u, acked\n", serial);
}
static const struct xdg_surface_listener xs_lis = {
    .configure = on_xdg_surface_configure,
};

static void on_toplevel_configure(void *d, struct xdg_toplevel *t,
                                     int32_t w, int32_t h,
                                     struct wl_array *states) {
    (void)d; (void)t; (void)w; (void)h; (void)states;
}
static void on_toplevel_close(void *d, struct xdg_toplevel *t) {
    (void)d; (void)t;
}
static void on_toplevel_configure_bounds(void *d, struct xdg_toplevel *t,
                                            int32_t w, int32_t h) {
    (void)d; (void)t; (void)w; (void)h;
}
static void on_toplevel_wm_capabilities(void *d, struct xdg_toplevel *t,
                                           struct wl_array *caps) {
    (void)d; (void)t; (void)caps;
}
static const struct xdg_toplevel_listener tl_lis = {
    .configure         = on_toplevel_configure,
    .close             = on_toplevel_close,
    .configure_bounds  = on_toplevel_configure_bounds,
    .wm_capabilities   = on_toplevel_wm_capabilities,
};

/* ----------------------- 2D drawing helpers ------------------------- */

#define W 240
#define H 320
#define BPP 4
#define STRIDE (W * BPP)

static uint32_t *g_px;

static inline void put(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    g_px[y * W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) put(x + xx, y + yy, c);
}

static void rounded_rect(int x, int y, int w, int h, int r, uint32_t c) {
    /* Cheap rounded corners: just chop the four 1-px corner pixels. */
    fill_rect(x + r, y, w - 2 * r, h, c);
    fill_rect(x, y + r, w, h - 2 * r, c);
    /* Approximate corner with quarter-disc. */
    for (int yy = 0; yy < r; yy++)
        for (int xx = 0; xx < r; xx++) {
            int dx = r - xx, dy = r - yy;
            if (dx * dx + dy * dy <= r * r) {
                put(x + xx,             y + yy,             c);
                put(x + w - 1 - xx,     y + yy,             c);
                put(x + xx,             y + h - 1 - yy,     c);
                put(x + w - 1 - xx,     y + h - 1 - yy,     c);
            }
        }
}

/* 7-segment digit:
 *
 *   aaaa
 *  f    b
 *  f    b
 *   gggg
 *  e    c
 *  e    c
 *   dddd
 */
static const uint8_t kSegMask[10] = {
    /*0*/ 0b1110111,
    /*1*/ 0b0100100,
    /*2*/ 0b1011101,
    /*3*/ 0b1101101,
    /*4*/ 0b0101110,
    /*5*/ 0b1101011,
    /*6*/ 0b1111011,
    /*7*/ 0b0100101,
    /*8*/ 0b1111111,
    /*9*/ 0b1101111,
};

/* Draw one 7-seg digit, top-left at (x,y), digit cell w*h. */
static void draw_digit(int x, int y, int w, int h, int digit, uint32_t on) {
    if (digit < 0 || digit > 9) return;
    uint8_t m = kSegMask[digit];
    int t = h / 14; if (t < 1) t = 1;        /* segment thickness */
    int sw = w - 2 * t;                      /* horizontal seg length */
    int sh = (h - 3 * t) / 2;                /* vertical   seg length */
    /* Segment positions (a=top mid, b=top right, c=bot right, d=bot mid,
       e=bot left, f=top left, g=mid). */
    if (m & (1 << 0)) fill_rect(x + t,         y,                 sw, t, on); /* a */
    if (m & (1 << 1)) fill_rect(x + t + sw,    y + t,             t, sh, on);/* b */
    if (m & (1 << 2)) fill_rect(x + t + sw,    y + 2*t + sh,      t, sh, on);/* c */
    if (m & (1 << 3)) fill_rect(x + t,         y + 2*t + 2*sh,    sw, t, on);/* d */
    if (m & (1 << 4)) fill_rect(x,             y + 2*t + sh,      t, sh, on);/* e */
    if (m & (1 << 5)) fill_rect(x,             y + t,             t, sh, on);/* f */
    if (m & (1 << 6)) fill_rect(x + t,         y + t + sh,        sw, t, on);/* g */
}

static void draw_digit_string(int x, int y, int dw, int dh, int gap,
                                 const char *s, uint32_t on) {
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            draw_digit(x, y, dw, dh, *s - '0', on);
            x += dw + gap;
        } else if (*s == '.') {
            fill_rect(x, y + dh - 4, 4, 4, on);
            x += 4 + gap;
        } else {
            x += dw + gap;
        }
    }
}

/* ----------------------- The actual calculator ---------------------- */

static const char *kButtons[5][4] = {
    { "C",  "+/-", "%",  "/" },
    { "7",  "8",   "9",  "*" },
    { "4",  "5",   "6",  "-" },
    { "1",  "2",   "3",  "+" },
    { "0",  ".",   "DEL","=" },
};

/* Palette (XRGB8888). */
#define COL_BG       0x00202028u   /* near-black */
#define COL_FRAME    0x00303040u
#define COL_DISPLAY  0x000c1418u   /* very dark green-blue */
#define COL_DIGIT    0x0078ffaau   /* LCD-style green */
#define COL_DIGIT_DIM 0x00102818u
#define COL_BTN_NUM  0x00404048u
#define COL_BTN_OP   0x00d2691eu   /* orange */
#define COL_BTN_FN   0x00606078u   /* lighter grey for C, +/-, %, DEL */
#define COL_BTN_EQ   0x002a8a2au   /* green */
#define COL_GLYPH    0x00f0f0f0u   /* near-white */
#define COL_GLYPH_OP 0x00ffffffu

/* Tiny 5x7 glyphs for button labels (only the ones we need). */
typedef struct { char ch; const char *rows[7]; } Glyph;
static const Glyph kGlyphs[] = {
    {'+', { "  X  ", "  X  ", "XXXXX", "  X  ", "  X  ", "     ", "     "}},
    {'-', { "     ", "     ", "XXXXX", "     ", "     ", "     ", "     "}},
    {'*', { "X X X", " XXX ", "X X X", " XXX ", "X X X", "     ", "     "}},
    {'/', { "    X", "   X ", "  X  ", " X   ", "X    ", "     ", "     "}},
    {'=', { "     ", "XXXXX", "     ", "XXXXX", "     ", "     ", "     "}},
    {'.', { "     ", "     ", "     ", "     ", "  XX ", "  XX ", "     "}},
    {'%', { "XX  X", "XX X ", "  X  ", " X XX", "X  XX", "     ", "     "}},
    {'C', { " XXX ", "X   X", "X    ", "X    ", "X   X", " XXX ", "     "}},
    {'D', { "XXX  ", "X  X ", "X   X", "X   X", "X  X ", "XXX  ", "     "}},
    {'E', { "XXXXX", "X    ", "XXX  ", "X    ", "X    ", "XXXXX", "     "}},
    {'L', { "X    ", "X    ", "X    ", "X    ", "X    ", "XXXXX", "     "}},
    {'/', { "    X", "   X ", "  X  ", " X   ", "X    ", "     ", "     "}},
    {'0', { " XXX ", "X   X", "X  XX", "X X X", "XX  X", " XXX ", "     "}},
    {'1', { "  X  ", " XX  ", "  X  ", "  X  ", "  X  ", " XXX ", "     "}},
    {'2', { " XXX ", "X   X", "    X", "  XX ", " X   ", "XXXXX", "     "}},
    {'3', { "XXXX ", "    X", "  XX ", "    X", "X   X", " XXX ", "     "}},
    {'4', { "   X ", "  XX ", " X X ", "X  X ", "XXXXX", "   X ", "     "}},
    {'5', { "XXXXX", "X    ", "XXXX ", "    X", "X   X", " XXX ", "     "}},
    {'6', { " XXX ", "X    ", "XXXX ", "X   X", "X   X", " XXX ", "     "}},
    {'7', { "XXXXX", "    X", "   X ", "  X  ", " X   ", " X   ", "     "}},
    {'8', { " XXX ", "X   X", " XXX ", "X   X", "X   X", " XXX ", "     "}},
    {'9', { " XXX ", "X   X", "X   X", " XXXX", "    X", " XXX ", "     "}},
};
#define GLYPH_W 5
#define GLYPH_H 7

static const Glyph *find_glyph(char c) {
    for (size_t i = 0; i < sizeof(kGlyphs)/sizeof(kGlyphs[0]); i++)
        if (kGlyphs[i].ch == c) return &kGlyphs[i];
    return NULL;
}

static void draw_glyph(int x, int y, int scale, char c, uint32_t col) {
    const Glyph *g = find_glyph(c);
    if (!g) return;
    for (int r = 0; r < GLYPH_H; r++) {
        const char *row = g->rows[r];
        for (int xx = 0; xx < GLYPH_W; xx++) {
            if (row[xx] == 'X') fill_rect(x + xx*scale, y + r*scale, scale, scale, col);
        }
    }
}

static void draw_label(int x, int y, int scale, const char *s, uint32_t col) {
    int adv = (GLYPH_W + 1) * scale;
    for (; *s; s++) { draw_glyph(x, y, scale, *s, col); x += adv; }
}

static int label_width(int scale, const char *s) {
    int n = 0; for (; *s; s++) n++;
    return n * (GLYPH_W + 1) * scale - scale;
}

static void render_calculator(void) {
    /* Background. */
    fill_rect(0, 0, W, H, COL_BG);

    /* Outer frame inset. */
    int M = 6;
    rounded_rect(M, M, W - 2*M, H - 2*M, 6, COL_FRAME);

    /* LCD display panel. */
    int dx = M + 6, dy = M + 6;
    int dw = W - 2*(M + 6), dh = 64;
    rounded_rect(dx, dy, dw, dh, 4, COL_DISPLAY);

    /* "Phantom" 7-seg digits in dim colour, then real digits in bright. */
    int digit_h = 40, digit_w = 22, gap = 4;
    int n_digits = 8;
    int total_w = n_digits * digit_w + (n_digits - 1) * gap;
    int sx = dx + dw - total_w - 8;
    int sy = dy + (dh - digit_h) / 2;
    for (int i = 0; i < n_digits; i++)
        draw_digit(sx + i * (digit_w + gap), sy, digit_w, digit_h, 8, COL_DIGIT_DIM);

    /* Show a sample number "1337". */
    const char *value = "1337";
    int vw = (int)strlen(value) * digit_w + ((int)strlen(value) - 1) * gap;
    int vx = dx + dw - vw - 8;
    draw_digit_string(vx, sy, digit_w, digit_h, gap, value, COL_DIGIT);

    /* Button grid: 4 cols x 5 rows below the display. */
    int gy0 = dy + dh + 8;
    int gh  = H - M - 8 - gy0;
    int gx0 = M + 6;
    int gw  = W - 2*(M + 6);
    int cols = 4, rows = 5;
    int gap_b = 6;
    int bw = (gw - (cols - 1) * gap_b) / cols;
    int bh = (gh - (rows - 1) * gap_b) / rows;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const char *lab = kButtons[r][c];
            uint32_t col;
            uint32_t lcol = COL_GLYPH;
            if (c == cols - 1) {
                col = (r == rows - 1) ? COL_BTN_EQ : COL_BTN_OP;
                lcol = COL_GLYPH_OP;
            } else if (r == 0) {
                col = COL_BTN_FN;
            } else if (r == rows - 1 && c == cols - 2) {
                col = COL_BTN_NUM;          /* "." */
            } else if (r == rows - 1 && c == cols - 2) {
                col = COL_BTN_NUM;
            } else if (lab[0] == 'D') {     /* DEL */
                col = COL_BTN_FN;
            } else {
                col = COL_BTN_NUM;
            }

            int bx = gx0 + c * (bw + gap_b);
            int by = gy0 + r * (bh + gap_b);
            rounded_rect(bx, by, bw, bh, 5, col);

            /* Choose label scale to fit. */
            int scale = (strlen(lab) >= 3) ? 1 : 2;
            int lw = label_width(scale, lab);
            int lh = GLYPH_H * scale;
            draw_label(bx + (bw - lw) / 2, by + (bh - lh) / 2,
                       scale, lab, lcol);
        }
    }

    /* Title bar text. */
    draw_label(M + 8, M - 1, 1, "BROOK CALC", 0x00b0b0c0u);
}

/* --------------------------------------------------------------------- */

static int g_got_release = 0;
static void on_buf_release(void *d, struct wl_buffer *b) {
    (void)d; (void)b; g_got_release = 1;
}
static const struct wl_buffer_listener buf_lis = { .release = on_buf_release };

int main(int argc, char **argv) {
    int hold_seconds = 8;
    if (argc >= 2) hold_seconds = atoi(argv[1]);
    if (hold_seconds < 1) hold_seconds = 1;
    if (hold_seconds > 60) hold_seconds = 60;

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    if (!getenv("WAYLAND_DISPLAY")) setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp", 1);

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "[calc] FAIL: wl_display_connect: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "[calc] connected\n");

    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_lis, NULL);
    wl_display_roundtrip(dpy);

    if (!g_shm || !g_comp || !g_wm) {
        fprintf(stderr, "[calc] FAIL: missing globals\n");
        return 1;
    }
    xdg_wm_base_add_listener(g_wm, &wm_lis, NULL);
    fprintf(stderr, "[calc] globals bound\n");

    int SIZE = STRIDE * H;
    int fd = memfd_create_shim("brook-calc", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, SIZE) < 0) {
        fprintf(stderr, "[calc] FAIL: memfd: %s\n", strerror(errno));
        return 1;
    }
    g_px = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_px == MAP_FAILED) {
        fprintf(stderr, "[calc] FAIL: mmap: %s\n", strerror(errno));
        return 1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, SIZE);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, W, H, STRIDE,
                                                       WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(buf, &buf_lis, NULL);

    struct wl_surface *surf = wl_compositor_create_surface(g_comp);
    struct xdg_surface *xs  = xdg_wm_base_get_xdg_surface(g_wm, surf);
    xdg_surface_add_listener(xs, &xs_lis, NULL);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_lis, NULL);
    xdg_toplevel_set_title(tl, "Brook Calc");
    xdg_toplevel_set_app_id(tl, "brook.calc");

    wl_surface_commit(surf);
    fprintf(stderr, "[calc] xdg_toplevel committed, awaiting configure\n");

    for (int i = 0; i < 40 && !g_got_configure; i++) {
        wl_display_roundtrip(dpy);
        if (g_got_configure) break;
        struct timespec t = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    if (!g_got_configure) {
        fprintf(stderr, "[calc] FAIL: never got configure\n");
        return 1;
    }
    wl_display_flush(dpy);

    render_calculator();

    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage(surf, 0, 0, W, H);
    wl_surface_commit(surf);
    fprintf(stderr, "[calc] frame 1 committed (%dx%d)\n", W, H);

    /* Hold for N seconds, dispatching events so the server can ping us. */
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        struct timespec n; clock_gettime(CLOCK_MONOTONIC, &n);
        long elapsed = n.tv_sec - t0.tv_sec;
        if (elapsed >= hold_seconds) break;
        struct timespec st = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&st, NULL);
    }

    fprintf(stderr, "[calc] hold complete (%ds), tearing down\n", hold_seconds);

    xdg_toplevel_destroy(tl);
    xdg_surface_destroy(xs);
    wl_surface_destroy(surf);
    wl_buffer_destroy(buf);
    wl_shm_pool_destroy(pool);
    munmap(g_px, SIZE);
    close(fd);
    wl_display_disconnect(dpy);

    fprintf(stderr, "[calc] PASS\n");
    return 0;
}
