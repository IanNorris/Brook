/*
 * waylandd.c — Brook OS Wayland server (Phase D rewrite).
 *
 * Each wayland xdg_toplevel now becomes a kernel-managed Window
 * (WM_CREATE_WINDOW syscall) with its own VFB allocated and mapped by
 * the kernel.  Layout, chrome, panel, drag, focus hit-testing all live
 * in the kernel WM (window.cpp + compositor.cpp).  Our job here is
 * just the wayland protocol relay:
 *
 *   surface_commit   →  copy wl_shm pixels into the per-window VFB,
 *                       WM_SIGNAL_DIRTY, fire frame callbacks.
 *   set_title        →  WM_SET_TITLE.
 *   surface destroy  →  WM_DESTROY_WINDOW.
 *   per-frame pump   →  WM_POP_INPUT(wm_id) → wl_pointer/wl_keyboard
 *                       events, plus WM-synthetic events
 *                       (CLOSE_REQUESTED → xdg_toplevel.send_close,
 *                        FOCUS_GAINED/LOST → wl_keyboard.enter/leave).
 *
 * No tiling, no SSD, no panel, no global VFB — that all lived in the
 * old waylandd and is now the kernel WM's responsibility.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"
#include "xdg-decoration-server-protocol.h"

#define BROOK_SYS_WM_CREATE_WINDOW   506
#define BROOK_SYS_WM_DESTROY_WINDOW  507
#define BROOK_SYS_WM_SIGNAL_DIRTY    508
#define BROOK_SYS_WM_SET_TITLE       509
#define BROOK_SYS_WM_POP_INPUT       510
#define BROOK_SYS_WM_RESIZE_VFB      511
#define BROOK_SYS_WM_SET_DECORATION_MODE 512
#define BROOK_SYS_WM_BEGIN_MOVE      513
#define BROOK_SYS_WM_MOVE_RELATIVE   514
#define BROOK_SYS_WM_SET_MAXIMIZED   515
#define BROOK_SYS_WM_SET_MINIMIZED   516
#define BROOK_SYS_WM_BEGIN_RESIZE    517
#define BROOK_SYS_WM_SET_CURSOR_VISIBLE 518
#define BROOK_WM_CREATE_FLAG_CSD     1u
#define BROOK_WM_CREATE_FLAG_NO_FOCUS 2u

#define BROOK_WM_EDGE_TOP     1u
#define BROOK_WM_EDGE_BOTTOM  2u
#define BROOK_WM_EDGE_LEFT    4u
#define BROOK_WM_EDGE_RIGHT   8u

/* Match the kernel's BrookWmCreateOut struct. */
struct brook_wm_create_out {
    uint32_t wm_id;
    uint32_t vfb_stride;   /* in pixels */
    uint64_t vfb_user;
};

/* Match window.h Window::WmInputEvent layout (12 bytes). */
struct brook_wm_event {
    uint8_t  type;
    uint8_t  scan;
    uint8_t  ascii;
    uint8_t  mods;
    int16_t  x;
    int16_t  y;
    uint32_t reserved;
};

#define EVT_KEY_PRESS        0
#define EVT_KEY_RELEASE      1
#define EVT_MOUSE_MOVE       2
#define EVT_MOUSE_BTN_DOWN   3
#define EVT_MOUSE_BTN_UP     4
#define EVT_MOUSE_SCROLL     5
#define WM_EVT_CLOSE_REQUESTED 0x80
#define WM_EVT_FOCUS_GAINED    0x81
#define WM_EVT_FOCUS_LOST      0x82
#define WM_EVT_RESIZED         0x83

static struct wl_display *g_display = NULL;
static volatile sig_atomic_t g_shutdown = 0;
static int g_commit_count = 0;

static uint32_t g_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint32_t)(ts.tv_nsec / 1000000));
}

static long wm_create_window(uint32_t w, uint32_t h, const char* title,
                             uint32_t flags, struct brook_wm_create_out* out) {
    return syscall(BROOK_SYS_WM_CREATE_WINDOW, (long)w, (long)h,
                   (long)title, (long)out, (long)flags);
}
static long wm_destroy_window(uint32_t id) {
    return syscall(BROOK_SYS_WM_DESTROY_WINDOW, (long)id);
}
static long wm_signal_dirty(uint32_t id) {
    return syscall(BROOK_SYS_WM_SIGNAL_DIRTY, (long)id);
}
static long wm_set_title(uint32_t id, const char* title) {
    size_t n = title ? strlen(title) : 0;
    return syscall(BROOK_SYS_WM_SET_TITLE, (long)id, (long)title, (long)n);
}
static long wm_pop_input(uint32_t id, struct brook_wm_event* buf, long max) {
    return syscall(BROOK_SYS_WM_POP_INPUT, (long)id, (long)buf, max);
}
static long wm_resize_vfb(uint32_t id, uint32_t w, uint32_t h,
                          struct brook_wm_create_out* out) {
    return syscall(BROOK_SYS_WM_RESIZE_VFB, (long)id, (long)w, (long)h, (long)out);
}
static long wm_set_decoration_mode(uint32_t id, int csd) {
    return syscall(BROOK_SYS_WM_SET_DECORATION_MODE, (long)id, (long)csd);
}
static long wm_begin_move(uint32_t id) {
    return syscall(BROOK_SYS_WM_BEGIN_MOVE, (long)id);
}
static long wm_move_relative(uint32_t id, uint32_t parent_id, int32_t x, int32_t y) {
    return syscall(BROOK_SYS_WM_MOVE_RELATIVE, (long)id, (long)parent_id,
                   (long)x, (long)y);
}
static long wm_set_maximized(uint32_t id, int enable) {
    return syscall(BROOK_SYS_WM_SET_MAXIMIZED, (long)id, (long)enable);
}
static long wm_set_minimized(uint32_t id) {
    return syscall(BROOK_SYS_WM_SET_MINIMIZED, (long)id);
}
static long wm_begin_resize(uint32_t id, uint32_t edges) {
    return syscall(BROOK_SYS_WM_BEGIN_RESIZE, (long)id, (long)edges);
}
static long wm_set_cursor_visible(int visible) {
    return syscall(BROOK_SYS_WM_SET_CURSOR_VISIBLE, (long)(visible != 0));
}

/* ---------------- per-surface state ---------------- */

struct brook_seat_client;
struct brook_surface;
static void seat_clients_scrub_surface(struct brook_surface *s);

/* xdg_positioner state — captured here so xdg_surface.get_popup can
 * read its size/anchor without a forward declaration dance. */
struct brook_positioner {
    int32_t w, h;
    int32_t ax, ay, aw, ah;
    uint32_t anchor, gravity, constraint;
    int32_t ox, oy;
};

struct brook_surface {
    struct wl_resource *resource;
    struct wl_resource *pending_buffer;

    /* xdg-shell role state. */
    struct wl_resource *xdg_surface;
    struct wl_resource *xdg_toplevel;
    struct wl_resource *xdg_popup;        /* popup role (mutex with toplevel) */
    int  xdg_initial_commit_done;
    int  xdg_acked;
    uint32_t next_configure_serial;
    /* Popup geometry derived from positioner at get_popup time. */
    int32_t popup_w, popup_h;
    int32_t popup_x, popup_y;
    struct brook_surface *popup_parent;

    /* Pending frame callbacks queued by wl_surface.frame() since the
     * last commit. Fired with a millisecond timestamp at commit time. */
    struct wl_resource *pending_frame_cbs[8];
    int pending_frame_cb_count;

    /* Latest set_title — copied so it survives the wayland string. */
    char title[64];

    /* Kernel Window backing this xdg_toplevel.  Created lazily on the
     * first real (post-ack) commit when we know the buffer size. */
    uint32_t wm_id;
    uint32_t *vfb;          /* mapped into our address space by the kernel */
    uint32_t  vfb_stride;   /* in pixels */
    uint32_t  vfb_w, vfb_h; /* the size we created the Window at */

    /* Decoration mode requested by client.  Brook's policy is server-side
     * chrome for normal toplevels; popups are forced CSD/no-chrome. */
    int  deco_csd;          /* 1 = client-side, 0 = server-side */
    int  deco_pending;      /* mode change requested but wm_id not yet known */

    /* Linked list of all surfaces — used by the input pump. */
    struct brook_surface *next;
};

static struct brook_surface *g_surfaces = NULL;

struct brook_surface *find_surface_by_wm_id(uint32_t id);
struct brook_surface *find_surface_by_wm_id(uint32_t id) {
    if (!id) return NULL;
    for (struct brook_surface *s = g_surfaces; s; s = s->next)
        if (s->wm_id == id) return s;
    return NULL;
}

