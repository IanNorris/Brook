/*
 * waylandd.c — Brook OS Wayland server (Phase D rewrite).
 *
 * Each wayland xdg_toplevel now becomes a kernel-managed Window
 * (WM_CREATE_WINDOW syscall) with its own VFB allocated and mapped by
 * the kernel.  Layout, chrome, panel, drag, focus hit-testing all live
 * in the kernel WM (window.cpp + compositor.cpp).  Our job here is
 * just the wayland protocol relay:
 *
 *   surface_commit   →  retain immutable SHM/GPU images and commit dependencies,
 *                       present the synchronized tree, queue frame callbacks.
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"
#include "xdg-decoration-server-protocol.h"
#include "kde-server-decoration-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "dmabuf-layout.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "text-input-v3-server-protocol.h"
#include "relative-pointer-v1-server-protocol.h"
#include "pointer-constraints-v1-server-protocol.h"
#include "pointer-region.h"
#include "surface-tree.h"
#include "surface-geometry.h"
#include "gpu-scene-abi.h"
#include "wm-window-state.h"
#include "xwm-wire.h"

#define BROOK_SYS_WM_CREATE_WINDOW   506
#define BROOK_SYS_WM_DESTROY_WINDOW  507
#define BROOK_SYS_WM_SIGNAL_DIRTY    508
#define BROOK_SYS_WM_SET_TITLE       509
#define BROOK_SYS_WM_POINTER_CAPTURE  0xB0A
#define BROOK_SYS_WM_SET_APP_ID      0xB01
#define BROOK_SYS_WM_SET_CLIENT_SOCKET 0xB05
#define BROOK_SYS_WM_POP_INPUT       510
#define BROOK_SYS_WM_RESIZE_VFB      511
#define BROOK_SYS_WM_SET_DECORATION_MODE 512
#define BROOK_SYS_WM_BEGIN_MOVE      513
#define BROOK_SYS_WM_MOVE_RELATIVE   514
#define BROOK_SYS_WM_SET_MAXIMIZED   515
#define BROOK_SYS_WM_SET_MINIMIZED   516
#define BROOK_SYS_WM_BEGIN_RESIZE    517
#define BROOK_SYS_WM_SET_CURSOR_VISIBLE 518
#define BROOK_SYS_WM_SET_CURSOR_IMAGE  519
#define BROOK_SYS_WM_GET_SCREEN_INFO  520
#define BROOK_SYS_WM_PRESENT_GRES     525
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
#define EVT_MOUSE_RELATIVE   6
#define WM_EVT_CLOSE_REQUESTED 0x80
#define WM_EVT_FOCUS_GAINED    0x81
#define WM_EVT_FOCUS_LOST      0x82
#define WM_EVT_RESIZED         0x83
#define WM_RESIZE_MAXIMIZED     1
#define WM_RESIZE_RESIZING      2

static struct wl_display *g_display = NULL;
static uint32_t next_serial(void);
static volatile sig_atomic_t g_shutdown = 0;
static int g_commit_count = 0;
/* Opt-in diagnostics: no extra clock reads or logging in normal operation. */
static int g_trace_frames;
static uint64_t g_trace_surface_id, g_trace_ticks, g_trace_dispatches;
static uint32_t g_trace_last_tick, g_trace_tick_max_ms, g_trace_last_report;
static uint32_t g_trace_input_max_ms, g_trace_dispatch_max_ms;

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
static long wm_set_app_id(uint32_t id, const char* app_id) {
    return syscall(BROOK_SYS_WM_SET_APP_ID, (long)id, (long)app_id);
}
static void wm_register_client(uint32_t id, struct wl_resource* surface) {
    struct wl_client* client=wl_resource_get_client(surface);
    int fd=wl_client_get_fd(client);
    if (syscall(BROOK_SYS_WM_SET_CLIENT_SOCKET,(long)id,(long)fd)<0)
        fprintf(stderr,"[waylandd] client-origin registration wm=%u failed: %s\n",id,strerror(errno));
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
static long wm_set_cursor_image(const uint32_t *pixels, uint32_t w, uint32_t h,
                                int32_t hot_x, int32_t hot_y) {
    return syscall(BROOK_SYS_WM_SET_CURSOR_IMAGE, (long)pixels, (long)w,
                   (long)h, (long)hot_x, (long)hot_y);
}
static long wm_get_screen_info(uint32_t *w_out, uint32_t *h_out) {
    uint32_t buf[2] = {0, 0};
    long rc = syscall(BROOK_SYS_WM_GET_SCREEN_INFO, (long)buf, 0, 0, 0, 0);
    if (rc == 0) { *w_out = buf[0]; *h_out = buf[1]; }
    return rc;
}

/* ---------------- per-surface state ---------------- */

struct brook_seat_client;
struct brook_surface;
static void seat_clients_scrub_surface(struct brook_surface *s);
static void seat_clients_unmap_surface(struct brook_surface *s);
static void pointer_constraints_unmap(struct brook_surface *s,int destroy);
static void pointer_constraints_commit(struct brook_surface *s);
static void pointer_constraints_refresh(void);

/* xdg_positioner state — captured here so xdg_surface.get_popup can
 * read its size/anchor without a forward declaration dance. */
struct brook_positioner {
    int32_t w, h;
    int32_t ax, ay, aw, ah;
    uint32_t anchor, gravity, constraint;
    int32_t ox, oy;
};

/* A dmabuf-backed wl_buffer: carries the geometry the client declared plus the
 * host virgl resource id we resolved it to. Defined here (before surface_commit)
 * so the commit handler can read its fields. */
struct brook_dmabuf_buffer {
    unsigned refs;
    int32_t  width, height;
    uint32_t stride, fourcc, offset;
    uint64_t modifier;
    uint32_t gres;        /* resolved host virgl resource id (0 = unresolved) */
    uint32_t bo_handle;   /* our DRM GEM alias of the imported resource */
};
static int is_dmabuf_buffer(struct wl_resource *buf);

/* Protocol resource lifetime is independent of a pending/retained attachment.
 * SHM holds its pool through libwayland; dmabuf holds the import-cache GEM.
 * The destroy listener only invalidates the event destination. */
static void dmabuf_buffer_unref(struct brook_dmabuf_buffer *);
struct brook_buffer {
    struct wl_resource *resource;
    struct wl_listener destroy;
    struct wl_shm_buffer *shm;
    struct brook_dmabuf_buffer *dmabuf;
    unsigned refs,uses;
    int uncertain;
};
static void buffer_resource_destroyed(struct wl_listener *listener,void *data) {
    (void)data;
    struct brook_buffer *buffer=wl_container_of(listener,buffer,destroy);
    wl_list_remove(&buffer->destroy.link);wl_list_init(&buffer->destroy.link);
    buffer->resource=NULL;
}
static struct brook_buffer *buffer_hold(struct wl_resource *resource) {
    struct wl_listener *existing=wl_resource_get_destroy_listener(resource,buffer_resource_destroyed);
    if (existing) {
        struct brook_buffer *buffer=wl_container_of(existing,buffer,destroy);++buffer->refs;return buffer;
    }
    struct wl_shm_buffer *shm=wl_shm_buffer_get(resource);
    struct brook_dmabuf_buffer *db=is_dmabuf_buffer(resource)?wl_resource_get_user_data(resource):NULL;
    if (!shm && !db) { errno=EINVAL;return NULL; }
    struct brook_buffer *buffer=calloc(1,sizeof(*buffer));
    if (!buffer) { errno=ENOMEM;return NULL; }
    buffer->refs=1;buffer->resource=resource;buffer->shm=shm;buffer->dmabuf=db;
    if (shm) wl_shm_buffer_ref(shm);
    if (db) ++db->refs;
    buffer->destroy.notify=buffer_resource_destroyed;
    wl_resource_add_destroy_listener(resource,&buffer->destroy);
    return buffer;
}
static void buffer_drop(struct brook_buffer *buffer) {
    if (!buffer || --buffer->refs) return;
    if (buffer->resource) wl_list_remove(&buffer->destroy.link);
    if (buffer->shm) wl_shm_buffer_unref(buffer->shm);
    if (buffer->dmabuf) dmabuf_buffer_unref(buffer->dmabuf);
    free(buffer);
}
static void buffer_begin_use(struct brook_buffer *buffer) { ++buffer->uses; }
static void buffer_release(struct brook_buffer *buffer) {
    if (!buffer || !buffer->uses) return;
    if (!--buffer->uses && !buffer->uncertain && buffer->resource) wl_buffer_send_release(buffer->resource);
}

struct brook_frame_callback {
    struct wl_resource *resource;
    struct wl_list link;
    struct brook_surface *surface;
    uint32_t requested_ms, committed_ms;
    int done;
    uint64_t commit;
};

struct brook_scene_owner;
struct brook_xwindow;
struct brook_surface {
    struct bt_node tree;
    struct brook_scene_owner *scene;
    int scene_disabled;
    int role; /* permanent: 1=xdg, 2=subsurface, 3=cursor, 4=Xwayland */
    struct wl_resource *resource;
    struct pointer_region input_region,pending_input_region;
    int input_region_pending;
    struct brook_buffer *pending_buffer;
    int pending_attach;
    /* Own committed SHM pixels after releasing the client's buffer. Never use
     * the shared kernel VFB as the backing store for a surface tree. */
    uint32_t *image, *composite;
    uint32_t image_w, image_h;
    size_t composite_pixels;

    /* xdg-shell role state. */
    struct wl_resource *xdg_surface;
    struct wl_resource *xdg_toplevel;
    struct wl_resource *xdg_popup;        /* popup role (mutex with toplevel) */
    struct wl_resource *xwayland_surface;
    uint64_t xwayland_pending_serial, xwayland_serial, xwayland_generation;
    struct brook_xwindow *xwindow;
    int  xdg_initial_commit_done;
    int  xdg_acked; /* Initial handshake, not acknowledgement of the newest state. */
    int  toplevel_activated;
    int  toplevel_maximized,toplevel_resizing;
    int32_t configure_width,configure_height;
    int configure_size_pending,configure_size_acked;
    uint32_t configure_size_serial;
    int32_t window_geometry_w, window_geometry_h;
    int32_t pending_geometry_w, pending_geometry_h;
    int window_geometry_pending;
    struct brook_geometry_state geometry;
    struct wl_resource *viewport;
    uint32_t next_configure_serial; /* Last emitted serial, from next_serial(). */
    /* Popup geometry derived from positioner at get_popup time. */
    int32_t popup_w, popup_h;
    int32_t popup_x, popup_y;
    struct brook_surface *popup_parent;

    /* Requests become eligible only when their surface state is committed. */
    struct wl_list pending_frames, committed_frames, waiting_frames;
    uint64_t trace_id, trace_requests, trace_commits, trace_buffers;
    uint64_t trace_done, trace_cancelled;
    uint32_t trace_request_max_ms, trace_committed_max_ms;

    /* Latest set_title — copied so it survives the wayland string. */
    char title[64];
    char app_id[128]; /* xdg identity; independent of the document/window title */

    /* Kernel Window backing this xdg_toplevel.  Created lazily on the
     * first real (post-ack) commit when we know the buffer size. */
    uint32_t wm_id;
    uint32_t *vfb;          /* mapped into our address space by the kernel */
    uint32_t  vfb_stride;   /* in pixels */
    uint32_t  vfb_w, vfb_h; /* the size we created the Window at */

    /* Decoration mode requested by client.  Brook's policy is server-side
     * chrome for normal toplevels; popups are forced CSD/no-chrome.
     * If the client never negotiates xdg- or KDE server-decoration before the
     * first commit, we assume CSD — GTK3/4 apps draw their own header
     * bar and may not negotiate via xdg-decoration. */
    int  deco_csd;          /* 1 = client-side, 0 = server-side */
    int  deco_pending;      /* mode change requested but wm_id not yet known */
    int  deco_negotiated;   /* 1 = client negotiated xdg/KDE decoration */

    /* Cursor surface state. When a surface is assigned as cursor via
     * wl_pointer.set_cursor, its commits upload pixels to the kernel
     * compositor rather than blitting to a window VFB. */
    int     is_cursor;
    int32_t cursor_hot_x;
    int32_t cursor_hot_y;

    /* Linked list of all surfaces — used by the input pump. */
    struct brook_surface *next;

    /* Subsurface relationship (non-NULL when this surface has a subsurface role). */
    struct brook_surface *subsurface_parent;
    int32_t sub_x, sub_y;            /* position relative to parent */
    struct brook_surface *children;   /* linked list of child subsurfaces */
    struct brook_surface *sibling;    /* next child in parent's children list */
    struct wl_resource   *subsurface_resource; /* wl_subsurface resource if role is subsurface */
    int dmabuf_subsurface_reported; /* bounded diagnostic: once per surface */
};

static struct brook_surface *g_surfaces = NULL;
static int g_drm_fd = -1;
static void popup_apply_position(struct brook_surface *);
static int xwayland_surface_ready(const struct brook_surface *);
static unsigned xwayland_surface_flags(const struct brook_surface *);
static void xwayland_surface_created(struct brook_surface *);
static void xwayland_surface_detach(struct brook_surface *);
static void xwayland_native_unmapped(struct brook_surface *);
static void xwayland_pair_surfaces(void);
static void xwayland_generation_lost(void);
static int xwayland_accept_view(struct brook_surface *,uint32_t,uint32_t);
#include "surface-scene.h"
#include "xwayland-shell.h"
#include "xwayland-xwm.h"


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
    (void)x; (void)y;
    struct brook_surface *s = wl_resource_get_user_data(r);
    struct brook_buffer *next=buffer?buffer_hold(buffer):NULL;
    if (buffer && !next) {
        if (errno==ENOMEM) wl_client_post_no_memory(c);
        else wl_client_post_implementation_error(c,"unsupported Brook buffer type");
        return;
    }
    buffer_drop(s->pending_buffer);
    s->pending_buffer = next;
    s->pending_attach = 1;
}
static void surface_damage(struct wl_client *c, struct wl_resource *r,
                           int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void frame_callback_destroy(struct wl_resource *resource) {
    struct brook_frame_callback *callback = wl_resource_get_user_data(resource);
    if (g_trace_frames && !callback->done) ++callback->surface->trace_cancelled;
    wl_list_remove(&callback->link);
    free(callback);
}
static void surface_frame(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct brook_surface *surface = wl_resource_get_user_data(r);
    struct brook_frame_callback *callback = calloc(1, sizeof(*callback));
    if (!callback) { wl_client_post_no_memory(c); return; }
    callback->resource = wl_resource_create(c, &wl_callback_interface, 1, id);
    if (!callback->resource) { free(callback); wl_client_post_no_memory(c); return; }
    callback->surface = surface;
    if (g_trace_frames) {
        callback->requested_ms = g_now_ms();
        ++surface->trace_requests;
    }
    wl_list_insert(surface->pending_frames.prev, &callback->link);
    wl_resource_set_implementation(callback->resource, NULL, callback, frame_callback_destroy);
}
static void surface_send_frame_callbacks(struct brook_surface *surface, uint32_t now) {
    struct brook_frame_callback *callback, *next;
    wl_list_for_each_safe(callback, next, &surface->committed_frames, link) {
        if (g_trace_frames) {
            uint32_t requested = now - callback->requested_ms;
            uint32_t committed = now - callback->committed_ms;
            if (requested > surface->trace_request_max_ms) surface->trace_request_max_ms = requested;
            if (committed > surface->trace_committed_max_ms) surface->trace_committed_max_ms = committed;
            ++surface->trace_done;
        }
        callback->done = 1;
        wl_callback_send_done(callback->resource, now);
        wl_resource_destroy(callback->resource);
    }
}
static struct wl_event_source *g_frame_timer;
static int frame_clock_tick(void *unused) {
    (void)unused;
    uint32_t now = g_now_ms();
    if (g_trace_frames) {
        if (g_trace_ticks && now - g_trace_last_tick > g_trace_tick_max_ms)
            g_trace_tick_max_ms = now - g_trace_last_tick;
        g_trace_last_tick = now;
        ++g_trace_ticks;
    }
    for (struct brook_surface *surface = g_surfaces; surface; surface = surface->next) {
        struct brook_surface *root = surface;
        while (root->subsurface_parent) root = root->subsurface_parent;
        if (root->wm_id || surface->is_cursor) surface_send_frame_callbacks(surface, now);
    }
    wl_event_source_timer_update(g_frame_timer, 16);
    return 0;
}
static void trace_frames_report(uint32_t now) {
    if (!g_trace_frames || now - g_trace_last_report < 1000) return;
    g_trace_last_report = now;
    fprintf(stderr, "[wayland-frame] clock ms=%u ticks=%llu max_tick_gap_ms=%u dispatches=%llu max_dispatch_ms=%u max_input_ms=%u\n",
            now, (unsigned long long)g_trace_ticks, g_trace_tick_max_ms,
            (unsigned long long)g_trace_dispatches, g_trace_dispatch_max_ms, g_trace_input_max_ms);
    for (struct brook_surface *s = g_surfaces; s; s = s->next) {
        if (!s->trace_requests && !s->trace_buffers && !s->wm_id) continue;
        struct brook_surface *root = s;
        while (root->subsurface_parent) root = root->subsurface_parent;
        fprintf(stderr, "[wayland-frame] surface=%llu object=%u wm=%u root_wm=%u requests=%llu commits=%llu buffers=%llu done=%llu cancelled=%llu pending=%d eligible=%d max_request_ms=%u max_committed_ms=%u\n",
                (unsigned long long)s->trace_id, wl_resource_get_id(s->resource), s->wm_id, root->wm_id,
                (unsigned long long)s->trace_requests, (unsigned long long)s->trace_commits,
                (unsigned long long)s->trace_buffers, (unsigned long long)s->trace_done,
                (unsigned long long)s->trace_cancelled, wl_list_length(&s->pending_frames),
                wl_list_length(&s->committed_frames), s->trace_request_max_ms, s->trace_committed_max_ms);
    }
}
static void surface_set_opaque_region(struct wl_client *c, struct wl_resource *r,
                                       struct wl_resource *reg) {
    (void)c; (void)r; (void)reg;
}
static void surface_set_input_region(struct wl_client *c, struct wl_resource *r,
                                      struct wl_resource *reg) {
    struct brook_surface *s=wl_resource_get_user_data(r);
    const struct pointer_region *region=reg?wl_resource_get_user_data(reg):NULL;
    if(!pointer_region_copy(&s->pending_input_region,region)) {wl_client_post_no_memory(c);return;}
    s->input_region_pending=1;
}

static void surface_cursor_commit(struct brook_surface *s) {
    struct brook_view *view=s->tree.current?s->tree.current->value:NULL;
    if (!view || !view->image->pixels) return;
    const struct brook_geometry *g=&view->geometry;
    const struct brook_image *image=view->image;
    uint32_t dw=g->w>64?64:g->w,dh=g->h>64?64:g->h;
    struct brook_geometry cursor=*g;cursor.w=dw;cursor.h=dh;
    uint32_t pixels[64*64];
    for (uint32_t y=0;y<dh;++y) for (uint32_t x=0;x<dw;++x) {
        uint32_t px,py;
        brook_geometry_pixel(&cursor,image->width,image->height,x,y,&px,&py);
        pixels[y*dw+x]=image->pixels[(size_t)py*image->width+px];
    }
    wm_set_cursor_image(pixels,dw,dh,(int64_t)s->cursor_hot_x*dw/g->w,(int64_t)s->cursor_hot_y*dh/g->h);
}
/* Configure size and state form one transaction. Focus changes may supersede
 * its serial, but must preserve both dimensions and management state. */
static int toplevel_send_state(struct brook_surface *s,int32_t width,int32_t height) {
    if (!s->xdg_toplevel || !s->xdg_surface) return 1;
    struct wl_array states;wl_array_init(&states);
    const uint32_t values[]={XDG_TOPLEVEL_STATE_ACTIVATED,XDG_TOPLEVEL_STATE_MAXIMIZED,XDG_TOPLEVEL_STATE_RESIZING};
    const int enabled[]={s->toplevel_activated,s->toplevel_maximized,s->toplevel_resizing};
    for (unsigned i=0;i<3;++i) if(enabled[i]) {
        uint32_t *state=wl_array_add(&states,sizeof(*state));
        if(!state) { wl_array_release(&states);wl_client_post_no_memory(wl_resource_get_client(s->resource));return 0; }
        *state=values[i];
    }
    xdg_toplevel_send_configure(s->xdg_toplevel,width,height,&states);wl_array_release(&states);
    s->next_configure_serial=next_serial();
    if(s->configure_size_pending) {s->configure_size_serial=s->next_configure_serial;s->configure_size_acked=0;}
    xdg_surface_send_configure(s->xdg_surface,s->next_configure_serial);return 1;
}
static void surface_commit(struct wl_client *c,struct wl_resource *r) {
    struct brook_surface *s=wl_resource_get_user_data(r);
    if(s->input_region_pending) {
        pointer_region_clear(&s->input_region);s->input_region=s->pending_input_region;
        memset(&s->pending_input_region,0,sizeof(s->pending_input_region));s->input_region_pending=0;
    }
    pointer_constraints_commit(s);
    if (!xwayland_surface_commit(s)) return;
    if (s->role==4) xwayland_pair_surfaces();
    if (g_trace_frames) { ++s->trace_commits;if (s->pending_buffer) ++s->trace_buffers; }
    if (s->window_geometry_pending) {
        s->window_geometry_w=s->pending_geometry_w;s->window_geometry_h=s->pending_geometry_h;s->window_geometry_pending=0;

    }
    const int configure_committed=s->configure_size_pending && s->configure_size_acked;
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
        uint32_t serial = s->next_configure_serial = next_serial();
        xdg_surface_send_configure(s->xdg_surface, serial);
        fprintf(stderr, "[waylandd] xdg initial configure sent serial=%u (800x600)\n", serial);
        return;
    }
    if (s->xdg_popup && !s->xdg_initial_commit_done) {
        s->xdg_initial_commit_done = 1;
        int32_t pw = s->popup_w > 0 ? s->popup_w : 200;
        int32_t ph = s->popup_h > 0 ? s->popup_h : 100;
        xdg_popup_send_configure(s->xdg_popup, s->popup_x, s->popup_y, pw, ph);
        uint32_t serial = s->next_configure_serial = next_serial();
        xdg_surface_send_configure(s->xdg_surface, serial);
        fprintf(stderr, "[waylandd] xdg popup initial configure sent serial=%u %dx%d\n",
                serial, pw, ph);
        return;
    }


    struct brook_view *previous=s->tree.latest?s->tree.latest->value:NULL;
    struct brook_image *image=previous?previous->image:NULL;
    image_ref(image);
    if (s->pending_attach) {
        image_unref(image);image=NULL;
        struct brook_buffer *buffer=s->pending_buffer;
        if (buffer && buffer->dmabuf) {
            struct brook_dmabuf_buffer *db=buffer->dmabuf;
            if (db->width<=0 || db->height<=0 || db->width>16384 || db->height>16384) {
                wl_client_post_implementation_error(c,"invalid dmabuf dimensions");return;
            }
            image=calloc(1,sizeof(*image));
            if (image) {
                image->refs=1;image->width=db->width;image->height=db->height;
                image->format=db->fourcc==0x34325258?2:1;image->source=buffer;++buffer->refs;buffer_begin_use(buffer);
            }
        } else if (buffer && buffer->shm) {
            struct wl_shm_buffer *shm=buffer->shm;
            buffer_begin_use(buffer);
            wl_shm_buffer_begin_access(shm);
            image=image_snapshot(wl_shm_buffer_get_data(shm),wl_shm_buffer_get_width(shm),wl_shm_buffer_get_height(shm),wl_shm_buffer_get_stride(shm),wl_shm_buffer_get_format(shm));
            wl_shm_buffer_end_access(shm);buffer_release(buffer);
        }
        if (buffer && !image) { wl_client_post_no_memory(c);return; }
    }
    int committed=surface_tree_commit(s,image),published=1;
    if (!committed) wl_client_post_no_memory(c);
    else if (committed>0 && s->is_cursor) surface_cursor_commit(s);
    else if (committed>0 && !bt_effective_sync(&s->tree)) published=surface_repaint(s);
    if (committed>0 && published && configure_committed) {
        s->configure_size_pending=s->configure_size_acked=0;
        if(s->toplevel_resizing && s->wm_id) {
            // Finish only after the acknowledging commit has succeeded. Sending
            // this beside the resize would let clients coalesce away its size.
            s->toplevel_resizing=0;
            int32_t width=s->window_geometry_w>0?s->window_geometry_w:(int32_t)s->image_w;
            int32_t height=s->window_geometry_h>0?s->window_geometry_h:(int32_t)s->image_h;
            toplevel_send_state(s,width,height);
        }
    }
    image_unref(image);
    buffer_drop(s->pending_buffer);s->pending_buffer=NULL;s->pending_attach=0;
    ++g_commit_count;
}

