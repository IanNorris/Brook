#include "toplevel-size.h"
#include "latency-stats.h"
#include "wm-input-abi.h"
#include "input_preferences.h"
#include "brook-caption-strip.h"

#include <stddef.h>
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
#include <poll.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <limits.h>

#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"
#include "xdg-decoration-server-protocol.h"
#include "kde-server-decoration-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "fractional-scale-server-protocol.h"
#include "brook_desktop_scale.h"
#include "dmabuf-layout.h"
#include "nv-modifiers.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "text-input-v3-server-protocol.h"
#include "relative-pointer-v1-server-protocol.h"
#include "pointer-constraints-v1-server-protocol.h"
#include "idle-inhibit-unstable-v1-server-protocol.h"
#include "pointer-region.h"
#include "pointer-hit.h"
#include "surface-tree.h"
#include "surface-geometry.h"
#include "gpu-scene-abi.h"
#include "gpu_buffer_abi.h"
#include "gpu_window_source_abi.h"
#include "gpu_window_channel.h"
#include "intel-tiling.h"
#include "intel_pending_drain.h"
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
#define BROOK_SYS_WM_SHOW_WINDOW_MENU 0xB25
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
_Static_assert(sizeof(struct brook_wm_event) == 12 &&
               offsetof(struct brook_wm_event, x) == 4 &&
               offsetof(struct brook_wm_event, y) == 6 &&
               offsetof(struct brook_wm_event, reserved) == 8,
               "WM_POP_INPUT event layout");

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
#define WM_EVT_POINTER_LEFT    0x84 /* pointer left this window's client area */
#define WM_RESIZE_MAXIMIZED     1
#define WM_RESIZE_FULLSCREEN    4
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
/* One output, fixed for this provider lifetime. Surface geometry stays logical;
 * native WM buffers and coordinates stay physical. Xwayland sees scale 1 and
 * device pixels ("apps scale themselves", told by brook-xwm's Xft.dpi and
 * XSETTINGS); only windows the XWM marks BXWM_SCALED are presented at the
 * output scale (see xwayland-xwm.h). */
static uint32_t g_output_scale=1;
enum { BROOK_WL_SCALE_ONE=120 };
static uint32_t g_output_scale120=BROOK_WL_SCALE_ONE;

static int64_t logical_to_physical(int64_t pixels, uint32_t scale120)
{
    return pixels>=0 ? (pixels*scale120+60)/BROOK_WL_SCALE_ONE :
                       -((-pixels*scale120+60)/BROOK_WL_SCALE_ONE);
}

static uint32_t physical_to_logical(uint32_t pixels, uint32_t scale120)
{
    return (uint64_t)pixels*BROOK_WL_SCALE_ONE/scale120;
}

static uint64_t g_now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec*1000000ull+(uint64_t)ts.tv_nsec/1000u;
}
#include "buffer-trace.h"

/* Per-frame chain timing, always on (a few adds per frame), reported in the
 * status file for X11 (Xwayland) and native roots separately:
 *  interval     buffer commit to the next buffer commit of the window;
 *  offer        commit to GPU offer sent (producer fence obtained);
 *  presented    offer sent to the renderer's PRESENTED;
 *  callback     commit to its wl_surface.frame done;
 *  turnaround   frame done to the client's next buffer commit (the client,
 *               e.g. Xwayland Present plus the game, not this server). */
struct brook_pace { uint64_t count,sum_us,max_us; };
enum { PACE_INTERVAL,PACE_OFFER,PACE_PRESENTED,PACE_CALLBACK,PACE_TURNAROUND,PACE_KINDS };
static struct brook_pace g_pace[2][PACE_KINDS];
static uint64_t g_pace_dmabuf_created[2],g_pace_early_callbacks;
static void pace_add(int x11,int kind,uint64_t start_us,uint64_t end_us){
    if(!start_us || end_us<start_us)return;
    struct brook_pace *p=&g_pace[x11?1:0][kind];const uint64_t d=end_us-start_us;
    ++p->count;p->sum_us+=d;if(d>p->max_us)p->max_us=d;
}
static uint64_t now_us_since(uint64_t start_us){const uint64_t now=g_now_us();return now>start_us?now-start_us:0;}
static uint32_t g_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint32_t)(ts.tv_nsec / 1000000));
}

