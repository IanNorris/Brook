/*
 * brook-files.c — Brook-native Wayland file browser.
 *
 * Two-pane layout: tree view on the left, file/directory list on the
 * right, with a status bar showing file size and modified date.
 *
 * Pure C, depends only on libwayland-client + xdg-shell-protocol.
 * No font/cairo/pango — uses a built-in 6x10 bitmap font.
 *
 * Navigation: arrow keys (up/down to select, Enter to open dir / go
 * into tree node, Backspace to go up), pointer click to select.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001
#endif

static int memfd_create_shim(const char *name, unsigned int flags) {
    return (int)syscall(319, name, flags);
}

/* ========================= Configuration ========================= */

#define WIN_W  1024
#define WIN_H  700
#define BPP    4
#define STRIDE (WIN_W * BPP)

/* Layout */
#define TOOLBAR_H    30    /* toolbar/address bar height */
#define TREE_W       280   /* left pane width */
#define DIVIDER_W    2
#define LIST_X       (TREE_W + DIVIDER_W)
#define LIST_W       (WIN_W - LIST_X)
#define STATUS_H     28    /* status bar height */
#define HEADER_H     30    /* column header height */
#define ROW_H        24    /* row height */
#define PANES_TOP    TOOLBAR_H
#define CONTENT_H    (WIN_H - STATUS_H)

/* Colours (ARGB8888) */
#define COL_BG          0xFF1E1E2E   /* dark background */
#define COL_TREE_BG     0xFF181825   /* tree pane bg */
#define COL_TOOLBAR_BG  0xFF11111B   /* toolbar bg */
#define COL_TOOLBAR_BTN 0xFF313244   /* toolbar button bg */
#define COL_HEADER_BG   0xFF313244   /* column header */
#define COL_STATUS_BG   0xFF313244   /* status bar */
#define COL_DIVIDER     0xFF45475A   /* pane divider */
#define COL_TEXT        0xFFCDD6F4   /* main text */
#define COL_TEXT_DIM    0xFF6C7086   /* dimmed text */
#define COL_SELECT      0xFF45475A   /* selected row bg */
#define COL_DIR_TEXT    0xFF89B4FA   /* directory name colour */
#define COL_SYMLINK     0xFFF5C2E7   /* symlink colour */
#define COL_TREE_ARROW  0xFF89B4FA   /* tree expand arrow */

/* Limits */
#define MAX_ENTRIES  512
#define MAX_PATH_LEN 512
#define MAX_TREE     128
#define MAX_ASSOC    64
#define MAX_CRUMBS   32

/* Breadcrumb segment for clickable path navigation */
typedef struct {
    int x0, x1;         /* pixel extents in toolbar */
    int path_end;       /* index into g_cwd where this segment's path ends */
} Breadcrumb;

static Breadcrumb g_crumbs[MAX_CRUMBS];
static int        g_crumb_count = 0;

/* ========================= File type associations ========================= */

typedef struct {
    char ext[16];       /* file extension (lowercase, no dot) */
    char binary[256];   /* path to binary */
} FileAssoc;

static FileAssoc g_assoc[MAX_ASSOC];
static int       g_assoc_count = 0;

static void load_filetypes(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[files] no filetypes config at %s\n", path);
        return;
    }
    char line[300];
    while (fgets(line, sizeof(line), f) && g_assoc_count < MAX_ASSOC) {
        /* Strip newline */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\0') continue;
        /* Parse "ext=binary" */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        FileAssoc *a = &g_assoc[g_assoc_count];
        strncpy(a->ext, line, sizeof(a->ext) - 1);
        strncpy(a->binary, eq + 1, sizeof(a->binary) - 1);
        g_assoc_count++;
    }
    fclose(f);
    fprintf(stderr, "[files] loaded %d file associations\n", g_assoc_count);
}

static const char *find_assoc(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return NULL;
    const char *ext = dot + 1;
    for (int i = 0; i < g_assoc_count; i++) {
        if (strcasecmp(g_assoc[i].ext, ext) == 0)
            return g_assoc[i].binary;
    }
    return NULL;
}

static void launch_file(const char *filepath) {
    const char *binary = find_assoc(filepath);
    if (!binary) {
        fprintf(stderr, "[files] no association for: %s\n", filepath);
        return;
    }
    fprintf(stderr, "[files] launching: %s %s\n", binary, filepath);
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: exec the associated app */
        execl(binary, binary, filepath, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        /* Parent: don't wait — let the child run independently */
        fprintf(stderr, "[files] spawned pid %d\n", (int)pid);
    } else {
        fprintf(stderr, "[files] fork failed: %s\n", strerror(errno));
    }
}