static void surface_set_buffer_transform(struct wl_client *c, struct wl_resource *r, int32_t t) {
    (void)c;
    if (t<0 || t>7) { wl_resource_post_error(r,WL_SURFACE_ERROR_INVALID_TRANSFORM,"invalid buffer transform");return; }
    struct brook_surface *s=wl_resource_get_user_data(r);s->geometry.transform=t;
}
static void surface_set_buffer_scale(struct wl_client *c, struct wl_resource *r, int32_t sc) {
    (void)c;
    if (sc<=0) { wl_resource_post_error(r,WL_SURFACE_ERROR_INVALID_SCALE,"invalid buffer scale");return; }
    struct brook_surface *s=wl_resource_get_user_data(r);s->geometry.scale=sc;
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
    pointer_constraints_unmap(s,1);
    pointer_region_clear(&s->input_region);pointer_region_clear(&s->pending_input_region);
    xwayland_surface_detach(s);
    if (s->viewport) { wl_resource_set_user_data(s->viewport,NULL);s->viewport=NULL; }
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
    struct brook_surface *repaint_parent=s->subsurface_parent;
    bt_unlink(&s->tree);
    /* Clean up subsurface relationships */
    if (s->subsurface_parent) {
        struct brook_surface *parent = s->subsurface_parent;
        struct brook_surface **pp2 = &s->subsurface_parent->children;
        while (*pp2) {
            if (*pp2 == s) { *pp2 = s->sibling; break; }
            pp2 = &(*pp2)->sibling;
        }
        (void)parent;
    }
    /* Orphan any children */
    for (struct brook_surface *ch = s->children; ch; ) {
        struct brook_surface *next_ch = ch->sibling;
        ch->subsurface_parent = NULL;
        ch->sibling = NULL;
        ch = next_ch;
    }
    if (s->subsurface_resource) wl_resource_set_user_data(s->subsurface_resource, NULL);
    buffer_drop(s->pending_buffer);
    free(s->composite);s->composite=NULL;
    surface_unmap(s);
    struct brook_frame_callback *callback, *next;
    wl_list_for_each_safe(callback, next, &s->pending_frames, link)
        wl_resource_destroy(callback->resource);
    wl_list_for_each_safe(callback, next, &s->committed_frames, link)
        wl_resource_destroy(callback->resource);
    wl_list_for_each_safe(callback, next, &s->waiting_frames, link)
        wl_resource_destroy(callback->resource);
    if (s->xdg_surface) wl_resource_set_user_data(s->xdg_surface,NULL);
    if (s->xdg_toplevel) wl_resource_set_user_data(s->xdg_toplevel,NULL);
    if (s->xdg_popup) wl_resource_set_user_data(s->xdg_popup,NULL);
    if (s->xwayland_surface) wl_resource_set_user_data(s->xwayland_surface,NULL);
    s->resource=NULL;
    bt_destroy(&s->tree);
    if (repaint_parent) surface_repaint(repaint_parent);
}