static long wm_create_window(uint32_t w, uint32_t h, const char* title,
                             uint32_t flags, struct brook_wm_create_out* out) {
    long result = syscall(BROOK_SYS_WM_CREATE_WINDOW, (long)w, (long)h,
                          (long)title, (long)out, (long)(flags | BROOK_WM_CREATE_FLAG_FINE_SCROLL));
    if (result == -1 && errno == EINVAL) {
        /* Older kernels reject the fine scroll opt-in bit. */
        result = syscall(BROOK_SYS_WM_CREATE_WINDOW, (long)w, (long)h,
                         (long)title, (long)out, (long)flags);
    }
    return result;
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
/* mode 0 server side, 1 client side, 2 client side with Brook's caption
 * buttons overlaid at the top-right; insets = right | top << 16 (VFB px). */
static long wm_set_decoration(uint32_t id, int mode, uint32_t insets, uint32_t height,
                              uint32_t style, const BrookCaptionStrip *strip) {
    return syscall(BROOK_SYS_WM_SET_DECORATION_MODE, (long)id, (long)mode,
                   (long)insets, (long)height, (long)style, (long)strip);
}
static long wm_set_decoration_mode(uint32_t id, int mode, uint32_t insets, uint32_t height, uint32_t style) {
    return wm_set_decoration(id, mode, insets, height, style, NULL);
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
/* Fails (ENOSYS) on kernels without fullscreen support. */
static long wm_set_fullscreen(uint32_t id, int enable) {
    return syscall(BROOK_SYS_WM_SET_FULLSCREEN, (long)id, (long)(enable != 0));
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
/* The 6th argument names the window the cursor was set for (tag | wm id):
 * the kernel shows it only over that window's client area and the built-in
 * arrow elsewhere. 0 (no window known) leaves ownership to the kernel. */
#define BROOK_CURSOR_OWNER_TAG (0xC5C50000ull<<32)
static long wm_set_cursor_image_for(const uint32_t *pixels, uint32_t w, uint32_t h,
                                    int32_t hot_x, int32_t hot_y, uint32_t owner_wm) {
    return syscall(BROOK_SYS_WM_SET_CURSOR_IMAGE, (long)pixels, (long)w,
                   (long)h, (long)hot_x, (long)hot_y,
                   owner_wm ? (long)(BROOK_CURSOR_OWNER_TAG|owner_wm) : 0L);
}
static long wm_set_cursor_image(const uint32_t *pixels, uint32_t w, uint32_t h,
                                int32_t hot_x, int32_t hot_y) {
    return wm_set_cursor_image_for(pixels, w, h, hot_x, hot_y, 0);
}
static long wm_get_screen_info(uint32_t *w_out, uint32_t *h_out) {
    uint32_t buf[2] = {0, 0};
    long rc = syscall(BROOK_SYS_WM_GET_SCREEN_INFO, (long)buf, 0, 0, 0, 0);
    if (rc == 0) { *w_out = buf[0]; *h_out = buf[1]; }
    return rc;
}
static void output_scale_sample(int startup) {
    long percent=syscall(0xB04,2L,0L,0L,0L,0L,0L);
    if(startup) {
        // The first compositor frame may not yet have applied the saved value.
        long requested=syscall(0xB04,0L,0L,0L,0L,0L,0L);
        if(requested>=100 && requested<=200 && brook_desktop_scale_valid((unsigned)requested))
            percent=requested;
    }
    if(percent<100 || percent>200 || !brook_desktop_scale_valid((unsigned)percent)) return;
    g_output_scale120=(uint32_t)percent*BROOK_WL_SCALE_ONE/100;
    g_output_scale=(uint32_t)(percent+50)/100;
}
static void output_scale_init(void) {
    output_scale_sample(1);
    fprintf(stderr,"[waylandd] output scale=%u/%u legacy=%u\n",
            g_output_scale120,BROOK_WL_SCALE_ONE,g_output_scale);
}

/* ---------------- per-surface state ---------------- */

struct brook_seat_client;
struct brook_surface;
static void seat_clients_scrub_surface(struct brook_surface *s);
static void seat_clients_unmap_surface(struct brook_surface *s);
static void pointer_constraints_unmap(struct brook_surface *s,int destroy);
static void idle_inhibit_surface_destroyed(struct brook_surface *s);
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
    uint64_t trace_device, trace_inode;  /* producer storage, never the CPU snapshot */
    uint32_t nv_bo;       /* nv-drm (renderD130) GEM alias; 0 = not an nv-drm import */
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
    uint64_t trace_epoch, trace_cycle;
    unsigned trace_serial;
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
    if (!--buffer->uses && !buffer->uncertain && buffer->resource) {
        wl_buffer_send_release(buffer->resource);
        if (g_buffer_trace.pid && buffer->trace_epoch == g_buffer_trace.epoch && buffer->trace_cycle)
            buffer_trace_record(BUFFER_RELEASE, buffer->trace_serial, 0, buffer->trace_cycle, 0, 0, NULL);
    }
}

struct frame_latency_event {
    uint64_t done_us, commit, current_commit;
    uint32_t surface_object, root_wm;
    int pending, waiting, committed;
    const char *frames_state;
    int offer_state, renderer_presented; /* -1: no matching evidence */
};

struct frame_latency {
    struct latency_hist hist;
    struct frame_latency_event worst;
};

struct brook_frame_callback {
    struct wl_resource *resource;
    struct wl_list link;
    struct brook_surface *surface;
    uint32_t requested_ms, committed_ms;
    uint64_t latency_commit_us, latency_epoch;
    int done;
    uint64_t commit;
};

enum configure_event_kind { CONFIGURE_SENT, CONFIGURE_ACK, CONFIGURE_COMMIT };
enum { CONFIGURE_HISTORY_LIMIT = 16 };
struct configure_event {
    uint32_t ms, serial;
    int32_t width, height;
    enum configure_event_kind kind;
    unsigned resizing : 1;
    unsigned attached : 1;
    unsigned acked : 1;
};

struct brook_scene_owner;
struct brook_xwindow;
struct brook_surface {
    struct bt_node tree;
    struct brook_scene_owner *scene;
    int scene_disabled;
    int role; /* permanent: 1=xdg, 2=subsurface, 3=cursor, 4=Xwayland */
    struct wl_resource *resource;
    struct wl_resource *fractional_scale;
    struct pointer_region input_region,pending_input_region;
    int input_region_pending;
    /* A revoked pointer lock stays inhibited for this surface until the
     * kernel's per-window engage serial (a click in its content) advances. */
    int pointer_lock_inhibited;
    uint64_t pointer_lock_rearm_serial;
    struct brook_buffer *pending_buffer;
    uint64_t trace_pending_attachment, trace_pending_epoch;
    int pending_attach;
    /* Own committed SHM pixels after releasing the client's buffer. Never use
     * the shared kernel VFB as the backing store for a surface tree. */
    uint32_t *image, *composite;
    uint32_t image_w, image_h;
    size_t composite_pixels;
    /* Monotonic per-window number for every published source (GPU offer,
     * layered offer or tagged CPU snapshot). Independent of tree commits so
     * a desynchronised subsurface update still publishes a newer source. */
    uint64_t publish_commit;
    /* wl_surface.set_opaque_region, kept only when it is one added rectangle
     * (the exact, conservative case); applied on commit. */
    int opaque_set,pending_opaque_set,opaque_pending;
    int32_t opaque_x,opaque_y,opaque_w,opaque_h,pending_opaque_x,pending_opaque_y,pending_opaque_w,pending_opaque_h;

    /* xdg-shell role state. */
    struct wl_resource *xdg_surface;
    struct wl_resource *xdg_toplevel;
    struct wl_resource *xdg_popup;        /* popup role (mutex with toplevel) */
    struct wl_resource *xwayland_surface;
    uint64_t xwayland_pending_serial, xwayland_serial, xwayland_generation;
    struct brook_xwindow *xwindow;
    int  xdg_initial_commit_done;
    int  xdg_acked; /* Initial handshake, not acknowledgement of the newest state. */
    struct toplevel_size_hints size_hints,pending_size_hints;
    unsigned size_hints_pending;
    int  toplevel_activated;
    /* Native WM focus from this window's own FOCUS_GAINED/LOST events. Each
     * window's queue is ordered, but windows are pumped one after another,
     * so per-event focus handling can apply an older GAINED last. */
    int  native_focused;
    int  toplevel_maximized,toplevel_resizing;
    /* toplevel_fullscreen: the kernel window covers the output (configured
     * with the fullscreen state). want_fullscreen: the client's request,
     * applied once the kernel window exists. */
    int  toplevel_fullscreen,want_fullscreen;
    int32_t configure_width,configure_height;
    int32_t bounds_width,bounds_height;
    int configure_size_pending,configure_size_acked;
    uint32_t configure_size_serial;
    int32_t window_geometry_w, window_geometry_h;
    int32_t pending_geometry_w, pending_geometry_h;
    int32_t window_geometry_x, window_geometry_y;
    int32_t pending_geometry_x, pending_geometry_y;
    int window_geometry_pending;
    struct brook_geometry_state geometry;
    struct wl_resource *viewport;
    uint32_t next_configure_serial; /* Last emitted serial, from next_serial(). */
    /* Popup geometry derived from positioner at get_popup time. */
    int32_t popup_w, popup_h;
    int32_t popup_x, popup_y;
    struct brook_surface *popup_parent;
    int capture_excluded;

    /* Requests become eligible only when their surface state is committed. */
    struct wl_list pending_frames, committed_frames, waiting_frames;
    uint64_t trace_id, trace_requests, trace_commits, trace_buffers;
    uint64_t trace_done, trace_cancelled;
    uint32_t trace_request_max_ms, trace_committed_max_ms;

    /* Latest set_title — copied so it survives the wayland string. */
    char title[BROOK_WM_POLICY_TITLE_BYTES];
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
    /* xdg-decoration object and the mode the client asked for, so a policy
     * decided by app_id (which may arrive later) can reconfigure it. */
    struct wl_resource *deco_resource;
    int deco_requested_csd;
    /* Mode and insets last sent to the kernel WM (-1: nothing sent yet). */
    int deco_sent_mode;
    uint32_t deco_sent_insets, deco_sent_height, deco_sent_style;
    /* Client-matched caption style: the strip colour and corner radius
     * sampled from the client's buffer (packed for syscall 512), when the
     * next sample is due, and a newest GPU frame still waiting to be read. */
    uint32_t strip_style, strip_last_ms;
    BrookCaptionStrip strip, deco_sent_strip;
    int strip_due, strip_trailing;
    struct brook_image *strip_retry;
    unsigned strip_retry_count;

    /* Cursor surface state. When a surface is assigned as cursor via
     * wl_pointer.set_cursor, its commits upload pixels to the kernel
     * compositor rather than blitting to a window VFB. */
    int     is_cursor;
    int32_t cursor_hot_x;
    int32_t cursor_hot_y;
    uint32_t cursor_output_scale;
    uint32_t cursor_owner_wm; /* root window of the surface it was set over */

    /* Linked list of all surfaces — used by the input pump. */
    struct brook_surface *next;

    /* Subsurface relationship (non-NULL when this surface has a subsurface role). */
    struct brook_surface *subsurface_parent;
    int32_t sub_x, sub_y;            /* position relative to parent */
    struct brook_surface *children;   /* linked list of child subsurfaces */
    struct brook_surface *sibling;    /* next child in parent's children list */
    struct wl_resource   *subsurface_resource; /* wl_subsurface resource if role is subsurface */
    int dmabuf_subsurface_reported; /* bounded diagnostic: once per surface */
    /* Frame pacing (status "pacing"): last buffer commit and last frame
     * callback sent, in microseconds. */
    uint64_t pace_commit_us,pace_callback_us;
    /* Zero-copy step 2: the image whose nv dmabuf the kernel shows for this window (one
     * reference; its wl_buffer is released when it is replaced or detached). */
    struct brook_image *nv_layer_image;
    struct frame_latency latency;
    /* Status diagnostics ("surfaces" in the status file): the newest serial
     * the client acknowledged, and when it acked, was last configured and
     * last committed (with and without a buffer), in g_now_ms(). */
    uint32_t last_ack_serial,last_ack_ms,configure_ms,commit_ms,buffer_commit_ms;
    /* idle-inhibit.h: inhibitors resolving to this root, and the count last
     * sent for its current kernel window (reset when the window goes). */
    unsigned idle_inhibit_want,idle_inhibit_sent;
    struct configure_event configure_history[CONFIGURE_HISTORY_LIMIT];
    unsigned configure_history_next, configure_history_count;
};

static struct brook_surface *g_surfaces = NULL;

enum latency_input_kind { LATENCY_MOTION, LATENCY_AXIS, LATENCY_BUTTON, LATENCY_KEY, LATENCY_INPUT_KINDS };
struct client_latency_data {
    struct frame_latency commit_to_done;
    struct latency_hist done_to_commit;
    struct latency_hist input[LATENCY_INPUT_KINDS];
    uint64_t last_done_us, last_axis_us, axis_burst_max_gap_us;
};

struct client_latency {
    struct wl_listener destroy;
    struct wl_list link;
    pid_t pid;
    uint64_t connection;
    struct client_latency_data data;
};

static struct wl_list g_latency_clients = {&g_latency_clients, &g_latency_clients};
static uint64_t g_stats_epoch = 1, g_latency_connection_id;
static uint64_t g_input_batch_us;
static int g_input_recorded;

static void latency_client_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct client_latency *client = wl_container_of(listener, client, destroy);
    wl_list_remove(&client->link);
    wl_list_remove(&client->destroy.link);
    free(client);
}

static struct client_latency *latency_client(struct wl_client *client) {
    struct wl_listener *listener = wl_client_get_destroy_listener(client, latency_client_destroy);
    if (listener) {
        struct client_latency *stats = wl_container_of(listener, stats, destroy);
        return stats;
    }
    struct client_latency *stats = calloc(1, sizeof(*stats));
    if (!stats) return NULL;
    wl_client_get_credentials(client, &stats->pid, NULL, NULL);
    stats->connection = ++g_latency_connection_id;
    stats->destroy.notify = latency_client_destroy;
    wl_client_add_destroy_listener(client, &stats->destroy);
    wl_list_insert(g_latency_clients.prev, &stats->link);
    return stats;
}

static struct brook_surface *latency_toplevel(struct brook_surface *s) {
    if (s->is_cursor) return NULL;
    while (s->subsurface_parent) s = s->subsurface_parent;
    /* Popup frame callbacks are their own pacing stream. */
    return !s->xdg_popup && (s->xdg_toplevel || s->role == 4 || s->wm_id) ? s : NULL;
}

static void latency_input_sent(struct wl_client *client, enum latency_input_kind kind) {
    /* One kernel event can be sent on more than one seat binding. Count it
     * once for the owning connection, after its first protocol delivery. */
    if (!g_input_batch_us || g_input_recorded) return;
    const uint64_t sent = g_now_us();
    if (sent < g_input_batch_us) return;
    g_input_recorded = 1;
    struct client_latency *stats = latency_client(client);
    if (!stats) return;
    latency_record(&stats->data.input[kind], g_input_batch_us, sent);
    if (kind == LATENCY_AXIS)
        latency_axis_sent(sent, &stats->data.last_axis_us, &stats->data.axis_burst_max_gap_us);
}

static void latency_commit(struct brook_surface *s, struct wl_client *client) {
    struct client_latency *stats = latency_client(client);
    if (wl_list_empty(&s->pending_frames) && (!stats || !stats->data.last_done_us)) return;
    const uint64_t now = g_now_us();
    if (stats && stats->data.last_done_us && now) {
        latency_record(&stats->data.done_to_commit, stats->data.last_done_us, now);
        stats->data.last_done_us = 0;
    }
    struct brook_frame_callback *frame;
    wl_list_for_each(frame, &s->pending_frames, link) {
        frame->latency_commit_us = now;
        frame->latency_epoch = g_stats_epoch;
    }
}

static int g_drm_fd = -1;
static int g_drm_intel;
/* nv-drm dmabuf path (notes/m5/zero-copy-design.md): renderD130, opened only when no
 * virtio/i915 node exists and BROOK_WAYLAND_NV_DMABUF=1 or /etc/waylandd-nv-dmabuf is set. */
static int g_nv_fd = -1;
static struct brook_image *nv_dmabuf_snapshot(const struct brook_dmabuf_buffer *db,int *uncertain);
static int popup_apply_position(struct brook_surface *);
static int xwayland_surface_ready(const struct brook_surface *);
static unsigned xwayland_surface_flags(const struct brook_surface *);
static void xwayland_surface_created(struct brook_surface *);
static void xwayland_surface_detach(struct brook_surface *);
static void xwayland_native_unmapped(struct brook_surface *);
static void xwayland_focus_reconciled(struct brook_surface *);
static void xwayland_pair_surfaces(void);
static void xwayland_generation_lost(void);
static int xwayland_accept_view(struct brook_surface *,uint32_t,uint32_t);
static uint32_t xwayland_surface_scale(const struct brook_surface *s);
/* Native-scale clients (zero-copy step 2): a client started with BROOK_WAYLAND_NATIVE_SCALE=1
 * in its environment (/boot/bin/vkdirect sets it) sees output scale 1 (wl_output.scale 1,
 * fractional 120) and is configured in physical pixels, fullscreen and maximised included,
 * so its buffers are 1:1 with its window and the kernel can show them directly. Read once
 * per client from /proc/<pid>/environ. */
static struct wl_client *g_xwayland_client;   /* xwayland-shell.h */
struct native_client { struct wl_client *client;int native;struct wl_listener destroy;struct native_client *next; };
static struct native_client *g_native_clients;
static void native_client_destroyed(struct wl_listener *l,void *data){
    (void)data;struct native_client *n=wl_container_of(l,n,destroy);
    for(struct native_client **p=&g_native_clients;*p;p=&(*p)->next)if(*p==n){*p=n->next;break;}
    free(n);
}
static int environ_native_scale(pid_t pid){
    char path[48];snprintf(path,sizeof(path),"/proc/%d/environ",(int)pid);
    FILE *f=fopen(path,"r");if(!f)return 0;
    static const char key[]="BROOK_WAYLAND_NATIVE_SCALE=1";
    char buf[4096];size_t n,keep=0;int found=0;
    /* entries are NUL-separated; match a whole entry */
    while(!found && (n=fread(buf+keep,1,sizeof(buf)-1-keep,f))>0){
        n+=keep;buf[n]=0;size_t i=0;
        while(i<n){size_t len=strnlen(buf+i,n-i);
            if(i+len==n && !feof(f)){memmove(buf,buf+i,len);keep=len;break;}
            if(len==sizeof(key)-1 && !memcmp(buf+i,key,len)){found=1;break;}
            i+=len+1;keep=0;}
        if(keep>=sizeof(buf)-2)keep=0;
    }
    fclose(f);return found;
}
static int client_native_scale(struct wl_client *client){
    if(!client || client==g_xwayland_client)return 0;
    for(struct native_client *n=g_native_clients;n;n=n->next)if(n->client==client)return n->native;
    struct native_client *n=calloc(1,sizeof(*n));if(!n)return 0;
    pid_t pid=0;uid_t uid;gid_t gid;wl_client_get_credentials(client,&pid,&uid,&gid);
    n->client=client;n->native=pid>0 && environ_native_scale(pid);
    n->destroy.notify=native_client_destroyed;wl_client_add_destroy_listener(client,&n->destroy);
    n->next=g_native_clients;g_native_clients=n;
    if(n->native)fprintf(stderr,"[waylandd] client pid=%d: native scale (physical pixels)\n",(int)pid);
    return n->native;
}
static uint32_t surface_output_scale(const struct brook_surface *s) {
    if (s->is_cursor && s->cursor_output_scale) return s->cursor_output_scale;
    bool fractional=s->fractional_scale!=NULL;
    for (unsigned depth=0; depth<64 && (s->tree.parent || s->popup_parent); ++depth) {
        s=s->tree.parent?s->tree.parent->user:s->popup_parent;
        fractional|=s->fractional_scale!=NULL;
    }
    if (s->role==4) return xwayland_surface_scale(s)*BROOK_WL_SCALE_ONE;
    if (s->resource && client_native_scale(wl_resource_get_client(s->resource))) return BROOK_WL_SCALE_ONE;
    return fractional?g_output_scale120:g_output_scale*BROOK_WL_SCALE_ONE;
}
static wl_fixed_t surface_pointer_fixed(const struct brook_surface *s,int32_t physical) {
    int64_t value=(int64_t)physical*256*BROOK_WL_SCALE_ONE/surface_output_scale(s);
    return value<INT32_MIN?INT32_MIN:value>INT32_MAX?INT32_MAX:(wl_fixed_t)value;
}
static int32_t pointer_fixed_floor(wl_fixed_t value) {
    return (int32_t)(value>=0?(int64_t)value/256:-((-(int64_t)value+255)/256));
}
#define BROOK_WAYLAND_SOURCE_COMMIT_TAG 1
struct brook_image;
static void intel_image_want_pixels(struct brook_image *image);
static void surface_sync_decoration(struct brook_surface *s, int force);
static void strip_retry_drop(struct brook_surface *s);
struct brook_image;
static void surface_strip_commit(struct brook_surface *s, struct brook_image *older);
/* Fullscreen is native-window state: the kernel resizes the window to the
 * output and answers with WM_EVT_RESIZED carrying WM_RESIZE_FULLSCREEN, which
 * configures the client (fullscreen state, output size); leaving restores the
 * earlier geometry the same way. A request made before the window exists is
 * applied when it is created. There is one output, so set_fullscreen's
 * output argument is ignored. */
static void toplevel_apply_fullscreen(struct brook_surface *s) {
    if (!s || !s->wm_id || !s->xdg_toplevel || !!s->want_fullscreen==!!s->toplevel_fullscreen) return;
    long rc=wm_set_fullscreen(s->wm_id,s->want_fullscreen);
    if (rc==0) s->toplevel_fullscreen=s->want_fullscreen;
    fprintf(stderr,"[waylandd] xdg_toplevel fullscreen=%d wm=%u rc=%ld\n",s->want_fullscreen,s->wm_id,rc);
}
static int capture_excluded(const struct brook_surface *surface);
struct brook_draw;
static int nv_layer_present(struct brook_surface *root,struct brook_draw *draws,unsigned count);
static int nv_layer_materialize(struct brook_surface *root,struct brook_draw *draws,unsigned count);
static void nv_layer_detach(struct brook_surface *root);
static void nv_layer_forget(struct brook_surface *s);
struct brook_buffer;struct brook_dmabuf_buffer;
static int nv_layer_candidate(const struct brook_surface *s,const struct brook_dmabuf_buffer *db);
static struct brook_image *nv_layer_image(struct brook_buffer *buffer,int *uncertain);
#include "surface-scene.h"
static struct brook_image *intel_dmabuf_snapshot(const struct brook_dmabuf_buffer *db,int *uncertain);
static struct brook_image *intel_dmabuf_defer(struct brook_buffer *buffer);
static void intel_pending_poll(void);
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

static int popup_apply_position(struct brook_surface *s) {
    if (!s || !s->xdg_popup || !s->wm_id || !s->popup_parent || !s->popup_parent->wm_id)
        return 0;
    const int64_t scale=surface_output_scale(s);
    int64_t px=logical_to_physical(s->popup_x,scale),py=logical_to_physical(s->popup_y,scale);
    if (px<INT32_MIN || px>INT32_MAX || py<INT32_MIN || py>INT32_MAX) return 0;
    long rc = wm_move_relative(s->wm_id, s->popup_parent->wm_id, (int32_t)px, (int32_t)py);
    if (rc != 0) {
        fprintf(stderr, "[waylandd] WM_MOVE_RELATIVE popup=%u parent=%u at (%d,%d) failed rc=%ld errno=%d\n",
                s->wm_id, s->popup_parent->wm_id, s->popup_x, s->popup_y, rc, errno);
    } else {
        fprintf(stderr, "[waylandd] WM_MOVE_RELATIVE popup=%u parent=%u at (%d,%d)\n",
                s->wm_id, s->popup_parent->wm_id, s->popup_x, s->popup_y);
    }
    return rc == 0;
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
    s->trace_pending_attachment = 0;
    if (g_buffer_trace.pid && next) {
        pid_t pid;
        wl_client_get_credentials(c, &pid, NULL, NULL);
        if (pid == g_buffer_trace.pid) {
            const struct brook_dmabuf_buffer *db = next->dmabuf;
            next->trace_serial = buffer_trace_resource(buffer, db != NULL,
                db ? db->trace_device : 0, db ? db->trace_inode : 0,
                db ? db->width : wl_shm_buffer_get_width(next->shm),
                db ? db->height : wl_shm_buffer_get_height(next->shm));
            if (next->trace_epoch != g_buffer_trace.epoch) {
                next->trace_epoch = g_buffer_trace.epoch;
                next->trace_cycle = 0;
            }
            if (next->trace_serial) {
                s->trace_pending_attachment = ++g_buffer_trace.next_attachment;
                s->trace_pending_epoch = g_buffer_trace.epoch;
                buffer_trace_record(BUFFER_ATTACH, next->trace_serial, s->trace_pending_attachment,
                                    0, wl_resource_get_id(r), next->uses, NULL);
            }
        }
    }
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
    callback->requested_ms = g_now_ms();
    if (g_trace_frames) ++surface->trace_requests;
    wl_list_insert(surface->pending_frames.prev, &callback->link);
    wl_resource_set_implementation(callback->resource, NULL, callback, frame_callback_destroy);
}
static const char *surface_callback_state(struct brook_surface *, uint32_t now);

static void latency_frame_done(struct brook_surface *s, const struct brook_frame_callback *frame,
                               uint32_t now, const struct brook_image *presented) {
    if (!frame->latency_commit_us || frame->latency_epoch != g_stats_epoch) return;
    struct brook_surface *root = latency_toplevel(s);
    const uint64_t done = g_now_us();
    if (done < frame->latency_commit_us) return;
    struct client_latency *client = latency_client(wl_resource_get_client(frame->resource));
    if (!client) return;
    client->data.last_done_us = done;
    struct frame_latency *aggregate = &client->data.commit_to_done;
    const int client_worst = latency_record(&aggregate->hist, frame->latency_commit_us, done);
    const int surface_worst = root && latency_record(&root->latency.hist, frame->latency_commit_us, done);
    if (!client_worst && !surface_worst) return;

    const struct bt_state *current = s->tree.current;
    const struct brook_image *image = NULL;
    if (presented && presented->offer_tree_commit == frame->commit) image = presented;
    else if (current && current->commit == frame->commit && current->value) {
        const struct brook_view *view = current->value;
        image = view->image;
    }
    const struct frame_latency_event event = {
        .done_us = done, .commit = frame->commit, .current_commit = current ? current->commit : 0,
        .surface_object = wl_resource_get_id(s->resource), .root_wm = root ? root->wm_id : 0,
        .pending = wl_list_length(&s->pending_frames), .waiting = wl_list_length(&s->waiting_frames),
        .committed = wl_list_length(&s->committed_frames), .frames_state = surface_callback_state(s, now),
        .offer_state = image ? image->offer_state : -1,
        .renderer_presented = image && (image->offer_state || image->pending_cpu ||
                                      image->group_refs || image->gpu_presented) ? image->gpu_presented : -1,
    };
    if (client_worst) aggregate->worst = event;
    if (surface_worst) root->latency.worst = event;
}

/* Send eligible callbacks whose commit is at most limit. */
static void surface_send_frame_callbacks_upto(struct brook_surface *surface, uint32_t now, uint64_t limit,
                                             const struct brook_image *presented) {
    struct brook_frame_callback *callback, *next;
    wl_list_for_each_safe(callback, next, &surface->committed_frames, link) {
        if (callback->commit > limit) continue;
        if (!surface->subsurface_parent && surface->pace_commit_us>surface->pace_callback_us) {
            const uint64_t now_us=g_now_us();
            pace_add(surface->role==4,PACE_CALLBACK,surface->pace_commit_us,now_us);surface->pace_callback_us=now_us;
        }
        if (g_trace_frames) {
            uint32_t requested = now - callback->requested_ms;
            uint32_t committed = now - callback->committed_ms;
            if (requested > surface->trace_request_max_ms) surface->trace_request_max_ms = requested;
            if (committed > surface->trace_committed_max_ms) surface->trace_committed_max_ms = committed;
            ++surface->trace_done;
        }
        latency_frame_done(surface, callback, now, presented);
        callback->done = 1;
        wl_callback_send_done(callback->resource, now);
        wl_resource_destroy(callback->resource);
    }
}
static void surface_send_frame_callbacks(struct brook_surface *surface, uint32_t now) {
    surface_send_frame_callbacks_upto(surface, now, UINT64_MAX, NULL);
}
/* Frame callbacks of a surface whose content is an offered GPU source are
 * paced by the renderer's PRESENTED notification (gpu_window_presented).
 * The 16 ms clock serves CPU-path surfaces, metadata-only commits of an
 * already presented source, and, after this delay, offers the renderer does
 * not compose (minimized, occluded or rejected windows). */
enum { GPU_CALLBACK_FALLBACK_MS=100 };
static int image_callbacks_due(const struct brook_image *image, uint32_t now) {
    if (!image->pending_cpu || image->gpu_presented) return 1;
    if (image->offer_state==1 && image->offer_ms && (int32_t)(now-image->offer_ms)>=GPU_CALLBACK_FALLBACK_MS) return 1;
    /* Never hold callbacks indefinitely on a missing PRESENTED, a slow
     * producer fence, a layered wait or channel recovery: after the same
     * delay from queueing, release them regardless of offer state. */
    return image->queued_ms && (int32_t)(now-image->queued_ms)>=GPU_CALLBACK_FALLBACK_MS;
}
/* Rootless Xwayland paces Present by frame callbacks: a sync flip completes
 * (PresentCompleteNotify) at the next frame done after its commit, and Mesa
 * GLX with swap interval 1 lets only one swap be outstanding. Holding the
 * callback until PRESENTED (composite, plus the flip at vblank) therefore
 * serialises the game with the renderer's whole latency. By default an
 * Xwayland window's callbacks are released once its commit has been offered
 * to the renderer, at most once per XWAYLAND_FRAME_MIN_US; buffer release
 * (SAFE, after a newer commit is shown) still bounds how far the client gets
 * ahead. BROOK_WAYLAND_XWAYLAND_FRAME=presented keeps PRESENTED pacing. */
enum { XWAYLAND_FRAME_MIN_US=15000 };
static int xwayland_early_frames(void){
    static int enabled=-1;
    if(enabled<0){const char *v=getenv("BROOK_WAYLAND_XWAYLAND_FRAME");enabled=!v || strcmp(v,"presented");}
    return enabled;
}
static int root_callbacks_due(const struct brook_surface *root,const struct brook_image *image,uint32_t now){
    if(image_callbacks_due(image,now))return 1;
    return root->role==4 && image->offer_state==1 && xwayland_early_frames() &&
        g_now_us()-root->pace_callback_us>=XWAYLAND_FRAME_MIN_US;
}
static struct wl_event_source *g_frame_timer;
static void output_refresh_due(uint32_t now);
static int frame_clock_tick(void *unused) {
    (void)unused;
    intel_pending_poll();
    uint32_t now = g_now_ms();
    output_refresh_due(now);
    if (g_trace_frames) {
        if (g_trace_ticks && now - g_trace_last_tick > g_trace_tick_max_ms)
            g_trace_tick_max_ms = now - g_trace_last_tick;
        g_trace_last_tick = now;
        ++g_trace_ticks;
    }
    for (struct brook_surface *surface = g_surfaces; surface; surface = surface->next) {
        struct brook_surface *root = surface;
        while (root->subsurface_parent) root = root->subsurface_parent;
        struct brook_draw draws[64];unsigned n=0;int pending=0;
        if(!surface_layers(root,draws,&n))pending=1;
        for(unsigned i=0;i<n;++i)if(!root_callbacks_due(root,draws[i].view->image,now)){pending=1;break;}
        if(pending)continue;
        if (root->wm_id || surface->is_cursor) surface_send_frame_callbacks(surface, now);
    }
    wl_event_source_timer_update(g_frame_timer, 16);
    return 0;
}
/* Why a surface's committed frame callbacks are not being sent: the same
 * decision frame_clock_tick makes, named for the status file. */
static const char *surface_callback_state(struct brook_surface *surface,uint32_t now){
    if(!wl_list_empty(&surface->waiting_frames) && wl_list_empty(&surface->committed_frames))return "unapplied_commit";
    if(wl_list_empty(&surface->committed_frames))return wl_list_empty(&surface->pending_frames)?"none":"uncommitted";
    struct brook_surface *root=surface;
    while(root->subsurface_parent)root=root->subsurface_parent;
    if(!root->wm_id && !surface->is_cursor)return "no_window";
    struct brook_draw draws[64];unsigned n=0;
    if(!surface_layers(root,draws,&n))return "layers";
    for(unsigned i=0;i<n;++i){const struct brook_image *image=draws[i].view->image;
        if(root_callbacks_due(root,image,now))continue;
        return image->offer_state==1?"presented":image->group_refs?"group":"image_pending";}
    return "due";
}
static uint32_t frame_list_oldest(const struct wl_list *list,uint32_t now,uint32_t oldest){
    const struct brook_frame_callback *callback;
    wl_list_for_each(callback,list,link)if(callback->requested_ms && now-callback->requested_ms>oldest)oldest=now-callback->requested_ms;
    return oldest;
}
static void buffer_size(const struct brook_buffer *buffer,int32_t *w,int32_t *h){
    *w=*h=0;if(!buffer)return;
    if(buffer->dmabuf){*w=buffer->dmabuf->width;*h=buffer->dmabuf->height;}
    else if(buffer->shm){*w=wl_shm_buffer_get_width(buffer->shm);*h=wl_shm_buffer_get_height(buffer->shm);}
}
/* One "surfaces" entry: frame callbacks, configure handshake, buffers and
 * what the callbacks wait for (Chromium freeze diagnosis, gen 442). */
static void configure_history_add(struct brook_surface *s, enum configure_event_kind kind,
                                  uint32_t ms, uint32_t serial, int32_t width, int32_t height) {
    s->configure_history[s->configure_history_next] = (struct configure_event) {
        .ms = ms, .serial = serial, .width = width, .height = height, .kind = kind,
        .resizing = !!s->toplevel_resizing,
        .attached = kind == CONFIGURE_COMMIT && s->pending_attach && s->pending_buffer,
        .acked = !!s->configure_size_acked,
    };
    s->configure_history_next = (s->configure_history_next + 1) % CONFIGURE_HISTORY_LIMIT;
    if (s->configure_history_count < CONFIGURE_HISTORY_LIMIT) ++s->configure_history_count;
}

static void configure_history_json(FILE *f, const struct brook_surface *s, uint32_t now) {
    static const char *const kinds[] = { "configure", "ack", "commit" };
    fprintf(f, "\"configure_history\":[");
    unsigned index = (s->configure_history_next + CONFIGURE_HISTORY_LIMIT -
                      s->configure_history_count) % CONFIGURE_HISTORY_LIMIT;
    for (unsigned n = 0; n < s->configure_history_count; ++n) {
        const struct configure_event *event = &s->configure_history[index];
        fprintf(f, "%s{\"event\":\"%s\",\"ms_ago\":%u,\"serial\":%u,\"size\":[%d,%d],"
                   "\"resizing\":%u,\"attached\":%u,\"size_acked\":%u}",
                n ? "," : "", kinds[event->kind], now - event->ms, event->serial,
                event->width, event->height, (unsigned)event->resizing,
                (unsigned)event->attached, (unsigned)event->acked);
        index = (index + 1) % CONFIGURE_HISTORY_LIMIT;
    }
    fprintf(f, "],");
}

static void surface_status_json(FILE *f,struct brook_surface *s,uint32_t now){
    struct brook_surface *root=s;while(root->subsurface_parent)root=root->subsurface_parent;
    const int pending=wl_list_length(&s->pending_frames),waiting=wl_list_length(&s->waiting_frames),
        committed=wl_list_length(&s->committed_frames);
    uint32_t oldest=frame_list_oldest(&s->pending_frames,now,0);
    oldest=frame_list_oldest(&s->waiting_frames,now,oldest);oldest=frame_list_oldest(&s->committed_frames,now,oldest);
    pid_t pid=0;uid_t uid;gid_t gid;
    if(s->resource)wl_client_get_credentials(wl_resource_get_client(s->resource),&pid,&uid,&gid);
    fprintf(f,"{\"object\":%u,\"client_pid\":%d,\"role\":%d,\"wm\":%u,\"root_wm\":%u,\"cursor\":%d,",
        s->resource?wl_resource_get_id(s->resource):0,(int)pid,s->role,s->wm_id,root->wm_id,s->is_cursor);
    fprintf(f,"\"frames\":{\"pending\":%d,\"waiting\":%d,\"committed\":%d,\"oldest_ms\":%u,\"state\":\"%s\"},",
        pending,waiting,committed,oldest,surface_callback_state(s,now));
    fprintf(f,"\"configure\":{\"sent\":%u,\"acked\":%u,\"sent_ms_ago\":%u,\"acked_ms_ago\":%u,\"size\":[%d,%d],"
        "\"size_pending\":%d,\"size_serial\":%u,\"size_acked\":%d,\"maximized\":%d,\"fullscreen\":%d,\"resizing\":%d},",
        s->next_configure_serial,s->last_ack_serial,s->configure_ms?now-s->configure_ms:0,s->last_ack_ms?now-s->last_ack_ms:0,
        s->configure_width,s->configure_height,s->configure_size_pending,s->configure_size_serial,s->configure_size_acked,
        s->toplevel_maximized,s->toplevel_fullscreen,s->toplevel_resizing);
    if (s->xdg_toplevel) configure_history_json(f, s, now);
    int32_t pw=0,ph=0;if(s->pending_attach)buffer_size(s->pending_buffer,&pw,&ph);
    fprintf(f,"\"pending_buffer\":[%d,%d],\"commit_ms_ago\":%u,\"buffer_commit_ms_ago\":%u,",pw,ph,
        s->commit_ms?now-s->commit_ms:0,s->buffer_commit_ms?now-s->buffer_commit_ms:0);
    const struct brook_view *current=s->tree.current?s->tree.current->value:NULL;
    const struct brook_view *latest=s->tree.latest?s->tree.latest->value:NULL;
    fprintf(f,"\"tree\":{\"sequence\":%llu,\"current\":%llu,\"latest\":%llu,\"sync\":%d},",(unsigned long long)s->tree.sequence,
        (unsigned long long)(s->tree.current?s->tree.current->commit:0),(unsigned long long)(s->tree.latest?s->tree.latest->commit:0),s->tree.sync);
    for(int k=0;k<2;++k){const struct brook_view *view=k?latest:current;
        fprintf(f,"\"%s\":",k?"latest_image":"current_image");
        if(!view || !view->image){fprintf(f,"null,");continue;}
        const struct brook_image *im=view->image;const struct brook_buffer *src=im->source;
        int32_t bw=0,bh=0;buffer_size(src,&bw,&bh);
        fprintf(f,"{\"size\":[%u,%u],\"view\":[%d,%d],\"pending_cpu\":%d,\"offer_state\":%d,\"gpu_presented\":%d,\"group_refs\":%u,"
            "\"offer_ms_ago\":%u,\"queued_ms_ago\":%u,\"buffer\":[%d,%d],\"buffer_uses\":%u,\"buffer_alive\":%d},",
            im->width,im->height,(int)view->geometry.w,(int)view->geometry.h,im->pending_cpu,im->offer_state,im->gpu_presented,im->group_refs,
            im->offer_ms?now-im->offer_ms:0,im->queued_ms?now-im->queued_ms:0,bw,bh,src?src->uses:0,src && src->resource?1:0);}
    fprintf(f,"\"viewport\":%d}",s->viewport!=NULL);
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
static unsigned long long g_opaque_null,g_opaque_infinite,g_opaque_rects[5];static int32_t g_opaque_last[6];static int g_opaque_last_kind=-1;
static void surface_set_opaque_region(struct wl_client *c, struct wl_resource *r,
                                       struct wl_resource *reg) {
    (void)c;
    struct brook_surface *s=wl_resource_get_user_data(r);
    const struct pointer_region *region=reg?wl_resource_get_user_data(reg):NULL;
    s->pending_opaque_set=region && !region->infinite && region->count==1 && region->ops[0].add;
    /* What clients send (status file "opaque_regions"). */
    g_opaque_last_kind=!region?0:region->infinite?2:1;
    if(!region)++g_opaque_null;else if(region->infinite)++g_opaque_infinite;
    else{++g_opaque_rects[region->count<4?region->count:4];
        if(region->count){g_opaque_last[0]=region->ops[0].x;g_opaque_last[1]=region->ops[0].y;g_opaque_last[2]=region->ops[0].w;
            g_opaque_last[3]=region->ops[0].h;g_opaque_last[4]=region->ops[0].add;g_opaque_last[5]=(int32_t)region->count;}}
    if(s->pending_opaque_set){s->pending_opaque_x=region->ops[0].x;s->pending_opaque_y=region->ops[0].y;
        s->pending_opaque_w=region->ops[0].w;s->pending_opaque_h=region->ops[0].h;}
    s->opaque_pending=1;
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
    /* No owning window: set without pointer focus (see pointer_set_cursor).
     * Uploading it unowned would show it over other windows and the shell. */
    if (!s->cursor_owner_wm) return;
    if (!view) {
        /* A cursor surface without content hides the pointer. GTK3 (Firefox's
         * blank cursor after inactivity in fullscreen video) keeps its cursor
         * surface and attaches a NULL buffer instead of set_cursor(NULL);
         * returning here left the previous arrow showing. Same per-window
         * 1x1 transparent image as set_cursor(NULL): hidden over this
         * window only, and replaced by the next committed cursor buffer. */
        static const uint32_t clear_pixel = 0;
        long rc = wm_set_cursor_image_for(&clear_pixel, 1, 1, 0, 0, s->cursor_owner_wm);
        if (rc != 0) fprintf(stderr, "[waylandd] empty cursor surface for wm %u failed rc=%ld\n", s->cursor_owner_wm, rc);
        return;
    }
    /* GPU content not yet read back: the readback path commits again. */
    if (!view->image->pixels) return;
    const struct brook_geometry *g=&view->geometry;
    const struct brook_image *image=view->image;
    uint32_t scale=surface_output_scale(s);
    uint32_t dw=logical_to_physical(g->w,scale),dh=logical_to_physical(g->h,scale);
    const uint32_t side=dw>dh?dw:dh;
    if (side>BROOK_CURSOR_PHYSICAL_MAX) {
        dw=(uint64_t)dw*BROOK_CURSOR_PHYSICAL_MAX/side;
        dh=(uint64_t)dh*BROOK_CURSOR_PHYSICAL_MAX/side;
        if (!dw) dw=1;
        if (!dh) dh=1;
    }
    struct brook_geometry cursor=*g;cursor.w=dw;cursor.h=dh;
    static uint32_t pixels[BROOK_CURSOR_PHYSICAL_MAX*BROOK_CURSOR_PHYSICAL_MAX];
    for (uint32_t y=0;y<dh;++y) for (uint32_t x=0;x<dw;++x) {
        uint32_t px,py;
        brook_geometry_pixel(&cursor,image->width,image->height,x,y,&px,&py);
        pixels[y*dw+x]=image->pixels[(size_t)py*image->width+px];
    }
    wm_set_cursor_image_for(pixels,dw,dh,(int64_t)s->cursor_hot_x*dw/g->w,(int64_t)s->cursor_hot_y*dh/g->h,s->cursor_owner_wm);
}
static void toplevel_output_size(const struct brook_surface *s, uint32_t *width, uint32_t *height)
{
    *width = *height = 0;
    wm_get_screen_info(width, height);
    const uint32_t scale = surface_output_scale(s);
    *width=physical_to_logical(*width,scale);
    *height=physical_to_logical(*height,scale);
}

static void toplevel_send_bounds(struct brook_surface *s, uint32_t width, uint32_t height)
{
    if (wl_resource_get_version(s->xdg_toplevel) < XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION) return;
    if (!width || !height || width > INT32_MAX || height > INT32_MAX) width = height = 0;

    if (s->bounds_width == (int32_t)width && s->bounds_height == (int32_t)height) return;
    s->bounds_width = (int32_t)width;
    s->bounds_height = (int32_t)height;
    xdg_toplevel_send_configure_bounds(s->xdg_toplevel, s->bounds_width, s->bounds_height);
}

static void toplevel_send_wm_capabilities(struct wl_resource *toplevel)
{
    if (wl_resource_get_version(toplevel) < XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION) return;
    // Every request has a WM implementation; the window menu is the kernel's
    // (Move to side dock, Minimise, Maximise, Close).
    uint32_t supported[] = { XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU,
                             XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE,
                             XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE,
                             XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN };
    struct wl_array capabilities = { .size = sizeof(supported), .data = supported };
    xdg_toplevel_send_wm_capabilities(toplevel, &capabilities);
}

/* Configure size and state form one transaction. Focus changes may supersede
 * its serial, but must preserve both dimensions and management state. */
static int toplevel_send_state(struct brook_surface *s,int32_t width,int32_t height) {
    if (!s->xdg_toplevel || !s->xdg_surface) return 1;
    struct wl_array states;wl_array_init(&states);
    const uint32_t values[]={XDG_TOPLEVEL_STATE_ACTIVATED,XDG_TOPLEVEL_STATE_MAXIMIZED,XDG_TOPLEVEL_STATE_RESIZING,
        XDG_TOPLEVEL_STATE_FULLSCREEN};
    const int enabled[]={s->toplevel_activated,s->toplevel_maximized,s->toplevel_resizing,s->toplevel_fullscreen};
    for (unsigned i=0;i<4;++i) if(enabled[i]) {
        uint32_t *state=wl_array_add(&states,sizeof(*state));
        if(!state) { wl_array_release(&states);wl_client_post_no_memory(wl_resource_get_client(s->resource));return 0; }
        *state=values[i];
    }
    if (wl_resource_get_version(s->xdg_toplevel) >= XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION) {
        uint32_t screen_w, screen_h;
        toplevel_output_size(s, &screen_w, &screen_h);
        toplevel_send_bounds(s, screen_w, screen_h);
    }
    xdg_toplevel_send_configure(s->xdg_toplevel,width,height,&states);wl_array_release(&states);
    s->next_configure_serial=next_serial();s->configure_ms=g_now_ms();
    if(s->configure_size_pending) {s->configure_size_serial=s->next_configure_serial;s->configure_size_acked=0;}
    xdg_surface_send_configure(s->xdg_surface,s->next_configure_serial);
    configure_history_add(s, CONFIGURE_SENT, s->configure_ms, s->next_configure_serial, width, height);
    return 1;
}
static void surface_commit(struct wl_client *c,struct wl_resource *r) {
    struct brook_surface *s=wl_resource_get_user_data(r);
    latency_commit(s, c);
    if(s->opaque_pending){
        s->opaque_set=s->pending_opaque_set;s->opaque_x=s->pending_opaque_x;s->opaque_y=s->pending_opaque_y;
        s->opaque_w=s->pending_opaque_w;s->opaque_h=s->pending_opaque_h;s->opaque_pending=0;
    }
    if(s->input_region_pending) {
        pointer_region_clear(&s->input_region);s->input_region=s->pending_input_region;
        memset(&s->pending_input_region,0,sizeof(s->pending_input_region));s->input_region_pending=0;
    }
    pointer_constraints_commit(s);
    if (!xwayland_surface_commit(s)) return;
    if (s->role==4) xwayland_pair_surfaces();
    if (g_trace_frames) { ++s->trace_commits;if (s->pending_buffer) ++s->trace_buffers; }
    s->commit_ms=g_now_ms();if(s->pending_attach && s->pending_buffer)s->buffer_commit_ms=s->commit_ms;
    if (s->window_geometry_pending) {
        s->window_geometry_w=s->pending_geometry_w;s->window_geometry_h=s->pending_geometry_h;s->window_geometry_pending=0;
        s->window_geometry_x=s->pending_geometry_x;s->window_geometry_y=s->pending_geometry_y;

    }
    if(s->size_hints_pending){
        struct toplevel_size_hints hints=s->size_hints;
        if(s->size_hints_pending&1){hints.min_w=s->pending_size_hints.min_w;hints.min_h=s->pending_size_hints.min_h;}
        if(s->size_hints_pending&2){hints.max_w=s->pending_size_hints.max_w;hints.max_h=s->pending_size_hints.max_h;}
        if(!toplevel_size_valid(&hints)){
            wl_resource_post_error(s->xdg_toplevel,XDG_TOPLEVEL_ERROR_INVALID_SIZE,"Maximum size is below minimum size");return;
        }
        s->size_hints=hints;s->size_hints_pending=0;
    }
    const int configure_committed=s->configure_size_pending && s->configure_size_acked;
    if (s->xdg_toplevel && s->configure_size_pending) {
        int32_t width = 0, height = 0;
        if (s->pending_attach) buffer_size(s->pending_buffer, &width, &height);
        configure_history_add(s, CONFIGURE_COMMIT, s->commit_ms, s->last_ack_serial, width, height);
    }
    /* xdg-shell role: the very first commit must have NO buffer; we
     * respond with a configure. The client acks, then commits with a
     * buffer for real. */
    if (s->xdg_toplevel && !s->xdg_initial_commit_done) {
        s->xdg_initial_commit_done = 1;
        struct wl_array states;
        wl_array_init(&states);
        /* Keep the bounded GTK fallback, honoring committed client constraints.
         * Hints are advisory: never turn an absurd minimum into a huge SHM pool. */
        uint32_t screen_w, screen_h;
        toplevel_output_size(s, &screen_w, &screen_h);
        s->bounds_width = s->bounds_height = 0;
        toplevel_send_bounds(s, screen_w, screen_h);
        const int32_t cap_w=screen_w && screen_w<4096?(int32_t)screen_w:4096;
        const int32_t cap_h=screen_h && screen_h<2160?(int32_t)screen_h:2160;
        const int32_t width=toplevel_initial_axis(800,s->size_hints.min_w,s->size_hints.max_w,cap_w);
        const int32_t height=toplevel_initial_axis(600,s->size_hints.min_h,s->size_hints.max_h,cap_h);
        xdg_toplevel_send_configure(s->xdg_toplevel,width,height,&states);
        wl_array_release(&states);
        uint32_t serial = s->next_configure_serial = next_serial();s->configure_ms=g_now_ms();
        xdg_surface_send_configure(s->xdg_surface, serial);
        configure_history_add(s, CONFIGURE_SENT, s->configure_ms, serial, width, height);
        fprintf(stderr, "[waylandd] xdg initial configure sent serial=%u (%dx%d)\n", serial,width,height);
        return;
    }
    if (s->xdg_popup && !s->xdg_initial_commit_done) {
        s->xdg_initial_commit_done = 1;
        int32_t pw = s->popup_w > 0 ? s->popup_w : 200;
        int32_t ph = s->popup_h > 0 ? s->popup_h : 100;
        xdg_popup_send_configure(s->xdg_popup, s->popup_x, s->popup_y, pw, ph);
        uint32_t serial = s->next_configure_serial = next_serial();s->configure_ms=g_now_ms();
        xdg_surface_send_configure(s->xdg_surface, serial);
        fprintf(stderr, "[waylandd] xdg popup initial configure sent serial=%u %dx%d\n",
                serial, pw, ph);
        return;
    }


    struct brook_view *previous=s->tree.latest?s->tree.latest->value:NULL;
    struct brook_image *image=previous?previous->image:NULL;
    struct brook_image *strip_older=image;image_ref(strip_older);
    image_ref(image);
    if (s->pending_attach) {
        image_unref(image);image=NULL;
        struct brook_buffer *buffer=s->pending_buffer;
        const uint64_t trace_attachment = g_buffer_trace.pid && buffer &&
            s->trace_pending_epoch == g_buffer_trace.epoch ? s->trace_pending_attachment : 0;
        const uint32_t trace_surface = trace_attachment ? wl_resource_get_id(r) : 0;
        if (trace_attachment) {
            if (!buffer->uses || !buffer->trace_cycle) buffer->trace_cycle = trace_attachment;
            buffer_trace_record(BUFFER_COMMIT, buffer->trace_serial, trace_attachment,
                                buffer->trace_cycle, trace_surface, buffer->uses, NULL);
        }
        if (buffer && buffer->dmabuf) {
            struct brook_dmabuf_buffer *db=buffer->dmabuf;
            if (db->width<=0 || db->height<=0 || db->width>16384 || db->height>16384) {
                wl_client_post_implementation_error(c,"invalid dmabuf dimensions");return;
            }
            if (g_drm_intel) {
                const char *async=getenv("BROOK_WAYLAND_INTEL_ASYNC");
                if(async && !strcmp(async,"1")){
                    image=intel_dmabuf_defer(buffer);
                    if(!image){wl_client_post_implementation_error(c,"Unable to queue GPU buffer completion");return;}
                }else{
                    buffer_begin_use(buffer);
                    int uncertain=0;image=intel_dmabuf_snapshot(db,&uncertain);
                    if(uncertain)buffer->uncertain=1;
                    if (trace_attachment && (image || uncertain))
                        buffer_trace_record(image ? BUFFER_CPU_COPY : BUFFER_UNCERTAIN, buffer->trace_serial,
                                            trace_attachment, buffer->trace_cycle, trace_surface, buffer->uses, NULL);
                    buffer_release(buffer);
                }
            } else if (db->nv_bo && nv_layer_candidate(s,db)) {
                /* nv-drm, zero-copy step 2: wait for the producer, then hold the buffer. */
                buffer_begin_use(buffer);
                int uncertain=0;image=nv_layer_image(buffer,&uncertain);
                if(uncertain)buffer->uncertain=1;
                if(!image)buffer_release(buffer);
            } else if (db->nv_bo) {
                /* nv-drm: wait for the producer's implicit fence, then CPU snapshot. */
                buffer_begin_use(buffer);
                int uncertain=0;image=nv_dmabuf_snapshot(db,&uncertain);
                if(uncertain)buffer->uncertain=1;
                if (trace_attachment && (image || uncertain))
                    buffer_trace_record(image ? BUFFER_CPU_COPY : BUFFER_UNCERTAIN, buffer->trace_serial,
                                        trace_attachment, buffer->trace_cycle, trace_surface, buffer->uses, NULL);
                buffer_release(buffer);
            } else image=calloc(1,sizeof(*image));
            if (image && !g_drm_intel && !db->nv_bo) {
                image->refs=1;image->width=db->width;image->height=db->height;
                image->format=db->fourcc==0x34325258?2:1;image->source=buffer;++buffer->refs;buffer_begin_use(buffer);
            }
        } else if (buffer && buffer->shm) {
            struct wl_shm_buffer *shm=buffer->shm;
            buffer_begin_use(buffer);
            wl_shm_buffer_begin_access(shm);
            image=image_snapshot(wl_shm_buffer_get_data(shm),wl_shm_buffer_get_width(shm),wl_shm_buffer_get_height(shm),wl_shm_buffer_get_stride(shm),wl_shm_buffer_get_format(shm));
            wl_shm_buffer_end_access(shm);
            if (trace_attachment && image)
                buffer_trace_record(BUFFER_CPU_COPY, buffer->trace_serial, trace_attachment,
                                    buffer->trace_cycle, trace_surface, buffer->uses, NULL);
            buffer_release(buffer);
        }
        if (image && trace_attachment) {
            image->buffer_trace_epoch = g_buffer_trace.epoch;
            image->buffer_trace_attachment = trace_attachment;
            image->buffer_trace_cycle = buffer->trace_cycle;
            image->buffer_trace_serial = buffer->trace_serial;
            image->buffer_trace_surface = trace_surface;
        }
        if (image && buffer && !s->subsurface_parent) {
            const uint64_t now_us=g_now_us();const int x11=s->role==4;
            pace_add(x11,PACE_INTERVAL,s->pace_commit_us,now_us);
            if(s->pace_callback_us>s->pace_commit_us)pace_add(x11,PACE_TURNAROUND,s->pace_callback_us,now_us);
            s->pace_commit_us=now_us;image->pace_commit_us=now_us;image->pace_x11=x11;
        }
        if (buffer && !image) {
            if(buffer->uncertain)wl_client_post_implementation_error(c,"GPU buffer completion could not be verified");
            else wl_client_post_no_memory(c);
            return;
        }
    }
    int committed=surface_tree_commit(s,image),published=1;
    if (!committed) wl_client_post_no_memory(c);
    else if (committed>0 && s->is_cursor) surface_cursor_commit(s);
    else if (committed>0 && !bt_effective_sync(&s->tree)) published=surface_repaint(s);
    /* Window geometry (the CSD shadow margin) places overlaid captions, and
     * the client's strip colours them. */
    if (committed>0 && s->xdg_toplevel && s->deco_sent_mode==2) {
        surface_strip_commit(s,strip_older);
        surface_sync_decoration(s,0);
    }
    image_unref(strip_older);
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
    idle_inhibit_surface_destroyed(s);
    pointer_constraints_unmap(s,1);
    pointer_region_clear(&s->input_region);pointer_region_clear(&s->pending_input_region);
    xwayland_surface_detach(s);
    if (s->viewport) { wl_resource_set_user_data(s->viewport,NULL);s->viewport=NULL; }
    if (s->fractional_scale) {
        wl_resource_set_user_data(s->fractional_scale,NULL);
        s->fractional_scale=NULL;
    }
    if (s->deco_resource) { wl_resource_set_user_data(s->deco_resource,NULL);s->deco_resource=NULL; }
    strip_retry_drop(s);
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
    s->deco_sent_mode = -1;
    if (g_trace_frames) s->trace_id = ++g_trace_surface_id;
    struct wl_resource *sr = wl_resource_create(client, &wl_surface_interface,
                                                wl_resource_get_version(res), id);
    if (!sr) { free(s); wl_client_post_no_memory(client); return; }
    surface_tree_init(s);
    s->input_region.infinite=1;
    s->resource = sr;
    (void)latency_client(client);
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

/* This small extension has no events or generated server marshallers.
 * Its client bindings can be generated from protocols/brook-capture-v1.xml. */
static const struct wl_interface *capture_request_types[] = { &wl_surface_interface };
static const struct wl_message capture_requests[] = {
    { "destroy", "", NULL },
    { "exclude", "o", capture_request_types },
};
static const struct wl_interface brook_capture_manager_v1_interface = {
    "brook_capture_manager_v1", 1, 2, capture_requests, 0, NULL,
};

static int capture_excluded(const struct brook_surface *surface)
{
    for (const struct brook_surface *s = surface; s; s = s->popup_parent)
        if (s->capture_excluded) return 1;
    return 0;
}

static void capture_manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void capture_manager_exclude(struct wl_client *client, struct wl_resource *manager,
                                    struct wl_resource *surface)
{
    (void)client;
    struct brook_surface *root = wl_resource_get_user_data(surface);
    if (!root || !root->xdg_toplevel) {
        wl_resource_post_error(manager, 0, "exclude requires an xdg_toplevel wl_surface");
        return;
    }
    root->capture_excluded = 1;
    for (struct brook_surface *s = g_surfaces; s; s = s->next) {
        if (!s->wm_id || !capture_excluded(s)) continue;
        if (syscall(BROOK_SYS_WM_EXCLUDE_CAPTURE, (long)s->wm_id) < 0) {
            wl_resource_post_error(manager, 0, "WM could not exclude the surface from capture");
            return;
        }
    }
}

static const struct {
    void (*destroy)(struct wl_client*, struct wl_resource*);
    void (*exclude)(struct wl_client*, struct wl_resource*, struct wl_resource*);
} capture_manager_impl = { capture_manager_destroy, capture_manager_exclude };

static void capture_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *manager = wl_resource_create(client, &brook_capture_manager_v1_interface, version, id);
    if (!manager) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(manager, &capture_manager_impl, NULL, NULL);
}

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
    if (strlen(title) >= sizeof(s->title)) s->capture_excluded = 1;
    snprintf(s->title, sizeof(s->title), "%s", title);
    // Let the kernel see the original string so its bound check and
    // privacy update happen together on an already mapped window.
    if (s->wm_id) wm_set_title(s->wm_id, title);
}
static void deco_apply_mode(struct wl_resource *r, int csd);
static int deco_force_server_side(const struct brook_surface *s);
static void xdg_toplevel_set_app_id(struct wl_client *c, struct wl_resource *r,
                                     const char *app_id) {
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s || !app_id) return;
    snprintf(s->app_id, sizeof(s->app_id), "%s", app_id);
    if (s->wm_id) wm_set_app_id(s->wm_id, s->app_id);
    /* The decoration request may precede the app_id: apply the policy now. */
    if (s->deco_resource && s->deco_csd && s->deco_requested_csd && deco_force_server_side(s))
        deco_apply_mode(s->deco_resource, 1);
    else if (s->wm_id && s->deco_csd) surface_sync_decoration(s, 0);
}
/* The Brook window menu (Move to side dock, Minimise, Maximise, Close) at
 * the request's surface-local point, which the kernel anchors to the window. */
