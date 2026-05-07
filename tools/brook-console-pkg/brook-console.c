/*
 * brook-console.c — Brook-native Wayland kernel console viewer.
 *
 * Opens /dev/klog, reads kernel log lines, and renders them in a
 * scrollable Wayland window with auto-scroll on new content.
 *
 * Usage: brook-console
 *
 * Keyboard:
 *   Page Up / Page Down  Scroll by page
 *   Home / End           Scroll to top / bottom (re-enables auto-scroll)
 *   Esc / Ctrl+Q         Quit
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
    return (int)syscall(319, name, flags);
}

/* ========================= Configuration ========================= */

#define WIN_W  800
#define WIN_H  600
#define BPP    4
#define STRIDE (WIN_W * BPP)

#define STATUS_H    20   /* status bar height */
#define MARGIN_L     6   /* left margin */
#define ROW_H       12   /* line height (font 10px + 2px spacing) */
#define VISIBLE_ROWS ((WIN_H - STATUS_H) / ROW_H)

/* Colours (ARGB8888) — Catppuccin Mocha */
#define COL_BG         0xFF1E1E2E
#define COL_TEXT       0xFFCDD6F4
#define COL_DIM        0xFF6C7086
#define COL_STATUS_BG  0xFF313244
#define COL_STATUS_TXT 0xFFCDD6F4
#define COL_ACCENT     0xFF89B4FA
#define COL_WARN       0xFFF9E2AF
#define COL_ERR        0xFFF38BA8

/* Ring buffer for displayed lines */
#define MAX_LINES 8192
#define MAX_LINE_LEN 256

/* ========================= Bitmap Font (6x10) ========================= */

