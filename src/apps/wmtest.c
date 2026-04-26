// wmtest.c — Smoke test for Phase A/B WM syscalls.
// Phase A: creates two windows from one process, paints them red/blue.
// Phase B: drains per-window input events and draws a tracker square at
//          the last mouse position inside each window.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define BROOK_WM_CREATE_WINDOW   506
#define BROOK_WM_DESTROY_WINDOW  507
#define BROOK_WM_SIGNAL_DIRTY    508
#define BROOK_WM_SET_TITLE       509
#define BROOK_WM_POP_INPUT       510

struct wm_create_out {
    uint32_t wm_id;
    uint32_t vfb_stride;
    uint64_t vfb_user;
};

struct wm_input_evt {
    uint8_t  type;     // 0=KeyPress 1=KeyRel 2=MouseMove 3=BtnDn 4=BtnUp 5=Scroll
    uint8_t  scan;
    uint8_t  ascii;
    uint8_t  mods;
    int16_t  x;
    int16_t  y;
    uint32_t reserved;
};

static long wm_create(uint16_t w, uint16_t h, const char* title,
                      struct wm_create_out* out) {
    return syscall(BROOK_WM_CREATE_WINDOW,
                   (long)w, (long)h, (long)title, (long)out);
}

static long wm_signal_dirty(uint32_t id) {
    return syscall(BROOK_WM_SIGNAL_DIRTY, (long)id);
}

static long wm_pop(uint32_t id, struct wm_input_evt* buf, long max) {
    return syscall(BROOK_WM_POP_INPUT, (long)id, (long)buf, max);
}

static void fill(uint32_t* fb, uint32_t stride, uint16_t w, uint16_t h,
                 uint32_t argb) {
    for (uint16_t y = 0; y < h; ++y)
        for (uint16_t x = 0; x < w; ++x)
            fb[y * stride + x] = argb;
}

static void box(uint32_t* fb, uint32_t stride, int cx, int cy, int half,
                int wmax, int hmax, uint32_t argb) {
    for (int dy = -half; dy <= half; ++dy) {
        int y = cy + dy;
        if (y < 0 || y >= hmax) continue;
        for (int dx = -half; dx <= half; ++dx) {
            int x = cx + dx;
            if (x < 0 || x >= wmax) continue;
            fb[y * stride + x] = argb;
        }
    }
}

struct win {
    struct wm_create_out info;
    uint32_t bg;
    uint32_t cursor;
    uint16_t w, h;
    int16_t  mx, my;
    int      keys;
};

static void redraw(struct win* W) {
    uint32_t* fb = (uint32_t*)(uintptr_t)W->info.vfb_user;
    fill(fb, W->info.vfb_stride, W->w, W->h, W->bg);
    // Top-row stripe so we can see each window owns its pixels.
    for (int x = 0; x < W->w; ++x)
        fb[10 * W->info.vfb_stride + x] = 0xFFFFFFFF;
    // Tracker box at last mouse pos.
    if (W->mx >= 0)
        box(fb, W->info.vfb_stride, W->mx, W->my, 6, W->w, W->h, W->cursor);
    // Key counter: draw N small dots top-left.
    for (int i = 0; i < W->keys && i < 32; ++i)
        box(fb, W->info.vfb_stride, 20 + i * 6, 20, 2, W->w, W->h, 0xFFFFFFFF);
    wm_signal_dirty(W->info.wm_id);
}

int main(void) {
    struct win A = { .w = 320, .h = 220, .bg = 0xFFCC3030,
                     .cursor = 0xFFFFFFFF, .mx = -1, .my = -1, .keys = 0 };
    struct win B = { .w = 380, .h = 180, .bg = 0xFF3060CC,
                     .cursor = 0xFFFFFF00, .mx = -1, .my = -1, .keys = 0 };

    if (wm_create(A.w, A.h, "wmtest red",  &A.info) != 0) return 1;
    if (wm_create(B.w, B.h, "wmtest blue", &B.info) != 0) return 1;
    printf("wmtest: A id=%u  B id=%u\n", A.info.wm_id, B.info.wm_id);

    redraw(&A);
    redraw(&B);

    struct wm_input_evt buf[16];
    for (int frame = 0; frame < 2400; ++frame) {  // ~120s @ 50ms
        int dirty_a = 0, dirty_b = 0;
        long n = wm_pop(A.info.wm_id, buf, 16);
        for (long i = 0; i < n; ++i) {
            if (buf[i].type == 2 || buf[i].type == 3 || buf[i].type == 4) {
                A.mx = buf[i].x; A.my = buf[i].y;
            } else if (buf[i].type == 0) {
                A.keys++;
            }
            dirty_a = 1;
        }
        n = wm_pop(B.info.wm_id, buf, 16);
        for (long i = 0; i < n; ++i) {
            if (buf[i].type == 2 || buf[i].type == 3 || buf[i].type == 4) {
                B.mx = buf[i].x; B.my = buf[i].y;
            } else if (buf[i].type == 0) {
                B.keys++;
            }
            dirty_b = 1;
        }
        if (dirty_a) redraw(&A);
        if (dirty_b) redraw(&B);
        usleep(50 * 1000);
    }
    return 0;
}
