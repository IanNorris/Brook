// wmtest.c — Smoke test for Phase A WM syscalls (506-509).
// Creates two top-level windows from a single process, paints them
// red and blue respectively, and idles so the kernel WM can render
// them with chrome.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define BROOK_WM_CREATE_WINDOW   506
#define BROOK_WM_DESTROY_WINDOW  507
#define BROOK_WM_SIGNAL_DIRTY    508
#define BROOK_WM_SET_TITLE       509

struct wm_create_out {
    uint32_t wm_id;
    uint32_t vfb_stride;
    uint64_t vfb_user;
};

static long wm_create(uint16_t w, uint16_t h, const char* title,
                      struct wm_create_out* out) {
    return syscall(BROOK_WM_CREATE_WINDOW,
                   (long)w, (long)h, (long)title, (long)out);
}

static long wm_signal_dirty(uint32_t id) {
    return syscall(BROOK_WM_SIGNAL_DIRTY, (long)id);
}

static void fill(uint32_t* fb, uint32_t stride, uint16_t w, uint16_t h,
                 uint32_t argb) {
    for (uint16_t y = 0; y < h; ++y)
        for (uint16_t x = 0; x < w; ++x)
            fb[y * stride + x] = argb;
}

int main(void) {
    struct wm_create_out a = {}, b = {};

    if (wm_create(320, 220, "wmtest red", &a) != 0) {
        printf("wmtest: create A failed\n");
        return 1;
    }
    printf("wmtest: A id=%u vfb=0x%lx stride=%u\n",
           a.wm_id, a.vfb_user, a.vfb_stride);

    if (wm_create(380, 180, "wmtest blue", &b) != 0) {
        printf("wmtest: create B failed\n");
        return 1;
    }
    printf("wmtest: B id=%u vfb=0x%lx stride=%u\n",
           b.wm_id, b.vfb_user, b.vfb_stride);

    uint32_t* fa = (uint32_t*)(uintptr_t)a.vfb_user;
    uint32_t* fb = (uint32_t*)(uintptr_t)b.vfb_user;

    fill(fa, a.vfb_stride, 320, 220, 0xFFCC3030);  // red
    fill(fb, b.vfb_stride, 380, 180, 0xFF3060CC);  // blue

    // Draw a couple of internal stripes so we can confirm the
    // window owns its own pixels (not a shared screen blit).
    for (int i = 0; i < 320; ++i) fa[10 * a.vfb_stride + i] = 0xFFFFFFFF;
    for (int i = 0; i < 380; ++i) fb[20 * b.vfb_stride + i] = 0xFFFFFF00;

    wm_signal_dirty(a.wm_id);
    wm_signal_dirty(b.wm_id);

    printf("wmtest: painted both, idling 120s\n");
    fflush(stdout);

    for (int i = 0; i < 120; ++i) {
        sleep(1);
    }
    return 0;
}