/* ---------------- wl_compositor ---------------- */

static void compositor_create_surface(struct wl_client *client,
                                      struct wl_resource *res,
                                      uint32_t id) {
    struct brook_surface *s = calloc(1, sizeof(*s));
    if (!s) { wl_client_post_no_memory(client); return; }
    if (g_trace_frames) s->trace_id = ++g_trace_surface_id;
    struct wl_resource *sr = wl_resource_create(client, &wl_surface_interface,
                                                wl_resource_get_version(res), id);
    if (!sr) { free(s); wl_client_post_no_memory(client); return; }
    surface_tree_init(s);
    s->input_region.infinite=1;
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
    pointer_region_append(c,r,x,y,w,h,1);
}
static void region_subtract(struct wl_client *c, struct wl_resource *r,
                            int32_t x, int32_t y, int32_t w, int32_t h) {
    pointer_region_append(c,r,x,y,w,h,0);
}
static void region_resource_destroy(struct wl_resource *r) {
    struct pointer_region *region=wl_resource_get_user_data(r);pointer_region_clear(region);free(region);
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
    struct pointer_region *region=calloc(1,sizeof(*region));
    if(!region) {wl_resource_destroy(rr);wl_client_post_no_memory(client);return;}
    wl_resource_set_implementation(rr, &region_impl, region, region_resource_destroy);
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
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s || !app_id) return;
    snprintf(s->app_id, sizeof(s->app_id), "%s", app_id);
    if (s->wm_id) wm_set_app_id(s->wm_id, s->app_id);
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
    if (s) { surface_unmap(s);s->xdg_toplevel = NULL; }
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
    /* KDE decoration belongs to wl_surface and may precede its shell role. */
    if (!s->deco_negotiated) {
        s->deco_csd = 0;
        s->deco_pending = 1;
    }
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
            uint32_t serial = s->next_configure_serial = next_serial();
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
    if (s) { surface_unmap(s);s->xdg_popup = NULL; }
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
    (void)c; (void)x; (void)y;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (w <= 0 || h <= 0) {
        wl_resource_post_error(r, XDG_SURFACE_ERROR_INVALID_SIZE,
                               "window geometry must have positive dimensions");
        return;
    }
    s->pending_geometry_w = w; s->pending_geometry_h = h;
    s->window_geometry_pending = 1;
}
static void xdg_surface_ack_configure(struct wl_client *c, struct wl_resource *r,
                                        uint32_t serial) {
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    s->xdg_acked = 1;
    if (s->configure_size_pending && serial==s->configure_size_serial) s->configure_size_acked=1;
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
    if (s->xdg_surface || (s->role && s->role!=1)) {
        wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE,
                                "wl_surface already has xdg_surface");
        return;
    }
    struct wl_resource *xs = wl_resource_create(c, &xdg_surface_interface,
                                                 wl_resource_get_version(r), id);
    if (!xs) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(xs, &xdg_surface_impl, s, xdg_surface_resource_destroy);
    s->xdg_surface = xs;s->role=1;

    /* A fresh xdg_surface role on this wl_surface starts a new configure
     * handshake: the next commit must be answered with an initial configure
     * before the client attaches a buffer.  SDL2's Wayland backend reuses one
     * wl_surface across window recreation (e.g. yquake2's vid restart into the
     * ref_soft renderer): it destroys the old xdg_toplevel/xdg_surface and
     * creates new ones on the same wl_surface.  Without clearing these sticky
     * flags the second toplevel never receives its initial configure, so the
     * client blocks forever waiting to commit its first frame. */
    s->xdg_initial_commit_done = 0;
    s->xdg_acked = 0;
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
    unsigned refs;
    int32_t pointer_x,pointer_y;
    uint32_t kb_mods_depressed;
    uint32_t kb_mods_locked;
    int keymap_fd;                          /* kept alive until client receives it */
    struct brook_seat_client *next;
};