static void xdg_toplevel_show_window_menu(struct wl_client *c, struct wl_resource *r,
                                            struct wl_resource *seat, uint32_t serial,
                                            int32_t x, int32_t y) {
    (void)c; (void)seat; (void)serial;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s || !s->wm_id) return;
    const int64_t scale = surface_output_scale(s);
    long rc = syscall(BROOK_SYS_WM_SHOW_WINDOW_MENU, (long)s->wm_id, (long)logical_to_physical(x,scale), (long)logical_to_physical(y,scale));
    fprintf(stderr, "[waylandd] xdg_toplevel.show_window_menu wm=%u at %d,%d rc=%ld\n", s->wm_id, x, y, rc);
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
    (void)c;struct brook_surface *s=wl_resource_get_user_data(r);
    if(w<0||h<0){wl_resource_post_error(r,XDG_TOPLEVEL_ERROR_INVALID_SIZE,"Negative maximum size");return;}
    if(s){s->pending_size_hints.max_w=w;s->pending_size_hints.max_h=h;s->size_hints_pending|=2;}
}
static void xdg_toplevel_set_min_size(struct wl_client *c, struct wl_resource *r,
                                       int32_t w, int32_t h) {
    (void)c;struct brook_surface *s=wl_resource_get_user_data(r);
    if(w<0||h<0){wl_resource_post_error(r,XDG_TOPLEVEL_ERROR_INVALID_SIZE,"Negative minimum size");return;}
    if(s){s->pending_size_hints.min_w=w;s->pending_size_hints.min_h=h;s->size_hints_pending|=1;}
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
    (void)c; (void)output;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->want_fullscreen=1;toplevel_apply_fullscreen(s);
}
static void xdg_toplevel_unset_fullscreen(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->want_fullscreen=0;toplevel_apply_fullscreen(s);
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
    toplevel_send_wm_capabilities(tr);
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
    /* xdg-shell: repositioned is sent first in the configuration sequence,
     * then xdg_popup.configure, then xdg_surface.configure. GTK4 reuses its
     * tooltip popup via reposition and ignores a configure that arrives before
     * repositioned, then blocks in gdk_wayland_surface_present_popup waiting
     * for one (Nautilus froze on the first tooltip after a move). */
    xdg_popup_send_repositioned(r, token);
    if (s) {
        positioner_compute_geometry(p, &s->popup_x, &s->popup_y,
                                    &s->popup_w, &s->popup_h);
        xdg_popup_send_configure(r, s->popup_x, s->popup_y,
                                 s->popup_w, s->popup_h);
        if (s->xdg_surface) {
            uint32_t serial = s->next_configure_serial = next_serial();s->configure_ms=g_now_ms();
            xdg_surface_send_configure(s->xdg_surface, serial);
        }
        popup_apply_position(s);
    }
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
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (w <= 0 || h <= 0) {
        wl_resource_post_error(r, XDG_SURFACE_ERROR_INVALID_SIZE,
                               "window geometry must have positive dimensions");
        return;
    }
    s->pending_geometry_w = w; s->pending_geometry_h = h;
    s->pending_geometry_x = x; s->pending_geometry_y = y;
    s->window_geometry_pending = 1;
}
static void xdg_surface_ack_configure(struct wl_client *c, struct wl_resource *r,
                                        uint32_t serial) {
    (void)c;
    struct brook_surface *s = wl_resource_get_user_data(r);
    s->xdg_acked = 1;
    s->last_ack_serial=serial;s->last_ack_ms=g_now_ms();
    if (s->configure_size_pending && serial==s->configure_size_serial) s->configure_size_acked=1;
    if (s->xdg_toplevel) configure_history_add(s, CONFIGURE_ACK, s->last_ack_ms, serial, 0, 0);
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

/* Version 6 adds repaint suspension, which the WM does not report. */
#define BROOK_XDG_WM_BASE_VERSION 5

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
    wl_fixed_t motion_lx,motion_ly;         /* last enter/motion position sent */
    uint32_t buttons;                       /* held buttons: implicit grab */
    unsigned finger_axes;                   /* axes moved on the entered surface */
    uint32_t kb_mods_depressed;
    uint32_t kb_mods_locked;
    int keymap_fd;                          /* kept alive until client receives it */
    struct brook_seat_client *next;
};

static struct brook_seat_client *g_seat_clients = NULL;
static uint32_t g_serial = 1;
static uint32_t next_serial(void) { return g_serial++; }

#include "text-input-v3.h"