static const uint8_t g_font6x10[][10] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x00,0x20,0x20,0x20,0x20,0x20,0x00,0x20,0x00,0x00},
    /* 34 '"' */ {0x00,0x50,0x50,0x50,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x00,0x50,0xF8,0x50,0x50,0xF8,0x50,0x00,0x00,0x00},
    /* 36 '$' */ {0x00,0x20,0x78,0xA0,0x70,0x28,0xF0,0x20,0x00,0x00},
    /* 37 '%' */ {0x00,0xC8,0xC8,0x10,0x20,0x40,0x98,0x98,0x00,0x00},
    /* 38 '&' */ {0x00,0x40,0xA0,0xA0,0x40,0xA8,0x90,0x68,0x00,0x00},
    /* 39 '\''*/ {0x00,0x20,0x20,0x20,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 40 '(' */ {0x00,0x10,0x20,0x40,0x40,0x40,0x20,0x10,0x00,0x00},
    /* 41 ')' */ {0x00,0x40,0x20,0x10,0x10,0x10,0x20,0x40,0x00,0x00},
    /* 42 '*' */ {0x00,0x00,0x20,0xA8,0x70,0xA8,0x20,0x00,0x00,0x00},
    /* 43 '+' */ {0x00,0x00,0x20,0x20,0xF8,0x20,0x20,0x00,0x00,0x00},
    /* 44 ',' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x20,0x40,0x00},
    /* 45 '-' */ {0x00,0x00,0x00,0x00,0xF8,0x00,0x00,0x00,0x00,0x00},
    /* 46 '.' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x00,0x00,0x00},
    /* 47 '/' */ {0x00,0x08,0x08,0x10,0x20,0x40,0x80,0x80,0x00,0x00},
    /* 48 '0' */ {0x00,0x70,0x88,0x98,0xA8,0xC8,0x88,0x70,0x00,0x00},
    /* 49 '1' */ {0x00,0x20,0x60,0x20,0x20,0x20,0x20,0x70,0x00,0x00},
    /* 50 '2' */ {0x00,0x70,0x88,0x08,0x10,0x20,0x40,0xF8,0x00,0x00},
    /* 51 '3' */ {0x00,0x70,0x88,0x08,0x30,0x08,0x88,0x70,0x00,0x00},
    /* 52 '4' */ {0x00,0x10,0x30,0x50,0x90,0xF8,0x10,0x10,0x00,0x00},
    /* 53 '5' */ {0x00,0xF8,0x80,0xF0,0x08,0x08,0x88,0x70,0x00,0x00},
    /* 54 '6' */ {0x00,0x30,0x40,0x80,0xF0,0x88,0x88,0x70,0x00,0x00},
    /* 55 '7' */ {0x00,0xF8,0x08,0x10,0x20,0x40,0x40,0x40,0x00,0x00},
    /* 56 '8' */ {0x00,0x70,0x88,0x88,0x70,0x88,0x88,0x70,0x00,0x00},
    /* 57 '9' */ {0x00,0x70,0x88,0x88,0x78,0x08,0x10,0x60,0x00,0x00},
    /* 58 ':' */ {0x00,0x00,0x00,0x20,0x00,0x00,0x20,0x00,0x00,0x00},
    /* 59 ';' */ {0x00,0x00,0x00,0x20,0x00,0x00,0x20,0x20,0x40,0x00},
    /* 60 '<' */ {0x00,0x08,0x10,0x20,0x40,0x20,0x10,0x08,0x00,0x00},
    /* 61 '=' */ {0x00,0x00,0x00,0xF8,0x00,0xF8,0x00,0x00,0x00,0x00},
    /* 62 '>' */ {0x00,0x80,0x40,0x20,0x10,0x20,0x40,0x80,0x00,0x00},
    /* 63 '?' */ {0x00,0x70,0x88,0x08,0x10,0x20,0x00,0x20,0x00,0x00},
    /* 64 '@' */ {0x00,0x70,0x88,0xB8,0xA8,0xB8,0x80,0x70,0x00,0x00},
    /* 65 'A' */ {0x00,0x70,0x88,0x88,0xF8,0x88,0x88,0x88,0x00,0x00},
    /* 66 'B' */ {0x00,0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0,0x00,0x00},
    /* 67 'C' */ {0x00,0x70,0x88,0x80,0x80,0x80,0x88,0x70,0x00,0x00},
    /* 68 'D' */ {0x00,0xE0,0x90,0x88,0x88,0x88,0x90,0xE0,0x00,0x00},
    /* 69 'E' */ {0x00,0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8,0x00,0x00},
    /* 70 'F' */ {0x00,0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0x00,0x00},
    /* 71 'G' */ {0x00,0x70,0x88,0x80,0xB8,0x88,0x88,0x70,0x00,0x00},
    /* 72 'H' */ {0x00,0x88,0x88,0x88,0xF8,0x88,0x88,0x88,0x00,0x00},
    /* 73 'I' */ {0x00,0x70,0x20,0x20,0x20,0x20,0x20,0x70,0x00,0x00},
    /* 74 'J' */ {0x00,0x38,0x10,0x10,0x10,0x10,0x90,0x60,0x00,0x00},
    /* 75 'K' */ {0x00,0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88,0x00,0x00},
    /* 76 'L' */ {0x00,0x80,0x80,0x80,0x80,0x80,0x80,0xF8,0x00,0x00},
    /* 77 'M' */ {0x00,0x88,0xD8,0xA8,0x88,0x88,0x88,0x88,0x00,0x00},
    /* 78 'N' */ {0x00,0x88,0xC8,0xA8,0x98,0x88,0x88,0x88,0x00,0x00},
    /* 79 'O' */ {0x00,0x70,0x88,0x88,0x88,0x88,0x88,0x70,0x00,0x00},
    /* 80 'P' */ {0x00,0xF0,0x88,0x88,0xF0,0x80,0x80,0x80,0x00,0x00},
    /* 81 'Q' */ {0x00,0x70,0x88,0x88,0x88,0xA8,0x90,0x68,0x00,0x00},
    /* 82 'R' */ {0x00,0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0x00,0x00},
    /* 83 'S' */ {0x00,0x70,0x88,0x80,0x70,0x08,0x88,0x70,0x00,0x00},
    /* 84 'T' */ {0x00,0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00},
    /* 85 'U' */ {0x00,0x88,0x88,0x88,0x88,0x88,0x88,0x70,0x00,0x00},
    /* 86 'V' */ {0x00,0x88,0x88,0x88,0x88,0x50,0x50,0x20,0x00,0x00},
    /* 87 'W' */ {0x00,0x88,0x88,0x88,0x88,0xA8,0xD8,0x88,0x00,0x00},
    /* 88 'X' */ {0x00,0x88,0x88,0x50,0x20,0x50,0x88,0x88,0x00,0x00},
    /* 89 'Y' */ {0x00,0x88,0x88,0x50,0x20,0x20,0x20,0x20,0x00,0x00},
    /* 90 'Z' */ {0x00,0xF8,0x08,0x10,0x20,0x40,0x80,0xF8,0x00,0x00},
    /* 91 '[' */ {0x00,0x70,0x40,0x40,0x40,0x40,0x40,0x70,0x00,0x00},
    /* 92 '\\'*/ {0x00,0x80,0x80,0x40,0x20,0x10,0x08,0x08,0x00,0x00},
    /* 93 ']' */ {0x00,0x70,0x10,0x10,0x10,0x10,0x10,0x70,0x00,0x00},
    /* 94 '^' */ {0x00,0x20,0x50,0x88,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 95 '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0x00,0x00},
    /* 96 '`' */ {0x00,0x40,0x20,0x10,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 97 'a' */ {0x00,0x00,0x00,0x70,0x08,0x78,0x88,0x78,0x00,0x00},
    /* 98 'b' */ {0x00,0x80,0x80,0xF0,0x88,0x88,0x88,0xF0,0x00,0x00},
    /* 99 'c' */ {0x00,0x00,0x00,0x70,0x88,0x80,0x88,0x70,0x00,0x00},
    /*100 'd' */ {0x00,0x08,0x08,0x78,0x88,0x88,0x88,0x78,0x00,0x00},
    /*101 'e' */ {0x00,0x00,0x00,0x70,0x88,0xF8,0x80,0x70,0x00,0x00},
    /*102 'f' */ {0x00,0x30,0x48,0x40,0xF0,0x40,0x40,0x40,0x00,0x00},
    /*103 'g' */ {0x00,0x00,0x00,0x78,0x88,0x88,0x78,0x08,0x70,0x00},
    /*104 'h' */ {0x00,0x80,0x80,0xF0,0x88,0x88,0x88,0x88,0x00,0x00},
    /*105 'i' */ {0x00,0x20,0x00,0x60,0x20,0x20,0x20,0x70,0x00,0x00},
    /*106 'j' */ {0x00,0x10,0x00,0x30,0x10,0x10,0x10,0x90,0x60,0x00},
    /*107 'k' */ {0x00,0x80,0x80,0x90,0xA0,0xC0,0xA0,0x90,0x00,0x00},
    /*108 'l' */ {0x00,0x60,0x20,0x20,0x20,0x20,0x20,0x70,0x00,0x00},
    /*109 'm' */ {0x00,0x00,0x00,0xD0,0xA8,0xA8,0xA8,0x88,0x00,0x00},
    /*110 'n' */ {0x00,0x00,0x00,0xF0,0x88,0x88,0x88,0x88,0x00,0x00},
    /*111 'o' */ {0x00,0x00,0x00,0x70,0x88,0x88,0x88,0x70,0x00,0x00},
    /*112 'p' */ {0x00,0x00,0x00,0xF0,0x88,0x88,0xF0,0x80,0x80,0x00},
    /*113 'q' */ {0x00,0x00,0x00,0x78,0x88,0x88,0x78,0x08,0x08,0x00},
    /*114 'r' */ {0x00,0x00,0x00,0xB0,0xC8,0x80,0x80,0x80,0x00,0x00},
    /*115 's' */ {0x00,0x00,0x00,0x78,0x80,0x70,0x08,0xF0,0x00,0x00},
    /*116 't' */ {0x00,0x40,0x40,0xF0,0x40,0x40,0x48,0x30,0x00,0x00},
    /*117 'u' */ {0x00,0x00,0x00,0x88,0x88,0x88,0x88,0x78,0x00,0x00},
    /*118 'v' */ {0x00,0x00,0x00,0x88,0x88,0x88,0x50,0x20,0x00,0x00},
    /*119 'w' */ {0x00,0x00,0x00,0x88,0x88,0xA8,0xA8,0x50,0x00,0x00},
    /*120 'x' */ {0x00,0x00,0x00,0x88,0x50,0x20,0x50,0x88,0x00,0x00},
    /*121 'y' */ {0x00,0x00,0x00,0x88,0x88,0x78,0x08,0x88,0x70,0x00},
    /*122 'z' */ {0x00,0x00,0x00,0xF8,0x10,0x20,0x40,0xF8,0x00,0x00},
    /*123 '{' */ {0x00,0x18,0x20,0x20,0xC0,0x20,0x20,0x18,0x00,0x00},
    /*124 '|' */ {0x00,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00},
    /*125 '}' */ {0x00,0xC0,0x20,0x20,0x18,0x20,0x20,0xC0,0x00,0x00},
    /*126 '~' */ {0x00,0x00,0x00,0x40,0xA8,0x10,0x00,0x00,0x00,0x00},
};

#define FONT_W 6
#define FONT_H 10

static uint32_t *g_px;

static inline void put_px(int x, int y, uint32_t c) {
    if (x >= 0 && y >= 0 && x < WIN_W && y < WIN_H)
        g_px[y * WIN_W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            put_px(x + xx, y + yy, c);
}

static void draw_char(int cx, int cy, char ch, uint32_t colour) {
    if (ch < 32 || ch > 126) ch = '?';
    int idx = ch - 32;
    const uint8_t *glyph = g_font6x10[idx];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (0x80 >> col))
                put_px(cx + col, cy + row, colour);
        }
    }
}