static struct brook_seat_client *g_seat_clients = NULL;
static uint32_t g_serial = 1;
static uint32_t next_serial(void) { return g_serial++; }

#include "text-input-v3.h"

static void seat_clients_scrub_surface(struct brook_surface *s) {
    for (struct brook_seat_client *sc = g_seat_clients; sc; sc = sc->next) {
        if (sc->entered_surface == s) {
            if (!sc->cursor_visible)
                wm_set_cursor_visible(1);
            sc->entered_surface = NULL;
        }
        if (sc->kb_focus == s) {
            text_input_focus(sc, NULL);
            sc->kb_focus = NULL;
        }
    }
}

static void seat_unref(struct brook_seat_client *sc);
static void pointer_objects_remove_pointer(struct wl_resource *r);
static void seat_clients_unmap_surface(struct brook_surface *s) {
    // A wl_surface may survive unmapping and be reused. End focus now so a
    // remap receives enter again; destroying the surface is a different path.
    for(struct brook_seat_client *sc=g_seat_clients;sc;sc=sc->next) {
        if(sc->entered_surface==s && sc->pointer && s->resource &&
           wl_resource_get_client(s->resource)==sc->client) {
            wl_pointer_send_leave(sc->pointer,next_serial(),s->resource);
            if(wl_resource_get_version(sc->pointer)>=5)wl_pointer_send_frame(sc->pointer);
        }
        if(sc->kb_focus==s && sc->keyboard && s->resource &&
           wl_resource_get_client(s->resource)==sc->client)
            wl_keyboard_send_leave(sc->keyboard,next_serial(),s->resource);
    }
    seat_clients_scrub_surface(s);
}
static void seat_remove_client(struct brook_seat_client *sc) {
    text_input_remove_seat(sc);
    if (sc && sc->entered_surface && !sc->cursor_visible)
        wm_set_cursor_visible(1);
    struct brook_seat_client **pp = &g_seat_clients;
    while (*pp) {
        if (*pp == sc) { *pp = sc->next; free(sc); return; }
        pp = &(*pp)->next;
    }
}
static void seat_unref(struct brook_seat_client *sc) {
    if(sc && --sc->refs==0)seat_remove_client(sc);
}
static struct brook_seat_client *seat_for_resource(struct wl_resource *r) {
    return wl_resource_get_user_data(r);
}

static void seat_apply_cursor_visibility(struct brook_seat_client *sc) {
    if (!sc || !sc->entered_surface) return;
    /* Always keep the kernel cursor visible.  Brook does not yet track
     * cursor visibility per-window in the kernel WM, so calling
     * wm_set_cursor_visible(0) would hide the cursor globally — even over
     * the desktop and taskbar.  When a client calls set_cursor(NULL) we
     * reset to the built-in arrow instead of hiding.  sc->cursor_visible
     * still tracks the Wayland protocol state for cleanup paths. */
    long rc = wm_set_cursor_visible(1);
    if (rc != 0) {
        fprintf(stderr, "[waylandd] WM_SET_CURSOR_VISIBLE visible=1 failed rc=%ld\n", rc);
    }
}

static void pointer_set_cursor(struct wl_client *c, struct wl_resource *r,
                                uint32_t serial, struct wl_resource *surface,
                                int32_t hx, int32_t hy) {
    (void)c; (void)serial;
    struct brook_seat_client *sc = seat_for_resource(r);
    if (!sc) return;
    sc->cursor_visible = surface != NULL;
    seat_apply_cursor_visibility(sc);

    if (surface) {
        /* Mark this surface as a cursor surface so commits upload pixels
         * to the kernel compositor instead of creating a window. */
        struct brook_surface *cs = wl_resource_get_user_data(surface);
        if (cs) {
            if (cs->role && cs->role!=3) {
                wl_resource_post_error(r,WL_POINTER_ERROR_ROLE,"surface already has a role");return;
            }
            cs->role=3;cs->is_cursor = 1;
            cs->cursor_hot_x = hx;
            cs->cursor_hot_y = hy;
        }

        /* set_cursor selects committed content. Pending attachments and
         * viewport/scale changes become visible only at wl_surface.commit. */
        if (cs) surface_cursor_commit(cs);
    } else {
        /* Reset to built-in cursor */
        wm_set_cursor_image(NULL, 0, 0, 0, 0);
    }
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
    pointer_objects_remove_pointer(r);
    if (sc && sc->pointer == r) sc->pointer = NULL;
    seat_unref(sc);
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
    seat_unref(sc);
}

#include "pointer-constraints.h"

#include "keyboard-keymap.h"

