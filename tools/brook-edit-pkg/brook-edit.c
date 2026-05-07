/*
 * brook-edit.c — Brook-native Wayland text editor.
 *
 * Simple editor: open a file, edit text, save with Ctrl+S.
 * Built-in 6x10 bitmap font, no external deps beyond libwayland-client.
 *
 * Usage: brook-edit [filename]
 *
 * Keyboard:
 *   Arrow keys      Move cursor
 *   Home / End      Start / end of line
 *   Page Up/Down    Scroll by page
 *   Backspace       Delete char before cursor
 *   Delete          Delete char at cursor
 *   Enter           Insert newline
 *   Ctrl+S          Save file
 *   Ctrl+Q / Esc    Quit
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
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

#define WIN_W  720
#define WIN_H  480
#define BPP    4
#define STRIDE (WIN_W * BPP)

/* Layout */
#define GUTTER_W     42   /* line number gutter width */
#define STATUS_H     20   /* status bar height */
#define EDIT_X       GUTTER_W
#define EDIT_W       (WIN_W - GUTTER_W)
#define EDIT_H       (WIN_H - STATUS_H)
#define ROW_H        12   /* line height (font is 10px + 2px spacing) */

/* Colours (ARGB8888) */
#define COL_BG        0xFF1E1E2E
#define COL_GUTTER    0xFF181825
#define COL_GUTTER_TXT 0xFF6C7086
#define COL_TEXT      0xFFCDD6F4
#define COL_CURSOR    0xFFF5E0DC
#define COL_STATUS_BG 0xFF313244
#define COL_STATUS_TXT 0xFFCDD6F4
#define COL_MODIFIED  0xFFF38BA8
#define COL_LINEHL    0xFF24243A   /* current line highlight */

/* Limits */
#define MAX_LINES    16384
#define MAX_LINE_LEN 1024

/* ========================= Bitmap Font (6x10) ========================= */

/* Same font as brook-files — minimal ASCII 32-126 */
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

static void draw_text(int x, int y, const char *s, uint32_t colour) {
    while (*s) {
        draw_char(x, y, *s, colour);
        x += FONT_W;
        s++;
    }
}

/* ========================= Text buffer ========================= */

typedef struct {
    char  *data;
    int    len;
    int    cap;
} Line;

static Line    g_lines[MAX_LINES];
static int     g_line_count = 0;
static char    g_filename[512] = "";
static int     g_modified = 0;

/* Cursor position */
static int     g_cx = 0;   /* column */
static int     g_cy = 0;   /* line */
static int     g_scroll_y = 0;

static int     g_running = 1;
static int     g_needs_redraw = 1;
static int     g_ctrl_held = 0;

static void line_ensure_cap(Line *l, int need) {
    if (l->cap >= need) return;
    int newcap = need + 64;
    l->data = realloc(l->data, (size_t)newcap);
    l->cap = newcap;
}

static void line_insert_char(Line *l, int pos, char ch) {
    if (pos < 0) pos = 0;
    if (pos > l->len) pos = l->len;
    line_ensure_cap(l, l->len + 2);
    memmove(l->data + pos + 1, l->data + pos, (size_t)(l->len - pos));
    l->data[pos] = ch;
    l->len++;
    l->data[l->len] = '\0';
}

static void line_delete_char(Line *l, int pos) {
    if (pos < 0 || pos >= l->len) return;
    memmove(l->data + pos, l->data + pos + 1, (size_t)(l->len - pos - 1));
    l->len--;
    l->data[l->len] = '\0';
}

static void buffer_init(void) {
    g_line_count = 1;
    g_lines[0].data = calloc(64, 1);
    g_lines[0].len = 0;
    g_lines[0].cap = 64;
}

static void buffer_insert_line(int after) {
    if (g_line_count >= MAX_LINES) return;
    /* Shift lines down */
    for (int i = g_line_count; i > after + 1; i--)
        g_lines[i] = g_lines[i - 1];
    g_line_count++;
    Line *nl = &g_lines[after + 1];
    nl->data = calloc(64, 1);
    nl->len = 0;
    nl->cap = 64;
}

static void buffer_delete_line(int idx) {
    if (idx < 0 || idx >= g_line_count || g_line_count <= 1) return;
    free(g_lines[idx].data);
    for (int i = idx; i < g_line_count - 1; i++)
        g_lines[i] = g_lines[i + 1];
    g_line_count--;
    g_lines[g_line_count].data = NULL;
    g_lines[g_line_count].len = 0;
    g_lines[g_line_count].cap = 0;
}

