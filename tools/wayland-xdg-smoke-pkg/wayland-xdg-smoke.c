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
 *   [xdg-smoke] popup configured ...
 *   [xdg-smoke] committed popup frame
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
static int g_popup_acked = 0;
static int g_got_popup_configure = 0;
static uint32_t g_popup_configure_serial = 0;
static int g_got_release = 0;
static int g_popup_got_release = 0;

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

static void on_popup_xdg_surface_configure(void *data, struct xdg_surface *xs,
                                           uint32_t serial) {
    (void)data;
    g_got_popup_configure = 1;
    g_popup_configure_serial = serial;
    xdg_surface_ack_configure(xs, serial);
    g_popup_acked = 1;
    fprintf(stderr, "[xdg-smoke] popup xdg_surface.configure serial=%u, acked\n", serial);
}
static const struct xdg_surface_listener popup_xs_lis = {
    .configure = on_popup_xdg_surface_configure,
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

static void on_popup_buf_release(void *data, struct wl_buffer *buf) {
    (void)data; (void)buf;
    g_popup_got_release = 1;
}
static const struct wl_buffer_listener popup_buf_lis = { .release = on_popup_buf_release };

static void on_popup_configure(void *data, struct xdg_popup *popup,
                               int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)data; (void)popup;
    fprintf(stderr, "[xdg-smoke] popup configured x=%d y=%d w=%d h=%d\n",
            x, y, w, h);
}
static void on_popup_done(void *data, struct xdg_popup *popup) {
    (void)data; (void)popup;
    fprintf(stderr, "[xdg-smoke] popup done\n");
}
static void on_popup_repositioned(void *data, struct xdg_popup *popup,
                                  uint32_t token) {
    (void)data; (void)popup;
    fprintf(stderr, "[xdg-smoke] popup repositioned token=%u\n", token);
}
static const struct xdg_popup_listener popup_lis = {
    .configure    = on_popup_configure,
    .popup_done   = on_popup_done,
    .repositioned = on_popup_repositioned,
};

static struct wl_buffer *make_shm_buffer(const char *name, int w, int h,
                                         uint32_t fill_base,
                                         uint8_t **out_px, int *out_fd,
                                         size_t *out_size) {
    const int bpp = 4;
    int stride = w * bpp;
    size_t size = (size_t)stride * (size_t)h;
    int fd = memfd_create_shim(name, MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        fprintf(stderr, "[xdg-smoke] FAIL: memfd setup for %s: %s\n",
                name, strerror(errno));
        if (fd >= 0) close(fd);
        return NULL;
    }
    uint8_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        fprintf(stderr, "[xdg-smoke] FAIL: mmap for %s: %s\n", name, strerror(errno));
        close(fd);
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t*)(px + (size_t)y * (size_t)stride);
        for (int x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((fill_base >> 16) + x * 3);
            uint8_t g = (uint8_t)((fill_base >> 8) + y * 4);
            uint8_t b = (uint8_t)(fill_base + (x + y) * 2);
            row[x] = (0xFFu << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
        }
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int32_t)size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                       WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    *out_px = px;
    *out_fd = fd;
    *out_size = size;
    return buf;
}

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

    const int W = 64, H = 64;

    int fd = -1;
    size_t size = 0;
    uint8_t *px = NULL;
    struct wl_buffer *buf = make_shm_buffer("xdg-smoke", W, H, 0x203040,
                                            &px, &fd, &size);
    if (!buf)
        return 1;
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

    const int PW = 96, PH = 48;
    int popup_fd = -1;
    size_t popup_size = 0;
    uint8_t *popup_px = NULL;
    struct wl_buffer *popup_buf = make_shm_buffer("xdg-popup-smoke", PW, PH, 0x802030,
                                                  &popup_px, &popup_fd, &popup_size);
    if (!popup_buf)
        return 1;
    wl_buffer_add_listener(popup_buf, &popup_buf_lis, NULL);

    struct wl_surface *popup_surf = wl_compositor_create_surface(g_comp);
    struct xdg_surface *popup_xs = xdg_wm_base_get_xdg_surface(g_wm, popup_surf);
    xdg_surface_add_listener(popup_xs, &popup_xs_lis, NULL);
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(g_wm);
    xdg_positioner_set_size(pos, PW, PH);
    xdg_positioner_set_anchor_rect(pos, 8, H, 48, 12);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_offset(pos, 4, 2);
    struct xdg_popup *popup = xdg_surface_get_popup(popup_xs, xs, pos);
    xdg_popup_add_listener(popup, &popup_lis, NULL);
    xdg_positioner_destroy(pos);
    wl_surface_commit(popup_surf);
    fprintf(stderr, "[xdg-smoke] popup created, awaiting configure\n");

    for (int i = 0; i < 40 && !g_got_popup_configure; i++) {
        wl_display_roundtrip(dpy);
        if (g_got_popup_configure) break;
        struct timespec t = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    if (!g_got_popup_configure || !g_popup_acked) {
        fprintf(stderr, "[xdg-smoke] FAIL: popup never configured\n");
        return 1;
    }

    wl_surface_attach(popup_surf, popup_buf, 0, 0);
    wl_surface_damage(popup_surf, 0, 0, PW, PH);
    wl_surface_commit(popup_surf);
    fprintf(stderr, "[xdg-smoke] committed popup frame serial=%u\n",
            g_popup_configure_serial);

    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);

    fprintf(stderr, "[xdg-smoke] popup buffer release received: %s\n",
            g_popup_got_release ? "yes" : "no (non-fatal)");

    xdg_popup_destroy(popup);
    xdg_surface_destroy(popup_xs);
    wl_surface_destroy(popup_surf);
    wl_buffer_destroy(popup_buf);
    munmap(popup_px, popup_size);
    close(popup_fd);

    xdg_toplevel_destroy(tl);
    xdg_surface_destroy(xs);
    wl_surface_destroy(surf);
    wl_buffer_destroy(buf);
    munmap(px, size);
    close(fd);
    wl_display_disconnect(dpy);

    fprintf(stderr, "[xdg-smoke] PASS\n");
    return 0;
}
