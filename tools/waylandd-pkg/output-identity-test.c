/* Exercise real output wire events, including updates for older clients. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>

static uint32_t screen_width, screen_height;
static int screen_error;
static unsigned screen_queries;
static long test_syscall(long number, ...);
#define syscall test_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall

static unsigned test_outputs;
static uint64_t test_output_generation;
static long test_syscall(long number, ...) {
    if (number == BROOK_SYS_WM_GET_OUTPUTS) {
        va_list args;
        va_start(args, number);
        struct brook_outputs_header *header = (void *)(uintptr_t)va_arg(args, long);
        struct brook_output_desc *out = (void *)(uintptr_t)va_arg(args, long);
        long capacity = va_arg(args, long);
        va_end(args);
        const struct brook_output_desc heads[2] = {
            {0, 0, 0, 3840, 2160, 60000, 100, BROOK_OUTPUT_CONNECTED | BROOK_OUTPUT_ENABLED | BROOK_OUTPUT_PRIMARY, "BROOK-1"},
            {1, 3840, 0, 2560, 1440, 60000, 100, BROOK_OUTPUT_CONNECTED | BROOK_OUTPUT_ENABLED, "BROOK-2"}};
        header->count = test_outputs; header->generation = test_output_generation;
        long n = capacity < (long)test_outputs ? capacity : (long)test_outputs;
        for (long i = 0; i < n; ++i) out[i] = heads[i];
        return n;
    }
    if (number != BROOK_SYS_WM_GET_SCREEN_INFO) return 0;
    ++screen_queries;
    if (screen_error) return -1;
    va_list args;
    va_start(args, number);
    uint32_t *screen = (uint32_t *)(uintptr_t)va_arg(args, long);
    va_end(args);
    screen[0] = screen_width;
    screen[1] = screen_height;
    return 0;
}

static void expect_no_updates(struct wl_display *display, int socket) {
    uint32_t event;
    wl_display_flush_clients(display);
    assert(recv(socket, &event, sizeof(event), MSG_DONTWAIT) < 0 && errno == EAGAIN);
}

static void expect_output(struct wl_display *display, int socket, uint32_t object,
                          unsigned version, unsigned x11, int initial, uint32_t w, uint32_t h) {
    wl_display_flush_clients(display);
    uint32_t wire[512];
    ssize_t bytes = recv(socket, wire, sizeof(wire), MSG_DONTWAIT);
    assert(bytes > 0);
    unsigned counts[6] = {0}, last = 99;
    for (size_t offset = 0; offset < (size_t)bytes;) {
        uint32_t *event = (void *)((char *)wire + offset);
        uint32_t size = event[1] >> 16, opcode = event[1] & 65535;
        assert(size >= 8 && !(size & 3) && offset + size <= (size_t)bytes);
        assert(event[0] == object && opcode < 6);
        ++counts[opcode]; last = opcode;
        if (opcode == WL_OUTPUT_GEOMETRY) {
            const uint32_t divisor = x11 ? g_output_scale : 1;
            assert(size >= 32 && event[4] == (w * 254 + 480) / 960 / divisor);
            assert(event[5] == (h * 254 + 480) / 960 / divisor);
        }
        if (opcode == WL_OUTPUT_MODE) {
            assert(counts[WL_OUTPUT_GEOMETRY] == 1);
            assert(size == 24 && event[2] == (WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED));
            assert(event[3] == w && event[4] == h && event[5] == 60000);
        }
        if (opcode == WL_OUTPUT_SCALE) assert(size == 12 && event[2] == (x11 ? 1 : g_output_scale));
        if (opcode == WL_OUTPUT_NAME || opcode == WL_OUTPUT_DESCRIPTION) {
            assert(size >= 12 && event[2] && event[2] <= size - 12);
            const char *value = (char *)&event[3];
            assert(value[event[2] - 1] == 0);
            assert(!strcmp(value, opcode == WL_OUTPUT_NAME ? "BROOK-1" : "Brook virtual display"));
        }
        offset += size;
    }
    assert(counts[WL_OUTPUT_GEOMETRY] == 1 && counts[WL_OUTPUT_MODE] == 1);
    assert(counts[WL_OUTPUT_DONE] == (version >= 2));
    assert(counts[WL_OUTPUT_SCALE] == (initial && version >= 2));
    assert(counts[WL_OUTPUT_NAME] == (initial && version >= 4));
    assert(counts[WL_OUTPUT_DESCRIPTION] == (initial && version >= 4));
    assert(last == (version >= 2 ? WL_OUTPUT_DONE : WL_OUTPUT_MODE));
}

static void check_client(unsigned version, unsigned x11) {
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    struct wl_display *display = wl_display_create(); assert(display);
    struct wl_client *client = wl_client_create(display, sockets[0]); assert(client);
    if (x11) assert(xwayland_attach_client(client));
    assert(!g_outputs);
    const unsigned before = screen_queries;
    output_refresh_due(1000);
    assert(screen_queries == before);

    screen_width = 1920; screen_height = 1080;
    output_bind(client, NULL, version, 0);
    assert(g_outputs && !g_outputs->next);
    const uint32_t object = wl_resource_get_id(g_outputs->resource);
    expect_output(display, sockets[1], object, version, x11, 1, 1920, 1080);
    assert(screen_queries == before + 1);

    screen_width = 1280; screen_height = 800;
    output_refresh_due(g_output_checked_ms + OUTPUT_CHECK_INTERVAL_MS - 1);
    expect_no_updates(display, sockets[1]);
    assert(screen_queries == before + 1);
    output_refresh_due(g_output_checked_ms + OUTPUT_CHECK_INTERVAL_MS);
    expect_output(display, sockets[1], object, version, x11, 0, 1280, 800);
    assert(screen_queries == before + 2);

    output_refresh_due(g_output_checked_ms + OUTPUT_CHECK_INTERVAL_MS);
    expect_no_updates(display, sockets[1]);
    screen_error = 1;
    screen_width = 1600; screen_height = 900;
    output_refresh_due(g_output_checked_ms + OUTPUT_CHECK_INTERVAL_MS);
    expect_no_updates(display, sockets[1]);
    screen_error = 0;
    screen_height = 0;
    output_refresh_due(g_output_checked_ms + OUTPUT_CHECK_INTERVAL_MS);
    expect_no_updates(display, sockets[1]);

    /* A new bind samples immediately and brings older bindings along. */
    screen_height = 900;
    int late[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, late));
    struct wl_client *new_client = wl_client_create(display, late[0]); assert(new_client);
    output_bind(new_client, NULL, 4, 0);
    assert(g_outputs->next && !g_outputs->next->next);
    const uint32_t new_object = wl_resource_get_id(g_outputs->resource);
    expect_output(display, sockets[1], object, version, x11, 0, 1600, 900);
    expect_output(display, late[1], new_object, 4, 0, 1, 1600, 900);

    screen_width = 1024;
    g_frame_timer = wl_event_loop_add_timer(wl_display_get_event_loop(display), frame_clock_tick, NULL);
    assert(g_frame_timer);
    g_output_checked_ms = g_now_ms() - OUTPUT_CHECK_INTERVAL_MS;
    assert(frame_clock_tick(NULL) == 0);
    expect_output(display, sockets[1], object, version, x11, 0, 1024, 900);
    expect_output(display, late[1], new_object, 4, 0, 0, 1024, 900);
    wl_event_source_remove(g_frame_timer);
    g_frame_timer = NULL;

    wl_client_destroy(client);
    wl_client_destroy(new_client);
    assert(!g_outputs);
    close(sockets[1]); close(late[1]); wl_display_destroy(display);
}