/* Load file into buffer */
static int buffer_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    /* Free existing lines */
    for (int i = 0; i < g_line_count; i++)
        free(g_lines[i].data);
    g_line_count = 0;

    char buf[MAX_LINE_LEN];
    while (fgets(buf, MAX_LINE_LEN, f) && g_line_count < MAX_LINES) {
        int len = (int)strlen(buf);
        /* Strip trailing newline */
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';

        Line *l = &g_lines[g_line_count];
        l->cap = len + 64;
        l->data = calloc((size_t)l->cap, 1);
        memcpy(l->data, buf, (size_t)len);
        l->len = len;
        g_line_count++;
    }
    fclose(f);

    if (g_line_count == 0) {
        g_line_count = 1;
        g_lines[0].data = calloc(64, 1);
        g_lines[0].len = 0;
        g_lines[0].cap = 64;
    }

    strncpy(g_filename, path, sizeof(g_filename) - 1);
    g_modified = 0;
    g_cx = 0;
    g_cy = 0;
    g_scroll_y = 0;
    return 0;
}

/* Save buffer to file */
static int buffer_save(void) {
    if (g_filename[0] == '\0') return -1;

    FILE *f = fopen(g_filename, "w");
    if (!f) {
        fprintf(stderr, "[edit] save failed: %s\n", strerror(errno));
        return -1;
    }

    for (int i = 0; i < g_line_count; i++) {
        if (g_lines[i].data && g_lines[i].len > 0)
            fwrite(g_lines[i].data, 1, (size_t)g_lines[i].len, f);
        fputc('\n', f);
    }
    fclose(f);
    g_modified = 0;
    g_needs_redraw = 1;
    fprintf(stderr, "[edit] saved: %s (%d lines)\n", g_filename, g_line_count);
    return 0;
}

/* ========================= Editing operations ========================= */

static void clamp_cursor(void) {
    if (g_cy < 0) g_cy = 0;
    if (g_cy >= g_line_count) g_cy = g_line_count - 1;
    if (g_cx < 0) g_cx = 0;
    if (g_cx > g_lines[g_cy].len) g_cx = g_lines[g_cy].len;
}

static void insert_char(char ch) {
    Line *l = &g_lines[g_cy];
    line_insert_char(l, g_cx, ch);
    g_cx++;
    g_modified = 1;
    g_needs_redraw = 1;
}

static void insert_newline(void) {
    Line *cur = &g_lines[g_cy];
    int tail_len = cur->len - g_cx;

    buffer_insert_line(g_cy);
    Line *next = &g_lines[g_cy + 1];

    if (tail_len > 0) {
        line_ensure_cap(next, tail_len + 1);
        memcpy(next->data, cur->data + g_cx, (size_t)tail_len);
        next->len = tail_len;
        next->data[next->len] = '\0';
        cur->len = g_cx;
        cur->data[cur->len] = '\0';
    }

    g_cy++;
    g_cx = 0;
    g_modified = 1;
    g_needs_redraw = 1;
}

static void do_backspace(void) {
    if (g_cx > 0) {
        line_delete_char(&g_lines[g_cy], g_cx - 1);
        g_cx--;
        g_modified = 1;
    } else if (g_cy > 0) {
        /* Join with previous line */
        Line *prev = &g_lines[g_cy - 1];
        Line *cur = &g_lines[g_cy];
        int old_len = prev->len;
        line_ensure_cap(prev, prev->len + cur->len + 1);
        memcpy(prev->data + prev->len, cur->data, (size_t)cur->len);
        prev->len += cur->len;
        prev->data[prev->len] = '\0';
        buffer_delete_line(g_cy);
        g_cy--;
        g_cx = old_len;
        g_modified = 1;
    }
    g_needs_redraw = 1;
}

static void do_delete(void) {
    Line *cur = &g_lines[g_cy];
    if (g_cx < cur->len) {
        line_delete_char(cur, g_cx);
        g_modified = 1;
    } else if (g_cy < g_line_count - 1) {
        /* Join with next line */
        Line *next = &g_lines[g_cy + 1];
        line_ensure_cap(cur, cur->len + next->len + 1);
        memcpy(cur->data + cur->len, next->data, (size_t)next->len);
        cur->len += next->len;
        cur->data[cur->len] = '\0';
        buffer_delete_line(g_cy + 1);
        g_modified = 1;
    }
    g_needs_redraw = 1;
}

