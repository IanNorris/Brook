/*
 * waylandd.c — Brook OS Wayland server.
 *
 * Beyond first-light: now handles wl_compositor.create_surface and
 * wl_surface.attach/damage/commit. On commit, the server maps the
 * attached wl_shm buffer, reads pixel data, and prints a digest so we
 * can verify end-to-end shm round-trip without a real display sink.
 *
 * What this does NOT do yet (intentionally):
 *   - blit into the Brook kernel compositor (next step)
 *   - implement xdg-shell (no window role, no real weston clients yet)
 *   - anything fancy: no damage tracking, no frame callbacks
 *
 * Build via tools/waylandd-pkg/default.nix.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

static uint32_t g_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint32_t)(ts.tv_nsec / 1000000));
}

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"

static struct wl_display *g_display = NULL;
static volatile sig_atomic_t g_shutdown = 0;
static int g_commit_count = 0;

/* Waylandd's own virtual framebuffer — blit committed client buffers here
 * so the kernel compositor can show them on screen. */
static uint32_t *g_vfb       = NULL;
static uint32_t  g_vfb_w     = 0;
static uint32_t  g_vfb_h     = 0;
static uint32_t  g_vfb_bytes = 0;

static int open_vfb(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[waylandd] /dev/fb0 open failed: %s\n", strerror(errno));
        return -1;
    }
    struct fb_var_screeninfo vi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0) {
        fprintf(stderr, "[waylandd] FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    g_vfb_w = vi.xres;
    g_vfb_h = vi.yres;
    g_vfb_bytes = g_vfb_w * g_vfb_h * 4;

    void *p = mmap(NULL, g_vfb_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "[waylandd] VFB mmap failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    g_vfb = (uint32_t*)p;
    /* fd can be closed now — kernel retains the mapping. */
    close(fd);

    /* Paint a distinct background so it's obvious what's ours vs empty. */
    uint32_t bg = 0xff101830; /* deep navy */
    for (uint32_t i = 0; i < g_vfb_w * g_vfb_h; i++) g_vfb[i] = bg;

    fprintf(stderr, "[waylandd] vfb %ux%u mapped at %p (%u bytes)\n",
            g_vfb_w, g_vfb_h, (void*)g_vfb, g_vfb_bytes);
    return 0;
}

static void blit_into_vfb(const uint8_t *src, int sw, int sh, int sstride)
{
    if (!g_vfb || sw <= 0 || sh <= 0) return;
    /* Centre the client surface in our VFB. */
    int dx = ((int)g_vfb_w - sw) / 2; if (dx < 0) dx = 0;
    int dy = ((int)g_vfb_h - sh) / 2; if (dy < 0) dy = 0;
    int cw = sw; if (dx + cw > (int)g_vfb_w) cw = (int)g_vfb_w - dx;
    int ch = sh; if (dy + ch > (int)g_vfb_h) ch = (int)g_vfb_h - dy;

    for (int y = 0; y < ch; y++) {
        const uint32_t *srow = (const uint32_t*)(src + (size_t)y * sstride);
        uint32_t *drow = g_vfb + (size_t)(dy + y) * g_vfb_w + dx;
        for (int x = 0; x < cw; x++) {
            /* XRGB8888 -> ARGB8888 with opaque alpha. */
            drow[x] = 0xff000000u | (srow[x] & 0x00ffffffu);
        }
    }
}

static void on_sigint(int sig) { (void)sig; g_shutdown = 1; }

/* ---------------- wl_surface ---------------- */

struct brook_surface {
    struct wl_resource *resource;
    struct wl_resource *pending_buffer; /* set by attach, consumed by commit */
    int pending_w, pending_h;

    /* xdg-shell role state. role==NULL until get_xdg_surface; once we
     * have an xdg_toplevel we treat the surface as "windowed" — we send
     * an initial configure on the first commit, and only blit committed
     * frames after the client has acked at least one configure. */
    struct wl_resource *xdg_surface;   /* xdg_surface resource, if any */
    struct wl_resource *xdg_toplevel;  /* xdg_toplevel resource, if any */
    int  xdg_initial_commit_done;      /* did we send the first configure? */
    int  xdg_acked;                    /* did client ack a configure? */
    uint32_t next_configure_serial;

    /* Pending frame callbacks queued by wl_surface.frame() since the
     * last commit. Fired with a millisecond timestamp at commit time so
     * the client redraws on a real cadence. Kept as a tiny ring; weston
     * demos only ever queue one. */
    struct wl_resource *pending_frame_cbs[8];
    int pending_frame_cb_count;
};

static void surface_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void surface_attach(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *buffer, int32_t x, int32_t y) {
    (void)c; (void)x; (void)y;
    struct brook_surface *s = wl_resource_get_user_data(r);
    s->pending_buffer = buffer;
}
static void surface_damage(struct wl_client *c, struct wl_resource *r,
                           int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void surface_frame(struct wl_client *c, struct wl_resource *r, uint32_t cb) {
    /* Queue the callback; we'll fire it at commit time with a real
     * timestamp so the client redraws on a steady cadence. */
    struct brook_surface *s = wl_resource_get_user_data(r);
    struct wl_resource *cb_r = wl_resource_create(c, &wl_callback_interface, 1, cb);
    if (!cb_r) return;
    if (s->pending_frame_cb_count < (int)(sizeof(s->pending_frame_cbs)/sizeof(s->pending_frame_cbs[0]))) {
        s->pending_frame_cbs[s->pending_frame_cb_count++] = cb_r;
    } else {
        /* Overflow: just fire it now to avoid leaking. */
        wl_callback_send_done(cb_r, g_now_ms());
        wl_resource_destroy(cb_r);
    }
}
static void surface_set_opaque_region(struct wl_client *c, struct wl_resource *r,
                                       struct wl_resource *reg) {
    (void)c; (void)r; (void)reg;
}
static void surface_set_input_region(struct wl_client *c, struct wl_resource *r,
                                      struct wl_resource *reg) {
    (void)c; (void)r; (void)reg;
}
static void surface_commit(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    fprintf(stderr, "[waylandd] surface_commit entry\n"); fflush(stderr);
    struct brook_surface *s = wl_resource_get_user_data(r);

    /* xdg-shell role: the very first commit must have NO buffer; we
     * respond with a configure. The client acks, then commits with a
     * buffer for real. */
    if (s->xdg_toplevel && !s->xdg_initial_commit_done) {
        s->xdg_initial_commit_done = 1;
        struct wl_array states;
        wl_array_init(&states);
        /* No states for first configure — client picks its own size. */
        xdg_toplevel_send_configure(s->xdg_toplevel, 0, 0, &states);
        wl_array_release(&states);
        uint32_t serial = ++s->next_configure_serial;
        xdg_surface_send_configure(s->xdg_surface, serial);
        fprintf(stderr, "[waylandd] xdg initial configure sent serial=%u\n", serial);
        return;
    }

    if (!s->pending_buffer) {
        fprintf(stderr, "[waylandd] commit with no attached buffer\n");
        return;
    }
    struct wl_shm_buffer *shm = wl_shm_buffer_get(s->pending_buffer);
    if (!shm) {
        fprintf(stderr, "[waylandd] commit: buffer is not wl_shm\n");
        wl_buffer_send_release(s->pending_buffer);
        s->pending_buffer = NULL;
        return;
    }

    int32_t w      = wl_shm_buffer_get_width(shm);
    int32_t h      = wl_shm_buffer_get_height(shm);
    int32_t stride = wl_shm_buffer_get_stride(shm);
    uint32_t fmt   = wl_shm_buffer_get_format(shm);

    wl_shm_buffer_begin_access(shm);
    const uint8_t *px = wl_shm_buffer_get_data(shm);
    uint32_t digest = 0;
    uint32_t first_pix = 0, last_pix = 0;
    if (px && w > 0 && h > 0) {
        /* FNV-1a over the whole buffer — proves bytes transited correctly. */
        digest = 2166136261u;
        uint64_t total = (uint64_t)stride * (uint64_t)h;
        for (uint64_t i = 0; i < total; i++) {
            digest = (digest ^ px[i]) * 16777619u;
        }
        first_pix = *(const uint32_t*)px;
        last_pix  = *(const uint32_t*)(px + total - 4);
        /* Only blit toplevels that have completed the configure handshake.
         * Bare-shm clients (no xdg role) still get blitted as before for
         * the wl_shm round-trip diagnostic. */
        int may_blit = (!s->xdg_toplevel) || s->xdg_acked;
        if (may_blit) blit_into_vfb(px, w, h, stride);
    }
    wl_shm_buffer_end_access(shm);

    const char *kind = s->xdg_toplevel ? "xdg-toplevel" : "bare";
    fprintf(stderr,
            "[waylandd] commit #%d (%s): %dx%d stride=%d fmt=0x%x "
            "digest=0x%08x first=0x%08x last=0x%08x px=%p\n",
            ++g_commit_count, kind, w, h, stride, fmt, digest, first_pix, last_pix,
            (void*)px);

    /* Release the buffer so the client can reuse it. */
    wl_buffer_send_release(s->pending_buffer);
    s->pending_buffer = NULL;

    /* Fire any frame callbacks queued via wl_surface.frame() — tells the
     * client this commit has been "presented" and it's a good time to
     * draw the next frame. Without this, animated demos (weston-flower)
     * stall after the first commit. */
    uint32_t now = g_now_ms();
    for (int i = 0; i < s->pending_frame_cb_count; i++) {
        wl_callback_send_done(s->pending_frame_cbs[i], now);
        wl_resource_destroy(s->pending_frame_cbs[i]);
        s->pending_frame_cbs[i] = NULL;
    }
    s->pending_frame_cb_count = 0;
}
static void surface_set_buffer_transform(struct wl_client *c, struct wl_resource *r, int32_t t) {
    (void)c; (void)r; (void)t;
}
static void surface_set_buffer_scale(struct wl_client *c, struct wl_resource *r, int32_t s) {
    (void)c; (void)r; (void)s;
}
static void surface_damage_buffer(struct wl_client *c, struct wl_resource *r,
                                   int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void surface_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
    (void)c; (void)r; (void)x; (void)y;
}

static const struct wl_surface_interface surface_impl = {
    .destroy              = surface_destroy,
    .attach               = surface_attach,
    .damage               = surface_damage,
    .frame                = surface_frame,
    .set_opaque_region    = surface_set_opaque_region,
    .set_input_region     = surface_set_input_region,
    .commit               = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale     = surface_set_buffer_scale,
    .damage_buffer        = surface_damage_buffer,
    .offset               = surface_offset,
};

static void surface_destroy_userdata(struct wl_resource *r) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    free(s);
}

/* ---------------- wl_compositor ---------------- */

static void compositor_create_surface(struct wl_client *client,
                                      struct wl_resource *res,
                                      uint32_t id)
{
    (void)res;
    fprintf(stderr, "[waylandd] create_surface entry id=%u\n", id);
    struct brook_surface *s = calloc(1, sizeof(*s));
    if (!s) { wl_client_post_no_memory(client); return; }
    struct wl_resource *sr = wl_resource_create(client, &wl_surface_interface,
                                                 wl_resource_get_version(res), id);
    if (!sr) { free(s); wl_client_post_no_memory(client); return; }
    s->resource = sr;
    wl_resource_set_implementation(sr, &surface_impl, s, surface_destroy_userdata);
    fprintf(stderr, "[waylandd] wl_surface created id=%u\n", id); fflush(stderr);
}

static void region_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void region_add(struct wl_client *c, struct wl_resource *r,
                       int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void region_subtract(struct wl_client *c, struct wl_resource *r,
                            int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static const struct wl_region_interface region_impl = {
    .destroy  = region_destroy,
    .add      = region_add,
    .subtract = region_subtract,
};

static void compositor_create_region(struct wl_client *client,
                                     struct wl_resource *res,
                                     uint32_t id)
{
    /* Regions are stubbed (we ignore opaque/input regions) but we MUST
     * register the resource so subsequent set_opaque_region(id) doesn't
     * fail with "unknown object". */
    struct wl_resource *rr = wl_resource_create(client, &wl_region_interface,
                                                 wl_resource_get_version(res), id);
    if (!rr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(rr, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region  = compositor_create_region,
};

/* ---------------- xdg-shell ---------------- */

/* xdg_toplevel: most requests are stash-or-stub for now. We only need
 * configure/ack and destroy to get a real client to commit a frame. */

static void xdg_toplevel_destroy_req(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void xdg_toplevel_set_parent(struct wl_client *c, struct wl_resource *r,
                                     struct wl_resource *parent) {
    (void)c; (void)r; (void)parent;
}
static void xdg_toplevel_set_title(struct wl_client *c, struct wl_resource *r,
                                    const char *title) {
    (void)c; (void)r;
    fprintf(stderr, "[waylandd] xdg_toplevel.set_title: %s\n", title ? title : "(null)");
}
static void xdg_toplevel_set_app_id(struct wl_client *c, struct wl_resource *r,
                                     const char *app_id) {
    (void)c; (void)r;
    fprintf(stderr, "[waylandd] xdg_toplevel.set_app_id: %s\n", app_id ? app_id : "(null)");
}
static void xdg_toplevel_show_window_menu(struct wl_client *c, struct wl_resource *r,
                                            struct wl_resource *seat, uint32_t serial,
                                            int32_t x, int32_t y) {
    (void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y;
}
static void xdg_toplevel_move(struct wl_client *c, struct wl_resource *r,
                               struct wl_resource *seat, uint32_t serial) {
    (void)c; (void)r; (void)seat; (void)serial;
}
static void xdg_toplevel_resize(struct wl_client *c, struct wl_resource *r,
                                 struct wl_resource *seat, uint32_t serial,
                                 uint32_t edges) {
    (void)c; (void)r; (void)seat; (void)serial; (void)edges;
}
static void xdg_toplevel_set_max_size(struct wl_client *c, struct wl_resource *r,
                                       int32_t w, int32_t h) {
    (void)c; (void)r; (void)w; (void)h;
}
static void xdg_toplevel_set_min_size(struct wl_client *c, struct wl_resource *r,
                                       int32_t w, int32_t h) {
    (void)c; (void)r; (void)w; (void)h;
}
static void xdg_toplevel_set_maximized(struct wl_client *c, struct wl_resource *r) {
    (void)c; (void)r;
}
static void xdg_toplevel_unset_maximized(struct wl_client *c, struct wl_resource *r) {
    (void)c; (void)r;
}
static void xdg_toplevel_set_fullscreen(struct wl_client *c, struct wl_resource *r,
                                         struct wl_resource *output) {
    (void)c; (void)r; (void)output;
}
static void xdg_toplevel_unset_fullscreen(struct wl_client *c, struct wl_resource *r) {
    (void)c; (void)r;
}
static void xdg_toplevel_set_minimized(struct wl_client *c, struct wl_resource *r) {
    (void)c; (void)r;
}

static const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy           = xdg_toplevel_destroy_req,
    .set_parent        = xdg_toplevel_set_parent,
    .set_title         = xdg_toplevel_set_title,
    .set_app_id        = xdg_toplevel_set_app_id,
    .show_window_menu  = xdg_toplevel_show_window_menu,
    .move              = xdg_toplevel_move,
    .resize            = xdg_toplevel_resize,
    .set_max_size      = xdg_toplevel_set_max_size,
    .set_min_size      = xdg_toplevel_set_min_size,
    .set_maximized     = xdg_toplevel_set_maximized,
    .unset_maximized   = xdg_toplevel_unset_maximized,
    .set_fullscreen    = xdg_toplevel_set_fullscreen,
    .unset_fullscreen  = xdg_toplevel_unset_fullscreen,
    .set_minimized     = xdg_toplevel_set_minimized,
};

static void xdg_toplevel_resource_destroy(struct wl_resource *r) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (s) s->xdg_toplevel = NULL;
}

/* xdg_surface */

static void xdg_surface_destroy_req(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void xdg_surface_get_toplevel(struct wl_client *c, struct wl_resource *r,
                                       uint32_t id) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    struct wl_resource *tr = wl_resource_create(c, &xdg_toplevel_interface,
                                                 wl_resource_get_version(r), id);
    if (!tr) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(tr, &xdg_toplevel_impl, s, xdg_toplevel_resource_destroy);
    s->xdg_toplevel = tr;
    fprintf(stderr, "[waylandd] xdg_surface.get_toplevel id=%u\n", id);
}
static void xdg_surface_get_popup(struct wl_client *c, struct wl_resource *r,
                                    uint32_t id, struct wl_resource *parent,
                                    struct wl_resource *positioner) {
    (void)id; (void)parent; (void)positioner;
    wl_resource_post_error(r, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                            "popups not implemented");
    (void)c;
}
static void xdg_surface_set_window_geometry(struct wl_client *c, struct wl_resource *r,
                                              int32_t x, int32_t y,
                                              int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void xdg_surface_ack_configure(struct wl_client *c, struct wl_resource *r,
                                        uint32_t serial) {
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    s->xdg_acked = 1;
    fprintf(stderr, "[waylandd] xdg_surface.ack_configure serial=%u\n", serial);
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy             = xdg_surface_destroy_req,
    .get_toplevel        = xdg_surface_get_toplevel,
    .get_popup           = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure       = xdg_surface_ack_configure,
};

static void xdg_surface_resource_destroy(struct wl_resource *r) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (s) s->xdg_surface = NULL;
}

/* xdg_positioner — minimal stub (we never use the values). */

static void positioner_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void positioner_set_size(struct wl_client *c, struct wl_resource *r,
                                  int32_t w, int32_t h) {
    (void)c; (void)r; (void)w; (void)h;
}
static void positioner_set_anchor_rect(struct wl_client *c, struct wl_resource *r,
                                         int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void positioner_set_anchor(struct wl_client *c, struct wl_resource *r,
                                    uint32_t a) { (void)c; (void)r; (void)a; }
static void positioner_set_gravity(struct wl_client *c, struct wl_resource *r,
                                     uint32_t g) { (void)c; (void)r; (void)g; }
static void positioner_set_constraint_adjustment(struct wl_client *c, struct wl_resource *r,
                                                    uint32_t v) { (void)c; (void)r; (void)v; }
static void positioner_set_offset(struct wl_client *c, struct wl_resource *r,
                                    int32_t x, int32_t y) {
    (void)c; (void)r; (void)x; (void)y;
}
static void positioner_set_reactive(struct wl_client *c, struct wl_resource *r) {
    (void)c; (void)r;
}
static void positioner_set_parent_size(struct wl_client *c, struct wl_resource *r,
                                         int32_t w, int32_t h) {
    (void)c; (void)r; (void)w; (void)h;
}
static void positioner_set_parent_configure(struct wl_client *c, struct wl_resource *r,
                                               uint32_t serial) {
    (void)c; (void)r; (void)serial;
}

static const struct xdg_positioner_interface positioner_impl = {
    .destroy                   = positioner_destroy,
    .set_size                  = positioner_set_size,
    .set_anchor_rect           = positioner_set_anchor_rect,
    .set_anchor                = positioner_set_anchor,
    .set_gravity               = positioner_set_gravity,
    .set_constraint_adjustment = positioner_set_constraint_adjustment,
    .set_offset                = positioner_set_offset,
    .set_reactive              = positioner_set_reactive,
    .set_parent_size           = positioner_set_parent_size,
    .set_parent_configure      = positioner_set_parent_configure,
};

/* xdg_wm_base */

static void wm_base_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void wm_base_create_positioner(struct wl_client *c, struct wl_resource *r,
                                        uint32_t id) {
    struct wl_resource *pr = wl_resource_create(c, &xdg_positioner_interface,
                                                 wl_resource_get_version(r), id);
    if (!pr) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(pr, &positioner_impl, NULL, NULL);
}
static void wm_base_get_xdg_surface(struct wl_client *c, struct wl_resource *r,
                                      uint32_t id, struct wl_resource *surface) {
    struct brook_surface *s = wl_resource_get_user_data(surface);
    if (!s) {
        wl_resource_post_error(r, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
                                "no surface state");
        return;
    }
    if (s->xdg_surface) {
        wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE,
                                "wl_surface already has xdg_surface");
        return;
    }
    struct wl_resource *xs = wl_resource_create(c, &xdg_surface_interface,
                                                 wl_resource_get_version(r), id);
    if (!xs) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(xs, &xdg_surface_impl, s, xdg_surface_resource_destroy);
    s->xdg_surface = xs;
    fprintf(stderr, "[waylandd] xdg_wm_base.get_xdg_surface id=%u\n", id);
}
static void wm_base_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial) {
    (void)c; (void)r; (void)serial;
}

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy           = wm_base_destroy,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface   = wm_base_get_xdg_surface,
    .pong              = wm_base_pong,
};

static void wm_base_bind(struct wl_client *client, void *data,
                           uint32_t version, uint32_t id) {
    (void)data;
    fprintf(stderr, "[waylandd] xdg_wm_base bind v=%u id=%u\n", version, id);
    struct wl_resource *r = wl_resource_create(client, &xdg_wm_base_interface,
                                                (int)version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &wm_base_impl, NULL, NULL);
}

static void brook_wl_log(const char *fmt, va_list ap) {
    fprintf(stderr, "[waylandd][libwayland] ");
    vfprintf(stderr, fmt, ap);
    fflush(stderr);
}

static void client_destroyed(struct wl_listener *l, void *data) {
    (void)l;
    fprintf(stderr, "[waylandd] client %p destroyed\n", data); fflush(stderr);
}
static struct wl_listener g_client_destroy_listener = { .notify = client_destroyed };

static void client_created(struct wl_listener *l, void *data) {
    (void)l;
    struct wl_client *c = data;
    fprintf(stderr, "[waylandd] client %p created\n", (void*)c); fflush(stderr);
    wl_client_add_destroy_listener(c, &g_client_destroy_listener);
}
static struct wl_listener g_client_create_listener = { .notify = client_created };

static void compositor_bind(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id)
{
    (void)data;
    fprintf(stderr, "[waylandd] compositor_bind client=%p v=%u id=%u\n",
            (void*)client, version, id);
    struct wl_resource *r = wl_resource_create(client, &wl_compositor_interface,
                                               (int)version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    int run_seconds = 10;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            run_seconds = atoi(argv[++i]);
    }

    fprintf(stderr, "[waylandd] starting (first-light, %ds)\n", run_seconds);

    /* Best-effort: map our VFB so we can actually show committed buffers. */
    (void)open_vfb();

    g_display = wl_display_create();
    if (!g_display) {
        fprintf(stderr, "[waylandd] FAIL: wl_display_create returned NULL\n");
        return 1;
    }
    fprintf(stderr, "[waylandd] wl_display_create OK\n");

    wl_log_set_handler_server(brook_wl_log);
    wl_display_add_client_created_listener(g_display, &g_client_create_listener);

    const char *sock_name = getenv("WAYLAND_DISPLAY");
    if (!sock_name) sock_name = "wayland-0";

    const char *sock = NULL;
    if (getenv("XDG_RUNTIME_DIR"))
    {
        if (wl_display_add_socket(g_display, sock_name) == 0)
            sock = sock_name;
    }
    if (!sock)
    {
        int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (lfd < 0) {
            fprintf(stderr, "[waylandd] FAIL: socket: %s\n", strerror(errno));
            return 1;
        }
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "/tmp/%s", sock_name);
        unlink(a.sun_path);
        if (bind(lfd, (struct sockaddr*)&a, sizeof(a)) < 0) {
            fprintf(stderr, "[waylandd] FAIL: bind %s: %s\n",
                    a.sun_path, strerror(errno));
            return 1;
        }
        if (listen(lfd, 16) < 0) {
            fprintf(stderr, "[waylandd] FAIL: listen: %s\n", strerror(errno));
            return 1;
        }
        if (wl_display_add_socket_fd(g_display, lfd) < 0) {
            fprintf(stderr, "[waylandd] FAIL: add_socket_fd\n");
            return 1;
        }
        fprintf(stderr, "[waylandd] bound explicit socket at %s (fd=%d)\n",
                a.sun_path, lfd);
        sock = sock_name;
    }
    fprintf(stderr, "[waylandd] listening on WAYLAND_DISPLAY=%s\n", sock);

    if (wl_display_init_shm(g_display) < 0)
        fprintf(stderr, "[waylandd] WARN: wl_display_init_shm failed\n");
    else
        fprintf(stderr, "[waylandd] wl_shm initialised\n");

    struct wl_global *comp = wl_global_create(g_display, &wl_compositor_interface,
                                              4, NULL, compositor_bind);
    if (!comp) {
        fprintf(stderr, "[waylandd] FAIL: wl_global_create(wl_compositor)\n");
        return 1;
    }
    fprintf(stderr, "[waylandd] wl_compositor global advertised (v4)\n");

    struct wl_global *xdg = wl_global_create(g_display, &xdg_wm_base_interface,
                                              3, NULL, wm_base_bind);
    if (!xdg) {
        fprintf(stderr, "[waylandd] FAIL: wl_global_create(xdg_wm_base)\n");
        return 1;
    }
    fprintf(stderr, "[waylandd] xdg_wm_base global advertised (v3)\n");

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    struct wl_event_loop *loop = wl_display_get_event_loop(g_display);
    if (!loop) {
        fprintf(stderr, "[waylandd] FAIL: get_event_loop\n");
        return 1;
    }

    for (int i = 0; i < run_seconds && !g_shutdown; ++i) {
        wl_display_flush_clients(g_display);
        int n = wl_event_loop_dispatch(loop, 1000);
        if (n < 0) {
            fprintf(stderr, "[waylandd] event loop error: %s\n", strerror(errno));
            break;
        }
    }

    fprintf(stderr, "[waylandd] shutting down (commits=%d)\n", g_commit_count);
    wl_display_destroy(g_display);
    fprintf(stderr, "[waylandd] PASS\n");
    return 0;
}