static void draw_str(int x, int y, const char *s, uint32_t colour) {
    while (*s) {
        draw_char(x, y, *s, colour);
        x += FONT_W;
        s++;
    }
}

/* ========================= Log Ring Buffer ========================= */

static char g_lines[MAX_LINES][MAX_LINE_LEN];
static int  g_line_count = 0;
static int  g_line_head  = 0; /* ring index of oldest line */

static int  g_scroll_pos = 0; /* index of topmost visible line (0 = oldest) */
static int  g_auto_scroll = 1;

static void add_line(const char *text, int len) {
    if (len >= MAX_LINE_LEN) len = MAX_LINE_LEN - 1;
    int idx;
    if (g_line_count < MAX_LINES) {
        idx = g_line_count++;
    } else {
        idx = g_line_head;
        g_line_head = (g_line_head + 1) % MAX_LINES;
    }
    memcpy(g_lines[idx], text, len);
    g_lines[idx][len] = '\0';

    if (g_auto_scroll) {
        g_scroll_pos = g_line_count > VISIBLE_ROWS ? g_line_count - VISIBLE_ROWS : 0;
    }
}

static const char *get_line(int logical_idx) {
    if (logical_idx < 0 || logical_idx >= g_line_count) return "";
    int ring_idx = (g_line_head + logical_idx) % MAX_LINES;
    return g_lines[ring_idx];
}