static int make_keymap_fd(size_t *out_size) {
    /* Full US-101 xkb keymap.  evdev keycodes (= scancode + 8).
     * The previous version only declared a-l on the home row -- every
     * other letter, number, and punctuation key reached the client as
     * an unmapped keycode, which xkbcommon translates to NoSymbol and
     * apps like mousepad drop.  Cover the standard typing block. */
    const char *keymap = brook_xkb_keymap;
    size_t len = sizeof(brook_xkb_keymap);
    int fd = syscall(SYS_memfd_create, "brook-keymap", 0u);
    if (fd < 0) { fprintf(stderr, "[waylandd] keymap: memfd_create FAILED errno=%d\n", errno); return -1; }
    if (ftruncate(fd, (off_t)len) < 0) { fprintf(stderr, "[waylandd] keymap: ftruncate FAILED\n"); close(fd); return -1; }

    /* Use write() to populate the memfd instead of mmap+memcpy.
     * This ensures pages are allocated in the kernel's MemFdData pageMap
     * via sys_write, guaranteeing cross-process visibility when the client
     * later mmap's this fd. The mmap+memcpy approach wrote to pages that
     * were installed in waylandd's page table but might not have been
     * properly committed to the shared MemFdData backing store. */
    ssize_t written = 0;
    const char *src = keymap;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, src + written, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[waylandd] keymap: write FAILED errno=%d\n", errno);
            close(fd);
            return -1;
        }
        written += n;
        remaining -= (size_t)n;
    }
    /* Seek back to start so client can read from offset 0 (mmap ignores
     * file position, but some implementations check it). */
    lseek(fd, 0, SEEK_SET);

    /* Verify: mmap the fd and check the data is readable */
    void *verify = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (verify != MAP_FAILED) {
        int match = (memcmp(verify, keymap, len) == 0);
        fprintf(stderr, "[waylandd] keymap: wrote %zu bytes to memfd=%d via write(), verify=%s first4='%.4s'\n",
                len, fd, match ? "OK" : "MISMATCH",
                (const char*)verify);
        munmap(verify, len);
    } else {
        fprintf(stderr, "[waylandd] keymap: wrote %zu bytes via write(), verify mmap FAILED errno=%d\n", len, errno);
    }
    *out_size = len;
    return fd;
}

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct brook_seat_client *sc = seat_for_resource(r);
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface,
                                                wl_resource_get_version(r), id);
    if (!p) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(p, &pointer_impl, sc, pointer_resource_destroy);
    if (sc) {sc->pointer = p;++sc->refs;}
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct brook_seat_client *sc = seat_for_resource(r);
    struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface,
                                                wl_resource_get_version(r), id);
    if (!k) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(k, &keyboard_impl, sc, keyboard_resource_destroy);
    if (sc) {sc->keyboard = k;++sc->refs;}

    size_t kmsize = 0;
    int kfd = make_keymap_fd(&kmsize);
    if (kfd >= 0) {
        wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                kfd, (uint32_t)kmsize);
        /* Do NOT close(kfd) here! libwayland queues the fd for delivery via
         * SCM_RIGHTS on the next wl_display_flush_clients(). If we close it
         * now, the fd number may be recycled before sendmsg transmits it,
         * causing the client to receive a stale/wrong fd.
         *
         * We intentionally leak this fd — it's one memfd per client session
         * and keeps the backing MemFdData alive so the client can mmap it.
         * The fd is cleaned up when waylandd exits or the client disconnects. */
        if (sc) sc->keymap_fd = kfd;
    } else {
        fprintf(stderr, "[waylandd] keyboard keymap FAILED, sending no_keymap\n");
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
        seat_unref(sc);
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
    sc->seat = r;sc->refs=1;
    sc->client = client;
    sc->cursor_visible = 1;
    sc->keymap_fd = -1;
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
    {
        uint32_t sw = 0, sh = 0;
        if (wm_get_screen_info(&sw, &sh) == 0 && sw > 0 && sh > 0) {
            w = (int)sw; h = (int)sh;
        }
    }
    wl_output_send_geometry(r, 0, 0,
        (int)((w * 254 + 480) / 960), (int)((h * 254 + 480) / 960),
        WL_OUTPUT_SUBPIXEL_UNKNOWN, "Brook", "vfb-0",
        WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        w, h, 60000);
    if (version >= 2) {
        wl_output_send_scale(r, 1);
        if (version >= 4) {
            wl_output_send_name(r, "BROOK-1");
            wl_output_send_description(r, "Brook virtual display");
        }
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
    /* When entering a new surface, restore cursor visibility — a previous
     * client may have hidden it with set_cursor(NULL). Reset the per-client
     * flag so the cursor reappears with the built-in arrow until the new
     * client calls set_cursor. */
    if (s && !sc->cursor_visible) {
        sc->cursor_visible = 1;
        wm_set_cursor_image(NULL, 0, 0, 0, 0); /* restore built-in arrow */
    }
    seat_apply_cursor_visibility(sc);
}

static void dispatch_surface_event(struct brook_surface *s,
                                   struct brook_seat_client *sc,
                                   struct brook_wm_event *e, uint32_t now) {
    switch (e->type) {
    case EVT_MOUSE_RELATIVE:
        pointer_relative_dispatch(s,sc,e);break;
    case EVT_MOUSE_MOVE: {
        if (!sc || !sc->pointer || pointer_surface_locked(s)) break;
        sc->pointer_x=e->x;sc->pointer_y=e->y;
        pointer_enter_if_needed(sc, s, e->x, e->y);
        wl_pointer_send_motion(sc->pointer, now,
                               wl_fixed_from_int(e->x),
                               wl_fixed_from_int(e->y));
        if (wl_resource_get_version(sc->pointer) >= 5)
            wl_pointer_send_frame(sc->pointer);
        pointer_constraints_refresh();
        break;
    }
    case EVT_MOUSE_BTN_DOWN:
    case EVT_MOUSE_BTN_UP: {
        if (!sc || !sc->pointer) {
            fprintf(stderr, "[waylandd] btn wm=%u DROPPED (no seat/pointer)\n",
                    s->wm_id);
            break;
        }
        if(e->reserved && !pointer_event_token(s,e->reserved))break;
        sc->pointer_x=e->x;sc->pointer_y=e->y;
        pointer_enter_if_needed(sc, s, e->x, e->y);
        /* scan: 0=left, 1=right, 2=middle  →  BTN_LEFT 0x110, etc. */
        uint32_t btn = 0x110 + (e->scan & 0x3);
        uint32_t st  = (e->type == EVT_MOUSE_BTN_DOWN)
                         ? WL_POINTER_BUTTON_STATE_PRESSED
                         : WL_POINTER_BUTTON_STATE_RELEASED;
        wl_pointer_send_button(sc->pointer, next_serial(), now, btn, st);
        if (wl_resource_get_version(sc->pointer) >= 5)
            wl_pointer_send_frame(sc->pointer);
        fprintf(stderr, "[waylandd] btn wm=%u 0x%x %s at (%d,%d)\n",
                s->wm_id, btn,
                st == WL_POINTER_BUTTON_STATE_PRESSED ? "down" : "up",
                e->x, e->y);
        break;
    }
    case EVT_MOUSE_SCROLL: {
        if (!sc || !sc->pointer || (e->reserved && !pointer_event_token(s,e->reserved))) break;
            int32_t dy = (int8_t)e->scan;
            /* Brook wheel ticks are positive upward; Wayland axis values
             * are positive downward. */
            wl_pointer_send_axis(sc->pointer, now,
                                 WL_POINTER_AXIS_VERTICAL_SCROLL,
                                 wl_fixed_from_int(-dy * 10));
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
         * Control=2, Mod1(Alt)=3. */
        uint32_t depressed = 0;
        uint32_t locked    = 0;
        if (e->mods & 0x03) depressed |= (1u << 0); /* Shift */
        if (e->mods & 0x04) depressed |= (1u << 2); /* Control */
        if (e->mods & 0x08) depressed |= (1u << 3); /* Mod1/Alt */
        if (e->mods & 0x10) locked    |= (1u << 1); /* Lock/Caps */

        /* Send key event first — xkbcommon clients call
         * xkb_state_update_key() which internally tracks modifier
         * state from the keymap. */
        uint32_t xkb_code = scancode_to_xkb(e->scan);
        wl_keyboard_send_key(sc->keyboard, next_serial(), now,
                             xkb_code, st);

        /* Send modifiers only when they changed — avoids double-toggle
         * where xkb_state_update_key already updated the modifier
         * internally and a redundant wl_keyboard.modifiers would flip
         * it back. */
        if (depressed != sc->kb_mods_depressed ||
            locked    != sc->kb_mods_locked)
        {
            wl_keyboard_send_modifiers(sc->keyboard, next_serial(),
                                       depressed, 0u, locked, 0u);
        }

        /* Debug: log key events with modifier info */
        if (e->scan == 0x2A || e->scan == 0x36 || depressed || locked)
            fprintf(stderr, "[waylandd] KEY sc=0x%02x xkb=%u %s mods=0x%02x dep=%u lock=%u\n",
                    e->scan, xkb_code,
                    st == WL_KEYBOARD_KEY_STATE_PRESSED ? "DN" : "UP",
                    e->mods, depressed, locked);
        sc->kb_mods_depressed = depressed;
        sc->kb_mods_locked    = locked;
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
            /* Send current modifier state so client starts with
             * correct xkb_state (protocol requires modifiers after enter). */
            wl_keyboard_send_modifiers(sc->keyboard, next_serial(),
                                       sc->kb_mods_depressed, 0u,
                                       sc->kb_mods_locked, 0u);
            sc->kb_focus = s;
            text_input_focus(sc, s);
            fprintf(stderr, "[waylandd] FOCUS_GAINED wm=%u\n", s->wm_id);
        }
        break;
    }
    case WM_EVT_FOCUS_LOST: {
        if (sc && sc->keyboard && sc->kb_focus == s) {
            wl_keyboard_send_leave(sc->keyboard, next_serial(), s->resource);
            text_input_focus(sc, NULL);
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
            s->toplevel_maximized=(e->reserved&WM_RESIZE_MAXIMIZED)!=0;
            s->toplevel_resizing=!s->toplevel_maximized && (e->reserved&WM_RESIZE_RESIZING)!=0;
            s->configure_width=neww;s->configure_height=newh;s->configure_size_pending=1;
            toplevel_send_state(s,neww,newh);
            fprintf(stderr,"[waylandd] WM_EVT_RESIZED wm=%u configure %dx%d max=%d resizing=%d serial=%u\n",
                s->wm_id,neww,newh,s->toplevel_maximized,s->toplevel_resizing,s->next_configure_serial);
        }
        break;
    }
    default: break;
    }

}
/* Activation is a toplevel state, independent of seat bindings. GTK clients
 * need a post-map configure when the native WM activates the window; keyboard
 * enter alone does not communicate xdg-shell activation. The activated enum
 * exists in xdg_toplevel v1. Preserve committed client geometry: a zero-size
 * reconfigure makes some GTK clients choose a smaller natural size, whereas
 * buffer dimensions include CSD shadows and would grow the window. */
static void toplevel_set_activated(struct brook_surface *surface, int active) {
    if (!surface->xdg_toplevel || !surface->xdg_surface ||
        surface->toplevel_activated == active) return;
    surface->toplevel_activated=active;
    int32_t width=surface->window_geometry_w>0?surface->window_geometry_w:(int32_t)surface->vfb_w;
    int32_t height=surface->window_geometry_h>0?surface->window_geometry_h:(int32_t)surface->vfb_h;
    if(surface->toplevel_maximized || surface->configure_size_pending) {
        width=surface->configure_width;height=surface->configure_height;
    }
    // A state-only configure never resets the initial map/ack gate.
    toplevel_send_state(surface,width,height);
}

static void dispatch_window_event(struct brook_surface *surface,
                                  struct brook_wm_event *event,uint32_t now) {
    xwayland_window_event(surface,event->type);
    /* Window management requests are per window. Input is per resource: GTK
     * and application libraries can independently bind the same wl_seat. */
    if (event->type == WM_EVT_FOCUS_GAINED) toplevel_set_activated(surface, 1);
    if (event->type == WM_EVT_FOCUS_LOST) toplevel_set_activated(surface, 0);
    if (event->type==WM_EVT_CLOSE_REQUESTED || event->type==WM_EVT_RESIZED) {
        dispatch_surface_event(surface,NULL,event,now);
        return;
    }
    struct wl_client *owner=wl_resource_get_client(surface->resource);
    for(struct brook_seat_client *seat=g_seat_clients;seat;seat=seat->next)
        if(seat->client==owner)dispatch_surface_event(surface,seat,event,now);
}
static void pump_input_for_surface(struct brook_surface *surface) {
    if(!surface->wm_id)return;
    struct brook_wm_event events[16];
    long count=wm_pop_input(surface->wm_id,events,16);
    uint32_t now=g_now_ms();
    for(long i=0;i<count;++i)dispatch_window_event(surface,&events[i],now);
}

