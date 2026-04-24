/*
 * waylandd.c — Brook OS Wayland-server first-light.
 *
 * This is a *first-light* Wayland server.  It proves we can:
 *   - dynamically load libwayland-server.so on Brook (via the Nix
 *     store + ld-linux + our dynlink path)
 *   - wl_display_create a display
 *   - bind an AF_UNIX listening socket via wl_display_add_socket_auto
 *   - advertise the wl_compositor and wl_shm globals (bind handlers
 *     currently stubbed — no real surface plumbing yet)
 *   - run the event loop for ~N seconds (configurable) and exit cleanly
 *
 * What this does NOT do yet (intentionally):
 *   - accept buffers
 *   - talk to our WM
 *   - implement wl_surface.attach/damage/commit
 *   - anything a client can usefully draw with
 *
 * That's the next phase.  First-light is: "server starts, listens, can
 * be talked to by any client that just wants to enumerate globals".
 *
 * Build via tools/waylandd-pkg/default.nix (Nix derivation with wayland
 * headers + lib in the closure).  Runs on Brook using the Nix disk
 * closure already populated with libwayland-1.24.0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <wayland-server.h>
#include <wayland-server-protocol.h>

static struct wl_display *g_display = NULL;
static volatile sig_atomic_t g_shutdown = 0;

static void on_sigint(int sig) { (void)sig; g_shutdown = 1; }

/* ---------------- wl_compositor stub ---------------- */

static void compositor_create_surface(struct wl_client *client,
                                      struct wl_resource *res,
                                      uint32_t id)
{
    (void)client; (void)res; (void)id;
    fprintf(stderr, "[waylandd] compositor.create_surface id=%u (stub)\n", id);
    /* No resource created yet. Client will get a protocol error on first
     * request against the unknown id. That's fine for first-light. */
}

static void compositor_create_region(struct wl_client *client,
                                     struct wl_resource *res,
                                     uint32_t id)
{
    (void)client; (void)res; (void)id;
    fprintf(stderr, "[waylandd] compositor.create_region id=%u (stub)\n", id);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region  = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id)
{
    (void)data;
    fprintf(stderr, "[waylandd] client bound wl_compositor v=%u id=%u\n",
            version, id);
    struct wl_resource *r = wl_resource_create(client, &wl_compositor_interface,
                                               (int)version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

/* ---------------- wl_shm (let libwayland do the heavy lifting) ---------------- */
/* wl_display_init_shm() registers a working wl_shm global by itself.  We just
 * call it.  It depends on mmap/memfd — Brook has both. */

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

    const char *sock_name = getenv("WAYLAND_DISPLAY");
    if (!sock_name) sock_name = "wayland-0";

    /* Prefer an explicit, known path (avoid XDG_RUNTIME_DIR discovery
     * that varies with env).  First try add_socket with explicit name
     * in the default dir (libwayland prepends $XDG_RUNTIME_DIR).  If
     * that fails (no XDG_RUNTIME_DIR), fall back to a /tmp bind. */
    const char *sock = NULL;
    if (getenv("XDG_RUNTIME_DIR"))
    {
        if (wl_display_add_socket(g_display, sock_name) == 0)
            sock = sock_name;
    }
    if (!sock)
    {
        /* Fallback: bind the AF_UNIX socket ourselves at /tmp/<name>.
         * libwayland's wl_display_add_socket_fd accepts a ready fd. */
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

    /* Run the event loop.  wl_display_run blocks forever; we don't want
     * that for a smoke test, so drive the loop manually with a timeout. */
    struct wl_event_loop *loop = wl_display_get_event_loop(g_display);
    if (!loop) {
        fprintf(stderr, "[waylandd] FAIL: get_event_loop\n");
        return 1;
    }

    for (int i = 0; i < run_seconds && !g_shutdown; ++i) {
        wl_display_flush_clients(g_display);
        /* 1000 ms timeout per dispatch.  Returns 0 on timeout, events dispatched. */
        int n = wl_event_loop_dispatch(loop, 1000);
        if (n < 0) {
            fprintf(stderr, "[waylandd] event loop error: %s\n", strerror(errno));
            break;
        }
        if (n > 0)
            fprintf(stderr, "[waylandd] dispatched %d event(s) at t=%d\n", n, i);
    }

    fprintf(stderr, "[waylandd] shutting down\n");
    wl_display_destroy(g_display);
    fprintf(stderr, "[waylandd] PASS\n");
    return 0;
}
