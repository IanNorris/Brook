/*
 * wayland-xdg-smoke.c — Brook xdg-shell client smoke.
 *
 * Connects, binds wl_compositor / wl_shm / xdg_wm_base, creates an
 * xdg_toplevel, waits for the configure event, acks, then commits a
 * 64x64 gradient. Verifies the configure/ack handshake plus first
 * frame against waylandd.
 *
 * Expected log:
 *   [xdg-smoke] connected
 *   [xdg-smoke] globals bound (shm + comp + wm_base)
 *   [xdg-smoke] xdg_toplevel created, awaiting configure
 *   [xdg-smoke] configure received serial=...
 *   [xdg-smoke] ack_configure sent
 *   [xdg-smoke] committed frame
 *   [xdg-smoke] PASS
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

static struct wl_shm        *g_shm  = NULL;
static struct wl_compositor *g_comp = NULL;
static struct xdg_wm_base   *g_wm   = NULL;
static int g_acked = 0;
static int g_got_configure = 0;
static uint32_t g_configure_serial = 0;
static int g_got_release = 0;

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
    g_acked = 1;
    fprintf(stderr, "[xdg-smoke] configure received serial=%u, acked\n", serial);
}
static const struct xdg_surface_listener xs_lis = {
    .configure = on_xdg_surface_configure,
};

static void on_toplevel_configure(void *data, struct xdg_toplevel *t,
                                     int32_t w, int32_t h,
                                     struct wl_array *states) {
    (void)data; (void)t; (void)states;
    fprintf(stderr, "[xdg-smoke] toplevel.configure w=%d h=%d\n", w, h);
}
static void on_toplevel_close(void *data, struct xdg_toplevel *t) {
    (void)data; (void)t;
}
static void on_toplevel_configure_bounds(void *data, struct xdg_toplevel *t,
                                            int32_t w, int32_t h) {
    (void)data; (void)t; (void)w; (void)h;
}
static void on_toplevel_wm_capabilities(void *data, struct xdg_toplevel *t,
                                           struct wl_array *caps) {
    (void)data; (void)t; (void)caps;
}
static const struct xdg_toplevel_listener tl_lis = {
    .configure         = on_toplevel_configure,
    .close             = on_toplevel_close,
    .configure_bounds  = on_toplevel_configure_bounds,
    .wm_capabilities   = on_toplevel_wm_capabilities,
};

static void on_buf_release(void *data, struct wl_buffer *buf) {
    (void)data; (void)buf;
    g_got_release = 1;
}
static const struct wl_buffer_listener buf_lis = { .release = on_buf_release };

int main(void)
{
    /* Give waylandd time to bind its socket. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    if (!getenv("WAYLAND_DISPLAY")) setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp", 1);

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "[xdg-smoke] FAIL: wl_display_connect: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "[xdg-smoke] connected\n");

    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_lis, NULL);
    wl_display_roundtrip(dpy);

    if (!g_shm || !g_comp || !g_wm) {
        fprintf(stderr, "[xdg-smoke] FAIL: missing globals shm=%p comp=%p wm=%p\n",
                (void*)g_shm, (void*)g_comp, (void*)g_wm);
        return 1;
    }
    xdg_wm_base_add_listener(g_wm, &wm_lis, NULL);
    fprintf(stderr, "[xdg-smoke] globals bound (shm + comp + wm_base)\n");

    /* shm pool */
    const int W = 64, H = 64, BPP = 4;
    const int STRIDE = W * BPP;
    const int SIZE = STRIDE * H;

    int fd = memfd_create_shim("xdg-smoke", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, SIZE) < 0) {
        fprintf(stderr, "[xdg-smoke] FAIL: memfd setup: %s\n", strerror(errno));
        return 1;
    }
    uint8_t *px = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        fprintf(stderr, "[xdg-smoke] FAIL: mmap: %s\n", strerror(errno));
        return 1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, SIZE);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, W, H, STRIDE,
                                                       WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(buf, &buf_lis, NULL);

    /* Surface + xdg_toplevel role */
    struct wl_surface *surf = wl_compositor_create_surface(g_comp);
    struct xdg_surface *xs  = xdg_wm_base_get_xdg_surface(g_wm, surf);
    xdg_surface_add_listener(xs, &xs_lis, NULL);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_lis, NULL);
    xdg_toplevel_set_title(tl, "Brook xdg-smoke");
    xdg_toplevel_set_app_id(tl, "brook.xdg-smoke");

    /* Initial null commit — required to elicit configure. */
    wl_surface_commit(surf);
    fprintf(stderr, "[xdg-smoke] xdg_toplevel created, awaiting configure\n");

    /* Pump until configure arrives. */
    for (int i = 0; i < 40 && !g_got_configure; i++) {
        wl_display_roundtrip(dpy);
        if (g_got_configure) break;
        struct timespec t = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    if (!g_got_configure) {
        fprintf(stderr, "[xdg-smoke] FAIL: never got configure\n");
        return 1;
    }
    /* Push the ack out to the server. */
    wl_display_flush(dpy);

    /* Draw gradient. */
    for (int y = 0; y < H; y++) {
        uint32_t *row = (uint32_t*)(px + y * STRIDE);
        for (int x = 0; x < W; x++) {
            uint8_t r = (uint8_t)(x * 4);
            uint8_t g = (uint8_t)(y * 4);
            uint8_t b = (uint8_t)((x + y) * 2);
            row[x] = (0xFFu << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
        }
    }

    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage(surf, 0, 0, W, H);
    wl_surface_commit(surf);
    fprintf(stderr, "[xdg-smoke] committed frame\n");

    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);

    for (int i = 0; i < 10 && !g_got_release; i++) {
        wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        struct timespec t = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }

    fprintf(stderr, "[xdg-smoke] buffer release received: %s\n",
            g_got_release ? "yes" : "no (non-fatal)");

    xdg_toplevel_destroy(tl);
    xdg_surface_destroy(xs);
    wl_surface_destroy(surf);
    wl_buffer_destroy(buf);
    wl_shm_pool_destroy(pool);
    munmap(px, SIZE);
    close(fd);
    wl_display_disconnect(dpy);

    fprintf(stderr, "[xdg-smoke] PASS\n");
    return 0;
}