static void pump_input_once(void) {
    pointer_constraints_refresh();
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
static void surface_apply_decoration(struct brook_surface *s, int csd) {
    if (!s) return;
    /* Popups never acquire toplevel chrome, irrespective of negotiation. */
    csd = csd || s->xdg_popup != NULL;
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
}
static void deco_apply_mode(struct wl_resource *r, int csd) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s) return;
    surface_apply_decoration(s, csd);
    zxdg_toplevel_decoration_v1_send_configure(r,
        s->deco_csd ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
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
    if (s) s->deco_negotiated = 1;
    fprintf(stderr, "[waylandd] decoration get_toplevel_decoration surface=%p\n",
            (void*)s);
    /* Default to SERVER_SIDE: Brook's kernel WM draws all chrome. A client that
     * binds the decoration manager is asking us to negotiate, so our preference
     * (server-side) is the right default. Defaulting to CSD here broke
     * libdecor-based clients (e.g. es2gears/GL demos): libdecor would try to
     * draw client-side decorations via a plugin it can't load on Brook and hang
     * before ever committing a frame. Clients that genuinely want CSD still get
     * it by explicitly calling set_mode(CLIENT_SIDE). */
    deco_apply_mode(d, 0);
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

/* KDE's v1 protocol is what GTK3 uses (xdg-decoration alone is insufficient).
 * Each resource watches its wl_surface lifetime; it may safely be released
 * after that surface, and a late request cannot dereference freed userdata. */
struct kde_decoration {
    struct brook_surface *surface;
    struct wl_listener surface_destroy;
};
static void kde_decoration_surface_destroyed(struct wl_listener *listener, void *data) {
    (void)data;
    struct kde_decoration *d = wl_container_of(listener, d, surface_destroy);
    wl_list_remove(&d->surface_destroy.link);
    wl_list_init(&d->surface_destroy.link);
    d->surface = NULL;
}
static void kde_decoration_resource_destroy(struct wl_resource *r) {
    struct kde_decoration *d = wl_resource_get_user_data(r);
    if (!d) return;
    if (d->surface) wl_list_remove(&d->surface_destroy.link);
    free(d);
}
static void kde_decoration_apply(struct wl_resource *r, uint32_t mode) {
    struct kde_decoration *d = wl_resource_get_user_data(r);
    if (!d || !d->surface) return;
    /* NONE and CLIENT both suppress Brook chrome; their distinct protocol
     * values remain intact in the acknowledgement. Invalid preferences use
     * the advertised server default rather than echoing an invalid enum. */
    if (mode > ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER)
        mode = ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER;
    if (d->surface->xdg_popup) mode = ORG_KDE_KWIN_SERVER_DECORATION_MODE_NONE;
    d->surface->deco_negotiated = 1;
    surface_apply_decoration(d->surface, mode != ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER);
    org_kde_kwin_server_decoration_send_mode(r, mode);
}
static void kde_decoration_request_mode(struct wl_client *c, struct wl_resource *r, uint32_t mode) {
    (void)c;
    fprintf(stderr, "[waylandd] KDE decoration request_mode=%u\n", mode);
    kde_decoration_apply(r, mode);
}
static const struct org_kde_kwin_server_decoration_interface kde_decoration_impl = {
    .release = deco_destroy,
    .request_mode = kde_decoration_request_mode,
};
static void kde_decoration_create(struct wl_client *c, struct wl_resource *r,
                                  uint32_t id, struct wl_resource *surface) {
    struct brook_surface *s = surface ? wl_resource_get_user_data(surface) : NULL;
    if (!s) {
        wl_resource_post_error(r, WL_DISPLAY_ERROR_INVALID_OBJECT, "invalid decoration surface");
        return;
    }
    struct kde_decoration *d = calloc(1, sizeof(*d));
    if (!d) { wl_client_post_no_memory(c); return; }
    struct wl_resource *resource = wl_resource_create(c,
        &org_kde_kwin_server_decoration_interface, 1, id);
    if (!resource) { free(d); wl_client_post_no_memory(c); return; }
    d->surface = s;
    d->surface_destroy.notify = kde_decoration_surface_destroyed;
    wl_resource_add_destroy_listener(surface, &d->surface_destroy);
    wl_resource_set_implementation(resource, &kde_decoration_impl, d, kde_decoration_resource_destroy);
    kde_decoration_apply(resource, ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER);
}
static const struct org_kde_kwin_server_decoration_manager_interface kde_decoration_manager_impl = {
    .create = kde_decoration_create,
};
static void kde_decoration_manager_bind(struct wl_client *c, void *data,
                                        uint32_t version, uint32_t id) {
    (void)data; (void)version;
    struct wl_resource *r = wl_resource_create(c,
        &org_kde_kwin_server_decoration_manager_interface, 1, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &kde_decoration_manager_impl, NULL, NULL);
    org_kde_kwin_server_decoration_manager_send_default_mode(r,
        ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
    fprintf(stderr, "[waylandd] KDE decoration manager default=server\n");
}

/* ---------------- wp_viewporter (double-buffered surface geometry) ---------------- */
static struct brook_surface *viewport_surface(struct wl_resource *r) {
    struct brook_surface *s=wl_resource_get_user_data(r);
    if (!s) wl_resource_post_error(r,WP_VIEWPORT_ERROR_NO_SURFACE,"viewport surface destroyed");
    return s;
}
static void viewport_resource_destroy(struct wl_resource *r) {
    struct brook_surface *s=wl_resource_get_user_data(r);
    if (s) {
        s->viewport=NULL;s->geometry.source_set=s->geometry.destination_set=0;
    }
}
static void viewport_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;wl_resource_destroy(r);
}
static void viewport_set_source(struct wl_client *c, struct wl_resource *r,
                                wl_fixed_t x, wl_fixed_t y,wl_fixed_t w,wl_fixed_t h) {
    (void)c;struct brook_surface *s=viewport_surface(r);if (!s) return;
    if (x==-256 && y==-256 && w==-256 && h==-256) { s->geometry.source_set=0;return; }
    if (x<0 || y<0 || w<=0 || h<=0) {
        wl_resource_post_error(r,WP_VIEWPORT_ERROR_BAD_VALUE,"invalid source rectangle");return;
    }
    s->geometry.source_set=1;s->geometry.x=x;s->geometry.y=y;s->geometry.w=w;s->geometry.h=h;
}
static void viewport_set_destination(struct wl_client *c, struct wl_resource *r,int32_t w,int32_t h) {
    (void)c;struct brook_surface *s=viewport_surface(r);if (!s) return;
    if (w==-1 && h==-1) { s->geometry.destination_set=0;return; }
    if (w<=0 || h<=0) { wl_resource_post_error(r,WP_VIEWPORT_ERROR_BAD_VALUE,"invalid destination size");return; }
    s->geometry.destination_set=1;s->geometry.dw=w;s->geometry.dh=h;
}
static const struct wp_viewport_interface viewport_impl = {
    .destroy=viewport_destroy,.set_source=viewport_set_source,.set_destination=viewport_set_destination,
};
static void viewporter_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;wl_resource_destroy(r);
}
static void viewporter_get_viewport(struct wl_client *c, struct wl_resource *r,uint32_t id,struct wl_resource *surface) {
    struct brook_surface *s=wl_resource_get_user_data(surface);
    if (s->viewport) { wl_resource_post_error(r,WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,"surface already has a viewport");return; }
    struct wl_resource *vp=wl_resource_create(c,&wp_viewport_interface,wl_resource_get_version(r),id);
    if (!vp) { wl_client_post_no_memory(c);return; }
    s->viewport=vp;wl_resource_set_implementation(vp,&viewport_impl,s,viewport_resource_destroy);
}
static const struct wp_viewporter_interface viewporter_impl = {
    .destroy      = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};
static void viewporter_bind(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(client,
        &wp_viewporter_interface, (int)version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &viewporter_impl, NULL, NULL);
    fprintf(stderr, "[waylandd] wp_viewporter bind v=%u id=%u\n", version, id);
}