/* ========================= Bitmap Font (6x10) ========================= */

/* Minimal 6x10 bitmap font covering ASCII 32-126.
 * Each glyph is 10 bytes (one byte per row, MSB-first, 6 bits used). */
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
#define FONT_SCALE 2
#define CHAR_W (FONT_W * FONT_SCALE)
#define CHAR_H (FONT_H * FONT_SCALE)

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
            if (bits & (0x80 >> col)) {
                for (int sy = 0; sy < FONT_SCALE; sy++)
                    for (int sx = 0; sx < FONT_SCALE; sx++)
                        put_px(cx + col * FONT_SCALE + sx,
                               cy + row * FONT_SCALE + sy, colour);
            }
        }
    }
}

static void draw_text(int x, int y, const char *s, uint32_t colour) {
    while (*s) {
        draw_char(x, y, *s, colour);
        x += CHAR_W;
        s++;
    }
}

/* Draw text clipped to a maximum pixel width. */
static void draw_text_clipped(int x, int y, const char *s, uint32_t colour,
                               int max_w) {
    int max_chars = max_w / CHAR_W;
    int len = (int)strlen(s);
    if (len <= max_chars) {
        draw_text(x, y, s, colour);
    } else if (max_chars > 3) {
        for (int i = 0; i < max_chars - 3; i++) {
            draw_char(x + i * CHAR_W, y, s[i], colour);
        }
        draw_text(x + (max_chars - 3) * CHAR_W, y, "...", colour);
    }
}

/* ========================= File entry types ========================= */

typedef struct {
    char   name[256];
    int    is_dir;
    int    is_symlink;
    off_t  size;
    time_t mtime;
} FileEntry;

/* Tree node for left pane. */
typedef struct {
    char   path[MAX_PATH_LEN];
    char   name[256];
    int    depth;
    int    expanded;
    int    has_children;
} TreeNode;

/* ========================= Application state ========================= */

static char          g_cwd[MAX_PATH_LEN] = "/";
static FileEntry     g_entries[MAX_ENTRIES];
static int           g_entry_count = 0;
static int           g_selected = 0;
static int           g_scroll_offset = 0;

static TreeNode      g_tree[MAX_TREE];
static int           g_tree_count = 0;
static int           g_tree_selected = 0;

static int           g_active_pane = 1;  /* 0=tree, 1=list */
static int           g_running = 1;
static int           g_needs_redraw = 1;

/* Sort mode: 0=name, 1=size, 2=mtime; negative = descending */
static int           g_sort_mode = 0;

/* ========================= Directory loading ========================= */

static int entry_cmp(const void *a, const void *b) {
    const FileEntry *ea = (const FileEntry *)a;
    const FileEntry *eb = (const FileEntry *)b;
    /* Directories first always */
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;

    int mode = g_sort_mode < 0 ? -g_sort_mode : g_sort_mode;
    int dir  = g_sort_mode < 0 ? -1 : 1;
    int cmp = 0;
    switch (mode) {
    case 1: /* size */
        cmp = (ea->size > eb->size) - (ea->size < eb->size);
        break;
    case 2: /* mtime */
        cmp = (ea->mtime > eb->mtime) - (ea->mtime < eb->mtime);
        break;
    default: /* name */
        cmp = strcasecmp(ea->name, eb->name);
        break;
    }
    return cmp * dir;
}

static void resort_entries(void) {
    qsort(g_entries, (size_t)g_entry_count, sizeof(FileEntry), entry_cmp);
    g_needs_redraw = 1;
}

static void load_directory(const char *path) {
    strncpy(g_cwd, path, MAX_PATH_LEN - 1);
    g_cwd[MAX_PATH_LEN - 1] = '\0';
    g_entry_count = 0;
    g_selected = 0;
    g_scroll_offset = 0;

    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "[files] opendir(%s) failed: %s\n", path, strerror(errno));
        return;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && g_entry_count < MAX_ENTRIES) {
        if (de->d_name[0] == '.' && de->d_name[1] == '\0') continue;
        if (de->d_name[0] == '.' && de->d_name[1] == '.' && de->d_name[2] == '\0') continue;

        FileEntry *e = &g_entries[g_entry_count];
        strncpy(e->name, de->d_name, 255);
        e->name[255] = '\0';

        char fullpath[MAX_PATH_LEN];
        if (strlen(path) + strlen(de->d_name) + 2 < MAX_PATH_LEN) {
            snprintf(fullpath, MAX_PATH_LEN, "%s/%s", path, de->d_name);
        } else {
            fullpath[0] = '\0';
        }

        struct stat st;
        if (lstat(fullpath, &st) == 0) {
            e->is_dir = S_ISDIR(st.st_mode);
            e->is_symlink = S_ISLNK(st.st_mode);
            e->size = st.st_size;
            e->mtime = st.st_mtime;
        } else {
            e->is_dir = (de->d_type == DT_DIR);
            e->is_symlink = (de->d_type == DT_LNK);
            e->size = 0;
            e->mtime = 0;
        }
        g_entry_count++;
    }
    closedir(dir);

    qsort(g_entries, (size_t)g_entry_count, sizeof(FileEntry), entry_cmp);
    g_needs_redraw = 1;
}

