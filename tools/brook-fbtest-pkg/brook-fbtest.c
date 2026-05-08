/*
 * brook-fbtest.c — Minimal framebuffer stress test for Brook OS.
 *
 * Renders a sweeping vertical bar at a target FPS to a Wayland shm
 * surface, with NO video decoding.  Used to isolate display pipeline
 * stuttering from decode overhead.
 *
 * Usage: brook-fbtest [fps]       (default: 24)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "xdg-shell-client-protocol.h"

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001
#endif

static struct wl_display    *g_dpy   = NULL;
static struct wl_registry   *g_reg   = NULL;
static struct wl_shm        *g_shm   = NULL;
static struct wl_compositor *g_comp  = NULL;
static struct xdg_wm_base   *g_wm   = NULL;
static struct wl_surface    *g_surf  = NULL;
static struct xdg_surface   *g_xsurf = NULL;
static struct xdg_toplevel  *g_tl    = NULL;

static int g_configured = 0;
static int g_close_req  = 0;
static int g_width      = 800;
static int g_height     = 600;

typedef struct {
    struct wl_buffer *wl_buf;
    uint8_t          *pixels;
    int               fd;
    size_t            size;
    int               busy;
} ShmBuffer;

static ShmBuffer g_bufs[2];
static int       g_back = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int memfd_create_shim(const char *name, unsigned int flags)
{
    return (int)syscall(319, name, flags);
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------------ */
/* SHM buffer management                                               */
/* ------------------------------------------------------------------ */

static void on_buf_release(void *data, struct wl_buffer *wl_buf)
{
    ShmBuffer *b = data;
    (void)wl_buf;
    b->busy = 0;
}
static const struct wl_buffer_listener buf_lis = { .release = on_buf_release };

static int shm_buf_init(ShmBuffer *b, int w, int h)
{
    int stride = w * 4;
    size_t sz  = (size_t)stride * h;

    int fd = memfd_create_shim("brook-fbtest", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, sz) < 0) { close(fd); return -1; }

    uint8_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { close(fd); return -1; }

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, sz);
    b->wl_buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                           WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    wl_buffer_add_listener(b->wl_buf, &buf_lis, b);

    b->pixels = px;
    b->fd     = fd;
    b->size   = sz;
    b->busy   = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wayland listeners                                                   */
/* ------------------------------------------------------------------ */

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
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
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_lis = {
    .global = on_global, .global_remove = on_global_remove,
};