/* Parse raw bytes from /dev/klog into lines */
static char g_partial[MAX_LINE_LEN];
static int  g_partial_len = 0;

static void ingest_data(const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n' || g_partial_len >= MAX_LINE_LEN - 1) {
            add_line(g_partial, g_partial_len);
            g_partial_len = 0;
        } else {
            g_partial[g_partial_len++] = buf[i];
        }
    }
}

/* Choose colour based on line content */
static uint32_t line_colour(const char *line) {
    if (strstr(line, "PANIC") || strstr(line, "panic") ||
        strstr(line, "FAULT") || strstr(line, "fault") ||
        strstr(line, "ERROR") || strstr(line, "error"))
        return COL_ERR;
    if (strstr(line, "WARN") || strstr(line, "warn"))
        return COL_WARN;
    if (strstr(line, "sys_") || strstr(line, "syscall"))
        return COL_DIM;
    return COL_TEXT;
}

/* ========================= Wayland Globals ========================= */

static struct wl_display    *g_display;
static struct wl_registry   *g_registry;
static struct wl_compositor *g_compositor;
static struct wl_shm        *g_shm;
static struct wl_seat       *g_seat;
static struct xdg_wm_base   *g_xdg_wm_base;

static struct wl_surface      *g_surface;
static struct xdg_surface     *g_xdg_surface;
static struct xdg_toplevel    *g_xdg_toplevel;
static struct wl_keyboard     *g_keyboard;

static int  g_running    = 1;
static int  g_needs_draw = 1;
static int  g_configured = 0;
static int  g_ctrl_held  = 0;

/* SHM buffer */
static struct wl_buffer *g_buffer;
static int               g_shm_fd   = -1;
static int               g_shm_size;

/* /dev/klog fd */
static int g_klog_fd = -1;

/* ========================= Render ========================= */