/* ========================= Rendering ========================= */

static void render(void) {
    fill_rect(0, 0, WIN_W, WIN_H, COL_BG);

    int visible_lines = EDIT_H / ROW_H;

    /* Ensure cursor is visible */
    if (g_cy < g_scroll_y) g_scroll_y = g_cy;
    if (g_cy >= g_scroll_y + visible_lines)
        g_scroll_y = g_cy - visible_lines + 1;
    if (g_scroll_y < 0) g_scroll_y = 0;

    /* Gutter background */
    fill_rect(0, 0, GUTTER_W, EDIT_H, COL_GUTTER);

    /* Draw lines */
    for (int vi = 0; vi < visible_lines; vi++) {
        int line_idx = g_scroll_y + vi;
        if (line_idx >= g_line_count) break;

        int y = vi * ROW_H;

        /* Current line highlight */
        if (line_idx == g_cy)
            fill_rect(GUTTER_W, y, EDIT_W, ROW_H, COL_LINEHL);

        /* Line number */
        char lnum[8];
        snprintf(lnum, sizeof(lnum), "%4d", line_idx + 1);
        uint32_t lnum_col = (line_idx == g_cy) ? COL_TEXT : COL_GUTTER_TXT;
        draw_text(3, y + 1, lnum, lnum_col);

        /* Text content */
        Line *l = &g_lines[line_idx];
        int max_chars = EDIT_W / FONT_W;
        int draw_len = l->len < max_chars ? l->len : max_chars;
        for (int ci = 0; ci < draw_len; ci++) {
            char ch = l->data[ci];
            if (ch == '\t') ch = ' '; /* simple tab display */
            draw_char(EDIT_X + ci * FONT_W, y + 1, ch, COL_TEXT);
        }

        /* Cursor */
        if (line_idx == g_cy) {
            int cursor_x = EDIT_X + g_cx * FONT_W;
            if (cursor_x < WIN_W) {
                fill_rect(cursor_x, y, 2, ROW_H, COL_CURSOR);
            }
        }
    }

    /* Status bar */
    fill_rect(0, WIN_H - STATUS_H, WIN_W, STATUS_H, COL_STATUS_BG);

    /* Filename + modified indicator */
    char status[256];
    const char *fname = g_filename[0] ? g_filename : "[new file]";
    snprintf(status, sizeof(status), " %s%s", fname,
             g_modified ? " [modified]" : "");
    draw_text(4, WIN_H - STATUS_H + 5, status,
              g_modified ? COL_MODIFIED : COL_STATUS_TXT);

    /* Position indicator */
    char pos[64];
    snprintf(pos, sizeof(pos), "Ln %d, Col %d  ", g_cy + 1, g_cx + 1);
    int pos_x = WIN_W - (int)strlen(pos) * FONT_W - 4;
    draw_text(pos_x, WIN_H - STATUS_H + 5, pos, COL_STATUS_TXT);
}

/* ========================= Wayland plumbing ========================= */

static struct wl_shm        *g_shm  = NULL;
static struct wl_compositor *g_comp = NULL;
static struct xdg_wm_base   *g_wm   = NULL;
static struct wl_surface    *g_surf  = NULL;
static struct wl_buffer     *g_buf   = NULL;
static struct wl_seat       *g_seat  = NULL;
static struct wl_keyboard   *g_kbd   = NULL;
static int g_got_configure = 0;

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
    else if (!strcmp(iface, "wl_seat"))
        g_seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                   version < 5 ? version : 5);
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
    xdg_surface_ack_configure(xs, serial);
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
    g_running = 0;
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

/* --- Keyboard --- */