/* ---------------- wl_subcompositor ---------------- */
static void subsurface_resource_destroy(struct wl_resource *r) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s) return;
    struct brook_surface *parent = s->subsurface_parent;
    if (parent) {
        struct brook_surface **pp = &parent->children;
        while (*pp) {
            if (*pp == s) { *pp = s->sibling; break; }
            pp = &(*pp)->sibling;
        }
    }
    bt_unlink(&s->tree);
    s->subsurface_parent = NULL;
    s->subsurface_resource = NULL;
    s->sibling = NULL;
    if (parent) surface_repaint(parent);
}
static void subsurface_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wl_resource_destroy(r);
}
static void subsurface_set_position(struct wl_client *c, struct wl_resource *r,
                                    int32_t x, int32_t y) {
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (s) { s->tree.pending_x=x;s->tree.pending_y=y; }
}
static void subsurface_place_above(struct wl_client *c, struct wl_resource *r,
                                   struct wl_resource *sib) {
    (void)c;struct brook_surface *s=wl_resource_get_user_data(r),*other=wl_resource_get_user_data(sib);
    if (!s || !other || !bt_place(&s->tree,&other->tree,1))
        wl_resource_post_error(r,WL_SUBSURFACE_ERROR_BAD_SURFACE,"invalid stacking sibling");
}
static void subsurface_place_below(struct wl_client *c, struct wl_resource *r,
                                   struct wl_resource *sib) {
    (void)c;struct brook_surface *s=wl_resource_get_user_data(r),*other=wl_resource_get_user_data(sib);
    if (!s || !other || !bt_place(&s->tree,&other->tree,0))
        wl_resource_post_error(r,WL_SUBSURFACE_ERROR_BAD_SURFACE,"invalid stacking sibling");
}
static void subsurface_set_sync(struct wl_client *c, struct wl_resource *r) {
    (void)c;struct brook_surface *s=wl_resource_get_user_data(r);if (s) bt_set_sync(&s->tree,1);
}
static void subsurface_set_desync(struct wl_client *c, struct wl_resource *r) {
    struct brook_surface *s=wl_resource_get_user_data(r);if (!s) return;
    if (!bt_set_sync(&s->tree,0)) wl_client_post_no_memory(c);
    else surface_repaint(s);
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
    (void)r;
    struct brook_surface *child = wl_resource_get_user_data(surface);
    struct brook_surface *par   = wl_resource_get_user_data(parent);
    if (!child || !par || child->subsurface_resource || (child->role && child->role!=2) || child->xdg_surface || child->is_cursor) {
        wl_resource_post_error(r, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "surface already has a role");
        return;
    }
    for (struct brook_surface *ancestor = par; ancestor; ancestor = ancestor->subsurface_parent) {
        if (ancestor == child) {
            wl_resource_post_error(r, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                                   "subsurface parent would create a cycle");
            return;
        }
    }
    struct wl_resource *ss = wl_resource_create(c, &wl_subsurface_interface,
                                                 wl_resource_get_version(r), id);
    if (!ss) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(ss, &subsurface_impl, child, subsurface_resource_destroy);
    if (child && par) {
        if (!bt_attach(&child->tree,&par->tree)) {
            wl_resource_destroy(ss);wl_resource_post_error(r,WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,"invalid association");return;
        }
        child->role=2;
        child->subsurface_parent = par;
        child->subsurface_resource = ss;
        child->sub_x = 0;
        child->sub_y = 0;
        /* New subsurfaces start above existing siblings. */
        struct brook_surface **tail = &par->children;
        while (*tail) tail = &(*tail)->sibling;
        child->sibling = NULL;
        *tail = child;
        fprintf(stderr, "[waylandd] subsurface: child surface %p -> parent %p\n",
                (void*)child, (void*)par);
        waylandd_send_enter_for_surface(child->resource);
    }
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

/* ---------------- linux-dmabuf (hardware GL presentation) ---------------- */
/*
 * Mesa's Wayland EGL platform shares its hardware-rendered render target with
 * us as a dmabuf. We advertise zwp_linux_dmabuf_v1, import the fd through the
 * DRM render node and retain its GEM alias until scene capture completes.
 * surface-scene.h keeps producer release separate from immutable image and
 * parent-commit lifetimes; backend resource numbers never become scene IDs.
 *
 * The DRM ioctls mirror the kernel shim (syscall.cpp DrmRenderIoctl). The kernel
 * dispatches on ioctl type+nr only, so the _IOC size/dir bits are cosmetic.
 */
#define DRM_IOC(nr, sz) (( (unsigned)3u << 30) | ((unsigned)(sz) << 16) | ((unsigned)'d' << 8) | (unsigned)(nr))
struct drm_prime_handle { uint32_t handle; uint32_t flags; int32_t fd; };
struct drm_virtgpu_resource_info { uint32_t bo_handle, res_handle, size, blob_mem; };
struct drm_gem_close { uint32_t handle, pad; };
#define DRM_IOCTL_PRIME_FD_TO_HANDLE  DRM_IOC(0x2e, sizeof(struct drm_prime_handle))
#define DRM_IOCTL_VIRTGPU_RESOURCE_INFO DRM_IOC(0x45, sizeof(struct drm_virtgpu_resource_info))
#define DRM_IOCTL_GEM_CLOSE           DRM_IOC(0x09, sizeof(struct drm_gem_close))

/* g_drm_fd is shared with retained scene capture. */

/* PRIME imports return an existing GEM handle for the same object. wl_buffers
 * share ownership here; closing each alias independently can close a new
 * buffer after the numeric GEM handle is reused. The event loop is single
 * threaded, and this list is the sole owner of imported GEM handles. */
struct brook_dmabuf_import {
    uint32_t bo, gres, refs;
    struct brook_dmabuf_import *next;
};
static struct brook_dmabuf_import *g_dmabuf_imports;

static void dmabuf_release_bo(uint32_t bo)
{
    struct brook_dmabuf_import **link = &g_dmabuf_imports;
    while (*link && (*link)->bo != bo) link = &(*link)->next;
    if (!*link) return;
    struct brook_dmabuf_import *entry = *link;
    if (--entry->refs) return;
    *link = entry->next;
    if (g_drm_fd >= 0) {
        struct drm_gem_close gc = { bo, 0 };
        ioctl(g_drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
    }
    free(entry);
}


static const struct wl_buffer_interface dmabuf_buffer_impl;

/* Resolve an imported dmabuf fd to a host virgl resource id via the DRM node:
 * PRIME_FD_TO_HANDLE (fd -> GEM bo) then RESOURCE_INFO (bo -> res_handle/gres). */
static uint32_t dmabuf_resolve_gres(int fd, uint32_t *out_bo)
{
    if (out_bo) *out_bo = 0;
    if (g_drm_fd < 0) return 0;
    struct drm_prime_handle ph;
    memset(&ph, 0, sizeof(ph));
    ph.fd = fd;
    if (ioctl(g_drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) != 0) {
        fprintf(stderr, "[waylandd] dmabuf: FD_TO_HANDLE failed: %s\n", strerror(errno));
        return 0;
    }
    for (struct brook_dmabuf_import *entry = g_dmabuf_imports; entry; entry = entry->next) {
        if (entry->bo == ph.handle) {
            ++entry->refs;
            if (out_bo) *out_bo = entry->bo;
            return entry->gres;
        }
    }
    struct drm_virtgpu_resource_info ri;
    memset(&ri, 0, sizeof(ri));
    ri.bo_handle = ph.handle;
    if (ioctl(g_drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &ri) != 0) {
        fprintf(stderr, "[waylandd] dmabuf: RESOURCE_INFO failed: %s\n", strerror(errno));
        struct drm_gem_close gc = { ph.handle, 0 };
        ioctl(g_drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
        return 0;
    }
    struct brook_dmabuf_import *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        struct drm_gem_close gc = { ph.handle, 0 };
        ioctl(g_drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
        return 0;
    }
    entry->bo = ph.handle; entry->gres = ri.res_handle; entry->refs = 1;
    entry->next = g_dmabuf_imports; g_dmabuf_imports = entry;
    if (out_bo) *out_bo = ph.handle;
    return ri.res_handle;
}

static void dmabuf_buffer_unref(struct brook_dmabuf_buffer *db)
{
    if (db && !--db->refs) {
        if (db->bo_handle) dmabuf_release_bo(db->bo_handle);
        free(db);
    }
}
static void dmabuf_buffer_destroy(struct wl_resource *r)
{
    dmabuf_buffer_unref(wl_resource_get_user_data(r));
}
static void dmabuf_buffer_destroy_req(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
static const struct wl_buffer_interface dmabuf_buffer_impl = {
    .destroy = dmabuf_buffer_destroy_req,
};

/* True if a wl_buffer resource is one of our dmabuf buffers (vs wl_shm). */
static int is_dmabuf_buffer(struct wl_resource *buf)
{
    return buf && wl_resource_instance_of(buf, &wl_buffer_interface,
                                          &dmabuf_buffer_impl);
}

/* zwp_linux_buffer_params_v1: the client adds plane(s) then create[_immed]. We
 * support a single-plane buffer (plane 0) — enough for Mesa's window surface. */
struct brook_dmabuf_params {
    int      fd;          /* plane-0 dmabuf fd (-1 = none added) */
    uint32_t stride, offset;
    uint64_t modifier;
    int used;
};

static void params_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }

static void params_add(struct wl_client *c, struct wl_resource *r, int32_t fd,
                       uint32_t plane_idx, uint32_t offset, uint32_t stride,
                       uint32_t mod_hi, uint32_t mod_lo)
{
    (void)c;
    struct brook_dmabuf_params *p = wl_resource_get_user_data(r);
    if (p->used) {
        if (fd>=0) close(fd);
        wl_resource_post_error(r,ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,"params already used");return;
    }
    if (plane_idx!=0) {
        if (fd>=0) close(fd);
        wl_resource_post_error(r,ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,"only plane zero is supported");return;
    }
    if (p->fd>=0) {
        if (fd>=0) close(fd);
        wl_resource_post_error(r,ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,"plane zero already supplied");return;
    }
    p->fd=fd;p->stride=stride;p->offset=offset;
    p->modifier=((uint64_t)mod_hi<<32)|mod_lo;
}

static struct wl_resource *
params_make_buffer(struct wl_client *c, struct wl_resource *r, uint32_t id,
                   int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
    struct brook_dmabuf_params *p = wl_resource_get_user_data(r);
    if (p->used) {
        wl_resource_post_error(r,ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,"params already used");return NULL;
    }
    p->used=1;
    if (p->fd < 0) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                               "no plane added");
        return NULL;
    }
    const struct BrookDmabufLayout layout={(uint32_t)width,(uint32_t)height,format,p->stride,p->offset,flags,p->modifier};
    enum BrookDmabufLayoutError error=BrookDmabufValidate(&layout,UINT64_MAX,1);
    if (error!=BROOK_DMABUF_LAYOUT_OK) {
        uint32_t protocol_error=error==BROOK_DMABUF_LAYOUT_DIMENSIONS?ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS:
            error==BROOK_DMABUF_LAYOUT_FORMAT?ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT:ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS;
        close(p->fd);p->fd=-1;
        wl_resource_post_error(r,protocol_error,"unsupported or invalid DMA-BUF layout");return NULL;
    }
    struct brook_dmabuf_buffer *db = calloc(1, sizeof(*db));
    if (!db) { wl_client_post_no_memory(c); return NULL; }
    db->refs = 1;db->width = width; db->height = height;
    db->stride = p->stride; db->fourcc = format;db->offset=p->offset;db->modifier=p->modifier;
    db->gres = dmabuf_resolve_gres(p->fd, &db->bo_handle);
    close(p->fd); p->fd = -1;   /* the import cache now holds the GEM reference */
    if (!db->gres) {
        free(db);
        if (id) wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                                       "dmabuf import failed");
        return NULL;
    }
    fprintf(stderr, "[waylandd] dmabuf buffer %dx%d stride=%u fourcc=0x%x -> gres=%u\n",
            width, height, db->stride, format, db->gres);

    struct wl_resource *buf = wl_resource_create(c, &wl_buffer_interface, 1, id);
    if (!buf) {
        if (db->bo_handle) dmabuf_release_bo(db->bo_handle);
        free(db); wl_client_post_no_memory(c); return NULL;
    }
    wl_resource_set_implementation(buf, &dmabuf_buffer_impl, db,
                                   dmabuf_buffer_destroy);
    return buf;
}

static void params_create(struct wl_client *c, struct wl_resource *r,
                          int32_t width, int32_t height, uint32_t format,
                          uint32_t flags)
{
    struct wl_resource *buf = params_make_buffer(c, r, 0, width, height, format, flags);
    if (buf)
        zwp_linux_buffer_params_v1_send_created(r, buf);
    else
        zwp_linux_buffer_params_v1_send_failed(r);
}

static void params_create_immed(struct wl_client *c, struct wl_resource *r,
                                uint32_t buffer_id, int32_t width, int32_t height,
                                uint32_t format, uint32_t flags)
{
    params_make_buffer(c, r, buffer_id, width, height, format, flags);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy      = params_destroy,
    .add          = params_add,
    .create       = params_create,
    .create_immed = params_create_immed,
};

static void params_resource_destroy(struct wl_resource *r)
{
    struct brook_dmabuf_params *p = wl_resource_get_user_data(r);
    if (p) { if (p->fd >= 0) close(p->fd); free(p); }
}

static void dmabuf_create_params(struct wl_client *c, struct wl_resource *r,
                                 uint32_t id)
{
    struct brook_dmabuf_params *p = calloc(1, sizeof(*p));
    if (!p) { wl_client_post_no_memory(c); return; }
    p->fd = -1;
    /* The params object must inherit the dmabuf object's version, or methods
     * added in later versions (create_immed, since v2) are rejected as
     * "invalid method". */
    struct wl_resource *pr = wl_resource_create(
        c, &zwp_linux_buffer_params_v1_interface,
        wl_resource_get_version(r), id);
    if (!pr) { free(p); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(pr, &params_impl, p, params_resource_destroy);
}

static void dmabuf_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }

/* Common B8G8R8X8/A8 formats Mesa uses for a window surface. */
#define FMT_XRGB8888 0x34325258  /* 'XR24' */
#define FMT_ARGB8888 0x34325241  /* 'AR24' */

/* dev_t of the DRM render node, used as the dmabuf-feedback main_device.
 * Resolved once at startup from g_drm_fd. */
static uint64_t g_drm_dev = 0;

/* Send a full dmabuf-feedback sequence (v4+). This is how Mesa's Wayland EGL
 * platform learns *which render node* to use: without it (or wl_drm) Mesa can't
 * find the GPU, falls back to Zink/swrast and crashes. We expose a single tranche
 * for our render node advertising the LINEAR-modifier XR24/AR24 formats. */
static void dmabuf_send_feedback(struct wl_resource *fb)
{
    /* Format table: 16-byte entries { u32 format, u32 pad, u64 modifier }. */
    struct fmt_entry { uint32_t format; uint32_t pad; uint64_t modifier; };
    static const struct fmt_entry table[] = {
        { FMT_XRGB8888, 0, 0 /* LINEAR */ },
        { FMT_ARGB8888, 0, 0 /* LINEAR */ },
    };
    const uint32_t n = sizeof(table) / sizeof(table[0]);

    int tfd = syscall(SYS_memfd_create, "brook-dmabuf-fmt", 0u);
    if (tfd >= 0) {
        if (write(tfd, table, sizeof(table)) == (ssize_t)sizeof(table)) {
            zwp_linux_dmabuf_feedback_v1_send_format_table(fb, tfd, sizeof(table));
        }
        /* The client dup()s the fd during the format_table event; ours can close. */
    }

    struct wl_array dev;
    wl_array_init(&dev);
    uint64_t *dp = wl_array_add(&dev, sizeof(uint64_t));
    if (dp) *dp = g_drm_dev;
    zwp_linux_dmabuf_feedback_v1_send_main_device(fb, &dev);

    /* One tranche targeting the same device, all formats in the table. */
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(fb, &dev);
    struct wl_array idx;
    wl_array_init(&idx);
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t *ip = wl_array_add(&idx, sizeof(uint16_t));
        if (ip) *ip = (uint16_t)i;
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(fb, &idx);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(fb, 0);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(fb);
    zwp_linux_dmabuf_feedback_v1_send_done(fb);

    wl_array_release(&idx);
    wl_array_release(&dev);
    if (tfd >= 0) close(tfd);
}

static void feedback_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
    .destroy = feedback_destroy,
};

static struct wl_resource *make_feedback(struct wl_client *c, struct wl_resource *r,
                                         uint32_t id)
{
    struct wl_resource *fb = wl_resource_create(
        c, &zwp_linux_dmabuf_feedback_v1_interface,
        wl_resource_get_version(r), id);
    if (!fb) { wl_client_post_no_memory(c); return NULL; }
    wl_resource_set_implementation(fb, &feedback_impl, NULL, NULL);
    dmabuf_send_feedback(fb);
    return fb;
}

static void dmabuf_get_default_feedback(struct wl_client *c, struct wl_resource *r,
                                        uint32_t id)
{ make_feedback(c, r, id); }

static void dmabuf_get_surface_feedback(struct wl_client *c, struct wl_resource *r,
                                        uint32_t id, struct wl_resource *surface)
{ (void)surface; make_feedback(c, r, id); }

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy               = dmabuf_destroy,
    .create_params         = dmabuf_create_params,
    .get_default_feedback  = dmabuf_get_default_feedback,
    .get_surface_feedback  = dmabuf_get_surface_feedback,
};

static void dmabuf_bind(struct wl_client *client, void *data, uint32_t version,
                        uint32_t id)
{
    (void)data;
    /* Advertise up to v4 — v4 enables get_default_feedback, which is how Mesa's
     * Wayland EGL platform discovers the render node (main_device). Without it
     * Mesa can't find the GPU and falls back to Zink/swrast. */
    uint32_t use = version < 4 ? version : 4;
    struct wl_resource *r = wl_resource_create(
        client, &zwp_linux_dmabuf_v1_interface, (int)use, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &dmabuf_impl, NULL, NULL);
    /* v1/v2 advertise formats (and v3 modifiers) at bind time. v4 clients use
     * get_default_feedback instead and ignore these, but sending them is
     * harmless and keeps older clients working. */
    if (use < 4) {
        zwp_linux_dmabuf_v1_send_format(r, FMT_XRGB8888);
        zwp_linux_dmabuf_v1_send_format(r, FMT_ARGB8888);
        if (use >= 3) {
            zwp_linux_dmabuf_v1_send_modifier(r, FMT_XRGB8888, 0, 0);
            zwp_linux_dmabuf_v1_send_modifier(r, FMT_ARGB8888, 0, 0);
        }
    }
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    fprintf(stderr, "[waylandd] starting (Phase D — kernel-WM unified)\n");
    int run_seconds = 0;
    const char *xwayland_server=NULL,*xwm_helper=NULL,*xdisplay=":0";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--trace-frames")) g_trace_frames = 1;
        if (!strcmp(argv[i],"--xwayland") && i+1<argc) xwayland_server=argv[++i];
        if (!strcmp(argv[i],"--xwm") && i+1<argc) xwm_helper=argv[++i];
        if (!strcmp(argv[i],"--xdisplay") && i+1<argc) xdisplay=argv[++i];
        if ((!strcmp(argv[i], "--for") || !strcmp(argv[i], "--seconds"))
            && i + 1 < argc)
            run_seconds = atoi(argv[++i]);
    }
    if (!!xwayland_server!=!!xwm_helper || xdisplay[0]!=':' || !xdisplay[1] ||
        strspn(xdisplay+1,"0123456789")!=strlen(xdisplay+1) || strlen(xdisplay)>3 || atoi(xdisplay+1)>63) return 2;

    g_display = wl_display_create();
    if (!g_display) {
        fprintf(stderr, "[waylandd] FAIL: wl_display_create returned NULL\n");
        return 1;
    }

    if (!xwayland_shell_init(g_display)) return 1;

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
    if (!wl_global_create(g_display,&zwp_relative_pointer_manager_v1_interface,1,NULL,relative_manager_bind)) return 1;
    if (!wl_global_create(g_display,&zwp_pointer_constraints_v1_interface,1,NULL,pointer_constraints_bind)) return 1;
    if (!wl_global_create(g_display, &zwp_text_input_manager_v3_interface, 1, NULL, text_input_manager_bind)) return 1;
    if (!wl_global_create(g_display, &wl_output_interface, 4, NULL, output_bind)) return 1;
    if (!wl_global_create(g_display, &zxdg_decoration_manager_v1_interface, 1, NULL, deco_mgr_bind)) return 1;
    if (!wl_global_create(g_display, &org_kde_kwin_server_decoration_manager_interface, 1, NULL, kde_decoration_manager_bind)) return 1;
    if (!wl_global_create(g_display, &wp_viewporter_interface, 1, NULL, viewporter_bind)) return 1;
    if (!wl_global_create(g_display, &wl_subcompositor_interface, 1, NULL, subcomp_bind)) return 1;
    if (!wl_global_create(g_display, &wl_data_device_manager_interface, 3, NULL, ddm_bind)) return 1;
    if (!wl_global_create(g_display, &zwp_linux_dmabuf_v1_interface, 4, NULL, dmabuf_bind)) return 1;
    /* Open the DRM render node once for PRIME dmabuf import (hardware GL windows).
     * Absence is non-fatal: we simply won't advertise working dmabuf import. */
    g_drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (g_drm_fd < 0) {
        fprintf(stderr, "[waylandd] dmabuf: no DRM node (%s) — GL windows will be software\n",
                strerror(errno));
    } else {
        /* main_device for dmabuf feedback = the render node's dev_t, so Mesa's
         * Wayland EGL platform knows which GPU to open. */
        struct stat st;
        if (fstat(g_drm_fd, &st) == 0) g_drm_dev = (uint64_t)st.st_rdev;
        fprintf(stderr, "[waylandd] dmabuf: DRM node fd=%d dev=0x%llx\n",
                g_drm_fd, (unsigned long long)g_drm_dev);
    }
    fprintf(stderr, "[waylandd] globals advertised\n");

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    struct wl_event_loop *loop = wl_display_get_event_loop(g_display);
    if (!loop) return 1;
    g_frame_timer = wl_event_loop_add_timer(loop, frame_clock_tick, NULL);
    if (!g_frame_timer) return 1;
    wl_event_source_timer_update(g_frame_timer, 16);
    if (xwayland_server && !xwayland_start(xwayland_server,xwm_helper,xdisplay)) {
        fprintf(stderr,"[waylandd] owned Xwayland launch failed; native Wayland remains available\n");
    }

    uint32_t loop_start = g_now_ms();
    for (;;) {
        if (g_shutdown) break;
        wl_display_flush_clients(g_display);
        for (int sub = 0; sub < 60 && !g_shutdown; ++sub) {
            uint32_t input_start = g_trace_frames ? g_now_ms() : 0;
            xwayland_pump();
            pump_input_once();
            uint32_t dispatch_start = g_trace_frames ? g_now_ms() : 0;
            int n = wl_event_loop_dispatch(loop, 16);
            if (n < 0) {
                fprintf(stderr, "[waylandd] event loop error: %s\n", strerror(errno));
                goto done;
            }
            wl_display_flush_clients(g_display);
            xwayland_pump();
            if (g_trace_frames) {
                uint32_t now = g_now_ms();
                uint32_t input_ms = dispatch_start - input_start;
                uint32_t dispatch_ms = now - dispatch_start;
                if (input_ms > g_trace_input_max_ms) g_trace_input_max_ms = input_ms;
                if (dispatch_ms > g_trace_dispatch_max_ms) g_trace_dispatch_max_ms = dispatch_ms;
                ++g_trace_dispatches;
                trace_frames_report(now);
            }
        }
        if (run_seconds > 0 && (int)((g_now_ms() - loop_start) / 1000) >= run_seconds)
            break;
    }
done:
    fprintf(stderr, "[waylandd] shutting down (commits=%d)\n", g_commit_count);
    xwayland_generation_lost();
    wl_display_destroy(g_display);
    fprintf(stderr, "[waylandd] PASS\n");
    return 0;
}