static void render(void) {
    /* Background */
    fill_rect(0, 0, WIN_W, WIN_H, COL_BG);

    /* Log lines */
    int max_cols = (WIN_W - MARGIN_L * 2) / FONT_W;
    for (int row = 0; row < VISIBLE_ROWS; row++) {
        int line_idx = g_scroll_pos + row;
        if (line_idx >= g_line_count) break;
        const char *line = get_line(line_idx);
        uint32_t col = line_colour(line);
        int y = row * ROW_H;
        int x = MARGIN_L;
        for (int c = 0; line[c] && c < max_cols; c++) {
            draw_char(x, y, line[c], col);
            x += FONT_W;
        }
    }

    /* Status bar */
    fill_rect(0, WIN_H - STATUS_H, WIN_W, STATUS_H, COL_STATUS_BG);
    char status[128];
    int top = g_scroll_pos + 1;
    int bot = g_scroll_pos + VISIBLE_ROWS;
    if (bot > g_line_count) bot = g_line_count;
    snprintf(status, sizeof(status), " Brook Kernel Console  |  Lines %d-%d / %d  %s",
             top, bot, g_line_count,
             g_auto_scroll ? "[AUTO]" : "[MANUAL]");
    draw_str(0, WIN_H - STATUS_H + 5, status, COL_STATUS_TXT);
}

/* ========================= Wayland Callbacks ========================= */

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(xdg_surface, serial);
    g_configured = 1;
    g_needs_draw = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *d, struct xdg_toplevel *t,
                               int32_t w, int32_t h,
                               struct wl_array *states) {
    (void)d; (void)t; (void)w; (void)h; (void)states;
}

static void toplevel_close(void *d, struct xdg_toplevel *t) {
    (void)d; (void)t;
    g_running = 0;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close     = toplevel_close,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

/* Keyboard — evdev keycodes */
static void kbd_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t sz) {
    (void)d; (void)k; (void)fmt; (void)sz;
    close(fd);
}

static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t s,
                      struct wl_surface *sf, struct wl_array *keys) {
    (void)d; (void)k; (void)s; (void)sf; (void)keys;
}

static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t s,
                      struct wl_surface *sf) {
    (void)d; (void)k; (void)s; (void)sf;
}

static void kbd_key(void *d, struct wl_keyboard *k, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state) {
    (void)d; (void)k; (void)serial; (void)time;
    if (state != 1) return; /* press only */

    switch (key) {
    case 1:  /* Esc */
        g_running = 0;
        break;
    case 104: /* Page Down */
        g_auto_scroll = 0;
        g_scroll_pos += VISIBLE_ROWS;
        if (g_scroll_pos > g_line_count - VISIBLE_ROWS)
            g_scroll_pos = g_line_count > VISIBLE_ROWS ? g_line_count - VISIBLE_ROWS : 0;
        g_needs_draw = 1;
        break;
    case 109: /* Page Up */
        g_auto_scroll = 0;
        g_scroll_pos -= VISIBLE_ROWS;
        if (g_scroll_pos < 0) g_scroll_pos = 0;
        g_needs_draw = 1;
        break;
    case 102: /* Home */
        g_scroll_pos = 0;
        g_auto_scroll = 0;
        g_needs_draw = 1;
        break;
    case 107: /* End */
        g_scroll_pos = g_line_count > VISIBLE_ROWS ? g_line_count - VISIBLE_ROWS : 0;
        g_auto_scroll = 1;
        g_needs_draw = 1;
        break;
    case 16: /* Q */
        if (g_ctrl_held) g_running = 0;
        break;
    }
}

static void kbd_modifiers(void *d, struct wl_keyboard *k, uint32_t serial,
                          uint32_t depressed, uint32_t latched,
                          uint32_t locked, uint32_t group) {
    (void)d; (void)k; (void)serial; (void)latched; (void)locked; (void)group;
    g_ctrl_held = (depressed & 0x4) != 0;
}

static void kbd_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)d; (void)k; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener kbd_listener = {
    .keymap      = kbd_keymap,
    .enter       = kbd_enter,
    .leave       = kbd_leave,
    .key         = kbd_key,
    .modifiers   = kbd_modifiers,
    .repeat_info = kbd_repeat,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_keyboard) {
        g_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_keyboard, &kbd_listener, NULL);
    }
}

static void seat_name(void *d, struct wl_seat *s, const char *name) {
    (void)d; (void)s; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

/* Registry */
static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t id, const char *iface, uint32_t ver) {
    (void)data; (void)ver;
    if (!strcmp(iface, wl_compositor_interface.name))
        g_compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        g_shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name))
        g_xdg_wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) {
        g_seat = wl_registry_bind(reg, id, &wl_seat_interface, 5);
        wl_seat_add_listener(g_seat, &seat_listener, NULL);
    }
}