static void on_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state) {
    (void)data; (void)kb; (void)serial; (void)time;
    if (state != 1) return;

    enum {
        KEY_ESC = 1, KEY_ENTER = 28, KEY_BACKSPACE = 14,
        KEY_UP = 103, KEY_DOWN = 108, KEY_LEFT = 105, KEY_RIGHT = 106,
        KEY_DELETE = 111, KEY_HOME = 102, KEY_END = 107,
        KEY_PAGEUP = 104, KEY_PAGEDOWN = 109,
        KEY_S = 31, KEY_Q = 16,
        KEY_LEFTCTRL = 29, KEY_RIGHTCTRL = 97,
    };

    /* Ctrl+key shortcuts */
    if (g_ctrl_held) {
        switch (key) {
        case KEY_S: buffer_save(); return;
        case KEY_Q: g_running = 0; return;
        }
    }

    int visible = EDIT_H / ROW_H;

    switch (key) {
    case KEY_ESC:
        g_running = 0;
        break;
    case KEY_UP:
        if (g_cy > 0) g_cy--;
        clamp_cursor();
        g_needs_redraw = 1;
        break;
    case KEY_DOWN:
        if (g_cy < g_line_count - 1) g_cy++;
        clamp_cursor();
        g_needs_redraw = 1;
        break;
    case KEY_LEFT:
        if (g_cx > 0) {
            g_cx--;
        } else if (g_cy > 0) {
            g_cy--;
            g_cx = g_lines[g_cy].len;
        }
        g_needs_redraw = 1;
        break;
    case KEY_RIGHT:
        if (g_cx < g_lines[g_cy].len) {
            g_cx++;
        } else if (g_cy < g_line_count - 1) {
            g_cy++;
            g_cx = 0;
        }
        g_needs_redraw = 1;
        break;
    case KEY_HOME:
        g_cx = 0;
        g_needs_redraw = 1;
        break;
    case KEY_END:
        g_cx = g_lines[g_cy].len;
        g_needs_redraw = 1;
        break;
    case KEY_PAGEUP:
        g_cy -= visible;
        if (g_cy < 0) g_cy = 0;
        clamp_cursor();
        g_needs_redraw = 1;
        break;
    case KEY_PAGEDOWN:
        g_cy += visible;
        if (g_cy >= g_line_count) g_cy = g_line_count - 1;
        clamp_cursor();
        g_needs_redraw = 1;
        break;
    case KEY_BACKSPACE:
        do_backspace();
        break;
    case KEY_DELETE:
        do_delete();
        break;
    case KEY_ENTER:
        insert_newline();
        break;
    default:
        /* Map evdev keycode to ASCII (simplified US layout) */
        if (!g_ctrl_held) {
            /* Printable character mapping for evdev keycodes */
            static const char keymap_lower[] =
                /* 2-11 */ "1234567890"
                /* 12-13 */ "-="
                /* 16-25 */ "qwertyuiop"
                /* 26-27 */ "[]"
                /* 30-38 */ "asdfghjkl"
                /* 39-41 */ ";'`"
                /* 43-50 */ "\\zxcvbnm"
                /* 51-52 */ ",.";
            static const struct { int code; char ch; } simple[] = {
                {2,'1'},{3,'2'},{4,'3'},{5,'4'},{6,'5'},{7,'6'},{8,'7'},
                {9,'8'},{10,'9'},{11,'0'},{12,'-'},{13,'='},
                {16,'q'},{17,'w'},{18,'e'},{19,'r'},{20,'t'},{21,'y'},
                {22,'u'},{23,'i'},{24,'o'},{25,'p'},{26,'['},{27,']'},
                {30,'a'},{31,'s'},{32,'d'},{33,'f'},{34,'g'},{35,'h'},
                {36,'j'},{37,'k'},{38,'l'},{39,';'},{40,'\''},{41,'`'},
                {43,'\\'},{44,'z'},{45,'x'},{46,'c'},{47,'v'},{48,'b'},
                {49,'n'},{50,'m'},{51,','},{52,'.'},{53,'/'},
                {57,' '},
                {0, 0}
            };
            (void)keymap_lower;
            for (int i = 0; simple[i].code; i++) {
                if ((int)key == simple[i].code) {
                    insert_char(simple[i].ch);
                    return;
                }
            }
        }
        break;
    }
}