/* A second head (desktop x=3840) becomes a wl_output only Xwayland sees, at
 * its desktop position: the X root then covers it, so X windows there take
 * input. Native clients keep the single primary output. */
static void check_second_head(void) {
    int xs[2], ns[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, xs) && !socketpair(AF_UNIX, SOCK_STREAM, 0, ns));
    struct wl_display *display = wl_display_create(); assert(display);
    g_display = display;
    struct wl_client *x = wl_client_create(display, xs[0]), *native = wl_client_create(display, ns[0]);
    assert(x && native && xwayland_attach_client(x));
    test_outputs = 2; ++test_output_generation;
    assert(output_sync_extra() && g_xoutputs[1].global && !g_xoutputs[2].global);
    assert(!output_sync_extra()); /* same generation: nothing to do */
    assert(output_global_xwayland_only(g_xoutputs[1].global));
    assert(xwayland_global_filter(x, g_xoutputs[1].global, NULL));
    assert(!xwayland_global_filter(native, g_xoutputs[1].global, NULL));
    output_bind_extra(x, (void *)(uintptr_t)1, 4, 0);
    assert(g_outputs && g_outputs->index == 1);
    wl_display_flush_clients(display);
    uint32_t wire[512]; ssize_t bytes = recv(xs[1], wire, sizeof(wire), MSG_DONTWAIT);
    assert(bytes > 0);
    int geometry = 0, mode = 0, name = 0;
    for (size_t offset = 0; offset < (size_t)bytes;) {
        uint32_t *event = (void *)((char *)wire + offset);
        uint32_t size = event[1] >> 16, opcode = event[1] & 65535;
        if (opcode == WL_OUTPUT_GEOMETRY) geometry = (int32_t)event[2] == 3840 && event[3] == 0;
        if (opcode == WL_OUTPUT_MODE) mode = event[3] == 2560 && event[4] == 1440;
        if (opcode == WL_OUTPUT_NAME) name = !strcmp((char *)&event[3], "BROOK-2");
        offset += size;
    }
    assert(geometry && mode && name);
    test_outputs = 1; ++test_output_generation;
    assert(output_sync_extra() && !g_xoutputs[1].global && g_outputs->index == 0);
    wl_client_destroy(x); wl_client_destroy(native);
    close(xs[1]); close(ns[1]); wl_display_destroy(display);
    g_display = NULL; test_outputs = 0;
}

int main(void) {
    check_second_head();
    for (g_output_scale = 1; g_output_scale <= 2; ++g_output_scale)
        for (unsigned x11 = 0; x11 <= 1; ++x11)
            for (unsigned version = 1; version <= 4; ++version)
                check_client(version, x11);
    puts("OUTPUT_IDENTITY_PASS live size updates, 1x/2x, v1/v2/v3/v4, Wayland and Xwayland, second head for Xwayland only");
    return 0;
}
