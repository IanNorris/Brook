/*
 * wayland-shm-smoke.c — Brook Wayland client test: draw a pattern to a
 * wl_shm buffer, attach it to a wl_surface, commit, and exit.
 *
 * Verifies the full wl_shm round-trip path: memfd allocation, fd passing
 * via SCM_RIGHTS, both sides mapping the same pages, client-side draws
 * visible on the server-side read.
 *
 * Expected log (interleaved with server's commit digest):
 *   [shm-smoke] connected
 *   [shm-smoke] shm + compositor bound
 *   [shm-smoke] buffer 64x64 stride=256 pool=16384 bytes
 *   [shm-smoke] drew pattern (gradient)
 *   [shm-smoke] committed
 *   [waylandd] commit #1: 64x64 stride=256 fmt=0x1 digest=0x... first=... last=...
 *   [shm-smoke] PASS
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

#include <wayland-client.h>
#include <wayland-client-protocol.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001
#endif

static int memfd_create_shim(const char *name, unsigned int flags)
{
    /* glibc exposes memfd_create in <sys/mman.h> on recent builds, but
     * we call syscall directly to avoid surprises on Brook. */
    return (int)syscall(319 /* SYS_memfd_create */, name, flags);
}

static struct wl_shm        *g_shm  = NULL;
static struct wl_compositor *g_comp = NULL;
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
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) {
    (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_lis = {
    .global = on_global, .global_remove = on_global_remove,
};

static void on_buf_release(void *data, struct wl_buffer *buf) {
    (void)data; (void)buf;
    g_got_release = 1;
}
static const struct wl_buffer_listener buf_lis = { .release = on_buf_release };

int main(void)
{
    /* Give waylandd a moment to bind its socket. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    if (!getenv("WAYLAND_DISPLAY"))   setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    if (!getenv("XDG_RUNTIME_DIR"))   setenv("XDG_RUNTIME_DIR", "/tmp", 1);

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "[shm-smoke] FAIL: wl_display_connect: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "[shm-smoke] connected\n");

    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_lis, NULL);
    wl_display_roundtrip(dpy);

    if (!g_shm || !g_comp) {
        fprintf(stderr, "[shm-smoke] FAIL: missing globals shm=%p comp=%p\n",
                (void*)g_shm, (void*)g_comp);
        return 1;
    }
    fprintf(stderr, "[shm-smoke] shm + compositor bound\n");

    /* Allocate an shm pool via memfd. */
    const int W = 64, H = 64, BPP = 4;
    const int STRIDE = W * BPP;
    const int SIZE = STRIDE * H;

    int fd = memfd_create_shim("shm-smoke", MFD_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[shm-smoke] FAIL: memfd_create: %s\n", strerror(errno));
        return 1;
    }
    if (ftruncate(fd, SIZE) < 0) {
        fprintf(stderr, "[shm-smoke] FAIL: ftruncate: %s\n", strerror(errno));
        return 1;
    }
    uint8_t *px = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        fprintf(stderr, "[shm-smoke] FAIL: mmap: %s\n", strerror(errno));
        return 1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, SIZE);
    if (!pool) { fprintf(stderr, "[shm-smoke] FAIL: create_pool\n"); return 1; }
    wl_display_roundtrip(dpy);
    fprintf(stderr, "[shm-smoke] pool created, roundtrip OK\n");

    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, W, H, STRIDE,
                                                      WL_SHM_FORMAT_XRGB8888);
    if (!buf) { fprintf(stderr, "[shm-smoke] FAIL: pool_create_buffer\n"); return 1; }
    wl_buffer_add_listener(buf, &buf_lis, NULL);

    fprintf(stderr, "[shm-smoke] buffer %dx%d stride=%d pool=%d bytes\n",
            W, H, STRIDE, SIZE);

    /* Draw a diagonal gradient XRGB8888: R=x, G=y, B=x+y. */
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
    fprintf(stderr, "[shm-smoke] drew pattern (gradient) first=0x%08x last=0x%08x\n",
            ((uint32_t*)px)[0], ((uint32_t*)px)[W*H - 1]);

    /* Create surface, attach, damage, commit. */
    struct wl_surface *surf = wl_compositor_create_surface(g_comp);
    if (!surf) { fprintf(stderr, "[shm-smoke] FAIL: create_surface\n"); return 1; }
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage(surf, 0, 0, W, H);
    wl_surface_commit(surf);
    fprintf(stderr, "[shm-smoke] committed\n");

    /* Let the server process. Two roundtrips to give the release event
     * a chance to come back. */
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);

    /* Give the event loop a few more ticks just in case release is
     * pending on a later dispatch. */
    for (int i = 0; i < 10 && !g_got_release; i++) {
        wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        struct timespec t = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }

    fprintf(stderr, "[shm-smoke] buffer release received: %s\n",
            g_got_release ? "yes" : "no (non-fatal)");

    wl_surface_destroy(surf);
    wl_buffer_destroy(buf);
    wl_shm_pool_destroy(pool);
    munmap(px, SIZE);
    close(fd);
    wl_display_disconnect(dpy);

    fprintf(stderr, "[shm-smoke] PASS\n");
    return 0;
}
