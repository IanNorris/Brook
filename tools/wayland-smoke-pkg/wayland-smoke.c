/*
 * wayland-smoke.c — Brook Wayland client smoke test.
 *
 * Connects to the server at /tmp/wayland-0, enumerates globals, and
 * exits. Proves the Wayland protocol transport (AF_UNIX + libwayland
 * client wire protocol) works end-to-end on Brook.
 *
 * Companion to waylandd. Typical run:
 *   run .../bin/waylandd --seconds 8
 *   run .../bin/wayland-smoke
 *
 * Sleeps briefly at startup so the daemon's socket is ready when we
 * try to connect (we boot with no inter-process synchronisation
 * primitive handy beyond the filesystem).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include <wayland-client.h>

static int g_global_count = 0;

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t version)
{
    (void)data; (void)reg;
    fprintf(stderr, "[smoke] global: name=%u iface=%s v%u\n",
            name, iface, version);
    g_global_count++;
}

static void on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg;
    fprintf(stderr, "[smoke] global_remove: name=%u\n", name);
}

static const struct wl_registry_listener kRegistryListener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    fprintf(stderr, "[smoke] starting\n");

    /* Give the server a moment to bind its listening socket. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    if (!getenv("WAYLAND_DISPLAY"))
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    if (!getenv("XDG_RUNTIME_DIR"))
        setenv("XDG_RUNTIME_DIR", "/tmp", 1);

    struct wl_display *dpy = NULL;
    for (int i = 0; i < 10; i++) {
        dpy = wl_display_connect(NULL);
        if (dpy) break;
        fprintf(stderr, "[smoke] wl_display_connect attempt %d failed (errno=%d)\n",
                i, errno);
        nanosleep(&ts, NULL);
    }
    if (!dpy) {
        fprintf(stderr, "[smoke] FAIL: could not connect to compositor\n");
        return 1;
    }
    fprintf(stderr, "[smoke] connected to compositor\n");

    struct wl_registry *reg = wl_display_get_registry(dpy);
    if (!reg) {
        fprintf(stderr, "[smoke] FAIL: wl_display_get_registry\n");
        wl_display_disconnect(dpy);
        return 1;
    }

    wl_registry_add_listener(reg, &kRegistryListener, NULL);

    /* Two roundtrips: first delivers the globals, second flushes any follow-up. */
    if (wl_display_roundtrip(dpy) < 0) {
        fprintf(stderr, "[smoke] FAIL: roundtrip 1\n");
        wl_display_disconnect(dpy);
        return 1;
    }
    fprintf(stderr, "[smoke] roundtrip 1 done, globals seen so far=%d\n",
            g_global_count);

    if (wl_display_roundtrip(dpy) < 0) {
        fprintf(stderr, "[smoke] FAIL: roundtrip 2\n");
        wl_display_disconnect(dpy);
        return 1;
    }
    fprintf(stderr, "[smoke] roundtrip 2 done, globals=%d\n", g_global_count);

    wl_registry_destroy(reg);
    wl_display_disconnect(dpy);

    if (g_global_count < 1) {
        fprintf(stderr, "[smoke] FAIL: no globals advertised\n");
        return 1;
    }

    fprintf(stderr, "[smoke] PASS: %d global(s) enumerated\n", g_global_count);
    return 0;
}