static void pointer_stop_finger_scroll(struct brook_seat_client *sc, uint32_t now) {
    const unsigned axes = sc->finger_axes;
    sc->finger_axes = 0;
    if (!axes || !sc->pointer || wl_resource_get_version(sc->pointer) < WL_POINTER_AXIS_STOP_SINCE_VERSION)
        return;

    wl_pointer_send_axis_source(sc->pointer, WL_POINTER_AXIS_SOURCE_FINGER);
    if (axes & (1u << WL_POINTER_AXIS_VERTICAL_SCROLL))
        wl_pointer_send_axis_stop(sc->pointer, now, WL_POINTER_AXIS_VERTICAL_SCROLL);
    if (axes & (1u << WL_POINTER_AXIS_HORIZONTAL_SCROLL))
        wl_pointer_send_axis_stop(sc->pointer, now, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
    wl_pointer_send_frame(sc->pointer);
}

static void seat_clients_scrub_surface(struct brook_surface *s) {
    for (struct brook_seat_client *sc = g_seat_clients; sc; sc = sc->next) {
        if (sc->entered_surface == s) {
            if (!sc->cursor_visible)
                wm_set_cursor_visible(1);
            sc->entered_surface = NULL;
            sc->finger_axes = 0;
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
            if (sc->finger_axes) pointer_stop_finger_scroll(sc, g_now_ms());
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
    /* Only the client with pointer focus may set the cursor. Xwayland moves
     * its sprite to the root window on wl_pointer.leave (sent on the kernel's
     * POINTER_LEFT since a243a68c) and sets the root "X" cursor; uploaded
     * without an owner it showed over the desktop, taskbar and kernel
     * windows. The surface still takes the cursor role, but uploads nothing
     * until a set_cursor with focus gives it an owner. */
    if (!sc->entered_surface) {
        struct brook_surface *cs = surface ? wl_resource_get_user_data(surface) : NULL;
        if (cs) {
            if (cs->role && cs->role!=3) {
                wl_resource_post_error(r,WL_POINTER_ERROR_ROLE,"surface already has a role");return;
            }
            cs->role=3;cs->is_cursor=1;cs->cursor_owner_wm=0;
            cs->cursor_hot_x=hx;cs->cursor_hot_y=hy;
        }
        return;
    }
    sc->cursor_visible = surface != NULL;
    seat_apply_cursor_visibility(sc);

    if (!surface) {
        /* set_cursor(NULL) hides the pointer (games, Xwayland's blank or
         * XFixes-hidden cursor). Cursor images are owned per window since
         * a243a68c, so a transparent image owned by the hovered window hides
         * it there only; the kernel keeps its arrow over the desktop, the
         * taskbar, WM chrome and other windows. */
        struct brook_surface *owner = sc->entered_surface;
        while (owner && owner->subsurface_parent) owner = owner->subsurface_parent;
        if (owner && owner->wm_id) {
            static const uint32_t clear_pixel = 0;
            long rc = wm_set_cursor_image_for(&clear_pixel, 1, 1, 0, 0, owner->wm_id);
            if (rc != 0) fprintf(stderr, "[waylandd] hidden cursor for wm %u failed rc=%ld\n", owner->wm_id, rc);
        } else {
            wm_set_cursor_image(NULL, 0, 0, 0, 0); /* no window known: built-in arrow */
        }
        return;
    }
    {
        /* Mark this surface as a cursor surface so commits upload pixels
         * to the kernel compositor instead of creating a window. */
        struct brook_surface *cs = wl_resource_get_user_data(surface);
        if (cs) {
            if (cs->role && cs->role!=3) {
                wl_resource_post_error(r,WL_POINTER_ERROR_ROLE,"surface already has a role");return;
            }
            cs->role=3;cs->is_cursor = 1;
            cs->cursor_output_scale=sc->entered_surface?surface_output_scale(sc->entered_surface):g_output_scale*BROOK_WL_SCALE_ONE;
            cs->cursor_hot_x = hx;
            cs->cursor_hot_y = hy;
            struct brook_surface *owner = sc->entered_surface;
            while (owner && owner->subsurface_parent) owner = owner->subsurface_parent;
            cs->cursor_owner_wm = owner ? owner->wm_id : 0;
        }

        /* set_cursor selects committed content. Pending attachments and
         * viewport/scale changes become visible only at wl_surface.commit. */
        if (cs) surface_cursor_commit(cs);
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
    if (sc && sc->pointer == r) {
        sc->pointer = NULL;
        sc->finger_axes = 0;
    }
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

static void pointer_enter_if_needed(struct brook_seat_client *sc,struct brook_surface *s,wl_fixed_t lx,wl_fixed_t ly);
static void pointer_timeline_delivered(uint32_t wm_id, uint32_t type)
{
    static int reported;
    if (reported) return;
    reported = 1;
    (void)syscall(BROOK_SYS_WM_POINTER_TIMELINE, (long)BROOK_POINTER_TIMELINE_DELIVERY,
                  (long)wm_id, (long)type, 0L, 0L, 0L);
}
#include "pointer-constraints.h"
#include "idle-inhibit.h"

#include "keyboard-keymap.h"
static uint32_t g_keyboard_layout;

static int make_keymap_fd(size_t *out_size) {
    /* Full US-101 xkb keymap.  evdev keycodes (= scancode + 8).
     * The previous version only declared a-l on the home row -- every
     * other letter, number, and punctuation key reached the client as
     * an unmapped keycode, which xkbcommon translates to NoSymbol and
     * apps like mousepad drop.  Cover the standard typing block. */
    long layout = syscall(BROOK_SYS_INPUT_PREFERENCES, (long)BROOK_INPUT_PREF_KEYBOARD, 0L, 0L, 0L, 0L, 0L);
    g_keyboard_layout = layout == BROOK_KEYBOARD_GB ? BROOK_KEYBOARD_GB : BROOK_KEYBOARD_US;
    const char *keymap = g_keyboard_layout == BROOK_KEYBOARD_GB ? brook_xkb_keymap_gb : brook_xkb_keymap;
    size_t len = strlen(keymap) + 1;
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
    if (sc) {sc->pointer = p;sc->finger_axes = 0;++sc->refs;}
    static int reported;
    if (!reported) {
        reported = 1;
        (void)syscall(BROOK_SYS_WM_POINTER_TIMELINE, (long)BROOK_POINTER_TIMELINE_SEAT,0L,0L,0L,0L,0L);
    }
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
static uint32_t g_output_width = 1920, g_output_height = 1080;
static uint32_t g_output_checked_ms;
enum { OUTPUT_CHECK_INTERVAL_MS = 250 };

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
static void output_send_size(struct wl_resource *r) {
    /* wl_output.mode uses device pixels for every client. Xwayland binds
     * at scale 1; its millimetre dimensions still reflect Brook's DPI. */
    const uint32_t mm_scale = wl_resource_get_client(r) == g_xwayland_client ? g_output_scale : 1;
    const int32_t mm_width = (int32_t)(((uint64_t)g_output_width * 254 + 480) / 960 / mm_scale);
    const int32_t mm_height = (int32_t)(((uint64_t)g_output_height * 254 + 480) / 960 / mm_scale);
    wl_output_send_geometry(r, 0, 0, mm_width, mm_height,
        WL_OUTPUT_SUBPIXEL_UNKNOWN, "Brook", "vfb-0", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        (int32_t)g_output_width, (int32_t)g_output_height, 60000);
}

static void output_refresh(uint32_t now) {
    g_output_checked_ms = now;
    const uint32_t previous_scale=g_output_scale120;
    const uint32_t previous_legacy=g_output_scale;
    output_scale_sample(0);
    const int scale_changed=previous_scale!=g_output_scale120;
    uint32_t width, height;
    const int size_changed=!wm_get_screen_info(&width, &height) && width && height &&
                           width<=INT32_MAX && height<=INT32_MAX &&
                           (width!=g_output_width || height!=g_output_height);
    if (size_changed) {
        g_output_width=width;
        g_output_height=height;
    }
    if (!scale_changed && !size_changed) return;
    for (struct brook_output *o=g_outputs; o; o=o->next) {
        const bool xwayland=wl_resource_get_client(o->resource)==g_xwayland_client;
        output_send_size(o->resource);
        if (wl_resource_get_version(o->resource)>=2) {
            if (previous_legacy!=g_output_scale)
                wl_output_send_scale(o->resource,xwayland || client_native_scale(wl_resource_get_client(o->resource))?1:g_output_scale);
            wl_output_send_done(o->resource);
        }
    }
    if (scale_changed) {
        for (struct brook_surface *surface=g_surfaces; surface; surface=surface->next)
            if (surface->fractional_scale && !client_native_scale(wl_resource_get_client(surface->fractional_scale)))
                wp_fractional_scale_v1_send_preferred_scale(surface->fractional_scale,g_output_scale120);
    }
}

static void output_refresh_due(uint32_t now) {
    /* The kernel has a size snapshot but no display-change event. Reuse the
     * frame tick, sampling only while an output is bound. */
    if ((g_outputs || g_surfaces) && now - g_output_checked_ms >= OUTPUT_CHECK_INTERVAL_MS)
        output_refresh(now);
}

static void output_bind(struct wl_client *client, void *data,
                        uint32_t version, uint32_t id) {
    (void)data;
    struct brook_output *o = calloc(1, sizeof(*o));
    if (!o) { wl_client_post_no_memory(client); return; }
    struct wl_resource *r = wl_resource_create(client, &wl_output_interface,
                                                (int)version, id);
    if (!r) { free(o); wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &output_impl, NULL, NULL);
    fprintf(stderr, "[waylandd] wl_output bind v=%u id=%u client=%p\n",
            version, id, (void*)client);

    /* Update older bindings first if this bind notices a new output size. */
    output_refresh(g_now_ms());
    o->resource = r;
    o->destroy.notify = output_resource_destroyed;
    wl_resource_add_destroy_listener(r, &o->destroy);
    o->next = g_outputs;
    g_outputs = o;

    output_send_size(r);
    if (version >= 2) {
        wl_output_send_scale(r, client==g_xwayland_client || client_native_scale(client)?1:g_output_scale);
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
                                     wl_fixed_t lx, wl_fixed_t ly) {
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
            if (sc->finger_axes) pointer_stop_finger_scroll(sc, g_now_ms());
            wl_pointer_send_leave(sc->pointer, next_serial(), prev);
            if (wl_resource_get_version(sc->pointer) >= 5)
                wl_pointer_send_frame(sc->pointer);
        }
        sc->finger_axes = 0;
    }
    if (s) {
        wl_pointer_send_enter(sc->pointer, next_serial(), s->resource, lx, ly);
        sc->motion_lx = lx; sc->motion_ly = ly;
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

/* A button event acts where the client last saw the pointer.
 * The kernel reports the press position with the press; if the last
 * motion/enter sent differs (a move the client never saw), send a motion
 * first. Xwayland in particular picks the X window from its own sprite
 * position, so a stale one sends the click to another X window or the root. */
static void pointer_sync_position(struct brook_seat_client *sc, uint32_t now,
                                  wl_fixed_t lx, wl_fixed_t ly) {
    if (!sc || !sc->pointer || !sc->entered_surface ||
        (sc->motion_lx == lx && sc->motion_ly == ly)) return;
    wl_pointer_send_motion(sc->pointer, now, lx, ly);
    if (wl_resource_get_version(sc->pointer) >= 5)
        wl_pointer_send_frame(sc->pointer);
    sc->motion_lx = lx; sc->motion_ly = ly;
}

/* Pointer target within the window's surface tree. A window's input arrives
 * root-relative; subsurfaces (e.g. Chromium's WaylandBubble dialogs) must
 * receive the events over them, in surface-local coordinates. While a button
 * is held the pressed surface keeps the pointer (implicit grab). Points that
 * hit no mapped layer go to the root, as before subsurface hit-testing. */
static struct brook_surface *pointer_target(struct brook_surface *root,struct brook_seat_client *sc,
                                            int32_t px,int32_t py,wl_fixed_t *lx,wl_fixed_t *ly) {
    const int64_t fx=surface_pointer_fixed(root,px),fy=surface_pointer_fixed(root,py);
    *lx=(wl_fixed_t)fx;*ly=(wl_fixed_t)fy;
    struct pointer_hit_layer layers[64];unsigned count=0;
    struct frame { struct bt_node *node;size_t index;int own;int64_t x,y; } stack[65];
    unsigned depth=1;stack[0]=(struct frame){&root->tree,0,0,0,0};
    while (depth) {
        struct frame *f=&stack[depth-1];struct bt_state *state=f->node->current;
        if (!state || !state->value) { --depth;continue; }
        if (!f->own && (f->index==state->count || !state->children[f->index].below)) {
            if (count==64) break;
            const struct brook_view *view=state->value;struct brook_surface *owner=f->node->user;
            layers[count++]=(struct pointer_hit_layer){owner?&owner->input_region:NULL,f->x,f->y,
                view->geometry.w,view->geometry.h,owner};
            f->own=1;continue;
        }
        if (f->index==state->count) { --depth;continue; }
        struct bt_child *child=&state->children[f->index++];
        if (!bt_child_valid(f->node,child) || depth==65) continue;
        stack[depth++]=(struct frame){child->node,0,0,f->x+child->x,f->y+child->y};
    }
    int64_t hx=fx,hy=fy;
    int pick=(sc && sc->buttons)?pointer_hit_grab_index(layers,count,sc->entered_surface):-1;
    if (pick>=0) { hx=fx-layers[pick].x*256;hy=fy-layers[pick].y*256; }
    else pick=pointer_hit_pick(layers,count,fx,fy,&hx,&hy);
    if (pick<0 || !layers[pick].user) return root;
    struct brook_surface *target=layers[pick].user;
    if (!target->resource) return root;
    *lx=(wl_fixed_t)(hx<INT32_MIN?INT32_MIN:hx>INT32_MAX?INT32_MAX:hx);
    *ly=(wl_fixed_t)(hy<INT32_MIN?INT32_MIN:hy>INT32_MAX?INT32_MAX:hy);
    return target;
}

static void pointer_send_scroll_axis(struct wl_resource *pointer, uint32_t now,
                                     uint32_t axis, int32_t v120, int32_t ticks, int finger) {
    if (!v120) return;

    const int version = wl_resource_get_version(pointer);
    if (!finger) {
        if (version >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
            wl_pointer_send_axis_value120(pointer, axis, v120);
        else if (version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION && ticks)
            wl_pointer_send_axis_discrete(pointer, axis, ticks);
    }
    /* Round to the nearest 24.8 fixed-point value at ten pixels per tick. */
    const int32_t scaled = v120 * 10 * 256;
    wl_pointer_send_axis(pointer, now, axis, (scaled + (scaled < 0 ? -60 : 60)) / 120);
}

static void dispatch_surface_event(struct brook_surface *s,
                                   struct brook_seat_client *sc,
                                   struct brook_wm_event *e, uint32_t now) {
    switch (e->type) {
    case EVT_MOUSE_RELATIVE:
        pointer_relative_dispatch(s,sc,e);break;
    case EVT_MOUSE_MOVE: {
        if (!sc || !sc->pointer || pointer_surface_locked(s)) break;
        sc->pointer_x=pointer_fixed_floor(surface_pointer_fixed(s,e->x));
        sc->pointer_y=pointer_fixed_floor(surface_pointer_fixed(s,e->y));
        wl_fixed_t lx,ly;struct brook_surface *target=pointer_target(s,sc,e->x,e->y,&lx,&ly);
        pointer_enter_if_needed(sc, target, lx, ly);
        wl_pointer_send_motion(sc->pointer, now, lx, ly);
        sc->motion_lx = lx; sc->motion_ly = ly;
        if (wl_resource_get_version(sc->pointer) >= 5)
            wl_pointer_send_frame(sc->pointer);
        latency_input_sent(sc->client, LATENCY_MOTION);
        pointer_timeline_delivered(s->wm_id, e->type);
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
        sc->pointer_x=pointer_fixed_floor(surface_pointer_fixed(s,e->x));
        sc->pointer_y=pointer_fixed_floor(surface_pointer_fixed(s,e->y));
        {
            wl_fixed_t lx,ly;struct brook_surface *target=pointer_target(s,sc,e->x,e->y,&lx,&ly);
            pointer_enter_if_needed(sc, target, lx, ly);
            if (!pointer_surface_locked(s)) pointer_sync_position(sc, now, lx, ly);
        }
        /* scan: 0=left, 1=right, 2=middle  →  BTN_LEFT 0x110, etc. */
        uint32_t btn = 0x110 + (e->scan & 0x3);
        if (e->type == EVT_MOUSE_BTN_DOWN) sc->buttons |= 1u << (e->scan & 0x3);
        else sc->buttons &= ~(1u << (e->scan & 0x3));
        uint32_t st  = (e->type == EVT_MOUSE_BTN_DOWN)
                         ? WL_POINTER_BUTTON_STATE_PRESSED
                         : WL_POINTER_BUTTON_STATE_RELEASED;
        wl_pointer_send_button(sc->pointer, next_serial(), now, btn, st);
        if (wl_resource_get_version(sc->pointer) >= 5)
            wl_pointer_send_frame(sc->pointer);
        latency_input_sent(sc->client, LATENCY_BUTTON);
        pointer_timeline_delivered(s->wm_id, e->type);
        fprintf(stderr, "[waylandd] btn wm=%u 0x%x %s at (%d,%d)\n",
                s->wm_id, btn,
                st == WL_POINTER_BUTTON_STATE_PRESSED ? "down" : "up",
                e->x, e->y);
        break;
    }
    case EVT_MOUSE_SCROLL: {
        if (!sc || !sc->pointer) break;
        const int fine = e->mods & BROOK_WM_SCROLL_FINE;
        if (!fine && e->reserved && !pointer_event_token(s, e->reserved)) break;
        const int locked = pointer_surface_locked(s);
        if (fine && locked) break;
        if (fine && (e->mods & BROOK_WM_SCROLL_END)) {
            struct brook_surface *root = sc->entered_surface;
            while (root && root->subsurface_parent) root = root->subsurface_parent;
            if (root == s) pointer_stop_finger_scroll(sc, now);
            break;
        }

        /* Brook vertical is up positive. Wayland vertical is down positive. */
        const int32_t dy_ticks = -(int8_t)e->scan;
        const int32_t dx_ticks = (int8_t)e->ascii;
        const int32_t dy_v120 = fine ? -(int32_t)e->x : dy_ticks * 120;
        const int32_t dx_v120 = fine ? e->y : dx_ticks * 120;
        if (!dx_v120 && !dy_v120) break;

        if (!locked) {
            const int16_t px = fine ? (int16_t)e->reserved : e->x;
            const int16_t py = fine ? (int16_t)(e->reserved >> 16) : e->y;
            sc->pointer_x = pointer_fixed_floor(surface_pointer_fixed(s, px));
            sc->pointer_y = pointer_fixed_floor(surface_pointer_fixed(s, py));
            wl_fixed_t lx, ly;
            struct brook_surface *target = pointer_target(s, sc, px, py, &lx, &ly);
            pointer_enter_if_needed(sc, target, lx, ly);
        }
        const int finger = fine && (e->mods & BROOK_WM_SCROLL_FINGER);
        if (finger) {
            if (dy_v120) sc->finger_axes |= 1u << WL_POINTER_AXIS_VERTICAL_SCROLL;
            if (dx_v120) sc->finger_axes |= 1u << WL_POINTER_AXIS_HORIZONTAL_SCROLL;
        } else {
            pointer_stop_finger_scroll(sc, now);
        }
        const int version = wl_resource_get_version(sc->pointer);
        if (version >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
            wl_pointer_send_axis_source(sc->pointer, finger ? WL_POINTER_AXIS_SOURCE_FINGER : WL_POINTER_AXIS_SOURCE_WHEEL);
        pointer_send_scroll_axis(sc->pointer, now, WL_POINTER_AXIS_VERTICAL_SCROLL, dy_v120, dy_ticks, finger);
        pointer_send_scroll_axis(sc->pointer, now, WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx_v120, dx_ticks, finger);
        if (version >= WL_POINTER_FRAME_SINCE_VERSION) wl_pointer_send_frame(sc->pointer);
        latency_input_sent(sc->client, LATENCY_AXIS);
        pointer_timeline_delivered(s->wm_id, e->type);
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
        if ((e->mods & 0x08) || ((e->mods & 0x40) && g_keyboard_layout == BROOK_KEYBOARD_US))
            depressed |= (1u << 3); /* Mod1/Alt */
        if ((e->mods & 0x40) && g_keyboard_layout == BROOK_KEYBOARD_GB)
            depressed |= (1u << 7); /* Mod5/AltGr */
        if (e->mods & 0x10) locked    |= (1u << 1); /* Lock/Caps */

        /* Send key event first — xkbcommon clients call
         * xkb_state_update_key() which internally tracks modifier
         * state from the keymap. */
        uint32_t xkb_code = scancode_to_xkb(e->scan);
        wl_keyboard_send_key(sc->keyboard, next_serial(), now,
                             xkb_code, st);
        latency_input_sent(sc->client, LATENCY_KEY);

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
    case WM_EVT_POINTER_LEFT: {
        /* The kernel routes pointer input only to the client area under
         * it, so without this a client never learns the pointer went to
         * the desktop, WM chrome or another client's window. Chromium then
         * keeps its hover state and the cursor it last set (its resize
         * cursor from the shadow edge) when the pointer comes back; with
         * leave, the next event re-enters and it sets its cursor again. */
        if (!sc || !sc->pointer || !sc->entered_surface || sc->buttons) break;
        struct brook_surface *root = sc->entered_surface;
        while (root->subsurface_parent) root = root->subsurface_parent;
        if (root != s) break;
        pointer_stop_finger_scroll(sc, now);
        struct wl_resource *prev = sc->entered_surface->resource;
        if (prev && wl_resource_get_client(prev) == sc->client) {
            wl_pointer_send_leave(sc->pointer, next_serial(), prev);
            if (wl_resource_get_version(sc->pointer) >= 5)
                wl_pointer_send_frame(sc->pointer);
        }
        sc->entered_surface = NULL;
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
            const int32_t scale = (int32_t)surface_output_scale(s);
            int32_t neww = physical_to_logical(e->x,scale), newh = physical_to_logical(e->y,scale);
            const int was_managed = s->toplevel_maximized || s->toplevel_fullscreen;
            s->toplevel_fullscreen = (e->reserved & WM_RESIZE_FULLSCREEN) != 0;
            s->toplevel_maximized = !s->toplevel_fullscreen && (e->reserved & WM_RESIZE_MAXIMIZED) != 0;
            const int managed = s->toplevel_maximized || s->toplevel_fullscreen;
            s->toplevel_resizing = !managed && !was_managed && (e->reserved & WM_RESIZE_RESIZING) != 0;
            if (!managed && was_managed) {
                // The WM saved a buffer rectangle, including any CSD
                // shadows. A 0x0 restore asks the client to restore its
                // previous logical window geometry and decoration together.
                neww = newh = 0;
            } else if (!managed && s->deco_sent_mode > 0) {
                // xdg_toplevel.configure uses window geometry. The VFB and
                // the WM's outer client rectangle include client shadows.
                const int32_t shadow_w = (int32_t)physical_to_logical(s->vfb_w,scale) - s->window_geometry_w;
                const int32_t shadow_h = (int32_t)physical_to_logical(s->vfb_h,scale) - s->window_geometry_h;
                if (s->window_geometry_w > 0 && shadow_w > 0 && neww > shadow_w) neww -= shadow_w;
                if (s->window_geometry_h > 0 && shadow_h > 0 && newh > shadow_h) newh -= shadow_h;
            }
            s->configure_width = neww;
            s->configure_height = newh;
            s->configure_size_pending = 1;
            toplevel_send_state(s, neww, newh);
            fprintf(stderr,"[waylandd] WM_EVT_RESIZED wm=%u configure %dx%d max=%d full=%d resizing=%d serial=%u\n",
                s->wm_id,neww,newh,s->toplevel_maximized,s->toplevel_fullscreen,s->toplevel_resizing,s->next_configure_serial);
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
    surface->strip_due=3; /* Chromium redraws its strip in the (in)active colour. */
    int32_t width=surface->window_geometry_w>0?surface->window_geometry_w:(int32_t)physical_to_logical(surface->vfb_w,surface_output_scale(surface));
    int32_t height=surface->window_geometry_h>0?surface->window_geometry_h:(int32_t)physical_to_logical(surface->vfb_h,surface_output_scale(surface));
    if(surface->toplevel_maximized || surface->toplevel_fullscreen || surface->configure_size_pending) {
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
    if (event->type == WM_EVT_FOCUS_GAINED) { surface->native_focused=1;toplevel_set_activated(surface, 1); }
    if (event->type == WM_EVT_FOCUS_LOST) { surface->native_focused=0;toplevel_set_activated(surface, 0); }
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
    g_input_batch_us = count > 0 ? g_now_us() : 0;
    uint32_t now=g_now_ms();
    for(long i=0;i<count;++i) {
        g_input_recorded = 0;
        dispatch_window_event(surface,&events[i],now);
    }
    g_input_batch_us = 0;
}

/* After a pump, make keyboard focus and the X11 focus follow the window the
 * kernel WM has focused now, whatever order the windows' queues were read in:
 * A gained/lost then B gained, read as B then A, would otherwise leave the
 * keyboard on no surface and give X11 focus to A (keys then reach the wrong
 * X window, and clicking B changes nothing because B is already focused). */
static uint32_t g_focus_reconciled_wm;
static void focus_reconcile(void) {
    struct brook_surface *focused=NULL;
    for (struct brook_surface *s = g_surfaces; s; s = s->next)
        if (s->wm_id && s->native_focused && s->resource) focused=s;
    if (!focused) { g_focus_reconciled_wm=0;return; }
    struct wl_client *owner=wl_resource_get_client(focused->resource);
    for (struct brook_seat_client *sc=g_seat_clients;sc;sc=sc->next) {
        if (sc->client!=owner || !sc->keyboard || sc->kb_focus==focused) continue;
        if (sc->kb_focus && sc->kb_focus->resource && wl_resource_get_client(sc->kb_focus->resource)==sc->client)
            wl_keyboard_send_leave(sc->keyboard,next_serial(),sc->kb_focus->resource);
        struct wl_array keys; wl_array_init(&keys);
        wl_keyboard_send_enter(sc->keyboard,next_serial(),focused->resource,&keys);
        wl_array_release(&keys);
        wl_keyboard_send_modifiers(sc->keyboard,next_serial(),sc->kb_mods_depressed,0u,sc->kb_mods_locked,0u);
        sc->kb_focus=focused;text_input_focus(sc,focused);
    }
    if (g_focus_reconciled_wm==focused->wm_id) return;
    g_focus_reconciled_wm=focused->wm_id;
    xwayland_focus_reconciled(focused);
}

static void pump_input_once(void) {
    pointer_constraints_refresh();
    idle_inhibit_sync();
    for (struct brook_surface *s = g_surfaces; s; s = s->next)
        pump_input_for_surface(s);
    focus_reconcile();
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
static void deco_resource_destroyed(struct wl_resource *r) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (s && s->deco_resource==r) s->deco_resource=NULL;
}
/* Chromium-family browsers draw their tab strip in the title area when they
 * decorate client-side; Brook keeps its own caption buttons over the top-right
 * of that strip (the launcher asks Chromium for all three of its own buttons,
 * via GTK's button layout, so it keeps its tabs clear of the corner; Brook's
 * block covers them). BROOK_WAYLAND_CAPTION_APPS is a comma list of
 * app_id prefixes, each optionally ":height", the client's title-strip height
 * in surface units so the block spans it (Chromium's is 40 restored and
 * maximised); no height uses Brook's title height. Empty disables the overlay.
 * Returns the height, 0 for the native height, or -1 for no overlay. */
static int deco_caption_overlay_app(const struct brook_surface *s) {
    if (!s || !s->app_id[0]) return -1;
    const char *list = getenv("BROOK_WAYLAND_CAPTION_APPS");
    if (!list) list = "chromium:40,google-chrome:40";
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        const char *colon = memchr(p, ':', n);
        size_t prefix = colon ? (size_t)(colon - p) : n;
        if (prefix && !strncmp(s->app_id, p, prefix)) {
            int height = 0;
            for (const char *d = colon ? colon + 1 : p + n; d < p + n && *d >= '0' && *d <= '9'; ++d)
                if (height < 4096) height = height * 10 + (*d - '0');
            return height;
        }
        if (!e) break;
        p = e + 1;
    }
    return -1;
}
static int g_caption_overlay_unsupported, g_caption_rows_unsupported;
/* The kernel needs the client's shadow margin to place the buttons on its
 * visible frame: the surface's right/top edges to the window geometry. */
static uint32_t deco_caption_insets(const struct brook_surface *s) {
    if (s->window_geometry_w <= 0 || s->window_geometry_h <= 0 || !s->vfb_w) return 0;
    const int64_t scale = surface_output_scale(s);
    int64_t right = (int64_t)s->vfb_w - logical_to_physical((int64_t)s->window_geometry_x+s->window_geometry_w,scale);
    int64_t top = logical_to_physical(s->window_geometry_y,scale);
    if (right < 0) right = 0;
    if (top < 0) top = 0;
    if (right > 0xFFFF) right = 0xFFFF;
    if (top > 0xFFFF) top = 0xFFFF;
    return (uint32_t)right | ((uint32_t)top << 16);
}
/* Match each physical row of the client's title strip, including the thin
 * top highlight and the divider. The client owns the strip and reserves
 * space for its controls. Sampling beside Brook's block avoids reading its
 * glyphs; the edge outside the client's controls gives a second sample.
 * Intel samples share one nonblocking cache sync and mapping per image. */
enum { STRIP_SAMPLE_MS=500, STRIP_RETRY_MS=20, STRIP_RETRY_LIMIT=25 };
static struct wl_event_source *g_strip_timer;
struct strip_pixels {
    const uint32_t *base;
    uint32_t stride, alpha;
    void *mapping;
    size_t length;
};
static int strip_pixels_open(struct brook_image *im, uint32_t bottom, struct strip_pixels *out) {
    if (im->pixels) {
        *out=(struct strip_pixels){.base=im->pixels,.stride=im->width};
        return 1;
    }
    const struct brook_dmabuf_buffer *db=im->source?im->source->dmabuf:NULL;
    if (!g_drm_intel || !db || db->modifier || im->pending_fd<0 || im->uncertain ||
        (db->stride&3) || (db->offset&3) || db->stride/4<im->width) return 0;
    errno=0;
    long ready=syscall(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ,(long)im->pending_fd,0L,0L,0L,0L,0L);
    if (ready<0) return errno==EAGAIN || errno==EINTR ? -1 : 0;
    if (ready) return 0;
    struct { uint32_t handle,pad;uint64_t offset,flags,extensions; } map={db->bo_handle,0,0,2,0};
    /* DRM_IOCTL_I915_GEM_MMAP_OFFSET (write-back), as intel_dmabuf_copy. */
    if (ioctl(g_drm_fd,((3u<<30)|((unsigned)sizeof(map)<<16)|('d'<<8)|0x64u),&map)) return 0;
    const uint64_t bytes=(uint64_t)db->offset+(uint64_t)db->stride*bottom;
    if (bytes>SIZE_MAX-4095u) return 0;
    const size_t length=(size_t)((bytes+4095u)&~UINT64_C(4095));
    void *p=mmap(NULL,length,PROT_READ,MAP_SHARED,g_drm_fd,(off_t)map.offset);
    if (p==MAP_FAILED) return 0;
    *out=(struct strip_pixels){.base=(const uint32_t *)((const uint8_t *)p+db->offset),
        .stride=db->stride/4,.alpha=db->fourcc==0x34325258?0xff000000u:0,
        .mapping=p,.length=length};
    return 1;
}
static uint32_t strip_pixel(const struct strip_pixels *p, uint32_t x, uint32_t y) {
    return p->base[(size_t)y*p->stride+x]|p->alpha;
}
static uint32_t strip_majority(uint32_t a, uint32_t b, uint32_t c) {
    if ((a>>24)==255 && (a==b || a==c)) return a;
    return (b>>24)==255 && b==c ? b : 0;
}
/* 1 sampled, 0 not possible, -1 busy (retry later). */
static int strip_sample(struct brook_surface *s, struct brook_image *im) {
    const struct brook_view *view=s->tree.current?s->tree.current->value:NULL;
    const int strip=deco_caption_overlay_app(s);
    if (!view || !im || s->window_geometry_w<=0 || strip<=0) return 0;
    const struct brook_geometry *geometry=&view->geometry;
    if (geometry->transform || !geometry->w || !geometry->h || !geometry->sw || !geometry->sh) return 0;
    // Use the committed viewport's mapping. Fractional clients normally
    // attach physical-sized buffers with wl_surface.buffer_scale=1.
    const int64_t right=(geometry->sx+
        ((int64_t)s->window_geometry_x+s->window_geometry_w)*geometry->sw/geometry->w)/256;
    const int64_t top=(geometry->sy+(int64_t)s->window_geometry_y*geometry->sh/geometry->h)/256;
    const int64_t bottom=(geometry->sy+
        ((int64_t)s->window_geometry_y+strip)*geometry->sh/geometry->h)/256;
    const int64_t strip_buffer=bottom-top;
    if (right<8 || top<0 || strip_buffer<=0 || right>(int64_t)im->width || bottom>im->height) return 0;

    struct strip_pixels pixels;
    int r=strip_pixels_open(im,(uint32_t)bottom,&pixels);
    if (r<=0) return r;
    const uint32_t middle=(uint32_t)(top+strip_buffer/2);
    const uint32_t colour=strip_majority(strip_pixel(&pixels,right-3,middle),
        strip_pixel(&pixels,right-4,middle),strip_pixel(&pixels,right-5,middle));
    if (colour) {
        const uint32_t scale=surface_output_scale(s);
        unsigned radius=0;
        const unsigned span=right>=32?32:(unsigned)right;
        while (radius<span && (strip_pixel(&pixels,right-1-radius,top)>>24)<128) ++radius;
        unsigned physical=(uint64_t)radius*scale*geometry->w*256 /
                          ((uint64_t)BROOK_WL_SCALE_ONE*geometry->sw);
        if (physical>127) physical=127;
        s->strip_style=(colour&0xffffffu)|(1u<<24)|(physical<<25);
        s->strip.count=0;

        const uint64_t physical_height=logical_to_physical(strip,scale);
        if (physical_height<=BROOK_CAPTION_STRIP_MAX_ROWS) {
            long desktop_percent=syscall(0xB04,2L,0L,0L,0L,0L,0L);
            if (desktop_percent<100 || desktop_percent>200) desktop_percent=100;
            uint32_t width=(BROOK_CAPTION_BUTTON_LOGICAL_WIDTH*desktop_percent+50)/100;
            const uint32_t available=s->vfb_w-(deco_caption_insets(s)&0xffffu);
            if (width*3>available) width=available/3;
            const int64_t sample_offset=(uint64_t)width*3*geometry->sw*BROOK_WL_SCALE_ONE /
                                        ((uint64_t)scale*geometry->w*256);
            const int64_t edge=right-sample_offset;
            for (uint32_t row=0;row<physical_height;++row) {
                const uint32_t y=(uint32_t)top+(uint64_t)row*strip_buffer/physical_height;
                uint32_t rgb=0;
                if (edge>=9 && edge<=right-3)
                    rgb=strip_majority(strip_pixel(&pixels,edge-1,y),
                        strip_pixel(&pixels,edge-5,y),strip_pixel(&pixels,edge-9,y));
                if (!rgb)
                    rgb=strip_majority(strip_pixel(&pixels,right-3,y),
                        strip_pixel(&pixels,right-4,y),strip_pixel(&pixels,right-5,y));
                s->strip.rgb[row]=(rgb?rgb:colour)&0xffffffu;
            }
            s->strip.count=(uint32_t)physical_height;
        }
    }
    if (pixels.mapping) munmap(pixels.mapping,pixels.length);
    return colour!=0;
}
static void surface_sync_decoration(struct brook_surface *s, int force);
static void strip_retry_drop(struct brook_surface *s) {
    if (s->strip_retry) { image_unref(s->strip_retry);s->strip_retry=NULL; }
    s->strip_retry_count=0;
}
static void surface_strip_commit(struct brook_surface *s, struct brook_image *older);
static int strip_timer_tick(void *data) {
    (void)data;int again=0;
    for (struct brook_surface *s=g_surfaces;s;s=s->next) {
        if (s->strip_trailing && !s->strip_retry) {
            /* A commit inside the throttle window was skipped: sample the
             * latest frame now, so an idle client's last change is seen. */
            s->strip_trailing=0;s->strip_due=1;
            surface_strip_commit(s,NULL);
            if (s->deco_sent_mode==2) surface_sync_decoration(s,0);
            if (s->strip_retry) again=1;
            continue;
        }
        if (!s->strip_retry) continue;
        int r=strip_sample(s,s->strip_retry);
        if (r<0 && ++s->strip_retry_count<STRIP_RETRY_LIMIT) { again=1;continue; }
        strip_retry_drop(s);
        if (r>0 && s->deco_sent_mode==2) surface_sync_decoration(s,0);
    }
    if (again) wl_event_source_timer_update(g_strip_timer,STRIP_RETRY_MS);
    return 0;
}
/* After a root commit of an overlaid window: refresh the sampled style. */
static void surface_strip_commit(struct brook_surface *s, struct brook_image *older) {
    if (s->deco_sent_mode!=2) return;
    struct brook_view *view=s->tree.current?s->tree.current->value:NULL;
    struct brook_image *im=view?view->image:NULL;
    const uint32_t now=g_now_ms();
    if (!im) return;
    if (!s->strip_due && now-s->strip_last_ms<STRIP_SAMPLE_MS) {
        /* Throttled: sample this frame once the window has passed. */
        if (!s->strip_trailing) {
            s->strip_trailing=1;
            if (!g_strip_timer) g_strip_timer=wl_event_loop_add_timer(wl_display_get_event_loop(g_display),strip_timer_tick,NULL);
            if (g_strip_timer) wl_event_source_timer_update(g_strip_timer,(int)(STRIP_SAMPLE_MS-(now-s->strip_last_ms))+1);
        }
        return;
    }
    s->strip_last_ms=now;
    /* A focus or size change is followed by a few commits before the client
     * shows its new strip: keep sampling each of them. */
    if (s->strip_due>0) --s->strip_due;
    int r=strip_sample(s,im);
    if (r<0) {
        /* The newest frame is still being drawn: read the previous one now
         * (same strip unless the theme or focus just changed) and the newest
         * once it is idle. */
        if (older && older!=im) (void)strip_sample(s,older);
        strip_retry_drop(s);image_ref(im);s->strip_retry=im;
        if (!g_strip_timer) g_strip_timer=wl_event_loop_add_timer(wl_display_get_event_loop(g_display),strip_timer_tick,NULL);
        if (g_strip_timer) wl_event_source_timer_update(g_strip_timer,STRIP_RETRY_MS);
    }
}
static void surface_sync_decoration(struct brook_surface *s, int force) {
    if (!s || !s->wm_id) return;
    int mode = s->deco_csd ? 1 : 0;
    uint32_t insets = 0, height = 0, style = 0;
    int strip = -1;
    if (mode && !s->xdg_popup && s->xdg_toplevel && s->deco_resource && s->deco_requested_csd &&
        !g_caption_overlay_unsupported && (strip = deco_caption_overlay_app(s)) >= 0) {
        mode = 2;
        insets = deco_caption_insets(s);
        uint64_t physical = logical_to_physical(strip,surface_output_scale(s));
        height = physical > 0xFFFF ? 0xFFFF : (uint32_t)physical;
        style = s->strip_style;
    }
    const BrookCaptionStrip *rows =
        mode == 2 && !g_caption_rows_unsupported && height && s->strip.count == height ? &s->strip : NULL;
    const uint32_t row_count = rows ? rows->count : 0;
    if (mode != 2) s->strip_due = 1;
    if (!force && mode == s->deco_sent_mode && insets == s->deco_sent_insets &&
        height == s->deco_sent_height && style == s->deco_sent_style &&
        row_count == s->deco_sent_strip.count &&
        (!rows || !memcmp(rows->rgb, s->deco_sent_strip.rgb, row_count*sizeof(uint32_t)))) return;
    long rc = wm_set_decoration(s->wm_id, rows ? 3 : mode, insets, height, style, rows);
    if (rows && rc != 3) {
        g_caption_rows_unsupported = 1;
        rows = NULL;
        rc = wm_set_decoration_mode(s->wm_id, mode, insets, height, style);
    }
    if (mode == 2 && rc == 0) {
        /* An older kernel treats mode 2 as plain CSD: stop asking. */
        g_caption_overlay_unsupported = 1;
        fprintf(stderr, "[waylandd] caption overlay unsupported by kernel; plain client-side\n");
    }
    if (force || mode != s->deco_sent_mode)
        fprintf(stderr, "[waylandd] decoration wm=%u mode=%s insets=%u,%u height=%u rc=%ld\n", s->wm_id,
                mode == 2 ? "client_side+captions" : mode ? "client_side" : "server_side",
                insets & 0xFFFF, insets >> 16, height, rc);
    const uint32_t sent_rows = rows ? rows->count : 0;
    if (style != s->deco_sent_style || sent_rows != s->deco_sent_strip.count)
        fprintf(stderr, "[waylandd] caption style wm=%u strip=#%06x radius=%u rows=%u\n", s->wm_id,
                style & 0xFFFFFF, style >> 25, sent_rows);
    s->deco_sent_mode = mode;
    s->deco_sent_insets = insets;
    s->deco_sent_height = height;
    s->deco_sent_style = style;
    if (rows) s->deco_sent_strip=*rows; else s->deco_sent_strip.count=0;
}
static void surface_apply_decoration(struct brook_surface *s, int csd) {
    if (!s) return;
    /* Popups never acquire toplevel chrome, irrespective of negotiation. */
    csd = csd || s->xdg_popup != NULL;
    s->deco_csd = csd;
    if (s->wm_id) {
        surface_sync_decoration(s, 1);
        s->deco_pending = 0;
    } else {
        fprintf(stderr, "[waylandd] decoration pending mode=%s\n",
                csd ? "client_side" : "server_side");
        s->deco_pending = 1;
    }
}
/* Clients whose client-side frame is translucent (shadows, rounded corners)
 * and that support server-side decoration well. Answering their CLIENT_SIDE
 * request with SERVER_SIDE (the protocol lets the compositor choose) makes
 * the window opaque, so the renderer can cull beneath it, and Brook's WM
 * draws the chrome. Opt-in: BROOK_WAYLAND_SSD_APPS is a comma list of
 * app_id prefixes (unset or empty: honour the client's request). */
static int deco_force_server_side(const struct brook_surface *s){
    if(!s || !s->app_id[0])return 0;
    const char *list=getenv("BROOK_WAYLAND_SSD_APPS");
    /* Off by default: Chromium keeps drawing its custom frame inside the
     * server-side one (gen 405: a double frame with a transparent shadow
     * margin). Chromium keeps its own frame; Brook overlays its caption
     * buttons on it instead (deco_caption_overlay_app). */
    if(!list)return 0;
    for(const char *p=list;*p;){
        const char *e=strchr(p,',');size_t n=e?(size_t)(e-p):strlen(p);
        if(n && !strncmp(s->app_id,p,n))return 1;
        if(!e)break;p=e+1;
    }
    return 0;
}
static void deco_apply_mode(struct wl_resource *r, int csd) {
    struct brook_surface *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->deco_resource=r;s->deco_requested_csd=csd;
    if(csd && deco_force_server_side(s)){
        csd=0;fprintf(stderr,"[waylandd] decoration server_side policy app_id=%s\n",s->app_id);}
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
    wl_resource_set_implementation(d, &deco_impl, s, deco_resource_destroyed);
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

/* ---------------- wp_fractional_scale_v1 ---------------- */
static void fractional_destroy(struct wl_client *client,struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void fractional_resource_destroyed(struct wl_resource *resource)
{
    struct brook_surface *surface=wl_resource_get_user_data(resource);
    if (surface) surface->fractional_scale=NULL;
}

static const struct wp_fractional_scale_v1_interface fractional_impl={
    .destroy=fractional_destroy,
};

static void fractional_get(struct wl_client *client,struct wl_resource *manager,
                           uint32_t id,struct wl_resource *surface_resource)
{
    struct brook_surface *surface=wl_resource_get_user_data(surface_resource);
    if (!surface) return;
    if (surface->fractional_scale) {
        wl_resource_post_error(manager,WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
                               "fractional scale already exists for this surface");
        return;
    }
    struct wl_resource *resource=wl_resource_create(client,&wp_fractional_scale_v1_interface,1,id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->fractional_scale=resource;
    wl_resource_set_implementation(resource,&fractional_impl,surface,fractional_resource_destroyed);
    wp_fractional_scale_v1_send_preferred_scale(resource,client_native_scale(client)?BROOK_WL_SCALE_ONE:g_output_scale120);
}

static const struct wp_fractional_scale_manager_v1_interface fractional_manager_impl={
    .destroy=fractional_destroy,
    .get_fractional_scale=fractional_get,
};

static void fractional_bind(struct wl_client *client,void *data,uint32_t version,uint32_t id)
{
    (void)data;
    struct wl_resource *resource=wl_resource_create(client,&wp_fractional_scale_manager_v1_interface,version,id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource,&fractional_manager_impl,NULL,NULL);
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
    if (g_drm_intel) {
        struct brook_dmabuf_import *entry=calloc(1,sizeof(*entry));
        if (!entry) { struct drm_gem_close gc={ph.handle,0};ioctl(g_drm_fd,DRM_IOCTL_GEM_CLOSE,&gc);return 0; }
        entry->bo=ph.handle;entry->gres=1;entry->refs=1;entry->next=g_dmabuf_imports;g_dmabuf_imports=entry;
        if (out_bo) *out_bo=entry->bo;
        return entry->gres;
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

/* The caller has established CPU visibility and retains the producer until
 * this copy finishes. */
static struct brook_image *intel_dmabuf_copy(const struct brook_dmabuf_buffer *db)
{
    struct { uint32_t handle,pad;uint64_t offset,flags,extensions; } map={db->bo_handle,0,0,2,0};
    if (ioctl(g_drm_fd,DRM_IOC(0x64,sizeof(map)),&map)) return NULL;
    const int tiling=brook_tiling_from_modifier(db->modifier);
    if (tiling<0) return NULL;
    uint64_t bytes=(uint64_t)db->offset+brook_tiled_bytes(tiling,db->stride,(uint32_t)db->height);
    if (!bytes || bytes>SIZE_MAX-4095) return NULL;
    size_t length=(size_t)((bytes+4095)&~UINT64_C(4095));
    void *pixels=mmap(NULL,length,PROT_READ,MAP_SHARED,g_drm_fd,(off_t)map.offset);
    if (pixels==MAP_FAILED) return NULL;
    struct brook_image *image;
    if (tiling) {
        /* A tiled client buffer (glamor or a DRI3 game) read on the CPU
         * fallback path: detile into a linear copy, then snapshot it. */
        uint32_t *linear=malloc((size_t)db->width*(size_t)db->height*4u);
        if (!linear) { munmap(pixels,length);return NULL; }
        brook_detile32(linear,(size_t)db->width,(const uint8_t *)pixels+db->offset,tiling,db->stride,db->width,db->height);
        image=image_snapshot((const uint8_t *)linear,db->width,db->height,
            db->width*4u,db->fourcc==0x34325258?WL_SHM_FORMAT_XRGB8888:WL_SHM_FORMAT_ARGB8888);
        free(linear);
    } else image=image_snapshot((const uint8_t *)pixels+db->offset,db->width,db->height,
        db->stride,db->fourcc==0x34325258?WL_SHM_FORMAT_XRGB8888:WL_SHM_FORMAT_ARGB8888);
    if(image)g_layer_readback_bytes+=(uint64_t)db->width*db->height*4;
    munmap(pixels,length);return image;
}
/* ---- nv-drm (renderD130) dmabufs: the RTX 2080 / NVK path ----------------------------
 * Step 1 of notes/m5/zero-copy-design.md: NVK's Wayland WSI exports a LINEAR GART buffer
 * (its PRIME blit target), we import it on renderD130, wait for the frame's implicit fence
 * (Mesa imports the present semaphore's sync file into the dmabuf before commit; the
 * kernel's GEM_CPU_PREP waits on it) and take the same CPU snapshot as a wl_shm buffer.
 * That removes the client-side readback + memcpy of MESA_VK_WSI_DEBUG=sw; the compositor
 * still composes on the CPU, and CE present copies its shadow to scanout as before.
 *
 * main_device stays 0 (no virtio/i915 node), so Mesa sees a different "GPU" and takes its
 * cross-device path: render tiled in VRAM, blit to a linear buffer in host memory, which
 * is what a CPU compositor can read (VRAM has no CPU window on renderD130 yet). */
struct drm_nouveau_gem_info { uint32_t handle, domain; uint64_t size, offset, map_handle; uint32_t tile_mode, tile_flags; };
struct drm_nouveau_gem_cpu_prep { uint32_t handle, flags; };
#define DRM_IOCTL_NOUVEAU_GEM_INFO     DRM_IOC(0x40 + 0x44, sizeof(struct drm_nouveau_gem_info))
#define DRM_IOCTL_NOUVEAU_GEM_CPU_PREP DRM_IOC(0x40 + 0x42, sizeof(struct drm_nouveau_gem_cpu_prep))
#define NOUVEAU_GEM_DOMAIN_GART 4u
#define NOUVEAU_GEM_CPU_PREP_NOWAIT 1u
#define NV_DMABUF_WAIT_MS 1000u
struct brook_nv_import {
    uint32_t bo, refs;
    uint64_t size, map_handle;
    const uint8_t *map;   /* lazily mapped, read-only, unmapped with the last reference */
    struct brook_nv_import *next;
};
static struct brook_nv_import *g_nv_imports;
static uint64_t g_nv_imported, g_nv_refused, g_nv_snapshots, g_nv_wait_us_max;
static int nv_dmabuf_wanted(void){
    const char *v=getenv("BROOK_WAYLAND_NV_DMABUF");
    if (v) return !strcmp(v,"1");
    return access("/etc/waylandd-nv-dmabuf",F_OK)==0;
}
/* Block-linear NVIDIA modifiers in the format table: only for a future GPU present path.
 * Mesa never sends them to this compositor (main_device mismatch -> LINEAR prime blit), and
 * the CPU path refuses them. Off unless BROOK_WAYLAND_NV_MODIFIERS=1. */
static int nv_dmabuf_block_linear_advertised(void){
    const char *v=getenv("BROOK_WAYLAND_NV_MODIFIERS");
    return g_nv_fd>=0 && v && !strcmp(v,"1");
}
/* renderD130 appears only after GSP-RM's autostart, seconds after waylandd starts (27 Nov
 * hardware try: "unavailable (No such file or directory)" at startup). So the open is retried
 * whenever a client binds zwp_linux_dmabuf_v1 or creates a dmabuf buffer, until it succeeds;
 * a failure is logged once. */
static int g_nv_open_logged;
static void nv_dmabuf_open(void){
    if (g_drm_fd>=0 || g_nv_fd>=0 || !nv_dmabuf_wanted()) return;
    g_nv_fd=open("/dev/dri/renderD130",O_RDWR|O_CLOEXEC);
    if (g_nv_fd<0) { if (!g_nv_open_logged++) fprintf(stderr,"[waylandd] nv dmabuf: renderD130 unavailable (%s); retrying on dmabuf use\n",strerror(errno)); }
    else fprintf(stderr,"[waylandd] nv dmabuf: renderD130 fd=%d (LINEAR GART imports, CPU snapshot)\n",g_nv_fd);
}
static void nv_gem_close(uint32_t bo){
    struct drm_gem_close gc={bo,0};
    if (g_nv_fd>=0) ioctl(g_nv_fd,DRM_IOCTL_GEM_CLOSE,&gc);
}
static void nv_dmabuf_release(uint32_t bo){
    struct brook_nv_import **link=&g_nv_imports;
    while (*link && (*link)->bo!=bo) link=&(*link)->next;
    if (!*link) return;
    struct brook_nv_import *e=*link;
    if (--e->refs) return;
    *link=e->next;
    if (e->map) munmap((void *)e->map,(size_t)e->size);
    nv_gem_close(e->bo);
    free(e);
}
/* PRIME_FD_TO_HANDLE + GEM_INFO: accept a GART pitch-linear BO that holds the declared
 * layout. Repeated imports of one BO share the handle (the kernel returns the same one). */
static int nv_dmabuf_import(int fd, struct brook_dmabuf_buffer *db){
    db->nv_bo=0;
    struct drm_prime_handle ph={0,0,fd};
    if (ioctl(g_nv_fd,DRM_IOCTL_PRIME_FD_TO_HANDLE,&ph)) {
        fprintf(stderr,"[waylandd] nv dmabuf: FD_TO_HANDLE failed: %s\n",strerror(errno));++g_nv_refused;return 0;
    }
    struct brook_nv_import *e=g_nv_imports;
    while (e && e->bo!=ph.handle) e=e->next;
    const char *why=NULL;
    struct drm_nouveau_gem_info info={.handle=ph.handle};
    if (!e && ioctl(g_nv_fd,DRM_IOCTL_NOUVEAU_GEM_INFO,&info)) why="GEM_INFO failed";
    const uint64_t size=e?e->size:info.size;
    if (!why && !e) {
        if (!(info.domain&NOUVEAU_GEM_DOMAIN_GART)) why="VRAM BO (no CPU window; needs the GPU present path)";
        else if (((info.tile_flags>>8)&0xff) || info.tile_mode) why="tiled BO (kind/tile_mode set) declared linear";
        else if (!info.map_handle) why="BO not mappable";
    }
    if (!why && db->modifier!=BROOK_DMABUF_MOD_LINEAR && db->modifier!=BROOK_DMABUF_MOD_IMPLICIT) why="modifier";
    if (!why) {
        const struct BrookDmabufLayout layout={(uint32_t)db->width,(uint32_t)db->height,db->fourcc,db->stride,db->offset,0,BROOK_DMABUF_MOD_LINEAR};
        if (BrookDmabufValidate(&layout,size,0)!=BROOK_DMABUF_LAYOUT_OK) why="layout exceeds the BO";
    }
    if (!why && !e) {
        e=calloc(1,sizeof(*e));
        if (!e) why="out of memory";
        else { e->bo=ph.handle;e->size=info.size;e->map_handle=info.map_handle;e->next=g_nv_imports;g_nv_imports=e; }
    }
    if (why) {
        fprintf(stderr,"[waylandd] nv dmabuf refused: %s (%dx%d stride=%u size=%llu)\n",why,db->width,db->height,db->stride,
                (unsigned long long)size);
        if (!e) nv_gem_close(ph.handle);   /* a shared handle stays with its other users */
        ++g_nv_refused;return 0;
    }
    ++e->refs;db->nv_bo=e->bo;++g_nv_imported;
    return 1;
}
static uint64_t nv_now_us(void){ struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
/* Wait (bounded) for the producer's implicit fence, then copy the pixels. A failed or timed
 * out wait does not prove the GPU stopped writing: *uncertain withholds wl_buffer.release. */
static struct brook_image *nv_dmabuf_snapshot(const struct brook_dmabuf_buffer *db,int *uncertain){
    *uncertain=0;
    struct brook_nv_import *e=g_nv_imports;
    while (e && e->bo!=db->nv_bo) e=e->next;
    if (!e) return NULL;
    const uint64_t t0=nv_now_us();
    for (;;) {
        struct drm_nouveau_gem_cpu_prep prep={e->bo,NOUVEAU_GEM_CPU_PREP_NOWAIT};
        if (!ioctl(g_nv_fd,DRM_IOCTL_NOUVEAU_GEM_CPU_PREP,&prep)) break;
        const int error=errno;
        if (error==EINTR) continue;
        if (error!=EBUSY || nv_now_us()-t0>NV_DMABUF_WAIT_MS*1000u) {
            *uncertain=1;
            fprintf(stderr,"[waylandd] nv dmabuf: producer fence %s (errno=%d); release withheld\n",
                    error==EBUSY?"timed out":"failed",error);
            return NULL;
        }
        usleep(200);
    }
    const uint64_t waited=nv_now_us()-t0;
    if (waited>g_nv_wait_us_max) g_nv_wait_us_max=waited;
    if (!e->map) {
        void *m=mmap(NULL,(size_t)e->size,PROT_READ,MAP_SHARED,g_nv_fd,(off_t)e->map_handle);
        if (m==MAP_FAILED) { fprintf(stderr,"[waylandd] nv dmabuf: mmap failed: %s\n",strerror(errno));return NULL; }
        e->map=m;
    }
    struct brook_image *image=image_snapshot(e->map+db->offset,db->width,db->height,db->stride,
        db->fourcc==0x34325258?WL_SHM_FORMAT_XRGB8888:WL_SHM_FORMAT_ARGB8888);
    if (image) { g_layer_readback_bytes+=(uint64_t)db->width*db->height*4;++g_nv_snapshots; }
    return image;
}

/* ---- Zero-copy step 2: show the client's GART BO directly (WM_SET_NV_LAYER).
 * A plain xdg toplevel (no subsurfaces, viewport, transform or server-drawn caption strip)
 * committing an XRGB nv dmabuf keeps the wl_buffer instead of a CPU snapshot: once the
 * producer fence is done, surface_render hands the BO to the kernel when it is 1:1 with the
 * window. The kernel then copies it to scanout with the copy engine (or the compositor reads
 * it, when the window is covered), and the wl_buffer is released when a newer buffer
 * replaces it there. Anything else snapshots it as before. Off with BROOK_WAYLAND_NV_LAYER=0
 * or /etc/waylandd-nv-no-layer. */
#define BROOK_SYS_WM_SET_NV_LAYER 0xB2B
static uint64_t g_nv_layer_sets,g_nv_layer_materialized,g_nv_layer_detaches,g_nv_layer_failed;
static int nv_layer_wanted(void){
    static int wanted=-1;
    if(wanted<0){const char *v=getenv("BROOK_WAYLAND_NV_LAYER");
        wanted=v?strcmp(v,"0")!=0:access("/etc/waylandd-nv-no-layer",F_OK)!=0;}
    return wanted;
}
static struct brook_nv_import *nv_import_find(uint32_t bo){
    struct brook_nv_import *e=g_nv_imports;
    while (e && e->bo!=bo) e=e->next;
    return e;
}
static int nv_layer_candidate(const struct brook_surface *s,const struct brook_dmabuf_buffer *db){
    return nv_layer_wanted() && db->nv_bo && db->fourcc==0x34325258 && s->xdg_toplevel && !s->subsurface_parent &&
           !s->children && !s->viewport && !s->is_cursor && s->role==1 && !s->geometry.transform && s->deco_sent_mode!=2;
}
/* Wait for the producer (as nv_dmabuf_snapshot), then an image that holds the buffer. */
static struct brook_image *nv_layer_image(struct brook_buffer *buffer,int *uncertain){
    *uncertain=0;
    const struct brook_dmabuf_buffer *db=buffer->dmabuf;
    struct brook_nv_import *e=nv_import_find(db->nv_bo);
    if (!e) return NULL;
    const uint64_t t0=nv_now_us();
    for (;;) {
        struct drm_nouveau_gem_cpu_prep prep={e->bo,NOUVEAU_GEM_CPU_PREP_NOWAIT};
        if (!ioctl(g_nv_fd,DRM_IOCTL_NOUVEAU_GEM_CPU_PREP,&prep)) break;
        const int error=errno;
        if (error==EINTR) continue;
        if (error!=EBUSY || nv_now_us()-t0>NV_DMABUF_WAIT_MS*1000u) {
            *uncertain=1;
            fprintf(stderr,"[waylandd] nv dmabuf: producer fence %s (errno=%d); release withheld\n",
                    error==EBUSY?"timed out":"failed",error);
            return NULL;
        }
        usleep(200);
    }
    const uint64_t waited=nv_now_us()-t0;
    if (waited>g_nv_wait_us_max) g_nv_wait_us_max=waited;
    struct brook_image *image=calloc(1,sizeof(*image));
    if (!image) return NULL;
    image->refs=1;image->width=db->width;image->height=db->height;image->format=2;image->opaque=1;
    image->nv_layer=1;image->source=buffer;++buffer->refs;   /* the caller's buffer_begin_use is this image's */
    return image;
}
static long nv_layer_syscall(uint32_t wm,const struct brook_image *image){
    if (!image) return syscall(BROOK_SYS_WM_SET_NV_LAYER,(long)wm,0L,0L,0L,0L,0L);
    const struct brook_dmabuf_buffer *db=image->source->dmabuf;
    const struct brook_nv_import *e=nv_import_find(db->nv_bo);
    if (!e) { errno=ENOENT;return -1; }
    return syscall(BROOK_SYS_WM_SET_NV_LAYER,(long)wm,(long)g_nv_fd,(long)(e->map_handle+db->offset),(long)db->stride,
                   (long)((uint32_t)db->width|((uint32_t)db->height<<16)),1L);
}
/* 1 = the window now shows this layer (nothing else to publish), 0 = not applicable. */
static int nv_layer_present(struct brook_surface *root,struct brook_draw *draws,unsigned count){
    if (count!=1 || draws[0].x || draws[0].y || !root->wm_id || g_nv_fd<0) return 0;
    struct brook_image *image=draws[0].view->image;
    const struct brook_geometry *g=&draws[0].view->geometry;
    const uint32_t scale=surface_output_scale(root);
    if (!image->nv_layer || image->pixels || !image->source || !image->source->dmabuf || g->transform || g->sx || g->sy ||
        g->sw!=(int64_t)image->width*256 || g->sh!=(int64_t)image->height*256 ||
        logical_to_physical(g->w,scale)!=image->width || logical_to_physical(g->h,scale)!=image->height ||
        root->vfb_w!=image->width || root->vfb_h!=image->height) return 0;
    if (root->nv_layer_image==image) return 1;   /* a metadata-only commit */
    if (nv_layer_syscall(root->wm_id,image)) {
        if (!g_nv_layer_failed++) fprintf(stderr,"[waylandd] nv layer: WM_SET_NV_LAYER failed (%s); CPU snapshots\n",strerror(errno));
        return 0;
    }
    /* The kernel no longer reads the previous layer: its buffer may go back to the client. */
    struct brook_image *old=root->nv_layer_image;
    image_ref(image);root->nv_layer_image=image;image_unref(old);
    if (!g_nv_layer_sets++) fprintf(stderr,"[waylandd] nv layer: wm=%u %ux%u stride=%u shown directly\n",root->wm_id,
                                    image->width,image->height,image->source->dmabuf->stride);
    surface_layer_trace(root,draws,count,LAYER_PATH_SINGLE);
    return 1;
}
/* Give every held nv layer image in the scene its pixels (a CPU snapshot of the BO, as
 * nv_dmabuf_snapshot), releasing the buffer now unless the kernel still shows it. */
static int nv_layer_materialize(struct brook_surface *root,struct brook_draw *draws,unsigned count){
    for (unsigned i=0;i<count;++i) {
        struct brook_image *image=draws[i].view->image;
        if (!image->nv_layer || image->pixels || !image->source || !image->source->dmabuf) continue;
        const struct brook_dmabuf_buffer *db=image->source->dmabuf;
        struct brook_nv_import *e=nv_import_find(db->nv_bo);
        if (!e) return 0;
        if (!e->map) {
            void *m=mmap(NULL,(size_t)e->size,PROT_READ,MAP_SHARED,g_nv_fd,(off_t)e->map_handle);
            if (m==MAP_FAILED) { fprintf(stderr,"[waylandd] nv dmabuf: mmap failed: %s\n",strerror(errno));return 0; }
            e->map=m;
        }
        struct brook_image *copy=image_snapshot(e->map+db->offset,db->width,db->height,db->stride,WL_SHM_FORMAT_XRGB8888);
        if (!copy) return 0;
        image->pixels=copy->pixels;copy->pixels=NULL;image_unref(copy);
        g_layer_readback_bytes+=(uint64_t)db->width*db->height*4;++g_nv_snapshots;++g_nv_layer_materialized;
        if (image!=root->nv_layer_image) {
            struct brook_buffer *b=image->source;image->source=NULL;
            buffer_release(b);buffer_drop(b);
        }
    }
    return 1;
}
static void nv_layer_detach(struct brook_surface *root){
    if (!root->nv_layer_image) return;
    if (root->wm_id) (void)nv_layer_syscall(root->wm_id,NULL);
    struct brook_image *old=root->nv_layer_image;root->nv_layer_image=NULL;image_unref(old);
    ++g_nv_layer_detaches;
}
/* The window is gone (the kernel dropped its layer with it). */
static void nv_layer_forget(struct brook_surface *s){
    struct brook_image *old=s->nv_layer_image;s->nv_layer_image=NULL;image_unref(old);
}

/* The Wayland buffer ownership contract excludes new producer writes until
 * release. Wait for submitted work and transition the backing to CPU access. */
static struct brook_image *intel_dmabuf_snapshot(const struct brook_dmabuf_buffer *db,int *uncertain)
{
    if(!uncertain)return NULL;
    *uncertain=0;
    struct { uint32_t handle,flags;int64_t timeout; } wait={db->bo_handle,0,-1};
    struct { uint32_t handle,read,write; } domain={db->bo_handle,1,0};
    /* A failed wait/domain transition does not prove that the GPU stopped.
     * Suppress wl_buffer.release; the buffer's sticky uncertainty survives
     * reuse of the same protocol resource. Mapping/allocation failures after
     * both calls succeed can release the producer normally. */
    if (ioctl(g_drm_fd,DRM_IOC(0x6c,sizeof(wait)),&wait)) {
        int error=errno;*uncertain=1;
        fprintf(stderr,"[waylandd] Intel buffer completion wait failed: errno=%d; release withheld\n",error);
        errno=error;return NULL;
    }
    if (ioctl(g_drm_fd,DRM_IOC(0x5f,sizeof(domain)),&domain)) {
        int error=errno;*uncertain=1;
        fprintf(stderr,"[waylandd] Intel buffer CPU transition failed: errno=%d; release withheld\n",error);
        errno=error;return NULL;
    }
    return intel_dmabuf_copy(db);
}

/* Queue ownership outlives a surface or a superseding commit. Every buffer is
 * retained until completion is observed. Event-loop only.
 *
 * The queue is advanced by events rather than a frame clock: a commit
 * (idle source), a message on the renderer channel (fd source), and the
 * 16 ms frame clock as a safety net. Each pass advances every image as far
 * as it can. A short retry timer is armed only while an image waits on a
 * condition that has no wakeup and must be polled:
 *  - producer snapshot-fence admission returns EAGAIN while a submission is
 *    being prepared;
 *  - the producer sync-file has not signalled (Brook's epoll does not wake a
 *    waiter on sync-file signalling, so fd readiness is not used);
 *  - the CPU cache transition (TRY_CPU_READ) reports the buffer busy;
 *  - after renderer loss, PEER_DRAIN waits until the old renderer is reaped.
 * An image whose offer awaits the renderer needs no timer: its SAFE or
 * PRESENTED arrives on the channel fd. */
enum { INTEL_PENDING_LIMIT=64,INTEL_RETRY_MS=2,INTEL_RETRY_DRAIN_MS=10,
       INTEL_PEER_DRAIN_MS=5000,INTEL_PEER_RESTART_RETRY_MS=250 };
static struct brook_image *g_intel_pending_head,*g_intel_pending_tail;
static unsigned g_intel_pending_count;
static uint64_t g_intel_queued,g_intel_ready,g_intel_busy,g_intel_failed;
static uint64_t g_intel_discarded;
static uint64_t g_intel_fences,g_intel_fence_pending,g_intel_direct;
static uint64_t g_intel_retained_snapshots,g_intel_retry_fires;
static struct wl_event_source *g_intel_retry_timer,*g_intel_idle;
static uint32_t g_intel_retry_ms;
static void intel_retry_after(uint32_t ms){if(!g_intel_retry_ms || ms<g_intel_retry_ms)g_intel_retry_ms=ms;}
static void intel_pending_append(struct brook_image *image){
    image->pending_next=NULL;
    if(g_intel_pending_tail)g_intel_pending_tail->pending_next=image;else g_intel_pending_head=image;
    g_intel_pending_tail=image;++g_intel_pending_count;
}
static int intel_retry_tick(void *unused){(void)unused;++g_intel_retry_fires;intel_pending_poll();return 0;}
static void intel_retry_arm(void){
    if(!g_intel_retry_timer && g_intel_retry_ms && g_display)
        g_intel_retry_timer=wl_event_loop_add_timer(wl_display_get_event_loop(g_display),intel_retry_tick,NULL);
    if(g_intel_retry_timer)wl_event_source_timer_update(g_intel_retry_timer,(int)g_intel_retry_ms);
}
static void intel_pending_idle(void *unused){(void)unused;g_intel_idle=NULL;intel_pending_poll();}
/* Advance the queue once the current request has been handled. */
static void intel_pending_schedule(void){
    if(!g_intel_idle && g_display)g_intel_idle=wl_event_loop_add_idle(wl_display_get_event_loop(g_display),intel_pending_idle,NULL);
}
static void intel_image_want_pixels(struct brook_image *image){
    if(!image || image->want_pixels || image->pending_cpu!=1)return;
    image->want_pixels=1;intel_pending_schedule();
}
/* Experimental opt-in bridge to one trusted desktop consumer. Only records
 * actually delivered need acknowledgement. A lost channel never implies
 * that its previously delivered GPU reads have retired. Event-loop only. */
static int g_gpu_window_channel=-1;
static int g_gpu_window_dead=-1;
static uint32_t g_gpu_peer_lost_ms;
static int g_gpu_peer_restart_asked, g_gpu_peer_restart_error;
static uint32_t g_gpu_peer_restart_attempt_ms;
static const char *g_gpu_disconnect_reason = "none";
static const char *g_gpu_connect_refusal = "none";
static const char *g_gpu_connect_last_nonempty_refusal = "none";
static uint64_t g_gpu_channel_connections, g_gpu_channel_losses, g_gpu_drain_waits, g_gpu_offer_local_rejected;
static void gpu_groups_lost(void);
static int gpu_layers_enabled(void);
struct gpu_group;
static void gpu_group_release(struct gpu_group *g);
static uint64_t g_gpu_group_offers,g_gpu_group_safe,g_gpu_group_presented,g_gpu_group_wait,g_gpu_group_full;
static uint64_t g_gpu_group_skip_shape,g_gpu_group_skip_buffer,g_gpu_group_skip_upload,g_gpu_group_uploads;

static struct wl_event_source *g_gpu_window_source;
static uint64_t g_gpu_window_offers,g_gpu_window_acks,g_gpu_window_recovered,g_gpu_window_presented;
/* Aggregate failures instead of logging each frame; the latter perturbs pacing. */
static uint64_t g_gpu_offer_no_channel,g_gpu_offer_no_root,g_gpu_offer_layers,g_gpu_offer_geometry,g_gpu_offer_window,g_gpu_offer_metadata;
static uint64_t g_gpu_offer_size_mismatch,g_gpu_opaque_offers,g_gpu_scaled_offers;
static uint64_t g_gpu_xwayland_offers,g_gpu_xwayland_resize_wait;
/* A root the renderer may take a GPU source for: a configured xdg_toplevel,
 * or a rootless Xwayland surface paired with its mapped X window. Role 4
 * surfaces never have an xdg handshake; brook-xwm's map and serial pairing
 * play that part (xwayland_surface_ready). Without this, every X11 window,
 * including glamor's DMA-BUF window buffers and Present flips of GL games,
 * was read back and composed into the VFB on the CPU (offer_skipped.no_root).
 * BROOK_WAYLAND_GPU_XWAYLAND=0 restores that CPU path. */
static int gpu_xwayland_offers_enabled(void){
    static int enabled=-1;
    if(enabled<0){const char *v=getenv("BROOK_WAYLAND_GPU_XWAYLAND");enabled=!v || strcmp(v,"0");}
    return enabled;
}
static int gpu_offer_root_ready(const struct brook_surface *root){
    if(!root || root->tree.parent)return 0;
    if(root->xdg_toplevel)return root->xdg_acked;
    return root->role==4 && gpu_xwayland_offers_enabled() && xwayland_surface_ready(root);
}
/* BROOK_WAYLAND_GPU_SCALED=0 keeps scaled single-buffer windows on the
 * provider's composition path (the previous behaviour, for A/B runs). */
static int gpu_scaled_offers_enabled(void){
    static int enabled=-1;
    if(enabled<0){const char *e=getenv("BROOK_WAYLAND_GPU_SCALED");enabled=!e || strcmp(e,"0");}
    return enabled;
}
static uint64_t g_gpu_connect_fail,g_gpu_auth_fail;
static int g_gpu_connect_errno,g_gpu_auth_errno;
struct GpuWindowTiming {uint64_t count,sum,buckets[10];uint32_t max;};
static const uint32_t g_gpuTimingBounds[10]={10,20,50,100,200,500,1000,2000,5000,UINT32_MAX};
static struct GpuWindowTiming g_offerWait,g_snapshotWait,g_presentWait;
static void gpu_window_timing_add(struct GpuWindowTiming *s,uint32_t start,uint32_t end){
    if(!getenv("BROOK_WAYLAND_GPU_TIMING") || !start)return;
    uint32_t elapsed=end-start;
    if(elapsed>0x7fffffffu)return;
    ++s->count;s->sum+=elapsed;if(elapsed>s->max)s->max=elapsed;
    for(unsigned i=0;i<10;++i)if(elapsed<=g_gpuTimingBounds[i]){++s->buckets[i];break;}
}
static uint32_t gpu_window_timing_p80_bound(const struct GpuWindowTiming *s){
    if(!s->count)return 0;
    uint64_t target=(s->count/5)*4+((s->count%5)*4+4)/5,seen=0;
    for(unsigned i=0;i<10;++i){seen+=s->buckets[i];if(seen>=target)return g_gpuTimingBounds[i];}
    return UINT32_MAX;
}
static void gpu_image_mark_uncertain(struct brook_image *image){
    if (image->offer_state != 3) image_buffer_trace(image, BUFFER_UNCERTAIN, &image->offer_identity);
    image->offer_identity=(struct BrookGpuWindowIdentity){0};
    image->offer_state=3;
}
static void gpu_window_channel_lost(const char *reason){
    if (g_gpu_window_channel >= 0) {
        ++g_gpu_channel_losses;
        g_gpu_disconnect_reason = reason;
    }
    if(g_gpu_window_source){wl_event_source_remove(g_gpu_window_source);g_gpu_window_source=NULL;}
    if(g_gpu_window_channel>=0){
        (void)shutdown(g_gpu_window_channel,SHUT_RDWR);
        if(g_gpu_window_dead>=0)close(g_gpu_window_channel);
        else {
            g_gpu_window_dead=g_gpu_window_channel;
            g_gpu_peer_lost_ms=g_now_ms();
            g_gpu_peer_restart_asked=g_gpu_peer_restart_error=0;
            g_gpu_peer_restart_attempt_ms=0;
        }
    }
    g_gpu_window_channel=-1;
    for(struct brook_image *i=g_intel_pending_head;i;i=i->pending_next)if(i->offer_state==1)gpu_image_mark_uncertain(i);
    gpu_groups_lost();
}
static void gpu_window_reap_dead(void){
    if(g_gpu_window_dead<0)return;
    for(struct brook_image *i=g_intel_pending_head;i;i=i->pending_next)if(i->offer_state==3)return;
    close(g_gpu_window_dead);g_gpu_window_dead=-1;
}
static void gpu_window_maybe_restart(void){
    const uint32_t now=g_now_ms();
    if(g_gpu_window_dead<0 || g_gpu_peer_restart_asked ||
       now-g_gpu_peer_lost_ms<INTEL_PEER_DRAIN_MS)return;
    if(g_gpu_peer_restart_error==EAGAIN &&
       now-g_gpu_peer_restart_attempt_ms<INTEL_PEER_RESTART_RETRY_MS)return;
    g_gpu_peer_restart_attempt_ms=now;
    long result=syscall(BROOK_SYS_GPU_WINDOW_PEER_RECOVERY,(long)g_gpu_window_dead,
                        (long)BROOK_GPU_WINDOW_PEER_RESTART,0L,0L,0L,0L);
    g_gpu_peer_restart_error=result<0?errno:0;
    g_gpu_peer_restart_asked=g_gpu_peer_restart_error!=EAGAIN;
}
static int gpu_window_channel_event(int fd,uint32_t mask,void *unused){
    (void)fd;(void)unused;
    struct wl_event_source *self=g_gpu_window_source;
    intel_pending_poll();
    /* A hangup with nothing left to read must not spin the event loop. */
    if(self && g_gpu_window_source==self && (mask&(WL_EVENT_HANGUP|WL_EVENT_ERROR))){gpu_window_channel_lost("poll-hangup");intel_pending_poll();}
    return 0;
}
static int gpu_window_connect_refused(const char *reason){
    g_gpu_connect_refusal=g_gpu_connect_last_nonempty_refusal=reason;
    return 0;
}
static int gpu_window_channel_connect(void){
    const char *path=getenv("BROOK_WAYLAND_GPU_SOCKET");
    if(!path || !*path)return gpu_window_connect_refused("no-configured-path");
    if(g_gpu_window_channel>=0)return 1;
    if(g_gpu_window_dead>=0)return gpu_window_connect_refused("retiring-old-peer");
    struct sockaddr_un addr={.sun_family=AF_UNIX};size_t length=strlen(path);
    if(length>=sizeof(addr.sun_path))return gpu_window_connect_refused("path-too-long");
    memcpy(addr.sun_path,path,length+1);
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0)return gpu_window_connect_refused("socket");
    if(connect(fd,(struct sockaddr *)&addr,sizeof(addr))){
        ++g_gpu_connect_fail;g_gpu_connect_errno=errno;close(fd);return gpu_window_connect_refused("connect");
    }
    if(syscall(BROOK_SYS_GPU_WINDOW_PEER_RECOVERY,(long)fd,(long)BROOK_GPU_WINDOW_PEER_AUTH,0L,0L,0L,0L)){
        ++g_gpu_auth_fail;g_gpu_auth_errno=errno;close(fd);return gpu_window_connect_refused("peer-auth");
    }
    g_gpu_window_channel=fd;++g_gpu_channel_connections;g_gpu_connect_refusal="none";
    /* Without the fd source the frame clock still drains the channel. */
    if(g_display)g_gpu_window_source=wl_event_loop_add_fd(wl_display_get_event_loop(g_display),fd,WL_EVENT_READABLE,gpu_window_channel_event,NULL);
    return 1;
}
static int gpu_window_retain_enabled(void){
    const char *retain=getenv("BROOK_WAYLAND_GPU_RETAIN");
    return !retain || strcmp(retain,"0");
}
/* The renderer composed a frame including this offer and the OS accepted it.
 * Callbacks committed with (or before) that content are done; a newer commit
 * still waits for its own presentation. Unknown identities are ignored. */
static struct gpu_group *gpu_group_find(const struct BrookGpuWindowIdentity *identity);
static void gpu_group_presented(struct gpu_group *g);
static void gpu_window_presented(const struct BrookGpuWindowIdentity *identity){
    struct brook_image *match=NULL;
    for(struct brook_image *i=g_intel_pending_head;i;i=i->pending_next)
        if(i->offer_state==1 && brook_gpu_window_identity_equal(&i->offer_identity,identity)){match=i;break;}
    if(!match){struct gpu_group *g=gpu_group_find(identity);if(g)gpu_group_presented(g);return;}
    /* A layered offer can already have paced this image. The acknowledgement
     * in the trace belongs to this exact standalone offer nevertheless. */
    if (g_buffer_trace.pid && !match->buffer_trace_offer_ack) {
        image_buffer_trace(match, BUFFER_ACK, identity);
        match->buffer_trace_offer_ack = 1;
    }
    if(match->gpu_presented)return;
    match->gpu_presented=1;++g_gpu_window_presented;
    pace_add(match->pace_x11,PACE_PRESENTED,match->pace_offer_us,g_now_us());
    const uint32_t now=g_now_ms();
    gpu_window_timing_add(&g_presentWait,match->offer_sent_ms,now);
    for(struct brook_surface *s=g_surfaces;s;s=s->next){
        if(s->tree.parent || s->wm_id!=identity->window)continue;
        const struct brook_view *view=s->tree.current?s->tree.current->value:NULL;
        surface_send_frame_callbacks_upto(s,now,view && view->image==match?UINT64_MAX:match->offer_tree_commit,match);
    }
}
/* A source may appear in a standalone offer and several layered commits.
 * It enters the adjacent Intel poll only after all references are SAFE. */
static void gpu_release_if_unheld(struct brook_image *image) {
    if (image->offer_state != 1 || image->pending_fd < 0 || image->group_refs ||
        brook_gpu_window_identity_valid(&image->offer_identity)) return;
    image->offer_state = 2;
    image_buffer_trace(image, BUFFER_RENDERER_RETIRED, NULL);
}

static void gpu_window_channel_drain(void){
    if(g_gpu_window_channel<0)return;
    for(unsigned n=0;n<64;++n){
        struct BrookGpuWindowPacket packet={0};int fds[2];
        int result=brook_gpu_window_receive(g_gpu_window_channel,&packet,fds);
        if(!result)return;
        if(result<0 || (packet.type!=BROOK_GPU_WINDOW_SAFE && packet.type!=BROOK_GPU_WINDOW_PRESENTED)){
            if(result>0){if(fds[0]>=0)close(fds[0]);if(fds[1]>=0)close(fds[1]);}
            gpu_window_channel_lost(result<0?"receive-error-or-eof":"unexpected-message");return;
        }
        if(packet.type==BROOK_GPU_WINDOW_PRESENTED){gpu_window_presented(&packet.identity);continue;}
        struct brook_image *match=NULL;
        for(struct brook_image *i=g_intel_pending_head;i;i=i->pending_next)
            if(i->offer_state==1 && brook_gpu_window_identity_equal(&i->offer_identity,&packet.identity)){match=i;break;}
        if(!match){
            struct gpu_group *g=gpu_group_find(&packet.identity);
            if(!g){gpu_window_channel_lost("unknown-safe");return;}
            ++g_gpu_group_safe;gpu_group_release(g);continue;
        }
        ++g_gpu_window_acks;
        image_buffer_trace(match, BUFFER_SAFE, &packet.identity);
        match->offer_identity=(struct BrookGpuWindowIdentity){0};
        gpu_release_if_unheld(match);
        if(getenv("BROOK_WAYLAND_GPU_TIMING")){match->offer_ack_ms=g_now_ms();
            gpu_window_timing_add(&g_offerWait,match->offer_sent_ms,match->offer_ack_ms);}
    }
}
/* Returns 1 delivered, 0 unsupported/not delivered, -1 uncertain delivery. */
static int gpu_window_offer(struct brook_image *image){
    if(!image || !image->source || !image->source->dmabuf || image->producer_fence<0 || !gpu_window_channel_connect()){++g_gpu_offer_no_channel;return 0;}
    struct brook_surface *root=NULL;
    for(struct brook_surface *s=g_surfaces;s;s=s->next){struct brook_view *view=s->tree.current?s->tree.current->value:NULL;
        if(view && view->image==image && !s->tree.parent){root=s;break;}}
    /* A subsurface's buffer belongs to its tree's root window (layered offers). */
    if(!root && gpu_layers_enabled())for(struct brook_surface *s=g_surfaces;s && !root;s=s->next){
        struct brook_view *view=s->tree.current?s->tree.current->value:NULL;
        if(!view || view->image!=image || !s->tree.parent)continue;
        root=s;while(root->tree.parent)root=root->tree.parent->user;}
    if(!gpu_offer_root_ready(root)){++g_gpu_offer_no_root;return 0;}
    struct brook_draw draws[64];unsigned count=0;
    if(!surface_layers(root,draws,&count)){++g_gpu_offer_layers;return 0;}
    if(count>=2){
        int layered=0;for(unsigned i=0;i<count;++i)layered|=draws[i].view->image==image;
        const int sent=layered?gpu_window_offer_layers(root,draws,count):0;
        if(sent==1)return 1;
        if(sent<0)return -1;
        if(sent==2)return -2;
        ++g_gpu_offer_layers;return 0;
    }
    if(draws[0].view->image!=image){++g_gpu_offer_layers;return 0;}
    const struct brook_view *view=draws[0].view;const struct brook_geometry *g=&view->geometry;
    const struct brook_dmabuf_buffer *db=image->source->dmabuf;
    const uint32_t scale=surface_output_scale(root);
    /* exact: the buffer is the VFB, pixel for pixel. scaled: any other
     * whole-pixel source rectangle drawn to the surface (a client that is
     * not scale-aware on a scaled output, such as SDL's 640x480 yquake2 at
     * 200%, or a viewport). It is offered as a one-layer version 3 commit
     * carrying both rectangles, so the renderer samples the buffer directly
     * (and can cull beneath it) instead of this provider composing it into
     * the VFB on the CPU or GPU for every frame. */
    const int exact=!g->sx && !g->sy && g->sw==(int64_t)image->width*256 && g->sh==(int64_t)image->height*256 &&
        logical_to_physical(g->w,scale)==image->width && logical_to_physical(g->h,scale)==image->height;
    const int scaled=!exact && gpu_scaled_offers_enabled() && gpu_layers_enabled() && !((g->sx|g->sy|g->sw|g->sh)&255) &&
        g->sx>=0 && g->sy>=0 && g->sw>0 && g->sh>0 && g->sx+g->sw<=(int64_t)image->width*256 && g->sy+g->sh<=(int64_t)image->height*256 &&
        g->w>0 && g->h>0 && logical_to_physical(g->w,scale)<=16384 && logical_to_physical(g->h,scale)<=16384;
    const uint32_t tileFlags=db->modifier==BROOK_DRM_MOD_I915_X_TILED?BROOK_GPU_WINDOW_LAYER_TILED_X:
        db->modifier==BROOK_DRM_MOD_I915_Y_TILED?BROOK_GPU_WINDOW_LAYER_TILED_Y:0;
    if(g->transform || (!exact && !scaled) || (db->stride&3) || (db->offset&3) || (db->modifier && !tileFlags) ||
       (tileFlags && !gpu_layers_enabled())){++g_gpu_offer_geometry;return 0;}
    /* An X window resized by the native WM waits for a buffer of the new
     * size, exactly as surface_render does for the CPU path. */
    if(root->role==4 && !xwayland_accept_view(root,g->w,g->h)){++g_gpu_xwayland_resize_wait;return 0;}
    if(!surface_window(root,(struct brook_view *)view) || !root->wm_id || !root->vfb){++g_gpu_offer_window;return 0;}
    struct stat st={0};BrookWindowState window={0};
    if(fstat(image->pending_fd,&st) || !st.st_ino || st.st_size<=0 || (st.st_size&3) ||
       syscall(BROOK_SYS_WM_WINDOW_STATE,(long)root->wm_id,0L,(long)&window,(long)sizeof(window),0L,0L) ||
       window.version!=BROOK_WINDOW_STATE_VERSION || window.window!=root->wm_id || !root->tree.current ||
       root->vfb_w!=logical_to_physical(g->w,scale) || root->vfb_h!=logical_to_physical(g->h,scale)){++g_gpu_offer_metadata;return 0;}
    /* The WM client size (window.width/height) may differ from the committed
     * buffer while a resize is in flight or when the client picks its own
     * size. The renderer draws the offer exactly as it draws the VFB, 1:1
     * and cropped to the client rectangle, so that is not a reason to fall
     * back to CPU composition. */
    if(window.width!=root->vfb_w || window.height!=root->vfb_h)++g_gpu_offer_size_mismatch;
    struct BrookGpuWindowPacket offer={.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,
        .version=gpu_window_retain_enabled()?BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN:BROOK_GPU_WINDOW_WIRE_VERSION,
        .type=BROOK_GPU_WINDOW_OFFER,.identity={root->wm_id,0,window.incarnation,root->publish_commit+1,
            (uint64_t)st.st_dev,(uint64_t)st.st_ino},.words=(uint64_t)st.st_size/4,.offset=db->offset/4,
        .stride=db->stride/4,.width=image->width,.height=image->height,.fourcc=db->fourcc};
    int sent;
    const int opaqueArgb=view->opaque && db->fourcc==0x34325241 && gpu_layers_enabled();
    if(opaqueArgb || scaled || tileFlags){
        /* An ARGB buffer the client declared opaque, or a scaled buffer: a
         * one-layer version 3 offer carries the opaque flag (XRGB is opaque
         * by format) and the source and destination rectangles, so the
         * renderer can cull beneath the window instead of blending
         * everything under it. */
        struct BrookGpuWindowLayersPacket one;memset(&one,0,sizeof(one));
        one.header=offer;one.header.version=BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS;one.count=1;
        one.layers[0]=(struct BrookGpuWindowLayer){.words=offer.words,.device=offer.identity.device,.inode=offer.identity.inode,
            .offset=offer.offset,.stride=offer.stride,.width=offer.width,.height=offer.height,.fourcc=offer.fourcc,
            .flags=((view->opaque || db->fourcc==0x34325258)?BROOK_GPU_WINDOW_LAYER_OPAQUE:0)|tileFlags,
            .srcX=(uint32_t)(g->sx/256),.srcY=(uint32_t)(g->sy/256),.srcWidth=(uint32_t)(g->sw/256),.srcHeight=(uint32_t)(g->sh/256),
            .dstWidth=logical_to_physical(g->w,scale),.dstHeight=logical_to_physical(g->h,scale)};
        /* A tiled buffer whose size or pitch breaks the tile rules stays on
         * the CPU path rather than failing the whole channel. */
        if(tileFlags && !brook_gpu_window_layers_valid(&one)){++g_gpu_offer_geometry;return 0;}
        sent=brook_gpu_window_send_layers(g_gpu_window_channel,&one,&image->pending_fd,&image->producer_fence);
        if(sent==1 && opaqueArgb)++g_gpu_opaque_offers;
        if(sent==1 && scaled)++g_gpu_scaled_offers;
    }else sent=brook_gpu_window_send_offer(g_gpu_window_channel,&offer,image->pending_fd,image->producer_fence);
    if(sent==1){const uint64_t now_us=g_now_us();pace_add(root->role==4,PACE_OFFER,image->pace_commit_us,now_us);image->pace_offer_us=now_us;}
    if(sent==1 && root->role==4){
        ++g_gpu_xwayland_offers;
        if(xwayland_early_frames() && now_us_since(root->pace_callback_us)>=XWAYLAND_FRAME_MIN_US && draws[0].view==root->tree.current->value){
            ++g_pace_early_callbacks;surface_send_frame_callbacks(root,g_now_ms());}
    }
    if(sent==1){++root->publish_commit;image->offer_tree_commit=root->tree.current->commit;
        image->offer_identity=offer.identity;image->offer_state=1;++g_gpu_window_offers;++g_layer_offer_frames;
        image->buffer_trace_offer_ack = 0;
        image_buffer_trace(image, BUFFER_OFFER, &offer.identity);
        image->offer_geometry=*g;image->offer_ms=g_now_ms();if(!image->offer_ms)image->offer_ms=1;
        if(getenv("BROOK_WAYLAND_GPU_TIMING"))image->offer_sent_ms=g_now_ms();
        return 1;}
    if(sent==BROOK_GPU_WINDOW_SEND_REJECTED){++g_gpu_offer_local_rejected;return 0;}
    if(sent<0){gpu_image_mark_uncertain(image);gpu_window_channel_lost("send-error");return -1;}
    return 0;
}
/* ---- Version 3 layered window offers ----------------------------------
 * A window of 2..8 layers (a video subsurface over a page, a delegated
 * compositor's overlays) is offered as one commit of several buffers instead
 * of being read back and flattened on the CPU. DMA-BUF layers are offered
 * from the Intel queue (their prime fd and producer fence); CPU-pixel layers
 * (SHM, or a snapshot taken earlier) are copied once into a GEM buffer that
 * lives with the image. An image may be held by several groups (an
 * unchanged layer appears in consecutive commits); it is released only when
 * every group holding it has been acknowledged (SAFE). */
struct gpu_group {int live;struct BrookGpuWindowIdentity identity;uint64_t tree_commit;uint32_t sent_ms;int presented;
    unsigned count;struct brook_image *images[BROOK_GPU_WINDOW_LAYERS_MAX];uint32_t producer_layers;};
enum {GPU_GROUPS=48};
static struct gpu_group g_gpu_groups[GPU_GROUPS];
static int gpu_layers_enabled(void){
    static int enabled=-1;
    if(enabled<0){const char *e=getenv("BROOK_WAYLAND_GPU_LAYERS");enabled=!e || strcmp(e,"0");}
    return enabled && gpu_window_retain_enabled();
}
static void image_gem_free(struct brook_image *image){
    if(!image || !image->gem_handle)return;
    if(image->gem_fence>=0)close(image->gem_fence);
    if(image->gem_fd>=0)close(image->gem_fd);
    struct {uint32_t handle,pad;} gclose={image->gem_handle,0};(void)ioctl(g_drm_fd,DRM_IOC(0x09,sizeof(gclose)),&gclose);
    image->gem_handle=0;image->gem_fd=image->gem_fence=-1;image->gem_words=0;
}
/* One GEM copy of an image's CPU pixels (straight from the snapshot: SHM
 * ARGB is premultiplied, XRGB snapshots carry alpha 255). 1 ready, 0 no. */
static int image_gem_upload(struct brook_image *image){
    if(image->gem_handle)return image->gem_fd>=0 && image->gem_fence>=0;
    if(!image->pixels || g_drm_fd<0 || !g_drm_intel)return 0;
    const uint64_t bytes=((uint64_t)image->width*image->height*4+4095)&~UINT64_C(4095);
    struct {uint64_t size;uint32_t handle,pad;} create={bytes,0,0};
    if(ioctl(g_drm_fd,DRM_IOC(0x5b,sizeof(create)),&create) || !create.handle)return 0;
    image->gem_handle=create.handle;image->gem_fd=image->gem_fence=-1;image->gem_words=create.size/4;
    struct {uint32_t handle,pad;uint64_t offset,flags,extensions;} map={create.handle,0,0,2,0};
    void *pixels=MAP_FAILED;
    if(!ioctl(g_drm_fd,DRM_IOC(0x64,sizeof(map)),&map))
        pixels=mmap(NULL,(size_t)create.size,PROT_READ|PROT_WRITE,MAP_SHARED,g_drm_fd,(off_t)map.offset);
    if(pixels==MAP_FAILED){image_gem_free(image);return 0;}
    memcpy(pixels,image->pixels,(size_t)image->width*image->height*4);munmap(pixels,(size_t)create.size);
    struct drm_prime_handle prime={create.handle,O_CLOEXEC|O_RDWR,-1};
    if(ioctl(g_drm_fd,DRM_IOC(0x2d,sizeof(prime)),&prime) || prime.fd<0){image_gem_free(image);return 0;}
    image->gem_fd=prime.fd;
    /* Never written by the GPU: the kernel returns an already signalled fence. */
    errno=0;long fence=syscall(BROOK_SYS_GPU_BUFFER_SNAPSHOT_FENCE,(long)prime.fd,0L,0L,0L,0L,0L);
    if(fence<0){image_gem_free(image);return 0;}
    image->gem_fence=(int)fence;++g_gpu_group_uploads;return 1;
}
static void gpu_group_presented(struct gpu_group *g){
    if(g->presented)return;
    g->presented=1;++g_gpu_group_presented;++g_gpu_window_presented;
    const uint32_t now=g_now_ms();
    gpu_window_timing_add(&g_presentWait,g->sent_ms,now);
    /* Every layer of the commit is on screen: callbacks of every surface in
     * the window (subsurfaces included) are paced like a single offer. */
    for(unsigned i=0;i<g->count;++i){
        g->images[i]->gpu_presented=1;
        if (g->producer_layers & (1u << i))
            image_buffer_trace(g->images[i], BUFFER_ACK, &g->identity);
    }
    for(struct brook_surface *s=g_surfaces;s;s=s->next){
        struct brook_surface *root=s;while(root->subsurface_parent)root=root->subsurface_parent;
        if(root->wm_id!=g->identity.window)continue;
        struct brook_draw draws[64];unsigned n=0;int pending=!surface_layers(root,draws,&n);
        for(unsigned k=0;!pending && k<n;++k)if(!image_callbacks_due(draws[k].view->image,now))pending=1;
        if(!pending)surface_send_frame_callbacks(s,now);
    }
}
static void gpu_group_release(struct gpu_group *g){
    for(unsigned i=0;i<g->count;++i){struct brook_image *image=g->images[i];
        /* Also an image whose pixels were taken while the group held it
         * (pending_cpu 0, fd still open): it waits in the queue at state 1,
         * and only this SAFE can retire it and release the client's buffer. */
        if (g->producer_layers & (1u << i)) image_buffer_trace(image, BUFFER_SAFE, &g->identity);
        if(image->group_refs) --image->group_refs;
        gpu_release_if_unheld(image);
        image_unref(image);}
    *g=(struct gpu_group){0};
}
static struct gpu_group *gpu_group_find(const struct BrookGpuWindowIdentity *identity){
    for(unsigned i=0;i<GPU_GROUPS;++i)if(g_gpu_groups[i].live && brook_gpu_window_identity_equal(&g_gpu_groups[i].identity,identity))return &g_gpu_groups[i];
    return NULL;
}
/* Channel loss: images in the queue are already marked uncertain (state 3)
 * by gpu_window_channel_lost; drop the group references. */
static void gpu_groups_lost(void){
    for(unsigned i=0;i<GPU_GROUPS;++i)if(g_gpu_groups[i].live){struct gpu_group *g=&g_gpu_groups[i];
        for(unsigned k=0;k<g->count;++k){if(g->images[k]->group_refs)--g->images[k]->group_refs;image_unref(g->images[k]);}
        *g=(struct gpu_group){0};}
}
/* Returns 1 sent, 0 not offerable (use the CPU path), 2 wait (a DMA-BUF
 * layer's producer fence is not known yet), -1 uncertain delivery. */
static int gpu_window_offer_layers(struct brook_surface *root,const struct brook_draw *draws,unsigned count){
    if(!gpu_layers_enabled() || count<2 || count>BROOK_GPU_WINDOW_LAYERS_MAX || !gpu_window_channel_connect())return 0;
    if(!gpu_offer_root_ready(root) || !root->wm_id || !root->vfb || !root->tree.current)return 0;
    const uint32_t scale=surface_output_scale(root);
    struct BrookGpuWindowLayersPacket packet;memset(&packet,0,sizeof(packet));
    int buffers[BROOK_GPU_WINDOW_LAYERS_MAX],fences[BROOK_GPU_WINDOW_LAYERS_MAX];
    for(unsigned i=0;i<count;++i){
        const struct brook_view *view=draws[i].view;struct brook_image *image=view->image;const struct brook_geometry *g=&view->geometry;
        if(g->transform || ((g->sx|g->sy|g->sw|g->sh)&255) || !g->sw || !g->sh ||
           logical_to_physical(g->w,scale)>16384 || logical_to_physical(g->h,scale)>16384 ||
           logical_to_physical(draws[i].x,scale)<=-16384 || logical_to_physical(draws[i].x,scale)>=16384 ||
           logical_to_physical(draws[i].y,scale)<=-16384 || logical_to_physical(draws[i].y,scale)>=16384){++g_gpu_group_skip_shape;return 0;}
        struct BrookGpuWindowLayer *l=&packet.layers[i];
        struct stat st={0};
        if(image->pending_cpu==1){
            const struct brook_dmabuf_buffer *db=image->source && image->source->dmabuf?image->source->dmabuf:NULL;
            const uint32_t tileFlags=!db?0:db->modifier==BROOK_DRM_MOD_I915_X_TILED?BROOK_GPU_WINDOW_LAYER_TILED_X:
                db->modifier==BROOK_DRM_MOD_I915_Y_TILED?BROOK_GPU_WINDOW_LAYER_TILED_Y:0;
            if(!db || image->pending_fd<0 || image->uncertain || (db->modifier && !tileFlags) || (db->stride&3) || (db->offset&3) ||
               image->offer_state>=2){++g_gpu_group_skip_buffer;return 0;}
            if(image->producer_fence<0){++g_gpu_group_wait;return 2;}
            if(fstat(image->pending_fd,&st) || !st.st_ino || st.st_size<=0 || (st.st_size&3)){++g_gpu_group_skip_buffer;return 0;}
            *l=(struct BrookGpuWindowLayer){.words=(uint64_t)st.st_size/4,.device=(uint64_t)st.st_dev,.inode=(uint64_t)st.st_ino,
                .offset=db->offset/4,.stride=db->stride/4,.width=image->width,.height=image->height,.fourcc=db->fourcc,
                .flags=(view->opaque?BROOK_GPU_WINDOW_LAYER_OPAQUE:0)|tileFlags};
            if(tileFlags){struct BrookGpuWindowLayer probe=*l;probe.srcX=g->sx/256;probe.srcY=g->sy/256;probe.srcWidth=g->sw/256;
                probe.srcHeight=g->sh/256;probe.dstWidth=logical_to_physical(g->w,scale);probe.dstHeight=logical_to_physical(g->h,scale);
                if(!brook_gpu_window_layer_valid(&probe)){++g_gpu_group_skip_buffer;return 0;}}
            buffers[i]=image->pending_fd;fences[i]=image->producer_fence;
        }else if(image->pixels && !image->pending_cpu){
            if(!image_gem_upload(image)){++g_gpu_group_skip_upload;return 0;}
            if(fstat(image->gem_fd,&st) || !st.st_ino){++g_gpu_group_skip_upload;return 0;}
            *l=(struct BrookGpuWindowLayer){.words=(uint64_t)st.st_size/4,.device=(uint64_t)st.st_dev,.inode=(uint64_t)st.st_ino,
                .stride=image->width,.width=image->width,.height=image->height,.fourcc=image->format==2?0x34325258u:0x34325241u,
                .flags=(image->opaque || view->opaque)?BROOK_GPU_WINDOW_LAYER_OPAQUE:0};
            buffers[i]=image->gem_fd;fences[i]=image->gem_fence;
        }else{++g_gpu_group_skip_buffer;return 0;}
        /* Source rectangle: Wayland 24.8 in transformed buffer units; with no
         * transform these are buffer pixels. */
        l->srcX=g->sx/256;l->srcY=g->sy/256;l->srcWidth=g->sw/256;l->srcHeight=g->sh/256;
        l->dstX=(int32_t)(logical_to_physical(draws[i].x,scale));l->dstY=(int32_t)(logical_to_physical(draws[i].y,scale));
        l->dstWidth=logical_to_physical(draws[i].x+g->w,scale)-l->dstX;
        l->dstHeight=logical_to_physical(draws[i].y+g->h,scale)-l->dstY;
    }
    /* The root's committed size is the window's size, exactly as for a
     * single offer. Without this a window that resized while it had a
     * subsurface (Chromium's video or ad overlays) kept its old VFB, and so
     * its old kernel size: the renderer clipped every layer to it, and the
     * WM, captions and later configures used the stale size. */
    if(!root->tree.current->value || !surface_window(root,(struct brook_view *)root->tree.current->value) ||
       !root->wm_id || !root->vfb){++g_gpu_offer_window;return 0;}
    BrookWindowState window={0};
    if(syscall(BROOK_SYS_WM_WINDOW_STATE,(long)root->wm_id,0L,(long)&window,(long)sizeof(window),0L,0L) ||
       window.version!=BROOK_WINDOW_STATE_VERSION || window.window!=root->wm_id){++g_gpu_offer_metadata;return 0;}
    unsigned slot=0;while(slot<GPU_GROUPS && g_gpu_groups[slot].live)++slot;
    if(slot==GPU_GROUPS){++g_gpu_group_full;return 0;}
    const struct BrookGpuWindowLayer *l0=&packet.layers[0];
    packet.header=(struct BrookGpuWindowPacket){.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS,
        .type=BROOK_GPU_WINDOW_OFFER,.identity={root->wm_id,0,window.incarnation,++root->publish_commit,l0->device,l0->inode},
        .words=l0->words,.offset=l0->offset,.stride=l0->stride,.width=l0->width,.height=l0->height,.fourcc=l0->fourcc};
    packet.count=count;
    int sent=brook_gpu_window_send_layers(g_gpu_window_channel,&packet,buffers,fences);
    if(sent==BROOK_GPU_WINDOW_SEND_REJECTED){++g_gpu_offer_local_rejected;return 0;}
    if(sent<0){
        // Every producer DMA-BUF in the packet could still be read by the peer.
        // CPU uploads use their own GEM copies and do not hold producer storage.
        for(unsigned i=0;i<count;++i){
            struct brook_image *image=draws[i].view->image;
            if(image->pending_cpu==1)gpu_image_mark_uncertain(image);
        }
        gpu_window_channel_lost("send-error");return -1;
    }
    if(!sent)return 2;
    struct gpu_group *group=&g_gpu_groups[slot];
    *group=(struct gpu_group){.live=1,.identity=packet.header.identity,.tree_commit=root->tree.current->commit,
                              .sent_ms=g_now_ms(),.count=count};
    for(unsigned i=0;i<count;++i){struct brook_image *image=draws[i].view->image;
        image_ref(image);group->images[i]=image;++image->group_refs;
        if (image->pending_cpu == 1) {
            group->producer_layers |= 1u << i;
            image_buffer_trace(image, BUFFER_OFFER, &group->identity);
        }
        if(image->pending_cpu==1){image->offer_state=1;image->gpu_presented=0;image->offer_ms=g_now_ms();if(!image->offer_ms)image->offer_ms=1;}}
    ++g_gpu_group_offers;++g_layer_offer_frames;
    return 1;
}
static struct brook_image *intel_dmabuf_defer(struct brook_buffer *buffer){
    if(!buffer || !buffer->dmabuf || g_intel_pending_count>=INTEL_PENDING_LIMIT)return NULL;
    const struct brook_dmabuf_buffer *db=buffer->dmabuf;
    struct drm_prime_handle prime={db->bo_handle,O_CLOEXEC|O_RDWR,-1};
    if(ioctl(g_drm_fd,DRM_IOC(0x2d,sizeof(prime)),&prime) || prime.fd<0)return NULL;
    struct brook_image *image=calloc(1,sizeof(*image));if(!image){close(prime.fd);return NULL;}
    image->refs=1;image->width=db->width;image->height=db->height;image->format=db->fourcc==0x34325258?2:1;
    image->source=buffer;++buffer->refs;buffer_begin_use(buffer);
    image->pending_cpu=1;image->pending_fd=prime.fd;image->producer_fence=-1;
    image->queued_ms=g_now_ms();if(!image->queued_ms)image->queued_ms=1;
    if(getenv("BROOK_WAYLAND_GPU_TIMING"))image->trace_queued_ms=g_now_ms();
    const char *fenced=getenv("BROOK_WAYLAND_SNAPSHOT_FENCE");
    image->producer_fence_required=fenced && !strcmp(fenced,"1");
    image_ref(image);intel_pending_append(image);++g_intel_queued;
    intel_pending_schedule();
    return image;
}
/* Returns 1 when the producer fence has signalled successfully, 0 when it is
 * still pending, and -1 when completion is failed or unknown. */
static int intel_producer_fence_ready(const struct brook_image *image){
    struct pollfd p={.fd=image->producer_fence,.events=POLLIN};
    int polled=poll(&p,1,0);
    if(!polled || (polled<0 && errno==EINTR))return 0;
    if(polled<0 || !(p.revents&POLLIN) || (p.revents&(POLLERR|POLLHUP|POLLNVAL)))return -1;
    struct {char name[32];int32_t status;uint32_t flags,num_fences,pad;uint64_t fences;} info={0};
    if(ioctl(image->producer_fence,0xc0383e04u,&info) || info.status!=1)return -1;
    return 1;
}
/* The renderer still retains this offered source, but the window needs CPU
 * pixels (a new subsurface, a changed viewport). Readers may share the
 * buffer; the producer stays excluded because the buffer is not released.
 * Returns 0 on success or permanent failure, EAGAIN to retry later. */
static int intel_snapshot_retained(struct brook_image *image){
    if(image->producer_fence>=0){
        int ready=intel_producer_fence_ready(image);
        if(!ready)return EAGAIN;
        if(ready<0){image->want_pixels=0;return 0;} /* Release path reports it. */
        image_trace_producer_ready(image);
    }
    errno=0;long result=syscall(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ,(long)image->pending_fd,0L,0L,0L,0L,0L);
    if(result<0 && (errno==EAGAIN || errno==EINTR)){++g_intel_busy;return EAGAIN;}
    image->want_pixels=0;
    if(result)return 0;
    struct brook_image *copy=intel_dmabuf_copy(image->source->dmabuf);if(!copy)return 0;
    image->pixels=copy->pixels;copy->pixels=NULL;image_unref(copy);image->pending_cpu=0;++g_intel_retained_snapshots;
    image_buffer_trace(image, BUFFER_CPU_COPY, NULL);
    for(struct brook_surface *surface=g_surfaces;surface;surface=surface->next){
        struct brook_view *view=surface->tree.current?surface->tree.current->value:NULL;
        if(!view || view->image!=image)continue;
        surface->image=image->pixels;
        if(surface->is_cursor)surface_cursor_commit(surface);else surface_repaint(surface);
    }
    return 0;
}
static void intel_pending_poll(void){
    gpu_window_channel_drain();
    g_intel_retry_ms=0;
    unsigned budget=g_intel_pending_count;
    while(budget-- && g_intel_pending_head){
        struct brook_image *image=g_intel_pending_head;g_intel_pending_head=image->pending_next;
        if(!g_intel_pending_head)g_intel_pending_tail=NULL;
        --g_intel_pending_count;image->pending_next=NULL;
        if(image->trace_queued_ms && !image->trace_first_poll_ms)image->trace_first_poll_ms=g_now_ms();
        int fence_error=0;
        if(image->producer_fence_required){
            if(image->producer_fence<0){
                errno=0;long fd=syscall(BROOK_SYS_GPU_BUFFER_SNAPSHOT_FENCE,(long)image->pending_fd,0L,0L,0L,0L,0L);
                if(fd<0 && (errno==EAGAIN || errno==EINTR)){++g_intel_fence_pending;intel_retry_after(INTEL_RETRY_MS);intel_pending_append(image);continue;}
                if(fd<0)fence_error=errno?errno:EIO;
                else {image->producer_fence=(int)fd;++g_intel_fences;}
            }
            if(!fence_error && image->producer_fence>=0){
                if(image->offer_state==3){
                    errno=0;long recovered=g_gpu_window_dead<0?-1:
                        syscall(BROOK_SYS_GPU_WINDOW_PEER_RECOVERY,(long)g_gpu_window_dead,(long)BROOK_GPU_WINDOW_PEER_DRAIN,
                                (long)image->pending_fd,0L,0L,0L);
                    if(recovered<0 && (errno==EAGAIN || errno==EINTR)){
                        ++g_gpu_drain_waits;gpu_window_maybe_restart();
                        intel_retry_after(INTEL_RETRY_DRAIN_MS);intel_pending_append(image);continue;
                    }
                    if(recovered<0)fence_error=EIO;
                    else {
                        image->offer_identity=(struct BrookGpuWindowIdentity){0};
                        image->offer_state=2;++g_gpu_window_recovered;
                        image_buffer_trace(image, BUFFER_DRAIN, NULL);
                    }
                }
                else if(image->offer_state==1){
                    /* Held by the renderer: SAFE or PRESENTED arrive on the
                     * channel. Only an explicit CPU snapshot request polls. */
                    if(image->want_pixels && intel_snapshot_retained(image)==EAGAIN)intel_retry_after(INTEL_RETRY_MS);
                    intel_pending_append(image);continue;
                }
                else if(!image->offer_state){
                    int offered=gpu_window_offer(image);
                    if(offered==-2){
                        /* Layered window: another layer's producer fence is
                         * not known yet. Wait briefly, then use the CPU path. */
                        const uint32_t now=g_now_ms();
                        if(!image->group_wait_ms)image->group_wait_ms=now?now:1;
                        if(now-image->group_wait_ms<50){intel_retry_after(INTEL_RETRY_MS);intel_pending_append(image);continue;}
                        image->offer_state=4;
                    }
                    else if(offered<0){if(!image->offer_state)gpu_image_mark_uncertain(image);intel_pending_append(image);continue;}
                    else if(offered>0){intel_pending_append(image);continue;}
                    else image->offer_state=4; /* fallback, no fd transferred */
                }
            }
#ifdef BROOK_WAYLAND_VULKAN_COMPOSITOR
            if(!fence_error && image->producer_fence>=0 && !image->direct_attempted){
                image->direct_attempted=1;
                for(struct brook_surface *s=g_surfaces;s;s=s->next){
                    struct brook_view *view=s->tree.current?s->tree.current->value:NULL;
                    if(!view || view->image!=image || s->tree.parent)continue;
                    int direct=surface_vulkan_direct(s,image,image->pending_fd,image->producer_fence);
                    if(direct<0)fence_error=EIO;else if(direct>0)++g_intel_direct;
                    break;
                }
            }
#endif
            if(!fence_error){
                int ready=intel_producer_fence_ready(image);
                if(!ready){++g_intel_fence_pending;intel_retry_after(INTEL_RETRY_MS);intel_pending_append(image);continue;}
                if(ready<0)fence_error=EIO;
            }
        }
        if(fence_error){
            image_buffer_trace(image, BUFFER_UNCERTAIN, NULL);
            ++g_intel_failed;image->uncertain=1;image->source->uncertain=1;
            fprintf(stderr,"[waylandd] Intel producer fence failed: errno=%d; release withheld\n",fence_error);
            if(image->source->resource)wl_client_post_implementation_error(wl_resource_get_client(image->source->resource),"GPU producer completion could not be verified");
            if(image->producer_fence>=0)close(image->producer_fence);
            close(image->pending_fd);image->pending_fd=-1;image->pending_cpu=2;image_unref(image);continue;
        }
        if(image->trace_queued_ms && !image->trace_fence_ms)image->trace_fence_ms=g_now_ms();
        if (image->producer_fence_required) image_trace_producer_ready(image);
        /* A successfully fenced image held only by this queue has no view
         * left to paint. The offer path above has also retired its readers.
         * With retained offers this is the normal release: the renderer gave
         * the source back after a newer commit replaced it. No CPU copy. */
        if(image->producer_fence_required && image->refs==1){
            ++g_intel_discarded;
            close(image->producer_fence);
            close(image->pending_fd);
            image_unref(image);
            continue;
        }
        /* Pixels taken while the renderer retained the source need no
         * second copy; the window was repainted when they arrived. */
        const int had_pixels=image->pixels!=NULL;
        uint32_t trace_try_start=image->trace_queued_ms?g_now_ms():0,trace_try_done=trace_try_start;
        int uncertain=0,ready=had_pixels;
        if(!had_pixels){
            errno=0;long result=syscall(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ,(long)image->pending_fd,0L,0L,0L,0L,0L);
            trace_try_done=image->trace_queued_ms?g_now_ms():0;
            int error=result<0?errno:0;
            if(result<0 && (error==EAGAIN || error==EINTR)){++g_intel_busy;intel_retry_after(INTEL_RETRY_MS);intel_pending_append(image);continue;}
            struct brook_image *copy=NULL;
            if(!result)copy=intel_dmabuf_copy(image->source->dmabuf);
            else {
                /* Never fall back to a blocking wait in the event loop.  Without
                 * a successful transition the producer's completion is unknown. */
                uncertain=1;
                fprintf(stderr,"[waylandd] Intel buffer readiness failed: errno=%d; release withheld\n",error);
            }
            ready=copy!=NULL;
            if(ready){
                image->pixels=copy->pixels;copy->pixels=NULL;image_unref(copy);
                image_buffer_trace(image, BUFFER_CPU_COPY, NULL);
            }
        }
        uint32_t trace_copy_done=image->trace_queued_ms?g_now_ms():0;
        if(!ready){
            if (uncertain) image_buffer_trace(image, BUFFER_UNCERTAIN, NULL);
            ++g_intel_failed;
            image->uncertain=1;if(uncertain)image->source->uncertain=1;
            if(image->source->resource){struct wl_client *client=wl_resource_get_client(image->source->resource);
                if(uncertain)wl_client_post_implementation_error(client,"GPU buffer completion could not be verified");
                else wl_client_post_no_memory(client);}
            if(!uncertain){buffer_release(image->source);buffer_drop(image->source);image->source=NULL;}
        }else{
            ++g_intel_ready;
            buffer_release(image->source);buffer_drop(image->source);image->source=NULL;
        }
        if(image->producer_fence>=0)close(image->producer_fence);
        close(image->pending_fd);image->pending_fd=-1;image->pending_cpu=ready?0:2;
        if(ready && !had_pixels)for(struct brook_surface *surface=g_surfaces;surface;surface=surface->next){
            struct brook_view *view=surface->tree.current?surface->tree.current->value:NULL;
            if(!view || view->image!=image)continue;
            surface->image=image->pixels;
            if(surface->is_cursor)surface_cursor_commit(surface);else {
                surface_repaint(surface);
                if(image->offer_ack_ms && !surface->tree.parent){struct brook_draw draws[64];unsigned count=0;
                    if(surface_layers(surface,draws,&count) && count==1 && draws[0].view->image==image)
                        gpu_window_timing_add(&g_snapshotWait,image->offer_ack_ms,g_now_ms());}
            }
        }
        if(image->trace_queued_ms){
            uint32_t trace_done=g_now_ms();
            fprintf(stderr,"[wayland-gpu-stage] width=%u height=%u queued=%u first_poll=%u fence=%u offer=%u ack=%u try_start=%u try_done=%u copy_done=%u done=%u offer_state=%d ready=%d window=%u incarnation=%llu commit=%llu\n",
                image->width,image->height,image->trace_queued_ms,image->trace_first_poll_ms,image->trace_fence_ms,
                image->offer_sent_ms,image->offer_ack_ms,trace_try_start,trace_try_done,trace_copy_done,trace_done,image->offer_state,ready,
                image->offer_identity.window,(unsigned long long)image->offer_identity.incarnation,
                (unsigned long long)image->offer_identity.commit);
        }
        image_unref(image);
    }
    gpu_window_reap_dead();
    intel_retry_arm();
}
/* At exit, withdraw windows whose content the renderer still retains: the
 * renderer then returns those sources and the drain below can complete. */
static void gpu_window_unmap_retained(void){
    for(struct brook_surface *s=g_surfaces;s;s=s->next){
        const struct brook_view *view=s->tree.current?s->tree.current->value:NULL;
        if(s->wm_id && !s->tree.parent && view && view->image->offer_state==1)surface_unmap(s);
    }
}
static unsigned shutdown_pending_count(void *unused){(void)unused;return g_intel_pending_count;}
static void shutdown_pending_poll(void *unused){(void)unused;intel_pending_poll();wl_display_flush_clients(g_display);}
static void shutdown_pending_wait(void *unused){(void)unused;usleep(10000);}

static void dmabuf_buffer_unref(struct brook_dmabuf_buffer *db)
{
    if (db && !--db->refs) {
        if (db->bo_handle) dmabuf_release_bo(db->bo_handle);
        if (db->nv_bo) nv_dmabuf_release(db->nv_bo);
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

static int dmabuf_tiled_for(struct wl_client *client);
static uint64_t g_dmabuf_tiled_created;
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
    /* Tiled layouts are checked as linear plus the tile pitch/offset rules. */
    const int tiling=brook_tiling_from_modifier(p->modifier);
    const int tiled=tiling>0 && dmabuf_tiled_for(c);
    const struct BrookDmabufLayout layout={(uint32_t)width,(uint32_t)height,format,p->stride,p->offset,flags,tiled?0:p->modifier};
    enum BrookDmabufLayoutError error=BrookDmabufValidate(&layout,UINT64_MAX,1);
    if (error==BROOK_DMABUF_LAYOUT_OK && tiled && ((p->stride&(tiling==BROOK_TILING_X?511u:127u)) || (p->offset&4095u)))
        error=BROOK_DMABUF_LAYOUT_BOUNDS;
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
    if (tiled) ++g_dmabuf_tiled_created;
    struct stat storage;
    if (fstat(p->fd, &storage) == 0) {
        db->trace_device = storage.st_dev;
        db->trace_inode = storage.st_ino;
    }
    if (g_nv_fd < 0) nv_dmabuf_open();
    if (g_nv_fd >= 0) db->gres = nv_dmabuf_import(p->fd, db) ? 1u : 0u;   /* no gres: a CPU-read nv-drm BO */
    else db->gres = dmabuf_resolve_gres(p->fd, &db->bo_handle);
    close(p->fd); p->fd = -1;   /* the import cache now holds the GEM reference */
    if (!db->gres) {
        free(db);
        if (id) wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                                       "dmabuf import failed");
        return NULL;
    }
    ++g_pace_dmabuf_created[c==g_xwayland_client];
    fprintf(stderr, "[waylandd] dmabuf buffer %dx%d stride=%u fourcc=0x%x modifier=0x%llx -> gres=%u\n",
            width, height, db->stride, format, (unsigned long long)db->modifier, db->gres);

    struct wl_resource *buf = wl_resource_create(c, &wl_buffer_interface, 1, id);
    if (!buf) {
        if (db->bo_handle) dmabuf_release_bo(db->bo_handle);
        if (db->nv_bo) nv_dmabuf_release(db->nv_bo);
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
/* Intel X/Y-tiled buffers. Broadwell renders several times faster into a
 * tiled colour buffer than into a LINEAR one (host Gen9.5, 8 blended quads at
 * 3192x1632: LINEAR 48.6 ms, X 15.3 ms, Y 14.5 ms). The renderer's quad pass
 * samples them (layer flags TILED_X/TILED_Y) and the CPU fallback detiles.
 * Offered only with the Intel GPU channel. BROOK_WAYLAND_TILED: "xwayland"
 * (default: the Xwayland client, hence glamor and DRI3 games), "all", "0". */
static int dmabuf_tiled_for(struct wl_client *client){
    static int mode=-1;
    if(mode<0){const char *v=getenv("BROOK_WAYLAND_TILED");
        mode=!v || !strcmp(v,"xwayland")?1:!strcmp(v,"all")?2:0;}
    if(!mode || !g_drm_intel || !getenv("BROOK_WAYLAND_GPU_SOCKET"))return 0;
    return mode==2 || (client && client==g_xwayland_client);
}
static void dmabuf_send_feedback(struct wl_resource *fb)
{
    /* Format table: 16-byte entries { u32 format, u32 pad, u64 modifier }. */
    struct fmt_entry { uint32_t format; uint32_t pad; uint64_t modifier; };
    struct fmt_entry table[2 + 4 + 2 * BROOK_NV_MOD_COUNT];
    uint32_t n = 0;
    table[n++] = (struct fmt_entry){ FMT_XRGB8888, 0, 0 /* LINEAR */ };
    table[n++] = (struct fmt_entry){ FMT_ARGB8888, 0, 0 /* LINEAR */ };
    if (dmabuf_tiled_for(wl_resource_get_client(fb))) {
        table[n++] = (struct fmt_entry){ FMT_XRGB8888, 0, BROOK_DRM_MOD_I915_Y_TILED };
        table[n++] = (struct fmt_entry){ FMT_ARGB8888, 0, BROOK_DRM_MOD_I915_Y_TILED };
        table[n++] = (struct fmt_entry){ FMT_XRGB8888, 0, BROOK_DRM_MOD_I915_X_TILED };
        table[n++] = (struct fmt_entry){ FMT_ARGB8888, 0, BROOK_DRM_MOD_I915_X_TILED };
    }
    if (nv_dmabuf_block_linear_advertised())
        for (unsigned i = 0; i < BROOK_NV_MOD_COUNT; ++i) {
            table[n++] = (struct fmt_entry){ FMT_XRGB8888, 0, brook_nv_turing_modifier(i) };
            table[n++] = (struct fmt_entry){ FMT_ARGB8888, 0, brook_nv_turing_modifier(i) };
        }
    const size_t table_bytes = n * sizeof(table[0]);

    int tfd = syscall(SYS_memfd_create, "brook-dmabuf-fmt", 0u);
    if (tfd >= 0) {
        if (write(tfd, table, table_bytes) == (ssize_t)table_bytes) {
            zwp_linux_dmabuf_feedback_v1_send_format_table(fb, tfd, (uint32_t)table_bytes);
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
    nv_dmabuf_open();   /* late renderD130 (no-op once open or when not wanted) */
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
            if (dmabuf_tiled_for(client)) {
                static const uint64_t mods[2]={BROOK_DRM_MOD_I915_Y_TILED,BROOK_DRM_MOD_I915_X_TILED};
                for (int i=0;i<2;++i) {
                    zwp_linux_dmabuf_v1_send_modifier(r, FMT_XRGB8888, (uint32_t)(mods[i]>>32), (uint32_t)mods[i]);
                    zwp_linux_dmabuf_v1_send_modifier(r, FMT_ARGB8888, (uint32_t)(mods[i]>>32), (uint32_t)mods[i]);
                }
            }
        }
    }
}

/* ---------------- main ---------------- */

static void latency_stats_reset(void) {
    ++g_stats_epoch;
    struct client_latency *client;
    wl_list_for_each(client, &g_latency_clients, link)
        client->data = (struct client_latency_data){0};
    for (struct brook_surface *s = g_surfaces; s; s = s->next)
        s->latency = (struct frame_latency){0};
}

static void latency_stats_check_reset(const char *path) {
    static struct stat consumed;
    static int retained_marker;
    struct stat marker;
    if (lstat(path, &marker) != 0) {
        if (errno == ENOENT) retained_marker = 0;
        return;
    }
    if (!S_ISREG(marker.st_mode) || (marker.st_uid != 0 && marker.st_uid != geteuid())) return;

    /* A non-root provider cannot unlink root's marker from sticky /tmp.
     * Acknowledge that version once and treat a later touch as a new request. */
    if (retained_marker && marker.st_dev == consumed.st_dev && marker.st_ino == consumed.st_ino &&
        marker.st_mtim.tv_sec == consumed.st_mtim.tv_sec && marker.st_mtim.tv_nsec == consumed.st_mtim.tv_nsec &&
        marker.st_ctim.tv_sec == consumed.st_ctim.tv_sec && marker.st_ctim.tv_nsec == consumed.st_ctim.tv_nsec)
        return;
    if (unlink(path) == 0) {
        retained_marker = 0;
    } else {
        if (errno == ENOENT) return;
        consumed = marker;
        retained_marker = 1;
    }
    latency_stats_reset();
}

static void latency_hist_json(FILE *f, const struct latency_hist *hist) {
    fprintf(f, "{\"count\":%llu,\"max_us\":%llu,\"buckets\":[",
            (unsigned long long)hist->count, (unsigned long long)hist->max_us);
    for (unsigned i = 0; i < LATENCY_BUCKETS; ++i)
        fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)hist->buckets[i]);
    fputs("]", f);
}

static void latency_frame_json(FILE *f, const struct frame_latency *frame) {
    latency_hist_json(f, &frame->hist);
    const struct frame_latency_event *e = &frame->worst;
    if (!frame->hist.count) {
        fputs(",\"worst\":null}", f);
        return;
    }
    fprintf(f, ",\"worst\":{\"done_us\":%llu,\"commit\":%llu,\"current_commit\":%llu,"
               "\"surface_object\":%u,\"root_wm\":%u,\"frames\":{\"state\":\"%s\","
               "\"pending\":%d,\"waiting\":%d,\"committed\":%d},\"offer_state\":",
            (unsigned long long)e->done_us, (unsigned long long)e->commit, (unsigned long long)e->current_commit,
            e->surface_object, e->root_wm, e->frames_state, e->pending, e->waiting, e->committed);
    if (e->offer_state < 0) fputs("null", f);
    else fprintf(f, "%d", e->offer_state);
    fprintf(f, ",\"renderer_presented\":%s}}",
            e->renderer_presented < 0 ? "null" : e->renderer_presented ? "true" : "false");
}

static void latency_stats_json(FILE *f) {
    fprintf(f, "\"latency\":{\"stats_epoch\":%llu,\"bucket_upper_ms\":[",
            (unsigned long long)g_stats_epoch);
    for (unsigned i = 0; i + 1 < LATENCY_BUCKETS; ++i)
        fprintf(f, "%u,", latency_bounds_ms[i]);
    fprintf(f, "null],\"axis_burst_break_us\":%u,\"kernel_input_timestamp_available\":false,\"clients\":[",
            LATENCY_AXIS_BURST_US);
    int first = 1;
    struct client_latency *client;
    wl_list_for_each(client, &g_latency_clients, link) {
        fprintf(f, "%s{\"pid\":%d,\"connection\":%llu,\"commit_to_callback_done\":",
                first ? "" : ",", (int)client->pid, (unsigned long long)client->connection);
        first = 0;
        latency_frame_json(f, &client->data.commit_to_done);
        fputs(",\"callback_done_to_next_commit\":", f);
        latency_hist_json(f, &client->data.done_to_commit);
        fputs("},\"input_receive_to_queue\":{", f);
        static const char *kinds[] = {"motion", "axis", "button", "key"};
        for (unsigned i = 0; i < LATENCY_INPUT_KINDS; ++i) {
            fprintf(f, "%s\"%s\":", i ? "," : "", kinds[i]);
            latency_hist_json(f, &client->data.input[i]);
            fputc('}', f);
        }
        fprintf(f, "},\"axis_burst_max_gap_us\":%llu}", (unsigned long long)client->data.axis_burst_max_gap_us);
    }
    fputs("],\"toplevels\":[", f);
    first = 1;
    for (struct brook_surface *s = g_surfaces; s; s = s->next) {
        if (!s->resource || latency_toplevel(s) != s) continue;
        struct wl_listener *listener = wl_client_get_destroy_listener(wl_resource_get_client(s->resource), latency_client_destroy);
        if (!listener) continue;
        client = wl_container_of(listener, client, destroy);
        fprintf(f, "%s{\"pid\":%d,\"connection\":%llu,\"object\":%u,\"wm\":%u,\"role\":%d,"
                   "\"commit_to_callback_done\":",
                first ? "" : ",", (int)client->pid, (unsigned long long)client->connection,
                wl_resource_get_id(s->resource), s->wm_id, s->role);
        first = 0;
        latency_frame_json(f, &s->latency);
        fputc('}', f);
    }
    fputs("]}", f);
}

/* Live diagnostics: write $XDG_RUNTIME_DIR (or /tmp)/brook-waylandd-<display>.json
 * atomically, so the bridge can read why windows are composed on the CPU
 * without restarting the provider. */
static void waylandd_status_write(const char *display){
    static uint32_t last;const uint32_t now=g_now_ms();
    /* Leave slack for the normal 16 ms dispatch tick. Idle providers keep
     * the old 2 s publication cadence. Open clients include stalled ones. */
    const uint32_t interval_ms = wl_list_empty(&g_latency_clients) ? 2000 : 200;
    if(last && now-last<interval_ms)return;
    last=now?now:1;
    latency_stats_check_reset("/tmp/brook-waylandd-stats-reset");
    const char *dir=getenv("XDG_RUNTIME_DIR");if(!dir || !*dir)dir="/tmp";
    char path[256],tmp[272];
    if(snprintf(path,sizeof(path),"%s/brook-waylandd-%s.json",dir,display)>=(int)sizeof(path))return;
    buffer_trace_poll(path);
    snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    FILE *f=fopen(tmp,"w");if(!f)return;
    fprintf(f,"{\"schema\":\"brook-waylandd-status/1\",\"pid\":%ld,\"now_ms\":%u,\"gpu_socket\":%d,",
        (long)getpid(),now,getenv("BROOK_WAYLAND_GPU_SOCKET")!=NULL);
    fprintf(f,"\"buffer_trace_active_pid\":%ld,",g_buffer_trace.pid);
    {unsigned inert=0,live=idle_inhibit_count(&inert);
     fprintf(f,"\"idle_inhibit\":{\"inhibitors\":%u,\"inert\":%u,\"created\":%llu,\"destroyed\":%llu,\"failures\":%llu,\"windows\":[",
         live,inert,(unsigned long long)g_idle_inhibit_created,(unsigned long long)g_idle_inhibit_destroyed,(unsigned long long)g_idle_inhibit_failures);
     int first=1;
     for(struct brook_surface *s=g_surfaces;s;s=s->next)if(s->wm_id && s->idle_inhibit_sent){
         pid_t pid=0;if(s->resource)wl_client_get_credentials(wl_resource_get_client(s->resource),&pid,NULL,NULL);
         fprintf(f,"%s{\"wm\":%u,\"pid\":%ld,\"count\":%u,\"app_id\":",first?"":",",s->wm_id,(long)pid,s->idle_inhibit_sent);
         idle_inhibit_json_text(f,s->app_id);fputc('}',f);first=0;}
     fprintf(f,"]},");}
    fprintf(f,"\"gpu_channel\":{\"connected\":%d,\"draining\":%d,\"connections\":%llu,\"losses\":%llu,"
              "\"drain_retries\":%llu,\"locally_rejected\":%llu,\"last_disconnect\":\"%s\","
              "\"last_refusal\":\"%s\",\"last_nonempty_refusal\":\"%s\",\"restart_requested\":%d,\"restart_errno\":%d},",
        g_gpu_window_channel>=0,g_gpu_window_dead>=0,(unsigned long long)g_gpu_channel_connections,
        (unsigned long long)g_gpu_channel_losses,(unsigned long long)g_gpu_drain_waits,
        (unsigned long long)g_gpu_offer_local_rejected,g_gpu_disconnect_reason,g_gpu_connect_refusal,
        g_gpu_connect_last_nonempty_refusal,g_gpu_peer_restart_asked,g_gpu_peer_restart_error);
    fprintf(f,"\"offers\":{\"sent\":%llu,\"safe\":%llu,\"presented\":%llu,\"retained_snapshots\":%llu,\"recovered\":%llu,\"xwayland\":%llu,\"xwayland_enabled\":%d},",
        (unsigned long long)g_gpu_window_offers,(unsigned long long)g_gpu_window_acks,(unsigned long long)g_gpu_window_presented,
        (unsigned long long)g_intel_retained_snapshots,(unsigned long long)g_gpu_window_recovered,
        (unsigned long long)g_gpu_xwayland_offers,gpu_xwayland_offers_enabled());
    {static const char *kinds[PACE_KINDS]={"interval","offer","presented","callback","turnaround"};
     fprintf(f,"\"pacing\":{\"xwayland_frame\":\"%s\",\"early_callbacks\":%llu,\"tiled_created\":%llu,",xwayland_early_frames()?"offer":"presented",
         (unsigned long long)g_pace_early_callbacks,(unsigned long long)g_dmabuf_tiled_created);
     for(int x=0;x<2;++x){fprintf(f,"\"%s\":{\"dmabuf_created\":%llu",x?"x11":"native",(unsigned long long)g_pace_dmabuf_created[x]);
         for(int k=0;k<PACE_KINDS;++k)fprintf(f,",\"%s\":[%llu,%llu,%llu]",kinds[k],(unsigned long long)g_pace[x][k].count,
             (unsigned long long)g_pace[x][k].sum_us,(unsigned long long)g_pace[x][k].max_us);
         fprintf(f,"}%s",x?"},":",");}}
    fprintf(f,"\"offer_skipped\":{\"no_channel_or_fence\":%llu,\"no_root\":%llu,\"layers\":%llu,\"geometry\":%llu,\"window\":%llu,"
        "\"metadata\":%llu,\"connect_fail\":%llu,\"auth_fail\":%llu,\"sent_with_client_size_mismatch\":%llu,\"xwayland_resize_wait\":%llu},",
        (unsigned long long)g_gpu_offer_no_channel,(unsigned long long)g_gpu_offer_no_root,(unsigned long long)g_gpu_offer_layers,
        (unsigned long long)g_gpu_offer_geometry,(unsigned long long)g_gpu_offer_window,(unsigned long long)g_gpu_offer_metadata,
        (unsigned long long)g_gpu_connect_fail,(unsigned long long)g_gpu_auth_fail,(unsigned long long)g_gpu_offer_size_mismatch,
        (unsigned long long)g_gpu_xwayland_resize_wait);
    fprintf(f,"\"opaque_regions\":{\"null\":%llu,\"infinite\":%llu,\"by_ops\":[%llu,%llu,%llu,%llu,%llu],"
        "\"last_first_op\":[%d,%d,%d,%d,%d],\"last_ops\":%d,\"last_kind\":%d,\"opaque_views\":%llu},",g_opaque_null,g_opaque_infinite,g_opaque_rects[0],g_opaque_rects[1],
        g_opaque_rects[2],g_opaque_rects[3],g_opaque_rects[4],g_opaque_last[0],g_opaque_last[1],g_opaque_last[2],g_opaque_last[3],
        g_opaque_last[4],g_opaque_last[5],g_opaque_last_kind,g_opaque_views);
    fprintf(f,"\"layered\":{\"enabled\":%d,\"offers\":%llu,\"safe\":%llu,\"presented\":%llu,\"wait\":%llu,\"full\":%llu,"
        "\"skip_shape\":%llu,\"skip_buffer\":%llu,\"skip_upload\":%llu,\"uploads\":%llu,\"opaque_single\":%llu,\"scaled_single\":%llu},",gpu_layers_enabled(),
        (unsigned long long)g_gpu_group_offers,(unsigned long long)g_gpu_group_safe,(unsigned long long)g_gpu_group_presented,
        (unsigned long long)g_gpu_group_wait,(unsigned long long)g_gpu_group_full,(unsigned long long)g_gpu_group_skip_shape,
        (unsigned long long)g_gpu_group_skip_buffer,(unsigned long long)g_gpu_group_skip_upload,(unsigned long long)g_gpu_group_uploads,
        (unsigned long long)g_gpu_opaque_offers,(unsigned long long)g_gpu_scaled_offers);
    fprintf(f,"\"intel\":{\"queued\":%llu,\"ready\":%llu,\"discarded\":%llu,\"busy\":%llu,\"failed\":%llu,\"fences\":%llu,"
        "\"fence_pending\":%llu,\"pending\":%u,\"direct\":%llu,\"retry_fires\":%llu,\"pending_images\":[",
        (unsigned long long)g_intel_queued,(unsigned long long)g_intel_ready,(unsigned long long)g_intel_discarded,
        (unsigned long long)g_intel_busy,(unsigned long long)g_intel_failed,(unsigned long long)g_intel_fences,
        (unsigned long long)g_intel_fence_pending,g_intel_pending_count,(unsigned long long)g_intel_direct,
        (unsigned long long)g_intel_retry_fires);
    unsigned queued = 0;
    for (const struct brook_image *image = g_intel_pending_head;
         image && queued < INTEL_PENDING_LIMIT; image = image->pending_next, ++queued) {
        fprintf(f, "%s{\"size\":[%u,%u],\"queued_ms_ago\":%u,\"offer_state\":%d,"
                   "\"fence_created\":%d,\"group_refs\":%u,\"buffer_uses\":%u,\"refs\":%u,\"offer_window\":%u}",
                queued ? "," : "", image->width, image->height, image->queued_ms ? now - image->queued_ms : 0,
                image->offer_state, image->producer_fence >= 0, image->group_refs,
                image->source ? image->source->uses : 0, image->refs, image->offer_identity.window);
    }
    fprintf(f, "]},\"nv_layer\":{\"sets\":%llu,\"materialized\":%llu,\"detaches\":%llu,\"failed\":%llu,\"snapshots\":%llu,\"imported\":%llu},",
            (unsigned long long)g_nv_layer_sets,(unsigned long long)g_nv_layer_materialized,(unsigned long long)g_nv_layer_detaches,
            (unsigned long long)g_nv_layer_failed,(unsigned long long)g_nv_snapshots,(unsigned long long)g_nv_imported);
    fprintf(f, "\"readback_bytes\":%llu,\"paths\":{",g_layer_readback_bytes);
    for(unsigned i=0;i<LAYER_PATHS;++i)fprintf(f,"%s\"%s\":%llu",i?",":"",g_layer_path_names[i],g_layer_paths[i]);
    fprintf(f,"},\"windows\":[");
    int first=1;
    for(unsigned i=0;i<16;++i){const struct layer_summary *w=&g_layer_windows[i];if(!w->wm)continue;
        BrookWindowState state={0};
        const int known=!syscall(BROOK_SYS_WM_WINDOW_STATE,(long)w->wm,0L,(long)&state,(long)sizeof(state),0L,0L) &&
            state.version==BROOK_WINDOW_STATE_VERSION && state.window==w->wm;
        const struct brook_surface *rs=NULL;for(const struct brook_surface *x=g_surfaces;x;x=x->next)if(x->wm_id==w->wm && !x->tree.parent){rs=x;break;}
        if(rs)fprintf(f,"%s{\"opaque_region\":[%d,%d,%d,%d,%d],",first?"":",",rs->opaque_set,rs->opaque_x,rs->opaque_y,rs->opaque_w,rs->opaque_h);
        else fprintf(f,"%s{",first?"":",");
        fprintf(f,"\"wm\":%u,\"vfb\":[%u,%u],\"client\":[%u,%u],\"scale\":%u,\"path\":\"%s\",\"count\":%u,\"layers\":[",
            w->wm,w->vfb_w,w->vfb_h,known?state.width:0,known?state.height:0,
            w->scale,w->path>=0 && w->path<LAYER_PATHS?g_layer_path_names[w->path]:"?",w->count);first=0;
        for(unsigned k=0;k<w->count && k<4;++k)fprintf(f,"%s{\"kind\":\"%c\",\"buf\":[%u,%u],\"dst\":[%u,%u],\"at\":[%lld,%lld],"
            "\"src24_8\":[%u,%u],\"fmt\":%u,\"opaque\":%d,\"offer\":%d}",k?",":"",w->l[k].kind,w->l[k].w,w->l[k].h,w->l[k].dw,w->l[k].dh,
            (long long)w->l[k].x,(long long)w->l[k].y,w->l[k].sw,w->l[k].sh,w->l[k].fmt,w->l[k].opaque,w->l[k].offer);
        fprintf(f,"]}");}
    fprintf(f,"],\"surfaces\":[");
    first=1;unsigned listed=0;
    for(struct brook_surface *x=g_surfaces;x && listed<32;x=x->next){
        if(!x->wm_id && !x->xdg_toplevel && !x->subsurface_parent && x->role!=4)continue;
        if(!first)fputc(',',f);
        first=0;++listed;surface_status_json(f,x,now);}
    fprintf(f,"],\"stats_epoch\":%llu,", (unsigned long long)g_stats_epoch);
    latency_stats_json(f);
    fprintf(f,"}\n");
    if(fclose(f)==0)rename(tmp,path);else unlink(tmp);
}
/* 1 when a server accepts connections on dir/name (a stale file refuses). */
static int display_socket_live(const char *dir, const char *name) {
    if (!dir || !*dir || !name || strchr(name, '/')) return 0;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (snprintf(a.sun_path, sizeof(a.sun_path), "%s/%s", dir, name) >= (int)sizeof(a.sun_path)) return 0;
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe < 0) return 0;
    int live = connect(probe, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(probe);
    return live;
}
int main(int argc, char **argv)
{
    fprintf(stderr, "[waylandd] starting (Phase D — kernel-WM unified)\n");
    output_scale_init();
    int run_seconds = 0, no_xwayland = 0;
    const char *xwayland_server=NULL,*xwm_helper=NULL,*xdisplay=":0",*gpu_socket=NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--trace-frames")) g_trace_frames = 1;
        if (!strcmp(argv[i], "--no-xwayland")) no_xwayland = 1;
        if (!strcmp(argv[i], "--gpu-source")) {
            if(gpu_socket || i+1>=argc || argv[i+1][0]!='/')return 2;
            gpu_socket=argv[++i];
        }
        if (!strcmp(argv[i],"--xwayland") && i+1<argc) xwayland_server=argv[++i];
        if (!strcmp(argv[i],"--xwm") && i+1<argc) xwm_helper=argv[++i];
        if (!strcmp(argv[i],"--xdisplay") && i+1<argc) xdisplay=argv[++i];
        if ((!strcmp(argv[i], "--for") || !strcmp(argv[i], "--seconds"))
            && i + 1 < argc)
            run_seconds = atoi(argv[++i]);
    }
    if(gpu_socket && (setenv("BROOK_WAYLAND_GPU_SOCKET",gpu_socket,1) ||
       setenv("BROOK_WAYLAND_INTEL_ASYNC","1",1) || setenv("BROOK_WAYLAND_SNAPSHOT_FENCE","1",1)))return 1;
    if (!!xwayland_server!=!!xwm_helper || xdisplay[0]!=':' || !xdisplay[1] ||
        strspn(xdisplay+1,"0123456789")!=strlen(xdisplay+1) || strlen(xdisplay)>3 || atoi(xdisplay+1)>63) return 2;
    if (no_xwayland && xwayland_server) return 2;
    if (!no_xwayland && !xwayland_server &&
        access("/nix/profile/bin/Xwayland", X_OK) == 0 &&
        access("/nix/profile/bin/brook-xwm", X_OK) == 0) {
        xwayland_server = "/nix/profile/bin/Xwayland";
        xwm_helper = "/nix/profile/bin/brook-xwm";
    }

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
    /* Never take over a live display. The desktop launcher runs
     * `run --once waylandd` before each app, and --once matches only the
     * basename, so it does not see the boot provider (INTLWL.ELF). Both bind
     * paths below unlink an existing socket, so a second server used to
     * steal wayland-0 from the provider and compose every later app on the
     * CPU without the GPU source channel. */
    if (display_socket_live(getenv("XDG_RUNTIME_DIR"), sock_name) || display_socket_live("/tmp", sock_name)) {
        fprintf(stderr, "[waylandd] WAYLAND_DISPLAY=%s already has a live server; exiting\n", sock_name);
        return 0;
    }

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
    if (!wl_global_create(g_display, &xdg_wm_base_interface, BROOK_XDG_WM_BASE_VERSION, NULL, wm_base_bind)) return 1;
    if (!wl_global_create(g_display, &wl_seat_interface, WL_POINTER_AXIS_VALUE120_SINCE_VERSION,
                          NULL, seat_bind)) return 1;
    if (!wl_global_create(g_display,&zwp_relative_pointer_manager_v1_interface,1,NULL,relative_manager_bind)) return 1;
    if (!wl_global_create(g_display,&zwp_pointer_constraints_v1_interface,1,NULL,pointer_constraints_bind)) return 1;
    if (!wl_global_create(g_display, &brook_capture_manager_v1_interface, 1, NULL, capture_manager_bind)) return 1;
    if (!wl_global_create(g_display,&zwp_idle_inhibit_manager_v1_interface,1,NULL,idle_manager_bind)) return 1;
    if (!wl_global_create(g_display, &zwp_text_input_manager_v3_interface, 1, NULL, text_input_manager_bind)) return 1;
    if (!wl_global_create(g_display, &wl_output_interface, 4, NULL, output_bind)) return 1;
    if (!wl_global_create(g_display, &zxdg_decoration_manager_v1_interface, 1, NULL, deco_mgr_bind)) return 1;
    if (!wl_global_create(g_display, &org_kde_kwin_server_decoration_manager_interface, 1, NULL, kde_decoration_manager_bind)) return 1;
    if (!wl_global_create(g_display, &wp_viewporter_interface, 1, NULL, viewporter_bind)) return 1;
    if (!wl_global_create(g_display, &wp_fractional_scale_manager_v1_interface, 1, NULL, fractional_bind)) return 1;
    if (!wl_global_create(g_display, &wl_subcompositor_interface, 1, NULL, subcomp_bind)) return 1;
    if (!wl_global_create(g_display, &wl_data_device_manager_interface, 3, NULL, ddm_bind)) return 1;
    if (!wl_global_create(g_display, &zwp_linux_dmabuf_v1_interface, 4, NULL, dmabuf_bind)) return 1;
    /* Open the DRM render node once for PRIME dmabuf import (hardware GL windows).
     * Absence is non-fatal: we simply won't advertise working dmabuf import. */
    g_drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (g_drm_fd < 0) { g_drm_fd=open("/dev/dri/renderD129",O_RDWR | O_CLOEXEC);g_drm_intel=g_drm_fd>=0; }
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
    nv_dmabuf_open();
    fprintf(stderr, "[waylandd] globals advertised\n");

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    struct wl_event_loop *loop = wl_display_get_event_loop(g_display);
    if (!loop) return 1;
    g_frame_timer = wl_event_loop_add_timer(loop, frame_clock_tick, NULL);
    if (!g_frame_timer) return 1;
    wl_event_source_timer_update(g_frame_timer, 16);
    if (xwayland_server) xwayland_restart_configure(xwayland_server,xwm_helper,xdisplay);
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
            waylandd_status_write(sock);
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
done:;
    gpu_window_unmap_retained();
    const struct IntelPendingDrainOps drain={NULL,shutdown_pending_count,shutdown_pending_poll,shutdown_pending_wait};
    const int pendingDrained=intel_pending_drain(&drain,200);
    fprintf(stderr, "[waylandd] shutting down (commits=%d)\n", g_commit_count);
    if(g_intel_queued)fprintf(stderr,"[waylandd] Intel async queued=%llu ready=%llu discarded=%llu busy_polls=%llu failed=%llu pending=%u\n",
        (unsigned long long)g_intel_queued,(unsigned long long)g_intel_ready,(unsigned long long)g_intel_discarded,(unsigned long long)g_intel_busy,
        (unsigned long long)g_intel_failed,g_intel_pending_count);
    if(g_intel_fences || g_intel_fence_pending)fprintf(stderr,"[waylandd] Intel producer fences=%llu pending_polls=%llu\n",
        (unsigned long long)g_intel_fences,(unsigned long long)g_intel_fence_pending);
    if(g_intel_direct)fprintf(stderr,"[waylandd] Vulkan direct source presentations=%llu\n",(unsigned long long)g_intel_direct);
    if(g_gpu_window_offers)fprintf(stderr,"[waylandd] GPU window offers=%llu acknowledgements=%llu presented=%llu retained_snapshots=%llu retry_fires=%llu\n",
        (unsigned long long)g_gpu_window_offers,(unsigned long long)g_gpu_window_acks,(unsigned long long)g_gpu_window_presented,
        (unsigned long long)g_intel_retained_snapshots,(unsigned long long)g_intel_retry_fires);
    if(getenv("BROOK_WAYLAND_GPU_DIAGNOSTICS"))fprintf(stderr,"[waylandd] GPU offer skipped channel=%llu root=%llu layers=%llu geometry=%llu window=%llu metadata=%llu connect_fail=%llu connect_errno=%d auth_fail=%llu auth_errno=%d\n",
        (unsigned long long)g_gpu_offer_no_channel,(unsigned long long)g_gpu_offer_no_root,(unsigned long long)g_gpu_offer_layers,
        (unsigned long long)g_gpu_offer_geometry,(unsigned long long)g_gpu_offer_window,(unsigned long long)g_gpu_offer_metadata,
        (unsigned long long)g_gpu_connect_fail,g_gpu_connect_errno,(unsigned long long)g_gpu_auth_fail,g_gpu_auth_errno);
    if(g_presentWait.count)fprintf(stderr,"[waylandd] GPU window timing offer_presented_count=%llu offer_presented_mean_ms=%llu offer_presented_p80_le_ms=%u offer_presented_max_ms=%u\n",
        (unsigned long long)g_presentWait.count,(unsigned long long)(g_presentWait.sum/g_presentWait.count),gpu_window_timing_p80_bound(&g_presentWait),g_presentWait.max);
    if(g_offerWait.count || g_snapshotWait.count)fprintf(stderr,"[waylandd] GPU window timing offer_ack_count=%llu offer_ack_mean_ms=%llu offer_ack_p80_le_ms=%u offer_ack_max_ms=%u ack_repaint_count=%llu ack_repaint_mean_ms=%llu ack_repaint_p80_le_ms=%u ack_repaint_max_ms=%u\n",
        (unsigned long long)g_offerWait.count,(unsigned long long)(g_offerWait.count?g_offerWait.sum/g_offerWait.count:0),gpu_window_timing_p80_bound(&g_offerWait),g_offerWait.max,
        (unsigned long long)g_snapshotWait.count,(unsigned long long)(g_snapshotWait.count?g_snapshotWait.sum/g_snapshotWait.count:0),gpu_window_timing_p80_bound(&g_snapshotWait),g_snapshotWait.max);
    if(g_gpu_window_recovered)fprintf(stderr,"[waylandd] GPU window recovered after renderer exit=%llu\n",(unsigned long long)g_gpu_window_recovered);
    int xwayland_stopped=xwayland_shutdown();
    wl_display_destroy(g_display);
    if (!xwayland_stopped || !pendingDrained || g_intel_failed){
        fprintf(stderr,"[waylandd] INCOMPLETE pending=%u failed=%llu\n",g_intel_pending_count,(unsigned long long)g_intel_failed);
        return 1;
    }
    fprintf(stderr, "[waylandd] PASS\n");
    return 0;
}