static void positioner_compute_geometry(const struct brook_positioner *p,
                                        int32_t *out_x, int32_t *out_y,
                                        int32_t *out_w, int32_t *out_h) {
    int32_t pw = (p && p->w > 0) ? p->w : 200;
    int32_t ph = (p && p->h > 0) ? p->h : 100;
    int32_t x = p ? p->ax : 0;
    int32_t y = p ? p->ay : 0;

    if (p) {
        switch (p->anchor) {
            case XDG_POSITIONER_ANCHOR_TOP:
            case XDG_POSITIONER_ANCHOR_BOTTOM:
                x += p->aw / 2;
                break;
            case XDG_POSITIONER_ANCHOR_RIGHT:
            case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
            case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
                x += p->aw;
                break;
            default:
                break;
        }
        switch (p->anchor) {
            case XDG_POSITIONER_ANCHOR_LEFT:
            case XDG_POSITIONER_ANCHOR_RIGHT:
                y += p->ah / 2;
                break;
            case XDG_POSITIONER_ANCHOR_BOTTOM:
            case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
            case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
                y += p->ah;
                break;
            default:
                break;
        }

        switch (p->gravity) {
            case XDG_POSITIONER_GRAVITY_TOP:
            case XDG_POSITIONER_GRAVITY_BOTTOM:
                x -= pw / 2;
                break;
            case XDG_POSITIONER_GRAVITY_LEFT:
            case XDG_POSITIONER_GRAVITY_TOP_LEFT:
            case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
                x -= pw;
                break;
            default:
                break;
        }
        switch (p->gravity) {
            case XDG_POSITIONER_GRAVITY_LEFT:
            case XDG_POSITIONER_GRAVITY_RIGHT:
                y -= ph / 2;
                break;
            case XDG_POSITIONER_GRAVITY_TOP:
            case XDG_POSITIONER_GRAVITY_TOP_LEFT:
            case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
                y -= ph;
                break;
            default:
                break;
        }

        x += p->ox;
        y += p->oy;
    }

    *out_x = x;
    *out_y = y;
    *out_w = pw;
    *out_h = ph;
}

static void popup_apply_position(struct brook_surface *s) {
    if (!s || !s->xdg_popup || !s->wm_id || !s->popup_parent || !s->popup_parent->wm_id)
        return;
    long rc = wm_move_relative(s->wm_id, s->popup_parent->wm_id, s->popup_x, s->popup_y);
    if (rc != 0) {
        fprintf(stderr, "[waylandd] WM_MOVE_RELATIVE popup=%u parent=%u at (%d,%d) failed rc=%ld errno=%d\n",
                s->wm_id, s->popup_parent->wm_id, s->popup_x, s->popup_y, rc, errno);
    } else {
        fprintf(stderr, "[waylandd] WM_MOVE_RELATIVE popup=%u parent=%u at (%d,%d)\n",
                s->wm_id, s->popup_parent->wm_id, s->popup_x, s->popup_y);
    }
}

static void on_sigint(int sig) { (void)sig; g_shutdown = 1; }