static void on_keymap(void *d, struct wl_keyboard *k, uint32_t fmt,
                       int32_t fd, uint32_t sz) {
    (void)d; (void)k; (void)fmt; (void)sz;
    if (fd >= 0) close(fd);
}
static void on_kb_enter(void *d, struct wl_keyboard *k, uint32_t s,
                          struct wl_surface *sf, struct wl_array *keys) {
    (void)d; (void)k; (void)s; (void)sf; (void)keys;
}
static void on_kb_leave(void *d, struct wl_keyboard *k, uint32_t s,
                          struct wl_surface *sf) {
    (void)d; (void)k; (void)s; (void)sf;
}
static void on_kb_modifiers(void *d, struct wl_keyboard *k, uint32_t s,
                              uint32_t depressed, uint32_t latched,
                              uint32_t locked, uint32_t group) {
    (void)d; (void)k; (void)s; (void)latched; (void)locked; (void)group;
    /* Track Ctrl state via mods_depressed bit 2 (control) */
    g_ctrl_held = (depressed & 0x4) ? 1 : 0;
}
static void on_kb_repeat(void *d, struct wl_keyboard *k,
                           int32_t rate, int32_t delay) {
    (void)d; (void)k; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener kb_lis = {
    .keymap      = on_keymap,
    .enter       = on_kb_enter,
    .leave       = on_kb_leave,
    .key         = on_key,
    .modifiers   = on_kb_modifiers,
    .repeat_info = on_kb_repeat,
};

static void on_seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_kbd) {
        g_kbd = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_kbd, &kb_lis, NULL);
    }
}
static void on_seat_name(void *d, struct wl_seat *s, const char *n) {
    (void)d; (void)s; (void)n;
}
static const struct wl_seat_listener seat_lis = {
    .capabilities = on_seat_caps,
    .name = on_seat_name,
};

/* ========================= Frame callback ========================= */

static struct wl_callback *g_frame_cb = NULL;

static void on_frame(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_lis = { .done = on_frame };

static void commit_frame(void) {
    if (g_needs_redraw) {
        render();
        g_needs_redraw = 0;
    }
    wl_surface_attach(g_surf, g_buf, 0, 0);
    wl_surface_damage_buffer(g_surf, 0, 0, WIN_W, WIN_H);
    g_frame_cb = wl_surface_frame(g_surf);
    wl_callback_add_listener(g_frame_cb, &frame_lis, NULL);
    wl_surface_commit(g_surf);
}

static void on_frame(void *data, struct wl_callback *cb, uint32_t time) {
    (void)data; (void)time;
    wl_callback_destroy(cb);
    g_frame_cb = NULL;
    if (g_needs_redraw)
        commit_frame();
}

/* ========================= main ========================= */

int main(int argc, char *argv[]) {
    buffer_init();

    if (argc > 1) {
        if (buffer_load(argv[1]) < 0)
            fprintf(stderr, "[edit] new file: %s\n", argv[1]);
        strncpy(g_filename, argv[1], sizeof(g_filename) - 1);
    }

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "wl_display_connect failed\n"); return 1; }

    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_lis, NULL);
    wl_display_roundtrip(dpy);

    if (!g_shm || !g_comp || !g_wm) {
        fprintf(stderr, "Missing Wayland globals\n");
        return 1;
    }

    xdg_wm_base_add_listener(g_wm, &wm_lis, NULL);
    if (g_seat) wl_seat_add_listener(g_seat, &seat_lis, NULL);

    /* SHM buffer */
    int fd = memfd_create_shim("brook-edit", MFD_CLOEXEC);
    if (fd < 0) { perror("memfd_create"); return 1; }
    size_t buf_size = (size_t)(WIN_W * WIN_H * BPP);
    if (ftruncate(fd, (off_t)buf_size) != 0) { perror("ftruncate"); return 1; }
    void *map = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }
    g_px = (uint32_t *)map;

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)buf_size);
    g_buf = wl_shm_pool_create_buffer(pool, 0, WIN_W, WIN_H, STRIDE,
                                       WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    /* Surface + toplevel */
    g_surf = wl_compositor_create_surface(g_comp);
    struct xdg_surface *xsurf = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    xdg_surface_add_listener(xsurf, &xs_lis, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(toplevel, &tl_lis, NULL);

    /* Title: show filename */
    char title[256];
    snprintf(title, sizeof(title), "%s - Edit",
             g_filename[0] ? g_filename : "[new file]");
    xdg_toplevel_set_title(toplevel, title);
    xdg_toplevel_set_app_id(toplevel, "brook-edit");
    wl_surface_commit(g_surf);
    wl_display_roundtrip(dpy);

    while (!g_got_configure)
        wl_display_roundtrip(dpy);

    commit_frame();
    wl_display_flush(dpy);

    while (g_running && wl_display_dispatch(dpy) >= 0) {
        if (g_needs_redraw && !g_frame_cb)
            commit_frame();
    }

    /* Cleanup */
    if (g_kbd) wl_keyboard_destroy(g_kbd);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xsurf);
    wl_surface_destroy(g_surf);
    wl_buffer_destroy(g_buf);
    munmap(map, buf_size);
    wl_display_disconnect(dpy);

    /* Free text buffer */
    for (int i = 0; i < g_line_count; i++)
        free(g_lines[i].data);

    return 0;
}
