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
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <wayland-server.h>
#include <wayland-server-protocol.h>

static struct wl_display *g_display = NULL;
static volatile sig_atomic_t g_shutdown = 0;
static int g_commit_count = 0;

static void on_sigint(int sig) { (void)sig; g_shutdown = 1; }

/* ---------------- wl_surface ---------------- */

struct brook_surface {
    struct wl_resource *resource;
    struct wl_resource *pending_buffer; /* set by attach, consumed by commit */
    int pending_w, pending_h;
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
    /* Create and immediately fire the callback — static client gets one
     * ack per commit, done at commit time from our side. */
    struct wl_resource *cb_r = wl_resource_create(c, &wl_callback_interface, 1, cb);
    if (cb_r) {
        wl_callback_send_done(cb_r, 0);
        wl_resource_destroy(cb_r);
    }
    (void)r;
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
    }
    wl_shm_buffer_end_access(shm);

    fprintf(stderr,
            "[waylandd] commit #%d: %dx%d stride=%d fmt=0x%x "
            "digest=0x%08x first=0x%08x last=0x%08x\n",
            ++g_commit_count, w, h, stride, fmt, digest, first_pix, last_pix);

    /* Release the buffer so the client can reuse it. */
    wl_buffer_send_release(s->pending_buffer);
    s->pending_buffer = NULL;
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
    fprintf(stderr, "[waylandd] create_surface entry id=%u\n", id); fflush(stderr);
    struct brook_surface *s = calloc(1, sizeof(*s));
    if (!s) { wl_client_post_no_memory(client); return; }
    struct wl_resource *sr = wl_resource_create(client, &wl_surface_interface,
                                                 wl_resource_get_version(res), id);
    if (!sr) { free(s); wl_client_post_no_memory(client); return; }
    s->resource = sr;
    wl_resource_set_implementation(sr, &surface_impl, s, surface_destroy_userdata);
    fprintf(stderr, "[waylandd] wl_surface created id=%u\n", id); fflush(stderr);
}

static void compositor_create_region(struct wl_client *client,
                                     struct wl_resource *res,
                                     uint32_t id)
{
    (void)client; (void)res; (void)id;
    /* Regions are only used by set_opaque_region / set_input_region, which
     * we ignore; no need to back this with real state for first pixels. */
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region  = compositor_create_region,
};

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