/* Navigate into a directory or up (..) */
static void navigate_to(const char *path) {
    load_directory(path);
    fprintf(stderr, "[files] navigated to: %s (%d entries)\n", g_cwd, g_entry_count);
}

static void navigate_up(void) {
    char parent[MAX_PATH_LEN];
    strncpy(parent, g_cwd, MAX_PATH_LEN);
    char *last_slash = strrchr(parent, '/');
    if (last_slash && last_slash != parent) {
        *last_slash = '\0';
    } else {
        strcpy(parent, "/");
    }
    navigate_to(parent);
}

static void open_selected(void) {
    if (g_selected < 0 || g_selected >= g_entry_count) return;
    FileEntry *e = &g_entries[g_selected];

    char fullpath[MAX_PATH_LEN];
    if (strcmp(g_cwd, "/") == 0)
        snprintf(fullpath, MAX_PATH_LEN, "/%s", e->name);
    else
        snprintf(fullpath, MAX_PATH_LEN, "%s/%s", g_cwd, e->name);

    if (e->is_dir) {
        navigate_to(fullpath);
    } else {
        launch_file(fullpath);
    }
}

/* ========================= Tree management ========================= */

static int dir_has_subdirs(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (de->d_type == DT_DIR) { closedir(d); return 1; }
        /* Fallback: stat if d_type isn't available */
        if (de->d_type == DT_UNKNOWN) {
            char fp[MAX_PATH_LEN];
            snprintf(fp, MAX_PATH_LEN, "%s/%s", path, de->d_name);
            struct stat st;
            if (stat(fp, &st) == 0 && S_ISDIR(st.st_mode)) {
                closedir(d); return 1;
            }
        }
    }
    closedir(d);
    return 0;
}