/* ---------------- wl_surface ---------------- */

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
    struct brook_surface *s = wl_resource_get_user_data(r);
    struct wl_resource *cb_r = wl_resource_create(c, &wl_callback_interface, 1, cb);
    if (!cb_r) return;
    int cap = (int)(sizeof(s->pending_frame_cbs)/sizeof(s->pending_frame_cbs[0]));
    if (s->pending_frame_cb_count < cap) {
        s->pending_frame_cbs[s->pending_frame_cb_count++] = cb_r;
    } else {
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

/* Copy wl_shm buffer pixels into the per-window VFB.  Preserve ARGB alpha so
 * GTK/Qt CSD shadows stay transparent; force XRGB buffers opaque. */
static void blit_to_window(struct brook_surface *s,
                           const uint8_t *src, int sw, int sh, int sstride,
                           uint32_t format) {
    if (!s->vfb || sw <= 0 || sh <= 0) return;
    int cw = sw < (int)s->vfb_w ? sw : (int)s->vfb_w;
    int ch = sh < (int)s->vfb_h ? sh : (int)s->vfb_h;
    int has_alpha = (format == WL_SHM_FORMAT_ARGB8888);
    for (int y = 0; y < ch; y++) {
        const uint32_t *srow = (const uint32_t*)(src + (size_t)y * sstride);
        uint32_t *drow = s->vfb + (size_t)y * s->vfb_stride;
        for (int x = 0; x < cw; x++) {
            drow[x] = has_alpha ? srow[x] : (0xff000000u | (srow[x] & 0x00ffffffu));
        }
    }
}

static void surface_commit(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);

    /* xdg-shell role: the very first commit must have NO buffer; we
     * respond with a configure. The client acks, then commits with a
     * buffer for real. */
    if (s->xdg_toplevel && !s->xdg_initial_commit_done) {
        s->xdg_initial_commit_done = 1;
        struct wl_array states;
        wl_array_init(&states);
        /* Suggest a sane default size. Sending 0,0 lets the client pick
         * its own natural size; for some GTK layouts that produces a
         * pathological measurement (>65535 px). Bounding it here lets
         * the SHM pool allocation stay sane. */
        xdg_toplevel_send_configure(s->xdg_toplevel, 800, 600, &states);
        wl_array_release(&states);
        uint32_t serial = ++s->next_configure_serial;
        xdg_surface_send_configure(s->xdg_surface, serial);
        fprintf(stderr, "[waylandd] xdg initial configure sent serial=%u (800x600)\n", serial);
        return;
    }
    if (s->xdg_popup && !s->xdg_initial_commit_done) {
        s->xdg_initial_commit_done = 1;
        int32_t pw = s->popup_w > 0 ? s->popup_w : 200;
        int32_t ph = s->popup_h > 0 ? s->popup_h : 100;
        xdg_popup_send_configure(s->xdg_popup, s->popup_x, s->popup_y, pw, ph);
        uint32_t serial = ++s->next_configure_serial;
        xdg_surface_send_configure(s->xdg_surface, serial);
        fprintf(stderr, "[waylandd] xdg popup initial configure sent serial=%u %dx%d\n",
                serial, pw, ph);
        return;
    }

    if (!s->pending_buffer) return;

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

    int may_blit = (!s->xdg_toplevel && !s->xdg_popup) || s->xdg_acked;
    if (may_blit && w > 0 && h > 0 && (s->xdg_toplevel || s->xdg_popup)) {
        /* Lazy create the kernel Window now that we know the size. */
        if (s->wm_id == 0) {
            struct brook_wm_create_out out = {0};
            uint32_t create_flags = 0;
            if (s->xdg_popup || s->deco_csd)
                create_flags |= BROOK_WM_CREATE_FLAG_CSD;
            if (s->xdg_popup)
                create_flags |= BROOK_WM_CREATE_FLAG_NO_FOCUS;
            long rc = wm_create_window((uint32_t)w, (uint32_t)h,
                                       s->title[0] ? s->title : NULL,
                                       create_flags,
                                       &out);
            if (rc == 0 && out.wm_id) {
                s->wm_id      = out.wm_id;
                s->vfb        = (uint32_t*)(uintptr_t)out.vfb_user;
                s->vfb_stride = out.vfb_stride;
                s->vfb_w      = (uint32_t)w;
                s->vfb_h      = (uint32_t)h;
                fprintf(stderr,
                        "[waylandd] WM_CREATE_WINDOW id=%u %dx%d stride=%u vfb=%p deco=%s\n",
                        s->wm_id, w, h, s->vfb_stride, (void*)s->vfb,
                        s->deco_csd ? "client_side" : "server_side");
                /* The initial decoration mode is applied atomically by
                 * WM_CREATE_WINDOW; later mode changes use syscall 512. */
                if (s->deco_pending) {
                    s->deco_pending = 0;
                }
                popup_apply_position(s);
            } else {
                fprintf(stderr, "[waylandd] WM_CREATE_WINDOW failed rc=%ld\n", rc);
            }
        }

        if (s->wm_id && s->vfb) {
            /* If the client committed a buffer at a size different from
             * our current VFB (typical after we sent a configure with new
             * dimensions and the client allocated a fresh buffer), grow
             * the kernel VFB to match before blitting.  We only resize
             * when *both* dimensions changed or the new buffer is bigger
             * than current VFB so we don't churn on transient mismatches. */
            if ((uint32_t)w != s->vfb_w || (uint32_t)h != s->vfb_h) {
                struct brook_wm_create_out out = {0};
                long rc = wm_resize_vfb(s->wm_id, (uint32_t)w, (uint32_t)h, &out);
                if (rc == 0 && out.vfb_user) {
                    s->vfb        = (uint32_t*)(uintptr_t)out.vfb_user;
                    s->vfb_stride = out.vfb_stride;
                    s->vfb_w      = (uint32_t)w;
                    s->vfb_h      = (uint32_t)h;
                    fprintf(stderr,
                            "[waylandd] WM_RESIZE_VFB id=%u %dx%d stride=%u vfb=%p\n",
                            s->wm_id, w, h, s->vfb_stride, (void*)s->vfb);
                } else {
                    fprintf(stderr,
                            "[waylandd] WM_RESIZE_VFB id=%u %dx%d FAILED rc=%ld — clamping blit\n",
                            s->wm_id, w, h, rc);
                }
            }
            wl_shm_buffer_begin_access(shm);
            const uint8_t *px = wl_shm_buffer_get_data(shm);
            if (px) blit_to_window(s, px, w, h, stride, wl_shm_buffer_get_format(shm));
            wl_shm_buffer_end_access(shm);
            wm_signal_dirty(s->wm_id);
        }
    }

    ++g_commit_count;

    wl_buffer_send_release(s->pending_buffer);
    s->pending_buffer = NULL;

    /* Fire any frame callbacks queued via wl_surface.frame(). */
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
static void surface_set_buffer_scale(struct wl_client *c, struct wl_resource *r, int32_t sc) {
    (void)c; (void)r; (void)sc;
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
    if (!s) return;
    /* Scrub references to this surface from all seat clients before we free
     * it.  Without this, a subsequent input event would dereference freed
     * memory and (more visibly) call wl_pointer.send_leave with a surface
     * resource owned by another client, which libwayland aborts as a
     * compositor bug. */
    seat_clients_scrub_surface(s);
    /* unlink */
    struct brook_surface **pp = &g_surfaces;
    while (*pp) {
        if (*pp == s) { *pp = s->next; break; }
        pp = &(*pp)->next;
    }
    for (struct brook_surface *it = g_surfaces; it; it = it->next) {
        if (it->popup_parent == s) it->popup_parent = NULL;
    }
    if (s->wm_id) wm_destroy_window(s->wm_id);
    free(s);
}

/* ---------------- wl_compositor ---------------- */

static void compositor_create_surface(struct wl_client *client,
                                      struct wl_resource *res,
                                      uint32_t id) {
    struct brook_surface *s = calloc(1, sizeof(*s));
    if (!s) { wl_client_post_no_memory(client); return; }
    struct wl_resource *sr = wl_resource_create(client, &wl_surface_interface,
                                                wl_resource_get_version(res), id);
    if (!sr) { free(s); wl_client_post_no_memory(client); return; }
    s->resource = sr;
    s->next = g_surfaces;
    g_surfaces = s;
    wl_resource_set_implementation(sr, &surface_impl, s, surface_destroy_userdata);
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
                                     uint32_t id) {
    struct wl_resource *rr = wl_resource_create(client, &wl_region_interface,
                                                 wl_resource_get_version(res), id);
    if (!rr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(rr, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region  = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wl_compositor_interface,
                                               (int)version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

/* ---------------- xdg-shell ---------------- */

static void xdg_toplevel_destroy_req(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void xdg_toplevel_set_parent(struct wl_client *c, struct wl_resource *r,
                                     struct wl_resource *parent) {
    (void)c; (void)r; (void)parent;
}
static void xdg_toplevel_set_title(struct wl_client *c, struct wl_resource *r,
                                    const char *title) {
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s || !title) return;
    snprintf(s->title, sizeof(s->title), "%s", title);
    if (s->wm_id) wm_set_title(s->wm_id, s->title);
    fprintf(stderr, "[waylandd] xdg_toplevel.set_title: %s\n", s->title);
}
static void xdg_toplevel_set_app_id(struct wl_client *c, struct wl_resource *r,
                                     const char *app_id) {
    (void)c; (void)r; (void)app_id;
}
static void xdg_toplevel_show_window_menu(struct wl_client *c, struct wl_resource *r,
                                            struct wl_resource *seat, uint32_t serial,
                                            int32_t x, int32_t y) {
    (void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y;
}
static void xdg_toplevel_move(struct wl_client *c, struct wl_resource *r,
                               struct wl_resource *seat, uint32_t serial) {
    (void)c; (void)seat; (void)serial;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s || !s->wm_id) return;
    long rc = wm_begin_move(s->wm_id);
    fprintf(stderr, "[waylandd] xdg_toplevel.move wm=%u rc=%ld\n",
            s->wm_id, rc);
}
static void xdg_toplevel_resize(struct wl_client *c, struct wl_resource *r,
                                 struct wl_resource *seat, uint32_t serial,
                                 uint32_t edges) {
    (void)c; (void)seat; (void)serial;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s || !s->wm_id) return;
    uint32_t brook_edges = 0;
    switch (edges) {
    case 1:  brook_edges = BROOK_WM_EDGE_TOP; break;
    case 2:  brook_edges = BROOK_WM_EDGE_BOTTOM; break;
    case 4:  brook_edges = BROOK_WM_EDGE_LEFT; break;
    case 5:  brook_edges = BROOK_WM_EDGE_TOP | BROOK_WM_EDGE_LEFT; break;
    case 6:  brook_edges = BROOK_WM_EDGE_BOTTOM | BROOK_WM_EDGE_LEFT; break;
    case 8:  brook_edges = BROOK_WM_EDGE_RIGHT; break;
    case 9:  brook_edges = BROOK_WM_EDGE_TOP | BROOK_WM_EDGE_RIGHT; break;
    case 10: brook_edges = BROOK_WM_EDGE_BOTTOM | BROOK_WM_EDGE_RIGHT; break;
    default: return;
    }
    long rc = wm_begin_resize(s->wm_id, brook_edges);
    fprintf(stderr, "[waylandd] xdg_toplevel.resize wm=%u edges=%u rc=%ld\n",
            s->wm_id, edges, rc);
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
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s || !s->wm_id) return;
    long rc = wm_set_maximized(s->wm_id, 1);
    fprintf(stderr, "[waylandd] xdg_toplevel.set_maximized wm=%u rc=%ld\n",
            s->wm_id, rc);
}
static void xdg_toplevel_unset_maximized(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s || !s->wm_id) return;
    long rc = wm_set_maximized(s->wm_id, 0);
    fprintf(stderr, "[waylandd] xdg_toplevel.unset_maximized wm=%u rc=%ld\n",
            s->wm_id, rc);
}
static void xdg_toplevel_set_fullscreen(struct wl_client *c, struct wl_resource *r,
                                         struct wl_resource *output) {
    (void)c; (void)r; (void)output;
}
static void xdg_toplevel_unset_fullscreen(struct wl_client *c, struct wl_resource *r) {
    (void)c; (void)r;
}
static void xdg_toplevel_set_minimized(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s || !s->wm_id) return;
    long rc = wm_set_minimized(s->wm_id);
    fprintf(stderr, "[waylandd] xdg_toplevel.set_minimized wm=%u rc=%ld\n",
            s->wm_id, rc);
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

static void xdg_surface_destroy_req(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
void waylandd_send_enter_for_surface(struct wl_resource *surface_resource);

static void xdg_surface_get_toplevel(struct wl_client *c, struct wl_resource *r,
                                       uint32_t id) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    struct wl_resource *tr = wl_resource_create(c, &xdg_toplevel_interface,
                                                 wl_resource_get_version(r), id);
    if (!tr) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(tr, &xdg_toplevel_impl, s, xdg_toplevel_resource_destroy);
    s->xdg_toplevel = tr;
    s->deco_csd = 0;
    s->deco_pending = 1;
    fprintf(stderr, "[waylandd] xdg_surface.get_toplevel id=%u\n", id);
    /* Tell the client this surface is on our output so GDK's
     * window_update_scale finds a monitor and doesn't fall back to a
     * runaway natural-size pass that would clamp at 65535 px. */
    waylandd_send_enter_for_surface(s->resource);
}
/* xdg_popup — minimal: respond to grab/destroy, no nested popups. */
static void xdg_popup_destroy_req(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void xdg_popup_grab(struct wl_client *c, struct wl_resource *r,
                            struct wl_resource *seat, uint32_t serial) {
    (void)c; (void)r; (void)seat; (void)serial;
}
static void xdg_popup_reposition(struct wl_client *c, struct wl_resource *r,
                                    struct wl_resource *positioner,
                                    uint32_t token) {
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    struct brook_positioner *p = positioner ?
        wl_resource_get_user_data(positioner) : NULL;
    if (s) {
        positioner_compute_geometry(p, &s->popup_x, &s->popup_y,
                                    &s->popup_w, &s->popup_h);
        xdg_popup_send_configure(r, s->popup_x, s->popup_y,
                                 s->popup_w, s->popup_h);
        if (s->xdg_surface) {
            uint32_t serial = ++s->next_configure_serial;
            xdg_surface_send_configure(s->xdg_surface, serial);
        }
        popup_apply_position(s);
    }
    xdg_popup_send_repositioned(r, token);
}
static const struct xdg_popup_interface xdg_popup_impl = {
    .destroy    = xdg_popup_destroy_req,
    .grab       = xdg_popup_grab,
    .reposition = xdg_popup_reposition,
};
static void xdg_popup_resource_destroy(struct wl_resource *r) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (s) s->xdg_popup = NULL;
}

static void xdg_surface_get_popup(struct wl_client *c, struct wl_resource *r,
                                    uint32_t id, struct wl_resource *parent,
                                    struct wl_resource *positioner) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s) return;

    /* Capture popup geometry from the positioner. */
    struct brook_positioner *p = positioner ?
        wl_resource_get_user_data(positioner) : NULL;
    positioner_compute_geometry(p, &s->popup_x, &s->popup_y,
                                &s->popup_w, &s->popup_h);
    s->popup_parent = parent ? wl_resource_get_user_data(parent) : NULL;
    s->deco_csd = 1;
    s->deco_pending = 1;

    struct wl_resource *pr = wl_resource_create(c, &xdg_popup_interface,
                                                 wl_resource_get_version(r), id);
    if (!pr) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(pr, &xdg_popup_impl, s, xdg_popup_resource_destroy);
    s->xdg_popup = pr;
    fprintf(stderr, "[waylandd] xdg_surface.get_popup id=%u parent_wm=%u size=%dx%d at (%d,%d)\n",
            id, s->popup_parent ? s->popup_parent->wm_id : 0,
            s->popup_w, s->popup_h, s->popup_x, s->popup_y);
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