static void registry_global_remove(void *d, struct wl_registry *r, uint32_t id) {
    (void)d; (void)r; (void)id;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* Frame callback */
static struct wl_callback *g_frame_cb;

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)data; (void)time;
    wl_callback_destroy(cb);

    /* Poll /dev/klog for new data */
    if (g_klog_fd >= 0) {
        char buf[4096];
        int prev_count = g_line_count;
        for (;;) {
            int n = read(g_klog_fd, buf, sizeof(buf));
            if (n <= 0) break;
            ingest_data(buf, n);
        }
        if (g_line_count != prev_count)
            g_needs_draw = 1;
    }

    if (g_needs_draw && g_configured) {
        render();
        wl_surface_attach(g_surface, g_buffer, 0, 0);
        wl_surface_damage_buffer(g_surface, 0, 0, WIN_W, WIN_H);
        g_needs_draw = 0;
    }

    g_frame_cb = wl_surface_frame(g_surface);
    wl_callback_add_listener(g_frame_cb, &frame_listener, NULL);
    wl_surface_commit(g_surface);
}

/* ========================= Main ========================= */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Open /dev/klog in non-blocking mode */
    g_klog_fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    if (g_klog_fd < 0) {
        /* Fallback: add an info message */
        add_line("[brook-console] Could not open /dev/klog", 41);
        add_line("[brook-console] Running without kernel log feed", 47);
    }

    /* Connect to Wayland — retry loop for waylandd startup race */
    for (int i = 0; i < 20; i++) {
        g_display = wl_display_connect(NULL);
        if (g_display) break;
        usleep(100000);
    }
    if (!g_display) {
        fprintf(stderr, "brook-console: cannot connect to Wayland\n");
        return 1;
    }

    g_registry = wl_display_get_registry(g_display);
    wl_registry_add_listener(g_registry, &registry_listener, NULL);
    wl_display_roundtrip(g_display);

    if (!g_compositor || !g_shm || !g_xdg_wm_base) {
        fprintf(stderr, "brook-console: missing Wayland globals\n");
        return 1;
    }
    xdg_wm_base_add_listener(g_xdg_wm_base, &wm_base_listener, NULL);

    /* Create SHM buffer */
    g_shm_size = WIN_W * WIN_H * BPP;
    g_shm_fd = memfd_create_shim("brook-console-shm", MFD_CLOEXEC);
    if (g_shm_fd < 0) { perror("memfd_create"); return 1; }
    ftruncate(g_shm_fd, g_shm_size);

    g_px = mmap(NULL, g_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (g_px == MAP_FAILED) { perror("mmap"); return 1; }

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, g_shm_fd, g_shm_size);
    g_buffer = wl_shm_pool_create_buffer(pool, 0, WIN_W, WIN_H, STRIDE, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    /* Create surface */
    g_surface = wl_compositor_create_surface(g_compositor);
    g_xdg_surface = xdg_wm_base_get_xdg_surface(g_xdg_wm_base, g_surface);
    xdg_surface_add_listener(g_xdg_surface, &xdg_surface_listener, NULL);

    g_xdg_toplevel = xdg_surface_get_toplevel(g_xdg_surface);
    xdg_toplevel_add_listener(g_xdg_toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(g_xdg_toplevel, "Brook Console");
    xdg_toplevel_set_app_id(g_xdg_toplevel, "brook-console");

    /* Initial frame callback */
    g_frame_cb = wl_surface_frame(g_surface);
    wl_callback_add_listener(g_frame_cb, &frame_listener, NULL);
    wl_surface_commit(g_surface);

    /* Event loop */
    while (g_running && wl_display_dispatch(g_display) != -1)
        ;

    /* Cleanup */
    if (g_klog_fd >= 0) close(g_klog_fd);
    if (g_keyboard) wl_keyboard_destroy(g_keyboard);
    xdg_toplevel_destroy(g_xdg_toplevel);
    xdg_surface_destroy(g_xdg_surface);
    wl_surface_destroy(g_surface);
    wl_buffer_destroy(g_buffer);
    munmap(g_px, g_shm_size);
    close(g_shm_fd);
    wl_display_disconnect(g_display);
    return 0;
}