static void on_wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{ (void)data; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_lis = { .ping = on_wm_ping };

static void on_xdg_surface_configure(void *data, struct xdg_surface *xs,
                                        uint32_t serial)
{ (void)data; xdg_surface_ack_configure(xs, serial); g_configured = 1; }
static const struct xdg_surface_listener xs_lis = {
    .configure = on_xdg_surface_configure,
};

static void on_toplevel_configure(void *data, struct xdg_toplevel *t,
                                     int32_t w, int32_t h,
                                     struct wl_array *states)
{
    (void)data; (void)t; (void)states;
    if (w > 0 && h > 0) { g_width = w; g_height = h; }
}
static void on_toplevel_close(void *data, struct xdg_toplevel *t)
{ (void)data; (void)t; g_close_req = 1; }
static void on_toplevel_configure_bounds(void *data, struct xdg_toplevel *t,
                                            int32_t w, int32_t h)
{ (void)data; (void)t; (void)w; (void)h; }
static void on_toplevel_wm_capabilities(void *data, struct xdg_toplevel *t,
                                           struct wl_array *caps)
{ (void)data; (void)t; (void)caps; }
static const struct xdg_toplevel_listener tl_lis = {
    .configure        = on_toplevel_configure,
    .close            = on_toplevel_close,
    .configure_bounds = on_toplevel_configure_bounds,
    .wm_capabilities  = on_toplevel_wm_capabilities,
};

/* ------------------------------------------------------------------ */
/* Render + display                                                    */
/* ------------------------------------------------------------------ */

static void render_frame(int frame_num)
{
    ShmBuffer *b = &g_bufs[g_back];

    /* Wait for buffer release */
    while (b->busy)
        wl_display_dispatch(g_dpy);

    uint32_t *px = (uint32_t *)b->pixels;
    int stride_px = g_width;

    /* Clear to dark grey */
    uint32_t bg = 0xFF202020;
    for (int i = 0; i < g_width * g_height; i++)
        px[i] = bg;

    /* Sweeping vertical bar (green, 8px wide) */
    int bar_x = (frame_num * 4) % g_width;
    int bar_w = 8;
    uint32_t bar_color = 0xFF00FF00;
    for (int y = 0; y < g_height; y++)
        for (int bx = bar_x; bx < bar_x + bar_w && bx < g_width; bx++)
            px[y * stride_px + bx] = bar_color;

    /* Frame counter text (just render frame number as colored blocks) */
    int digit = frame_num % 1000;
    for (int d = 0; d < 3; d++) {
        int val = digit % 10;
        digit /= 10;
        uint32_t color = 0xFF000000 | ((val * 25) << 8) | (val * 25);
        int x0 = 20 + (2 - d) * 20;
        for (int y = 10; y < 30; y++)
            for (int x = x0; x < x0 + 16 && x < g_width; x++)
                px[y * stride_px + x] = color;
    }

    wl_surface_attach(g_surf, b->wl_buf, 0, 0);
    wl_surface_damage(g_surf, 0, 0, g_width, g_height);
    wl_surface_commit(g_surf);
    b->busy = 1;
    g_back = 1 - g_back;
    wl_display_flush(g_dpy);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int target_fps = 24;
    if (argc > 1) target_fps = atoi(argv[1]);
    if (target_fps < 1) target_fps = 1;
    if (target_fps > 120) target_fps = 120;

    int64_t frame_interval_us = 1000000LL / target_fps;

    fprintf(stderr, "[brook-fbtest] starting: %dx%d @ %d fps "
            "(interval=%lld us)\n",
            g_width, g_height, target_fps,
            (long long)frame_interval_us);

    /* Wayland init */
    if (!getenv("WAYLAND_DISPLAY")) setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp", 1);

    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) {
        fprintf(stderr, "[brook-fbtest] wl_display_connect failed\n");
        return 1;
    }

    g_reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(g_reg, &reg_lis, NULL);
    wl_display_roundtrip(g_dpy);

    if (!g_shm || !g_comp || !g_wm) {
        fprintf(stderr, "[brook-fbtest] missing globals\n");
        return 1;
    }

    xdg_wm_base_add_listener(g_wm, &wm_lis, NULL);
    g_surf  = wl_compositor_create_surface(g_comp);
    g_xsurf = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    g_tl    = xdg_surface_get_toplevel(g_xsurf);

    xdg_surface_add_listener(g_xsurf, &xs_lis, NULL);
    xdg_toplevel_add_listener(g_tl, &tl_lis, NULL);
    xdg_toplevel_set_title(g_tl, "FB Test");
    xdg_toplevel_set_app_id(g_tl, "brook-fbtest");

    wl_surface_commit(g_surf);
    wl_display_roundtrip(g_dpy);

    /* Wait for configure */
    for (int i = 0; i < 50 && !g_configured; i++) {
        wl_display_dispatch(g_dpy);
        /* busy-wait 20ms (Brook doesn't support nanosleep) */
        int64_t t0 = now_us();
        while (now_us() - t0 < 20000) ;
    }
    if (!g_configured) {
        fprintf(stderr, "[brook-fbtest] timeout waiting for configure\n");
        return 1;
    }

    /* Create double buffers */
    if (shm_buf_init(&g_bufs[0], g_width, g_height) < 0 ||
        shm_buf_init(&g_bufs[1], g_width, g_height) < 0) {
        fprintf(stderr, "[brook-fbtest] shm buffer alloc failed\n");
        return 1;
    }

    fprintf(stderr, "[brook-fbtest] rendering %dx%d @ %d fps\n",
            g_width, g_height, target_fps);

    /* Main render loop — PTS-based pacing via clock_gettime busy-wait */
    int64_t start_time = now_us();
    int frame = 0;

    while (!g_close_req) {
        int64_t target_time = start_time + (int64_t)frame * frame_interval_us;
        int64_t now;

        /* Busy-wait until target time */
        do {
            now = now_us();
        } while (now < target_time);

        render_frame(frame);
        frame++;

        /* Periodic stats */
        if (frame % 100 == 0) {
            int64_t elapsed = now_us() - start_time;
            fprintf(stderr, "[brook-fbtest] frame %d, %.1f fps\n",
                    frame, frame * 1e6 / (double)elapsed);
        }

        /* Process any pending Wayland events */
        wl_display_dispatch_pending(g_dpy);
    }

    fprintf(stderr, "[brook-fbtest] done: %d frames\n", frame);
    return 0;
}