/* xdg_positioner — captures size + offset for popup placement. */
static void positioner_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    free(wl_resource_get_user_data(r));
    wl_resource_destroy(r);
}
static void positioner_set_size(struct wl_client *c, struct wl_resource *r,
                                  int32_t w, int32_t h) {
    (void)c;
    struct brook_positioner *p = wl_resource_get_user_data(r);
    if (p) { p->w = w; p->h = h; }
}
static void positioner_set_anchor_rect(struct wl_client *c, struct wl_resource *r,
                                         int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    struct brook_positioner *p = wl_resource_get_user_data(r);
    if (p) { p->ax = x; p->ay = y; p->aw = w; p->ah = h; }
}
static void positioner_set_anchor(struct wl_client *c, struct wl_resource *r,
                                    uint32_t a) {
    (void)c;
    struct brook_positioner *p = wl_resource_get_user_data(r);
    if (p) p->anchor = a;
}
static void positioner_set_gravity(struct wl_client *c, struct wl_resource *r,
                                     uint32_t g) {
    (void)c;
    struct brook_positioner *p = wl_resource_get_user_data(r);
    if (p) p->gravity = g;
}
static void positioner_set_constraint_adjustment(struct wl_client *c, struct wl_resource *r,
                                                    uint32_t v) {
    (void)c;
    struct brook_positioner *p = wl_resource_get_user_data(r);
    if (p) p->constraint = v;
}
static void positioner_set_offset(struct wl_client *c, struct wl_resource *r,
                                    int32_t x, int32_t y) {
    (void)c;
    struct brook_positioner *p = wl_resource_get_user_data(r);
    if (p) { p->ox = x; p->oy = y; }
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
    struct brook_positioner *p = calloc(1, sizeof(*p));
    if (!p) { wl_resource_destroy(pr); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(pr, &positioner_impl, p, NULL);
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
    fprintf(stderr, "[waylandd] client %p destroyed\n", data); fflush(stderr);
    free(l);
}
static void client_created(struct wl_listener *l, void *data) {
    (void)l;
    struct wl_client *c = data;
    struct wl_listener *dl = calloc(1, sizeof(*dl));
    if (!dl) return;
    dl->notify = client_destroyed;
    wl_client_add_destroy_listener(c, dl);
}
static struct wl_listener g_client_create_listener = { .notify = client_created };

/* ---------------- wl_seat / wl_pointer / wl_keyboard ---------------- */

struct brook_seat_client {
    struct wl_resource *seat;
    struct wl_resource *pointer;
    struct wl_resource *keyboard;
    struct wl_client   *client;
    struct brook_surface *entered_surface;  /* pointer enter target */
    struct brook_surface *kb_focus;         /* keyboard focus target */
    int cursor_visible;                     /* policy; kernel is mechanism */
    uint32_t kb_mods_depressed;
    uint32_t kb_mods_locked;
    struct brook_seat_client *next;
};

static struct brook_seat_client *g_seat_clients = NULL;
static uint32_t g_serial = 1;
static uint32_t next_serial(void) { return g_serial++; }

static void seat_clients_scrub_surface(struct brook_surface *s) {
    for (struct brook_seat_client *sc = g_seat_clients; sc; sc = sc->next) {
        if (sc->entered_surface == s) {
            if (!sc->cursor_visible)
                wm_set_cursor_visible(1);
            sc->entered_surface = NULL;
        }
        if (sc->kb_focus == s)        sc->kb_focus = NULL;
    }
}

static void seat_remove_client(struct brook_seat_client *sc) {
    if (sc && sc->entered_surface && !sc->cursor_visible)
        wm_set_cursor_visible(1);
    struct brook_seat_client **pp = &g_seat_clients;
    while (*pp) {
        if (*pp == sc) { *pp = sc->next; free(sc); return; }
        pp = &(*pp)->next;
    }
}
static struct brook_seat_client *seat_for_resource(struct wl_resource *r) {
    for (struct brook_seat_client *sc = g_seat_clients; sc; sc = sc->next) {
        if (sc->seat == r || sc->pointer == r || sc->keyboard == r) return sc;
    }
    return NULL;
}

static void seat_apply_cursor_visibility(struct brook_seat_client *sc) {
    if (!sc || !sc->entered_surface) return;
    long rc = wm_set_cursor_visible(sc->cursor_visible);
    if (rc != 0) {
        fprintf(stderr, "[waylandd] WM_SET_CURSOR_VISIBLE visible=%d failed rc=%ld\n",
                sc->cursor_visible, rc);
    }
}

static void pointer_set_cursor(struct wl_client *c, struct wl_resource *r,
                                uint32_t serial, struct wl_resource *surface,
                                int32_t hx, int32_t hy) {
    (void)c; (void)serial; (void)hx; (void)hy;
    struct brook_seat_client *sc = seat_for_resource(r);
    if (!sc) return;
    sc->cursor_visible = surface != NULL;
    seat_apply_cursor_visibility(sc);
    fprintf(stderr, "[waylandd] wl_pointer.set_cursor visible=%d\n",
            sc->cursor_visible);
}
static void pointer_release(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release    = pointer_release,
};
static void pointer_resource_destroy(struct wl_resource *r) {
    struct brook_seat_client *sc = seat_for_resource(r);
    if (sc && sc->pointer == r) sc->pointer = NULL;
}
static void keyboard_release(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static const struct wl_keyboard_interface keyboard_impl = {
    .release = keyboard_release,
};
static void keyboard_resource_destroy(struct wl_resource *r) {
    struct brook_seat_client *sc = seat_for_resource(r);
    if (sc && sc->keyboard == r) sc->keyboard = NULL;
}

static int make_keymap_fd(size_t *out_size) {
    /* Full US-101 xkb keymap.  evdev keycodes (= scancode + 8).
     * The previous version only declared a-l on the home row -- every
     * other letter, number, and punctuation key reached the client as
     * an unmapped keycode, which xkbcommon translates to NoSymbol and
     * apps like mousepad drop.  Cover the standard typing block. */
    static const char keymap[] =
        "xkb_keymap {\n"
        "xkb_keycodes \"brook\" {\n"
        " minimum = 8; maximum = 255;\n"
        " <ESC>  =  9;\n"
        " <AE01> = 10; <AE02> = 11; <AE03> = 12; <AE04> = 13; <AE05> = 14;\n"
        " <AE06> = 15; <AE07> = 16; <AE08> = 17; <AE09> = 18; <AE10> = 19;\n"
        " <AE11> = 20; <AE12> = 21; <BKSP> = 22;\n"
        " <TAB>  = 23;\n"
        " <AD01> = 24; <AD02> = 25; <AD03> = 26; <AD04> = 27; <AD05> = 28;\n"
        " <AD06> = 29; <AD07> = 30; <AD08> = 31; <AD09> = 32; <AD10> = 33;\n"
        " <AD11> = 34; <AD12> = 35; <RTRN> = 36;\n"
        " <LCTL> = 37;\n"
        " <AC01> = 38; <AC02> = 39; <AC03> = 40; <AC04> = 41; <AC05> = 42;\n"
        " <AC06> = 43; <AC07> = 44; <AC08> = 45; <AC09> = 46; <AC10> = 47;\n"
        " <AC11> = 48; <TLDE> = 49;\n"
        " <LFSH> = 50; <BKSL> = 51;\n"
        " <AB01> = 52; <AB02> = 53; <AB03> = 54; <AB04> = 55; <AB05> = 56;\n"
        " <AB06> = 57; <AB07> = 58; <AB08> = 59; <AB09> = 60; <AB10> = 61;\n"
        " <RTSH> = 62; <KPMU> = 63;\n"
        " <LALT> = 64; <SPCE> = 65; <CAPS> = 66;\n"
        " <FK01> = 67; <FK02> = 68; <FK03> = 69; <FK04> = 70; <FK05> = 71;\n"
        " <FK06> = 72; <FK07> = 73; <FK08> = 74; <FK09> = 75; <FK10> = 76;\n"
        " <NMLK> = 77; <SCLK> = 78;\n"
        " <KP7>  = 79; <KP8>  = 80; <KP9>  = 81; <KPSU> = 82;\n"
        " <KP4>  = 83; <KP5>  = 84; <KP6>  = 85; <KPAD> = 86;\n"
        " <KP1>  = 87; <KP2>  = 88; <KP3>  = 89; <KP0>  = 90; <KPDL> = 91;\n"
        " <FK11> = 95; <FK12> = 96;\n"
        " <KPEN> = 104; <RCTL> = 105; <KPDV> = 106;\n"
        " <PRSC> = 107; <RALT> = 108;\n"
        " <HOME> = 110; <UP>   = 111; <PGUP> = 112;\n"
        " <LEFT> = 113; <RGHT> = 114;\n"
        " <END>  = 115; <DOWN> = 116; <PGDN> = 117;\n"
        " <INS>  = 118; <DELE> = 119;\n"
        " <PAUS> = 127;\n"
        " <LWIN> = 133; <RWIN> = 134; <MENU> = 135;\n"
        "};\n"
        "xkb_types \"brook\" {\n"
        " virtual_modifiers NumLock,Alt,LevelThree,LAlt,RAlt,RControl,LControl,ScrollLock,LevelFive,AltGr,Meta,Super,Hyper;\n"
        " type \"ONE_LEVEL\" { modifiers = none; level_name[Level1] = \"Any\"; };\n"
        " type \"TWO_LEVEL\" {\n"
        "   modifiers = Shift;\n"
        "   map[Shift] = Level2;\n"
        "   level_name[Level1] = \"Base\"; level_name[Level2] = \"Shift\";\n"
        " };\n"
        " type \"ALPHABETIC\" {\n"
        "   modifiers = Shift+Lock;\n"
        "   map[Shift] = Level2; map[Lock] = Level2;\n"
        "   level_name[Level1] = \"Base\"; level_name[Level2] = \"Caps\";\n"
        " };\n"
        " type \"KEYPAD\" {\n"
        "   modifiers = NumLock+Shift;\n"
        "   map[Shift] = Level2; map[NumLock] = Level2;\n"
        "   level_name[Level1] = \"Base\"; level_name[Level2] = \"Number\";\n"
        " };\n"
        "};\n"
        "xkb_compatibility \"brook\" {\n"
        " virtual_modifiers NumLock,Alt,LevelThree,LAlt,RAlt,RControl,LControl,ScrollLock,LevelFive,AltGr,Meta,Super,Hyper;\n"
        " interpret Any+AnyOf(all) { action = SetMods(modifiers=modMapMods,clearLocks); };\n"
        " interpret Caps_Lock+AnyOf(all) { action= LockMods(modifiers=Lock); };\n"
        " interpret Num_Lock+AnyOf(all) { action= LockMods(modifiers=NumLock); };\n"
        "};\n"
        "xkb_symbols \"brook\" {\n"
        " name[Group1] = \"Brook US\";\n"
        " key <ESC>  { [ Escape ] };\n"
        " key <AE01> { type=\"TWO_LEVEL\", [ 1, exclam       ] };\n"
        " key <AE02> { type=\"TWO_LEVEL\", [ 2, at           ] };\n"
        " key <AE03> { type=\"TWO_LEVEL\", [ 3, numbersign   ] };\n"
        " key <AE04> { type=\"TWO_LEVEL\", [ 4, dollar       ] };\n"
        " key <AE05> { type=\"TWO_LEVEL\", [ 5, percent      ] };\n"
        " key <AE06> { type=\"TWO_LEVEL\", [ 6, asciicircum  ] };\n"
        " key <AE07> { type=\"TWO_LEVEL\", [ 7, ampersand    ] };\n"
        " key <AE08> { type=\"TWO_LEVEL\", [ 8, asterisk     ] };\n"
        " key <AE09> { type=\"TWO_LEVEL\", [ 9, parenleft    ] };\n"
        " key <AE10> { type=\"TWO_LEVEL\", [ 0, parenright   ] };\n"
        " key <AE11> { type=\"TWO_LEVEL\", [ minus, underscore ] };\n"
        " key <AE12> { type=\"TWO_LEVEL\", [ equal, plus     ] };\n"
        " key <BKSP> { [ BackSpace ] };\n"
        " key <TAB>  { [ Tab, ISO_Left_Tab ] };\n"
        " key <AD01> { type=\"ALPHABETIC\", [ q, Q ] };\n"
        " key <AD02> { type=\"ALPHABETIC\", [ w, W ] };\n"
        " key <AD03> { type=\"ALPHABETIC\", [ e, E ] };\n"
        " key <AD04> { type=\"ALPHABETIC\", [ r, R ] };\n"
        " key <AD05> { type=\"ALPHABETIC\", [ t, T ] };\n"
        " key <AD06> { type=\"ALPHABETIC\", [ y, Y ] };\n"
        " key <AD07> { type=\"ALPHABETIC\", [ u, U ] };\n"
        " key <AD08> { type=\"ALPHABETIC\", [ i, I ] };\n"
        " key <AD09> { type=\"ALPHABETIC\", [ o, O ] };\n"
        " key <AD10> { type=\"ALPHABETIC\", [ p, P ] };\n"
        " key <AD11> { type=\"TWO_LEVEL\", [ bracketleft, braceleft   ] };\n"
        " key <AD12> { type=\"TWO_LEVEL\", [ bracketright, braceright ] };\n"
        " key <RTRN> { [ Return ] };\n"
        " key <LCTL> { [ Control_L ] };\n"
        " key <AC01> { type=\"ALPHABETIC\", [ a, A ] };\n"
        " key <AC02> { type=\"ALPHABETIC\", [ s, S ] };\n"
        " key <AC03> { type=\"ALPHABETIC\", [ d, D ] };\n"
        " key <AC04> { type=\"ALPHABETIC\", [ f, F ] };\n"
        " key <AC05> { type=\"ALPHABETIC\", [ g, G ] };\n"
        " key <AC06> { type=\"ALPHABETIC\", [ h, H ] };\n"
        " key <AC07> { type=\"ALPHABETIC\", [ j, J ] };\n"
        " key <AC08> { type=\"ALPHABETIC\", [ k, K ] };\n"
        " key <AC09> { type=\"ALPHABETIC\", [ l, L ] };\n"
        " key <AC10> { type=\"TWO_LEVEL\", [ semicolon, colon  ] };\n"
        " key <AC11> { type=\"TWO_LEVEL\", [ apostrophe, quotedbl ] };\n"
        " key <TLDE> { type=\"TWO_LEVEL\", [ grave, asciitilde ] };\n"
        " key <LFSH> { [ Shift_L ] };\n"
        " key <BKSL> { type=\"TWO_LEVEL\", [ backslash, bar ] };\n"
        " key <AB01> { type=\"ALPHABETIC\", [ z, Z ] };\n"
        " key <AB02> { type=\"ALPHABETIC\", [ x, X ] };\n"
        " key <AB03> { type=\"ALPHABETIC\", [ c, C ] };\n"
        " key <AB04> { type=\"ALPHABETIC\", [ v, V ] };\n"
        " key <AB05> { type=\"ALPHABETIC\", [ b, B ] };\n"
        " key <AB06> { type=\"ALPHABETIC\", [ n, N ] };\n"
        " key <AB07> { type=\"ALPHABETIC\", [ m, M ] };\n"
        " key <AB08> { type=\"TWO_LEVEL\", [ comma, less    ] };\n"
        " key <AB09> { type=\"TWO_LEVEL\", [ period, greater ] };\n"
        " key <AB10> { type=\"TWO_LEVEL\", [ slash, question ] };\n"
        " key <RTSH> { [ Shift_R ] };\n"
        " key <KPMU> { [ KP_Multiply ] };\n"
        " key <LALT> { [ Alt_L ] };\n"
        " key <SPCE> { [ space ] };\n"
        " key <CAPS> { [ Caps_Lock ] };\n"
        " key <FK01> { [ F1 ] }; key <FK02> { [ F2 ] };\n"
        " key <FK03> { [ F3 ] }; key <FK04> { [ F4 ] };\n"
        " key <FK05> { [ F5 ] }; key <FK06> { [ F6 ] };\n"
        " key <FK07> { [ F7 ] }; key <FK08> { [ F8 ] };\n"
        " key <FK09> { [ F9 ] }; key <FK10> { [ F10 ] };\n"
        " key <FK11> { [ F11 ] }; key <FK12> { [ F12 ] };\n"
        " key <NMLK> { [ Num_Lock ] };\n"
        " key <SCLK> { [ Scroll_Lock ] };\n"
        " key <KP0>  { type=\"KEYPAD\", [ KP_Insert,    KP_0 ] };\n"
        " key <KP1>  { type=\"KEYPAD\", [ KP_End,       KP_1 ] };\n"
        " key <KP2>  { type=\"KEYPAD\", [ KP_Down,      KP_2 ] };\n"
        " key <KP3>  { type=\"KEYPAD\", [ KP_Next,      KP_3 ] };\n"
        " key <KP4>  { type=\"KEYPAD\", [ KP_Left,      KP_4 ] };\n"
        " key <KP5>  { type=\"KEYPAD\", [ KP_Begin,     KP_5 ] };\n"
        " key <KP6>  { type=\"KEYPAD\", [ KP_Right,     KP_6 ] };\n"
        " key <KP7>  { type=\"KEYPAD\", [ KP_Home,      KP_7 ] };\n"
        " key <KP8>  { type=\"KEYPAD\", [ KP_Up,        KP_8 ] };\n"
        " key <KP9>  { type=\"KEYPAD\", [ KP_Prior,     KP_9 ] };\n"
        " key <KPDL> { type=\"KEYPAD\", [ KP_Delete, KP_Decimal ] };\n"
        " key <KPSU> { [ KP_Subtract ] };\n"
        " key <KPAD> { [ KP_Add ] };\n"
        " key <KPDV> { [ KP_Divide ] };\n"
        " key <KPEN> { [ KP_Enter ] };\n"
        " key <RCTL> { [ Control_R ] };\n"
        " key <RALT> { [ Alt_R ] };\n"
        " key <PRSC> { [ Print ] };\n"
        " key <PAUS> { [ Pause ] };\n"
        " key <HOME> { [ Home ] }; key <END>  { [ End  ] };\n"
        " key <PGUP> { [ Prior ] }; key <PGDN> { [ Next ] };\n"
        " key <INS>  { [ Insert ] }; key <DELE> { [ Delete ] };\n"
        " key <UP>   { [ Up    ] }; key <DOWN> { [ Down  ] };\n"
        " key <LEFT> { [ Left  ] }; key <RGHT> { [ Right ] };\n"
        " key <LWIN> { [ Super_L ] }; key <RWIN> { [ Super_R ] };\n"
        " key <MENU> { [ Menu ] };\n"
        " modifier_map Shift   { Shift_L, Shift_R };\n"
        " modifier_map Control { Control_L, Control_R };\n"
        " modifier_map Mod1    { Alt_L, Alt_R };\n"
        " modifier_map Mod4    { Super_L, Super_R };\n"
        " modifier_map Lock    { Caps_Lock };\n"
        " modifier_map Mod2    { Num_Lock };\n"
        "};\n"
        "};\n";
    size_t len = sizeof(keymap);
    int fd = syscall(SYS_memfd_create, "brook-keymap", 0u);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)len) < 0) { close(fd); return -1; }
    void *p = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return -1; }
    memcpy(p, keymap, len);
    munmap(p, len);
    *out_size = len;
    return fd;
}

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct brook_seat_client *sc = seat_for_resource(r);
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface,
                                                wl_resource_get_version(r), id);
    if (!p) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(p, &pointer_impl, sc, pointer_resource_destroy);
    if (sc) sc->pointer = p;
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct brook_seat_client *sc = seat_for_resource(r);
    struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface,
                                                wl_resource_get_version(r), id);
    if (!k) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(k, &keyboard_impl, sc, keyboard_resource_destroy);
    if (sc) sc->keyboard = k;

    size_t kmsize = 0;
    int kfd = make_keymap_fd(&kmsize);
    if (kfd >= 0) {
        wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                kfd, (uint32_t)kmsize);
        close(kfd);
    } else {
        wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, -1, 0);
    }
    if (wl_resource_get_version(k) >= 4)
        wl_keyboard_send_repeat_info(k, 25, 600);
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *t = wl_resource_create(c, &wl_touch_interface,
                                                wl_resource_get_version(r), id);
    if (!t) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(t, NULL, NULL, NULL);
}
static void seat_release(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static const struct wl_seat_interface seat_impl = {
    .get_pointer  = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch    = seat_get_touch,
    .release      = seat_release,
};
static void seat_resource_destroy(struct wl_resource *r) {
    struct brook_seat_client *sc = seat_for_resource(r);
    if (sc && sc->seat == r) {
        sc->seat = NULL;
        if (!sc->pointer && !sc->keyboard) seat_remove_client(sc);
    }
}

static void seat_bind(struct wl_client *client, void *data,
                      uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wl_seat_interface,
                                                (int)version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    struct brook_seat_client *sc = calloc(1, sizeof(*sc));
    if (!sc) { wl_resource_destroy(r); wl_client_post_no_memory(client); return; }
    sc->seat = r;
    sc->client = client;
    sc->cursor_visible = 1;
    sc->next = g_seat_clients;
    g_seat_clients = sc;
    wl_resource_set_implementation(r, &seat_impl, sc, seat_resource_destroy);
    wl_seat_send_capabilities(r,
        WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (version >= 2)
        wl_seat_send_name(r, "brook-seat");
}

/* wl_output: single output. We track every bound output resource so that
 * we can send wl_surface.enter to associate surfaces with their output —
 * GDK/GTK refuse to compute a sane natural size for a toplevel that
 * isn't on any monitor, and end up clamping at 65535 px. */
struct brook_output {
    struct wl_resource *resource;
    struct wl_listener  destroy;
    struct brook_output *next;
};
static struct brook_output *g_outputs = NULL;

static void output_resource_destroyed(struct wl_listener *l, void *data) {
    (void)data;
    struct brook_output *o = wl_container_of(l, o, destroy);
    struct brook_output **pp = &g_outputs;
    while (*pp) {
        if (*pp == o) { *pp = o->next; break; }
        pp = &(*pp)->next;
    }
    free(o);
}

void waylandd_send_enter_for_surface(struct wl_resource *surface_resource);
void waylandd_send_enter_for_surface(struct wl_resource *surface_resource) {
    if (!surface_resource) return;
    struct wl_client *sc = wl_resource_get_client(surface_resource);
    for (struct brook_output *o = g_outputs; o; o = o->next) {
        if (wl_resource_get_client(o->resource) == sc) {
            wl_surface_send_enter(surface_resource, o->resource);
            fprintf(stderr, "[waylandd] wl_surface.enter sent (output res=%p)\n",
                    (void*)o->resource);
        }
    }
}

static void output_release(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static const struct wl_output_interface output_impl = {
    .release = output_release,
};
static void output_bind(struct wl_client *client, void *data,
                        uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wl_output_interface,
                                                (int)version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &output_impl, NULL, NULL);
    fprintf(stderr, "[waylandd] wl_output bind v=%u id=%u client=%p\n",
            version, id, (void*)client);
    struct brook_output *o = calloc(1, sizeof(*o));
    if (o) {
        o->resource = r;
        o->destroy.notify = output_resource_destroyed;
        wl_resource_add_destroy_listener(r, &o->destroy);
        o->next = g_outputs;
        g_outputs = o;
    }
    int w = 1920, h = 1080;
    wl_output_send_geometry(r, 0, 0,
        (int)((w * 254 + 480) / 960), (int)((h * 254 + 480) / 960),
        WL_OUTPUT_SUBPIXEL_UNKNOWN, "Brook", "vfb-0",
        WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        w, h, 60000);
    if (version >= 2) {
        wl_output_send_scale(r, 1);
        wl_output_send_done(r);
    }
}

/* PS/2 scancode set 1 → XKB keycode (keycode = scancode + 8). */
/* Wayland convention: wl_keyboard.key carries an evdev keycode.  Clients
 * then add 8 to look up the xkb keymap (which uses xkb keycodes =
 * evdev + 8).  Our keymap declares AD01=24, AD02=25, ... = evdev + 8,
 * so we must hand the client a *bare* evdev keycode here and let it
 * do the +8 itself.  PS/2 set-1 scan codes 0x00-0x35 happen to match
 * Linux evdev keycodes for the typing block 1:1 (KEY_ESC=1, KEY_Q=16,
 * etc.), so the typing block needs no translation.  0x80-0x89 are
 * Brook's synthetic E0-extended codes (see input.h: SC_EXT_*); map
 * those to their proper evdev keycodes. */
static uint32_t scancode_to_xkb(uint8_t sc) {
    switch (sc) {
    case 0x80: return 103; /* SC_EXT_UP    -> KEY_UP */
    case 0x81: return 108; /* SC_EXT_DOWN  -> KEY_DOWN */
    case 0x82: return 105; /* SC_EXT_LEFT  -> KEY_LEFT */
    case 0x83: return 106; /* SC_EXT_RIGHT -> KEY_RIGHT */
    case 0x84: return 102; /* SC_EXT_HOME  -> KEY_HOME */
    case 0x85: return 107; /* SC_EXT_END   -> KEY_END */
    case 0x86: return 110; /* SC_EXT_INSERT-> KEY_INSERT */
    case 0x87: return 111; /* SC_EXT_DELETE-> KEY_DELETE */
    case 0x88: return 104; /* SC_EXT_PGUP  -> KEY_PAGEUP */
    case 0x89: return 109; /* SC_EXT_PGDN  -> KEY_PAGEDOWN */
    default:   return (uint32_t)sc;
    }
}

/* ---------------- input pump ---------------- */

/* Find the seat client owned by the same wl_client that owns `surf`. */
static struct brook_seat_client *seat_for_surface(struct brook_surface *surf) {
    if (!surf || !surf->resource) return NULL;
    struct wl_client *owner = wl_resource_get_client(surf->resource);
    for (struct brook_seat_client *sc = g_seat_clients; sc; sc = sc->next)
        if (sc->client == owner) return sc;
    return NULL;
}

static void pointer_enter_if_needed(struct brook_seat_client *sc,
                                     struct brook_surface *s,
                                     int lx, int ly) {
    if (!sc || !sc->pointer) return;
    if (sc->entered_surface == s) return;
    if (sc->entered_surface && sc->entered_surface != s) {
        /* Defensive: only send leave if entered_surface still belongs to
         * this seat's client.  If a previous surface was orphaned (client
         * destroyed without scrubbing in time, or cross-client
         * mis-tracking), libwayland aborts on a leave with an object from
         * a different client.  Drop silently in that case. */
        struct wl_resource *prev = sc->entered_surface->resource;
        if (prev && wl_resource_get_client(prev) == sc->client) {
            wl_pointer_send_leave(sc->pointer, next_serial(), prev);
            if (wl_resource_get_version(sc->pointer) >= 5)
                wl_pointer_send_frame(sc->pointer);
        }
    }
    if (s) {
        wl_pointer_send_enter(sc->pointer, next_serial(), s->resource,
                              wl_fixed_from_int(lx), wl_fixed_from_int(ly));
        if (wl_resource_get_version(sc->pointer) >= 5)
            wl_pointer_send_frame(sc->pointer);
    }
    sc->entered_surface = s;
    seat_apply_cursor_visibility(sc);
}

static void pump_input_for_surface(struct brook_surface *s) {
    if (!s->wm_id) return;
    struct brook_wm_event evs[16];
    long n = wm_pop_input(s->wm_id, evs, 16);
    if (n <= 0) return;

    struct brook_seat_client *sc = seat_for_surface(s);
    uint32_t now = g_now_ms();

    for (long i = 0; i < n; ++i) {
        struct brook_wm_event *e = &evs[i];
        switch (e->type) {
        case EVT_MOUSE_MOVE: {
            if (!sc || !sc->pointer) break;
            pointer_enter_if_needed(sc, s, e->x, e->y);
            wl_pointer_send_motion(sc->pointer, now,
                                   wl_fixed_from_int(e->x),
                                   wl_fixed_from_int(e->y));
            if (wl_resource_get_version(sc->pointer) >= 5)
                wl_pointer_send_frame(sc->pointer);
            break;
        }
        case EVT_MOUSE_BTN_DOWN:
        case EVT_MOUSE_BTN_UP: {
            if (!sc || !sc->pointer) break;
            pointer_enter_if_needed(sc, s, e->x, e->y);
            /* scan: 0=left, 1=right, 2=middle  →  BTN_LEFT 0x110, etc. */
            uint32_t btn = 0x110 + (e->scan & 0x3);
            uint32_t st  = (e->type == EVT_MOUSE_BTN_DOWN)
                             ? WL_POINTER_BUTTON_STATE_PRESSED
                             : WL_POINTER_BUTTON_STATE_RELEASED;
            wl_pointer_send_button(sc->pointer, next_serial(), now, btn, st);
            if (wl_resource_get_version(sc->pointer) >= 5)
                wl_pointer_send_frame(sc->pointer);
            break;
        }
        case EVT_MOUSE_SCROLL: {
            if (!sc || !sc->pointer) break;
            int32_t dy = (int8_t)e->scan;
            wl_pointer_send_axis(sc->pointer, now,
                                 WL_POINTER_AXIS_VERTICAL_SCROLL,
                                 wl_fixed_from_int(dy * 10));
            if (wl_resource_get_version(sc->pointer) >= 5)
                wl_pointer_send_frame(sc->pointer);
            break;
        }
        case EVT_KEY_PRESS:
        case EVT_KEY_RELEASE: {
            if (!sc || !sc->keyboard) break;
            uint32_t st = (e->type == EVT_KEY_PRESS)
                            ? WL_KEYBOARD_KEY_STATE_PRESSED
                            : WL_KEYBOARD_KEY_STATE_RELEASED;
            /* Translate kernel's modifier bitmask to xkb modifier indices.
             * Order matches our keymap's modifier_map: Shift=0, Lock=1,
             * Control=2, Mod1(Alt)=3.  Without this, clients (e.g. GTK
             * via xkbcommon) never see Shift held and silently drop
             * shifted keys. */
            uint32_t depressed = 0;
            uint32_t locked    = 0;
            if (e->mods & 0x03) depressed |= (1u << 0); /* Shift */
            if (e->mods & 0x04) depressed |= (1u << 2); /* Control */
            if (e->mods & 0x08) depressed |= (1u << 3); /* Mod1/Alt */
            if (e->mods & 0x10) locked    |= (1u << 1); /* Lock/Caps */
            if (depressed != sc->kb_mods_depressed ||
                locked    != sc->kb_mods_locked) {
                wl_keyboard_send_modifiers(sc->keyboard, next_serial(),
                                           depressed, 0u, locked, 0u);
                sc->kb_mods_depressed = depressed;
                sc->kb_mods_locked    = locked;
            }
            wl_keyboard_send_key(sc->keyboard, next_serial(), now,
                                 scancode_to_xkb(e->scan), st);
            break;
        }
        case WM_EVT_CLOSE_REQUESTED: {
            if (s->xdg_toplevel) {
                fprintf(stderr, "[waylandd] CLOSE_REQUESTED → xdg_toplevel.send_close (wm=%u)\n",
                        s->wm_id);
                xdg_toplevel_send_close(s->xdg_toplevel);
            }
            break;
        }
        case WM_EVT_FOCUS_GAINED: {
            if (sc && sc->keyboard && sc->kb_focus != s) {
                struct wl_array keys; wl_array_init(&keys);
                wl_keyboard_send_enter(sc->keyboard, next_serial(),
                                       s->resource, &keys);
                wl_array_release(&keys);
                sc->kb_focus = s;
                fprintf(stderr, "[waylandd] FOCUS_GAINED wm=%u\n", s->wm_id);
            }
            break;
        }
        case WM_EVT_FOCUS_LOST: {
            if (sc && sc->keyboard && sc->kb_focus == s) {
                wl_keyboard_send_leave(sc->keyboard, next_serial(), s->resource);
                sc->kb_focus = NULL;
                fprintf(stderr, "[waylandd] FOCUS_LOST wm=%u\n", s->wm_id);
            }
            break;
        }
        case WM_EVT_RESIZED: {
            /* The kernel WM has resized this window's chrome (drag-resize
             * release). Tell the client via xdg_toplevel.configure with
             * the new size — the client will allocate a fresh buffer at
             * those dimensions, ack_configure, and commit. */
            if (s->xdg_toplevel && s->xdg_surface) {
                int32_t neww = e->x;
                int32_t newh = e->y;
                struct wl_array states; wl_array_init(&states);
                /* Tell the client this is an active toplevel; resize is a
                 * normal configure, not a fullscreen/maximize. */
                if (wl_resource_get_version(s->xdg_toplevel) >= 2) {
                    uint32_t *st = wl_array_add(&states, sizeof(uint32_t));
                    if (st) *st = XDG_TOPLEVEL_STATE_ACTIVATED;
                }
                xdg_toplevel_send_configure(s->xdg_toplevel, neww, newh, &states);
                wl_array_release(&states);
                uint32_t serial = next_serial();
                s->next_configure_serial = serial;
                s->xdg_acked = 0;
                xdg_surface_send_configure(s->xdg_surface, serial);
                fprintf(stderr, "[waylandd] WM_EVT_RESIZED wm=%u → configure %dx%d serial=%u\n",
                        s->wm_id, neww, newh, serial);
            }
            break;
        }
        default: break;
        }
    }
}

static void pump_input_once(void) {
    for (struct brook_surface *s = g_surfaces; s; s = s->next)
        pump_input_for_surface(s);
}

/* ---------------- xdg-decoration-unstable-v1 ----------------
 *
 * zxdg_toplevel_decoration_v1 — server-managed decoration negotiation.
 *
 * Brook defaults normal toplevels to server-side decoration so clients
 * that do not draw reliable CSD still get kernel chrome.  Clients may
 * explicitly request client-side decoration; popups are always no-chrome.
 */

static void deco_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void deco_apply_mode(struct wl_resource *r, int csd) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->deco_csd = csd;
    if (s->wm_id) {
        long rc = wm_set_decoration_mode(s->wm_id, csd);
        fprintf(stderr, "[waylandd] decoration wm=%u mode=%s rc=%ld\n",
                s->wm_id, csd ? "client_side" : "server_side", rc);
        s->deco_pending = 0;
    } else {
        fprintf(stderr, "[waylandd] decoration pending mode=%s\n",
                csd ? "client_side" : "server_side");
        s->deco_pending = 1;
    }
    zxdg_toplevel_decoration_v1_send_configure(r,
        csd ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
            : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void deco_set_mode(struct wl_client *c, struct wl_resource *r, uint32_t mode) {
    (void)c;
    fprintf(stderr, "[waylandd] decoration set_mode=%u\n", mode);
    deco_apply_mode(r, mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ? 1 : 0);
}
static void deco_unset_mode(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    /* Spec: "client requests to use the surface's current mode without
     * a preference".  Keep Brook's server-side default. */
    deco_apply_mode(r, 0);
}
static const struct zxdg_toplevel_decoration_v1_interface deco_impl = {
    .destroy    = deco_destroy,
    .set_mode   = deco_set_mode,
    .unset_mode = deco_unset_mode,
};

static void deco_mgr_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void deco_mgr_get_toplevel_decoration(struct wl_client *c,
                                              struct wl_resource *r,
                                              uint32_t id,
                                              struct wl_resource *toplevel) {
    (void)r;
    struct brook_surface *s = toplevel ? wl_resource_get_user_data(toplevel) : NULL;
    struct wl_resource *d = wl_resource_create(c,
        &zxdg_toplevel_decoration_v1_interface,
        wl_resource_get_version(r), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &deco_impl, s, NULL);
    fprintf(stderr, "[waylandd] decoration get_toplevel_decoration surface=%p\n",
            (void*)s);
    /* Default to CSD: the client opted in by binding the manager. */
    deco_apply_mode(d, 1);
}
static const struct zxdg_decoration_manager_v1_interface deco_mgr_impl = {
    .destroy                = deco_mgr_destroy,
    .get_toplevel_decoration = deco_mgr_get_toplevel_decoration,
};

static void deco_mgr_bind(struct wl_client *client, void *data,
                          uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(client,
        &zxdg_decoration_manager_v1_interface, (int)version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &deco_mgr_impl, NULL, NULL);
    fprintf(stderr, "[waylandd] decoration manager bind v=%u id=%u\n",
            version, id);
}

/* ---------------- wl_subcompositor (stub) ---------------- */
static void subsurface_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void subsurface_set_position(struct wl_client *c, struct wl_resource *r,
                                    int32_t x, int32_t y) {
    (void)c; (void)r; (void)x; (void)y;
}
static void subsurface_place_above(struct wl_client *c, struct wl_resource *r,
                                   struct wl_resource *sib) {
    (void)c; (void)r; (void)sib;
}
static void subsurface_place_below(struct wl_client *c, struct wl_resource *r,
                                   struct wl_resource *sib) {
    (void)c; (void)r; (void)sib;
}
static void subsurface_set_sync(struct wl_client *c, struct wl_resource *r) {
    (void)c; (void)r;
}
static void subsurface_set_desync(struct wl_client *c, struct wl_resource *r) {
    (void)c; (void)r;
}
static const struct wl_subsurface_interface subsurface_impl = {
    .destroy      = subsurface_destroy,
    .set_position = subsurface_set_position,
    .place_above  = subsurface_place_above,
    .place_below  = subsurface_place_below,
    .set_sync     = subsurface_set_sync,
    .set_desync   = subsurface_set_desync,
};

static void subcomp_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void subcomp_get_subsurface(struct wl_client *c, struct wl_resource *r,
                                    uint32_t id,
                                    struct wl_resource *surface,
                                    struct wl_resource *parent) {
    (void)r; (void)surface; (void)parent;
    struct wl_resource *ss = wl_resource_create(c, &wl_subsurface_interface,
                                                 wl_resource_get_version(r), id);
    if (!ss) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(ss, &subsurface_impl, NULL, NULL);
}
static const struct wl_subcompositor_interface subcomp_impl = {
    .destroy        = subcomp_destroy,
    .get_subsurface = subcomp_get_subsurface,
};
static void subcomp_bind(struct wl_client *client, void *data,
                         uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wl_subcompositor_interface,
                                                (int)version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &subcomp_impl, NULL, NULL);
}

/* ---------------- wl_data_device_manager (stub clipboard) ---------------- */
static void data_offer_accept(struct wl_client *c, struct wl_resource *r,
                              uint32_t serial, const char *mt) {
    (void)c; (void)r; (void)serial; (void)mt;
}
static void data_offer_receive(struct wl_client *c, struct wl_resource *r,
                               const char *mt, int32_t fd) {
    (void)c; (void)r; (void)mt; close(fd);
}
static void data_offer_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void data_offer_finish(struct wl_client *c, struct wl_resource *r) {
    (void)c; (void)r;
}
static void data_offer_set_actions(struct wl_client *c, struct wl_resource *r,
                                   uint32_t da, uint32_t pa) {
    (void)c; (void)r; (void)da; (void)pa;
}
static const struct wl_data_offer_interface data_offer_impl = {
    .accept      = data_offer_accept,
    .receive     = data_offer_receive,
    .destroy     = data_offer_destroy,
    .finish      = data_offer_finish,
    .set_actions = data_offer_set_actions,
};

static void data_source_offer(struct wl_client *c, struct wl_resource *r,
                              const char *mt) {
    (void)c; (void)r; (void)mt;
}
static void data_source_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void data_source_set_actions(struct wl_client *c, struct wl_resource *r,
                                    uint32_t da) {
    (void)c; (void)r; (void)da;
}
static const struct wl_data_source_interface data_source_impl = {
    .offer       = data_source_offer,
    .destroy     = data_source_destroy,
    .set_actions = data_source_set_actions,
};

static void data_device_start_drag(struct wl_client *c, struct wl_resource *r,
                                   struct wl_resource *src,
                                   struct wl_resource *origin,
                                   struct wl_resource *icon, uint32_t serial) {
    (void)c; (void)r; (void)src; (void)origin; (void)icon; (void)serial;
}
static void data_device_set_selection(struct wl_client *c, struct wl_resource *r,
                                      struct wl_resource *src, uint32_t serial) {
    (void)c; (void)r; (void)src; (void)serial;
}
static void data_device_release(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static const struct wl_data_device_interface data_device_impl = {
    .start_drag    = data_device_start_drag,
    .set_selection = data_device_set_selection,
    .release       = data_device_release,
};

static void ddm_create_data_source(struct wl_client *c, struct wl_resource *r,
                                    uint32_t id) {
    struct wl_resource *s = wl_resource_create(c, &wl_data_source_interface,
                                                wl_resource_get_version(r), id);
    if (!s) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(s, &data_source_impl, NULL, NULL);
}
static void ddm_get_data_device(struct wl_client *c, struct wl_resource *r,
                                 uint32_t id, struct wl_resource *seat) {
    (void)seat;
    struct wl_resource *d = wl_resource_create(c, &wl_data_device_interface,
                                                wl_resource_get_version(r), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &data_device_impl, NULL, NULL);
}
static const struct wl_data_device_manager_interface ddm_impl = {
    .create_data_source = ddm_create_data_source,
    .get_data_device    = ddm_get_data_device,
};
static void ddm_bind(struct wl_client *client, void *data,
                     uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(client,
        &wl_data_device_manager_interface, (int)version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &ddm_impl, NULL, NULL);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    fprintf(stderr, "[waylandd] starting (Phase D — kernel-WM unified)\n");
    int run_seconds = 0;
    for (int i = 1; i < argc; ++i) {
        if ((!strcmp(argv[i], "--for") || !strcmp(argv[i], "--seconds"))
            && i + 1 < argc)
            run_seconds = atoi(argv[++i]);
    }

    g_display = wl_display_create();
    if (!g_display) {
        fprintf(stderr, "[waylandd] FAIL: wl_display_create returned NULL\n");
        return 1;
    }

    wl_log_set_handler_server(brook_wl_log);
    wl_display_add_client_created_listener(g_display, &g_client_create_listener);

    const char *sock_name = getenv("WAYLAND_DISPLAY");
    if (!sock_name) sock_name = "wayland-0";

    const char *sock = NULL;
    if (getenv("XDG_RUNTIME_DIR")) {
        if (wl_display_add_socket(g_display, sock_name) == 0) sock = sock_name;
    }
    if (!sock) {
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
            fprintf(stderr, "[waylandd] FAIL: bind %s: %s\n", a.sun_path, strerror(errno));
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
        fprintf(stderr, "[waylandd] bound explicit socket at %s\n", a.sun_path);
        sock = sock_name;
    }
    fprintf(stderr, "[waylandd] listening on WAYLAND_DISPLAY=%s\n", sock);

    if (wl_display_init_shm(g_display) < 0)
        fprintf(stderr, "[waylandd] WARN: wl_display_init_shm failed\n");

    if (!wl_global_create(g_display, &wl_compositor_interface, 4, NULL, compositor_bind)) return 1;
    if (!wl_global_create(g_display, &xdg_wm_base_interface, 3, NULL, wm_base_bind)) return 1;
    if (!wl_global_create(g_display, &wl_seat_interface, 5, NULL, seat_bind)) return 1;
    if (!wl_global_create(g_display, &wl_output_interface, 3, NULL, output_bind)) return 1;
    if (!wl_global_create(g_display, &zxdg_decoration_manager_v1_interface, 1, NULL, deco_mgr_bind)) return 1;
    if (!wl_global_create(g_display, &wl_subcompositor_interface, 1, NULL, subcomp_bind)) return 1;
    if (!wl_global_create(g_display, &wl_data_device_manager_interface, 3, NULL, ddm_bind)) return 1;
    fprintf(stderr, "[waylandd] globals advertised\n");

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    struct wl_event_loop *loop = wl_display_get_event_loop(g_display);
    if (!loop) return 1;

    uint32_t loop_start = g_now_ms();
    for (;;) {
        if (g_shutdown) break;
        wl_display_flush_clients(g_display);
        for (int sub = 0; sub < 60 && !g_shutdown; ++sub) {
            pump_input_once();
            int n = wl_event_loop_dispatch(loop, 16);
            if (n < 0) {
                fprintf(stderr, "[waylandd] event loop error: %s\n", strerror(errno));
                goto done;
            }
            wl_display_flush_clients(g_display);
        }
        if (run_seconds > 0 && (int)((g_now_ms() - loop_start) / 1000) >= run_seconds)
            break;
    }
done:
    fprintf(stderr, "[waylandd] shutting down (commits=%d)\n", g_commit_count);
    wl_display_destroy(g_display);
    fprintf(stderr, "[waylandd] PASS\n");
    return 0;
}