static void tree_init(void) {
    g_tree_count = 0;
    TreeNode *root = &g_tree[g_tree_count++];
    strcpy(root->path, "/");
    strcpy(root->name, "/");
    root->depth = 0;
    root->expanded = 1;
    root->has_children = 1;

    /* Expand root: add top-level dirs */
    DIR *d = opendir("/");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_tree_count < MAX_TREE) {
        if (de->d_name[0] == '.') continue;
        char fp[MAX_PATH_LEN];
        snprintf(fp, MAX_PATH_LEN, "/%s", de->d_name);
        struct stat st;
        if (stat(fp, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        TreeNode *n = &g_tree[g_tree_count++];
        strncpy(n->path, fp, MAX_PATH_LEN - 1);
        strncpy(n->name, de->d_name, 255);
        n->depth = 1;
        n->expanded = 0;
        n->has_children = dir_has_subdirs(fp);
    }
    closedir(d);
}

static void tree_toggle(int idx) {
    if (idx < 0 || idx >= g_tree_count) return;
    TreeNode *node = &g_tree[idx];
    if (!node->has_children) return;

    if (node->expanded) {
        /* Collapse: remove children (all nodes deeper than this one) */
        int depth = node->depth;
        int remove_start = idx + 1;
        int remove_end = remove_start;
        while (remove_end < g_tree_count && g_tree[remove_end].depth > depth)
            remove_end++;
        int remove_count = remove_end - remove_start;
        if (remove_count > 0) {
            memmove(&g_tree[remove_start], &g_tree[remove_end],
                    (size_t)(g_tree_count - remove_end) * sizeof(TreeNode));
            g_tree_count -= remove_count;
        }
        node->expanded = 0;
    } else {
        /* Expand: insert children after this node */
        DIR *d = opendir(node->path);
        if (!d) return;
        struct dirent *de;
        int insert_pos = idx + 1;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (g_tree_count >= MAX_TREE) break;

            char fp[MAX_PATH_LEN];
            snprintf(fp, MAX_PATH_LEN, "%s/%s", node->path, de->d_name);
            struct stat st;
            if (stat(fp, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

            /* Shift existing entries down */
            if (insert_pos < g_tree_count) {
                memmove(&g_tree[insert_pos + 1], &g_tree[insert_pos],
                        (size_t)(g_tree_count - insert_pos) * sizeof(TreeNode));
            }
            TreeNode *n = &g_tree[insert_pos];
            strncpy(n->path, fp, MAX_PATH_LEN - 1);
            strncpy(n->name, de->d_name, 255);
            n->depth = node->depth + 1;
            n->expanded = 0;
            n->has_children = dir_has_subdirs(fp);
            g_tree_count++;
            insert_pos++;
        }
        closedir(d);
        node->expanded = 1;
    }
    g_needs_redraw = 1;
}

static void tree_select(int idx) {
    if (idx < 0 || idx >= g_tree_count) return;
    g_tree_selected = idx;
    navigate_to(g_tree[idx].path);
}

/* ========================= Formatting helpers ========================= */

static void format_size(off_t bytes, char *out, int out_len) {
    if (bytes < 1024)
        snprintf(out, out_len, "%ld B", (long)bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, out_len, "%ld KB", (long)(bytes / 1024));
    else if (bytes < (off_t)1024 * 1024 * 1024)
        snprintf(out, out_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(out, out_len, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

static void format_time(time_t t, char *out, int out_len) {
    if (t == 0) {
        snprintf(out, out_len, "---");
        return;
    }
    struct tm *tm = localtime(&t);
    if (tm)
        snprintf(out, out_len, "%04d-%02d-%02d %02d:%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min);
    else
        snprintf(out, out_len, "%ld", (long)t);
}

/* ========================= Rendering ========================= */

static void render(void) {
    /* Clear background */
    fill_rect(0, 0, WIN_W, WIN_H, COL_BG);

    /* --- Toolbar / Address bar --- */
    fill_rect(0, 0, WIN_W, TOOLBAR_H, COL_TOOLBAR_BG);
    int tb_vpad = (TOOLBAR_H - CHAR_H) / 2;

    /* Back button */
    fill_rect(4, 4, CHAR_W * 2 + 8, TOOLBAR_H - 8, COL_TOOLBAR_BTN);
    draw_text(8, tb_vpad, "<-", COL_TEXT);

    /* Home button */
    fill_rect(CHAR_W * 2 + 16, 4, CHAR_W * 2 + 8, TOOLBAR_H - 8, COL_TOOLBAR_BTN);
    draw_char(CHAR_W * 2 + 20, tb_vpad, '~', COL_TEXT);

    /* Refresh button */
    fill_rect(CHAR_W * 4 + 28, 4, CHAR_W + 8, TOOLBAR_H - 8, COL_TOOLBAR_BTN);
    draw_char(CHAR_W * 4 + 32, tb_vpad, 'R', COL_TEXT);

    /* Breadcrumb path display */
    int crumb_x = CHAR_W * 5 + 44;
    int crumb_max_x = WIN_W - 8;
    g_crumb_count = 0;

    /* Always show root "/" as first crumb */
    if (g_crumb_count < MAX_CRUMBS) {
        int sx = crumb_x;
        draw_text(crumb_x, tb_vpad, "/", COL_DIR_TEXT);
        crumb_x += CHAR_W + 2;
        g_crumbs[g_crumb_count].x0 = sx;
        g_crumbs[g_crumb_count].x1 = crumb_x;
        g_crumbs[g_crumb_count].path_end = 0; /* "/" */
        g_crumb_count++;
    }

    /* Parse remaining path segments */
    if (g_cwd[0] == '/' && g_cwd[1] != '\0') {
        const char *p = g_cwd + 1;
        while (*p && crumb_x < crumb_max_x && g_crumb_count < MAX_CRUMBS) {
            const char *seg = p;
            while (*p && *p != '/') p++;
            int seg_len = (int)(p - seg);

            /* Draw separator */
            draw_text(crumb_x, tb_vpad, ">", COL_TEXT_DIM);
            crumb_x += CHAR_W + 2;

            int sx = crumb_x;
            /* Draw segment text, clipped */
            for (int i = 0; i < seg_len && crumb_x < crumb_max_x; i++) {
                draw_char(crumb_x, tb_vpad, seg[i], COL_DIR_TEXT);
                crumb_x += CHAR_W;
            }
            crumb_x += 4; /* padding after segment */

            g_crumbs[g_crumb_count].x0 = sx;
            g_crumbs[g_crumb_count].x1 = crumb_x;
            g_crumbs[g_crumb_count].path_end = (int)(p - g_cwd);
            g_crumb_count++;

            if (*p == '/') p++;
        }
    }

    /* Pane origins shifted down by toolbar */
    int pane_top = PANES_TOP;

    /* Tree pane background */
    fill_rect(0, pane_top, TREE_W, CONTENT_H - pane_top, COL_TREE_BG);

    /* Divider */
    fill_rect(TREE_W, pane_top, DIVIDER_W, WIN_H - pane_top, COL_DIVIDER);

    /* Text vertical centering within ROW_H */
    int text_vpad = (ROW_H - CHAR_H) / 2;
    int hdr_vpad  = (HEADER_H - CHAR_H) / 2;

    /* --- Tree pane --- */
    int tree_area_h = CONTENT_H - pane_top - HEADER_H;
    int tree_visible = tree_area_h / ROW_H;
    /* Tree header */
    fill_rect(0, pane_top, TREE_W, HEADER_H, COL_HEADER_BG);
    draw_text(8, pane_top + hdr_vpad, "Folders", COL_TEXT);

    for (int i = 0; i < g_tree_count && i < tree_visible; i++) {
        TreeNode *n = &g_tree[i];
        int y = pane_top + HEADER_H + i * ROW_H;

        /* Selection highlight */
        if (i == g_tree_selected && g_active_pane == 0)
            fill_rect(0, y, TREE_W, ROW_H, COL_SELECT);

        int indent = n->depth * CHAR_W * 2 + 8;

        /* Expand/collapse arrow */
        if (n->has_children) {
            char arrow = n->expanded ? 'v' : '>';
            draw_char(indent, y + text_vpad, arrow, COL_TREE_ARROW);
        }

        /* Folder icon (just a prefix char) */
        draw_text_clipped(indent + CHAR_W + 4, y + text_vpad, n->name, COL_DIR_TEXT,
                          TREE_W - indent - CHAR_W - 8);
    }

    /* --- File list pane --- */
    /* Column header */
    fill_rect(LIST_X, pane_top, LIST_W, HEADER_H, COL_HEADER_BG);
    /* Column headers with sort indicator */
    int abs_mode = g_sort_mode < 0 ? -g_sort_mode : g_sort_mode;
    const char *arrow = g_sort_mode >= 0 ? " v" : " ^";
    char name_hdr[16], size_hdr[16], mod_hdr[16];
    snprintf(name_hdr, sizeof(name_hdr), "Name%s", abs_mode == 0 ? arrow : "");
    snprintf(size_hdr, sizeof(size_hdr), "Size%s", abs_mode == 1 ? arrow : "");
    snprintf(mod_hdr, sizeof(mod_hdr), "Modified%s", abs_mode == 2 ? arrow : "");
    draw_text(LIST_X + 8, pane_top + hdr_vpad, name_hdr, COL_TEXT);
    draw_text(LIST_X + LIST_W - 200, pane_top + hdr_vpad, size_hdr, COL_TEXT);
    draw_text(LIST_X + LIST_W - 120, pane_top + hdr_vpad, mod_hdr, COL_TEXT);

    int list_area_h = CONTENT_H - pane_top - HEADER_H;
    int list_visible = list_area_h / ROW_H;

    /* Ensure selected is visible */
    if (g_selected < g_scroll_offset) g_scroll_offset = g_selected;
    if (g_selected >= g_scroll_offset + list_visible)
        g_scroll_offset = g_selected - list_visible + 1;
    if (g_scroll_offset < 0) g_scroll_offset = 0;

    for (int vi = 0; vi < list_visible; vi++) {
        int idx = g_scroll_offset + vi;
        if (idx >= g_entry_count) break;

        FileEntry *e = &g_entries[idx];
        int y = pane_top + HEADER_H + vi * ROW_H;

        /* Selection highlight */
        if (idx == g_selected && g_active_pane == 1)
            fill_rect(LIST_X, y, LIST_W, ROW_H, COL_SELECT);

        /* Name with type-based colour */
        uint32_t name_col = COL_TEXT;
        if (e->is_dir) name_col = COL_DIR_TEXT;
        else if (e->is_symlink) name_col = COL_SYMLINK;

        char display_name[270];
        if (e->is_dir)
            snprintf(display_name, sizeof(display_name), "[%s]", e->name);
        else
            snprintf(display_name, sizeof(display_name), "%s", e->name);

        draw_text_clipped(LIST_X + 8, y + text_vpad, display_name, name_col,
                          LIST_W - 210);

        /* Size */
        if (!e->is_dir) {
            char size_str[32];
            format_size(e->size, size_str, sizeof(size_str));
            draw_text(LIST_X + LIST_W - 200, y + text_vpad, size_str, COL_TEXT_DIM);
        }

        /* Modified date */
        char time_str[32];
        format_time(e->mtime, time_str, sizeof(time_str));
        draw_text(LIST_X + LIST_W - 120, y + text_vpad, time_str, COL_TEXT_DIM);
    }

    /* --- Status bar --- */
    fill_rect(0, WIN_H - STATUS_H, WIN_W, STATUS_H, COL_STATUS_BG);
    int status_vpad = (STATUS_H - CHAR_H) / 2;

    /* Keyboard shortcuts */
    draw_text(8, WIN_H - STATUS_H + status_vpad,
              "Enter:Open  Bksp:Up  Del:Delete  Tab:Switch pane",
              COL_TEXT_DIM);

    /* Entry count */
    char count_str[64];
    snprintf(count_str, sizeof(count_str), "%d items", g_entry_count);
    draw_text(WIN_W / 2 + 8, WIN_H - STATUS_H + status_vpad, count_str, COL_TEXT_DIM);

    /* Selected file info */
    if (g_selected >= 0 && g_selected < g_entry_count) {
        FileEntry *e = &g_entries[g_selected];
        if (!e->is_dir) {
            char info[96];
            char size_str[32];
            format_size(e->size, size_str, sizeof(size_str));
            const char *assoc = find_assoc(e->name);
            if (assoc) {
                const char *base = strrchr(assoc, '/');
                base = base ? base + 1 : assoc;
                snprintf(info, sizeof(info), "%s  [%s]", size_str, base);
            } else {
                snprintf(info, sizeof(info), "%s", size_str);
            }
            draw_text(WIN_W - 260, WIN_H - STATUS_H + status_vpad, info, COL_TEXT);
        }
    }
}

/* ========================= Wayland plumbing ========================= */

static struct wl_shm        *g_shm  = NULL;
static struct wl_compositor *g_comp = NULL;
static struct xdg_wm_base   *g_wm   = NULL;
static struct zxdg_decoration_manager_v1 *g_deco_mgr = NULL;
static struct wl_surface    *g_surf  = NULL;
static struct wl_buffer     *g_buf   = NULL;
static struct wl_seat       *g_seat  = NULL;
static struct wl_keyboard   *g_kbd   = NULL;
static struct wl_pointer    *g_ptr   = NULL;
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
    else if (!strcmp(iface, "zxdg_decoration_manager_v1"))
        g_deco_mgr = wl_registry_bind(reg, name,
                                       &zxdg_decoration_manager_v1_interface, 1);
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

/* --- Keyboard input --- */

static void on_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state) {
    (void)data; (void)kb; (void)serial; (void)time;
    if (state != 1) return; /* key press only */

    /* Linux evdev keycodes */
    enum {
        KEY_ESC = 1, KEY_ENTER = 28, KEY_BACKSPACE = 14,
        KEY_UP = 103, KEY_DOWN = 108, KEY_LEFT = 105, KEY_RIGHT = 106,
        KEY_TAB = 15, KEY_DELETE = 111,
    };

    switch (key) {
    case KEY_ESC:
        g_running = 0;
        break;
    case KEY_TAB:
        g_active_pane = 1 - g_active_pane;
        g_needs_redraw = 1;
        break;
    case KEY_UP:
        if (g_active_pane == 1) {
            if (g_selected > 0) { g_selected--; g_needs_redraw = 1; }
        } else {
            if (g_tree_selected > 0) {
                g_tree_selected--;
                tree_select(g_tree_selected);
            }
        }
        break;
    case KEY_DOWN:
        if (g_active_pane == 1) {
            if (g_selected < g_entry_count - 1) { g_selected++; g_needs_redraw = 1; }
        } else {
            if (g_tree_selected < g_tree_count - 1) {
                g_tree_selected++;
                tree_select(g_tree_selected);
            }
        }
        break;
    case KEY_ENTER:
        if (g_active_pane == 1) {
            open_selected();
        } else {
            tree_toggle(g_tree_selected);
        }
        break;
    case KEY_BACKSPACE:
        navigate_up();
        break;
    case KEY_DELETE:
        if (g_active_pane == 1 && g_selected >= 0 && g_selected < g_entry_count) {
            FileEntry *e = &g_entries[g_selected];
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", g_cwd, e->name);
            if (e->is_dir) {
                rmdir(full);
            } else {
                unlink(full);
            }
            load_directory(g_cwd);
            if (g_selected >= g_entry_count && g_entry_count > 0)
                g_selected = g_entry_count - 1;
            g_needs_redraw = 1;
        }
        break;
    case KEY_LEFT:
        if (g_active_pane == 0 && g_tree_selected < g_tree_count) {
            if (g_tree[g_tree_selected].expanded)
                tree_toggle(g_tree_selected);
        }
        break;
    case KEY_RIGHT:
        if (g_active_pane == 0 && g_tree_selected < g_tree_count) {
            if (!g_tree[g_tree_selected].expanded)
                tree_toggle(g_tree_selected);
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
    (void)d; (void)k; (void)s; (void)depressed;
    (void)latched; (void)locked; (void)group;
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

/* --- Pointer input --- */

static double g_ptr_x = 0, g_ptr_y = 0;

static void on_ptr_enter(void *d, struct wl_pointer *p, uint32_t s,
                           struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)s; (void)sf;
    g_ptr_x = wl_fixed_to_double(x);
    g_ptr_y = wl_fixed_to_double(y);
}
static void on_ptr_leave(void *d, struct wl_pointer *p, uint32_t s,
                           struct wl_surface *sf) {
    (void)d; (void)p; (void)s; (void)sf;
}
static void on_ptr_motion(void *d, struct wl_pointer *p, uint32_t t,
                            wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t;
    g_ptr_x = wl_fixed_to_double(x);
    g_ptr_y = wl_fixed_to_double(y);
}
static uint32_t g_last_click_time = 0;
static int      g_last_click_row = -1;
#define DOUBLE_CLICK_MS 400

static void on_ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                            uint32_t time, uint32_t button, uint32_t state) {
    (void)d; (void)p; (void)serial;
    if (state != 1 || button != 0x110 /* BTN_LEFT */) return;

    int mx = (int)g_ptr_x, my = (int)g_ptr_y;
    int pane_top = PANES_TOP;
    int content_top = pane_top + HEADER_H;

    /* Click in toolbar area */
    if (my < pane_top) {
        /* Back button: x=4..4+CHAR_W*2+8 */
        if (mx >= 4 && mx < 4 + CHAR_W * 2 + 8) {
            navigate_up();
            g_needs_redraw = 1;
        }
        /* Home button */
        else if (mx >= CHAR_W * 2 + 16 && mx < CHAR_W * 4 + 24) {
            strncpy(g_cwd, "/", sizeof(g_cwd));
            load_directory(g_cwd);
            g_needs_redraw = 1;
        }
        /* Refresh button */
        else if (mx >= CHAR_W * 4 + 28 && mx < CHAR_W * 5 + 36) {
            load_directory(g_cwd);
            g_needs_redraw = 1;
        }
        /* Breadcrumb click */
        else {
            for (int i = 0; i < g_crumb_count; i++) {
                if (mx >= g_crumbs[i].x0 && mx < g_crumbs[i].x1) {
                    if (g_crumbs[i].path_end == 0) {
                        navigate_to("/");
                    } else {
                        char target[MAX_PATH_LEN];
                        int len = g_crumbs[i].path_end;
                        if (len >= (int)sizeof(target)) len = (int)sizeof(target) - 1;
                        memcpy(target, g_cwd, (size_t)len);
                        target[len] = '\0';
                        navigate_to(target);
                    }
                    break;
                }
            }
        }
        g_last_click_time = time;
        return;
    }

    /* Click on list column headers — toggle sort mode */
    if (mx > LIST_X && my >= pane_top && my < content_top) {
        int new_mode;
        if (mx < LIST_X + LIST_W - 200) {
            new_mode = 0; /* Name */
        } else if (mx < LIST_X + LIST_W - 120) {
            new_mode = 1; /* Size */
        } else {
            new_mode = 2; /* Modified */
        }
        int abs_mode = g_sort_mode < 0 ? -g_sort_mode : g_sort_mode;
        if (abs_mode == new_mode) {
            g_sort_mode = -g_sort_mode; /* toggle direction */
        } else {
            g_sort_mode = new_mode;
        }
        resort_entries();
        g_last_click_time = time;
        return;
    }

    if (mx < TREE_W && my >= content_top && my < CONTENT_H) {
        /* Click in tree pane */
        int row = (my - content_top) / ROW_H;
        if (row >= 0 && row < g_tree_count) {
            g_active_pane = 0;
            if (row == g_tree_selected &&
                time - g_last_click_time < DOUBLE_CLICK_MS) {
                tree_toggle(row);
            } else {
                g_tree_selected = row;
                tree_select(row);
            }
        }
        g_last_click_time = time;
        g_last_click_row = -1;
    } else if (mx > LIST_X && my >= content_top && my < CONTENT_H) {
        /* Click in list pane */
        int row = (my - content_top) / ROW_H + g_scroll_offset;
        if (row >= 0 && row < g_entry_count) {
            g_active_pane = 1;
            if (row == g_last_click_row &&
                time - g_last_click_time < DOUBLE_CLICK_MS) {
                /* Double-click: open */
                g_selected = row;
                open_selected();
            } else {
                g_selected = row;
                g_needs_redraw = 1;
            }
            g_last_click_row = row;
            g_last_click_time = time;
        }
    }
}
static void on_ptr_axis(void *d, struct wl_pointer *p, uint32_t t,
                          uint32_t axis, wl_fixed_t value) {
    (void)d; (void)p; (void)t;
    if (axis != 0) return; /* only vertical scroll */
    int delta = wl_fixed_to_int(value);
    if (delta == 0) delta = (value > 0) ? 1 : -1;
    int scroll_lines = delta > 0 ? 3 : -3;

    int mx = (int)g_ptr_x;
    int pane_top = PANES_TOP;

    if (mx < TREE_W && g_tree_count > 0) {
        /* Scroll tree pane (move selection) */
        g_tree_selected += scroll_lines;
        if (g_tree_selected < 0) g_tree_selected = 0;
        if (g_tree_selected >= g_tree_count) g_tree_selected = g_tree_count - 1;
        g_active_pane = 0;
        tree_select(g_tree_selected);
    } else if (mx > LIST_X && g_entry_count > 0) {
        /* Scroll list pane */
        int list_area_h = CONTENT_H - pane_top - HEADER_H;
        int list_visible = list_area_h / ROW_H;
        g_scroll_offset += scroll_lines;
        if (g_scroll_offset < 0) g_scroll_offset = 0;
        int max_off = g_entry_count - list_visible;
        if (max_off < 0) max_off = 0;
        if (g_scroll_offset > max_off) g_scroll_offset = max_off;
        g_needs_redraw = 1;
    }
}
static void on_ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void on_ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) {
    (void)d; (void)p; (void)s;
}
static void on_ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {
    (void)d; (void)p; (void)t; (void)a;
}
static void on_ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {
    (void)d; (void)p; (void)a; (void)v;
}
static const struct wl_pointer_listener ptr_lis = {
    .enter  = on_ptr_enter,
    .leave  = on_ptr_leave,
    .motion = on_ptr_motion,
    .button = on_ptr_button,
    .axis   = on_ptr_axis,
    .frame  = on_ptr_frame,
    .axis_source   = on_ptr_axis_source,
    .axis_stop     = on_ptr_axis_stop,
    .axis_discrete = on_ptr_axis_discrete,
};

/* --- Seat capabilities --- */

static void on_seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_kbd) {
        g_kbd = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_kbd, &kb_lis, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_ptr) {
        g_ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_ptr, &ptr_lis, NULL);
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
    const char *start_dir = "/";
    if (argc > 1) start_dir = argv[1];

    struct wl_display *dpy = NULL;
    /* Retry connection — waylandd may not have bound the socket yet */
    for (int attempt = 0; attempt < 20; attempt++) {
        dpy = wl_display_connect(NULL);
        if (dpy) break;
        usleep(100000); /* 100ms */
    }
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

    /* Create SHM buffer */
    int fd = memfd_create_shim("brook-files", MFD_CLOEXEC);
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

    /* Create surface + xdg toplevel */
    g_surf = wl_compositor_create_surface(g_comp);
    struct xdg_surface *xsurf = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    xdg_surface_add_listener(xsurf, &xs_lis, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(toplevel, &tl_lis, NULL);
    xdg_toplevel_set_title(toplevel, "Files");
    xdg_toplevel_set_app_id(toplevel, "brook-files");

    /* Request server-side decoration so the kernel WM draws chrome. */
    if (g_deco_mgr) {
        struct zxdg_toplevel_decoration_v1 *deco =
            zxdg_decoration_manager_v1_get_toplevel_decoration(g_deco_mgr, toplevel);
        zxdg_toplevel_decoration_v1_set_mode(deco,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    wl_surface_commit(g_surf);
    wl_display_roundtrip(dpy);

    /* Initialise content */
    load_filetypes("/boot/FILETYPES.CFG");
    tree_init();
    load_directory(start_dir);

    /* Wait for configure */
    while (!g_got_configure)
        wl_display_roundtrip(dpy);

    /* Initial render + commit */
    commit_frame();
    wl_display_flush(dpy);

    /* Event loop */
    while (g_running && wl_display_dispatch(dpy) >= 0) {
        if (g_needs_redraw && !g_frame_cb)
            commit_frame();
    }

    /* Cleanup */
    if (g_kbd) wl_keyboard_destroy(g_kbd);
    if (g_ptr) wl_pointer_destroy(g_ptr);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xsurf);
    wl_surface_destroy(g_surf);
    wl_buffer_destroy(g_buf);
    munmap(map, buf_size);
    wl_display_disconnect(dpy);
    return 0;
}
