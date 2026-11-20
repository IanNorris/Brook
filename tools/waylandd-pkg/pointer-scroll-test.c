/* Assert actual Wayland wire events for the scroll input path. */
#include <assert.h>
#include <stdarg.h>
static long test_syscall(long, ...);
#define syscall test_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall

static int legacy_kernel;
static unsigned create_attempts;
static long test_syscall(long number, ...) {
    if (number != BROOK_SYS_WM_CREATE_WINDOW) return 0;
    va_list args;
    va_start(args, number);
    for (unsigned i = 0; i < 4; ++i) (void)va_arg(args, long);
    const unsigned flags = (unsigned)va_arg(args, long);
    va_end(args);
    ++create_attempts;
    assert(flags & BROOK_WM_CREATE_FLAG_CSD);
    if (legacy_kernel && create_attempts == 2) {
        assert(!(flags & BROOK_WM_CREATE_FLAG_FINE_SCROLL));
        return 0;
    }
    assert(flags & BROOK_WM_CREATE_FLAG_FINE_SCROLL);
    if (!legacy_kernel) return 0;
    errno = EINVAL;
    return -1;
}

struct wire {
    uint32_t words[128];
    size_t count, pos;
};

static struct wire read_wire(struct wl_display *display, int fd) {
    struct wire wire = {0};
    wl_display_flush_clients(display);
    const ssize_t bytes = recv(fd, wire.words, sizeof(wire.words), MSG_DONTWAIT);
    if (bytes == -1) assert(errno == EAGAIN);
    else {
        assert(bytes > 0 && bytes % 4 == 0 && bytes < (ssize_t)sizeof(wire.words));
        wire.count = (size_t)bytes / 4;
    }
    return wire;
}

static void expect(struct wire *wire, uint16_t opcode, size_t count, ...) {
    assert(wire->pos + 2 + count <= wire->count);
    assert(wire->words[wire->pos++] == 4); // pointer resource id
    assert(wire->words[wire->pos++] == ((2 + count) * 4 << 16 | opcode));
    va_list args;
    va_start(args, count);
    for (size_t i = 0; i < count; ++i) assert(wire->words[wire->pos++] == va_arg(args, uint32_t));
    va_end(args);
}

static void check_scroll(uint32_t version) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *display = wl_display_create();
    assert(display);
    struct wl_client *client = wl_client_create(display, sockets[0]);
    assert(client);
    struct brook_surface surface = {0};
    surface.resource = wl_resource_create(client, &wl_surface_interface, 4, 2);
    assert(surface.resource);
    seat_bind(client, NULL, version, 3);
    struct brook_seat_client *seat = g_seat_clients;
    assert(seat && seat->seat && (uint32_t)wl_resource_get_version(seat->seat) == version);
    seat_get_pointer(client, seat->seat, 4);
    assert(seat->pointer && (uint32_t)wl_resource_get_version(seat->pointer) == version);
    (void)read_wire(display, sockets[1]); // capabilities and seat name

    /* Hit routing uses the packed pointer position even before a motion event. */
    struct brook_wm_event event = {
        .type = EVT_MOUSE_SCROLL, .mods = BROOK_WM_SCROLL_FINE | BROOK_WM_SCROLL_FINGER,
        .x = 15, .y = -30, .reserved = 28u << 16 | 36u,
    };
    g_input_recorded = 0;
    g_input_batch_us = g_now_us();
    dispatch_window_event(&surface, &event, 100);
    assert(seat->entered_surface == &surface && seat->pointer_x == 36 && seat->pointer_y == 28);
    struct client_latency *latency = latency_client(client);
    assert(latency && latency->data.input[LATENCY_AXIS].count == 1);
    struct wire wire = read_wire(display, sockets[1]);
    expect(&wire, WL_POINTER_ENTER, 4, g_serial - 1, 2u, 36u * 256, 28u * 256);
    if (version >= 5) {
        expect(&wire, WL_POINTER_FRAME, 0);
        expect(&wire, WL_POINTER_AXIS_SOURCE, 1, (uint32_t)WL_POINTER_AXIS_SOURCE_FINGER);
    }
    expect(&wire, WL_POINTER_AXIS, 3, 100u, (uint32_t)WL_POINTER_AXIS_VERTICAL_SCROLL, (uint32_t)-320);
    expect(&wire, WL_POINTER_AXIS, 3, 100u, (uint32_t)WL_POINTER_AXIS_HORIZONTAL_SCROLL, (uint32_t)-640);
    if (version >= 5) expect(&wire, WL_POINTER_FRAME, 0);
    assert(wire.pos == wire.count);

    g_input_recorded = 0;
    event.mods |= BROOK_WM_SCROLL_END;
    event.x = event.y = 0;
    dispatch_window_event(&surface, &event, 101);
    wire = read_wire(display, sockets[1]);
    if (version >= 5) {
        expect(&wire, WL_POINTER_AXIS_SOURCE, 1, (uint32_t)WL_POINTER_AXIS_SOURCE_FINGER);
        expect(&wire, WL_POINTER_AXIS_STOP, 2, 101u, (uint32_t)WL_POINTER_AXIS_VERTICAL_SCROLL);
        expect(&wire, WL_POINTER_AXIS_STOP, 2, 101u, (uint32_t)WL_POINTER_AXIS_HORIZONTAL_SCROLL);
        expect(&wire, WL_POINTER_FRAME, 0);
    }
    assert(wire.pos == wire.count && seat->finger_axes == 0);
    assert(latency->data.input[LATENCY_AXIS].count == 1);
    dispatch_window_event(&surface, &event, 102);
    assert(read_wire(display, sockets[1]).count == 0); // duplicate stop has no axes

    {
        /* Verify both new-kernel and legacy tick-only reports. */
        const uint32_t axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
        for (unsigned fine = 0; fine <= 1; ++fine) {
            event = (struct brook_wm_event){.type = EVT_MOUSE_SCROLL, .scan = (uint8_t)-2};
            if (fine) {
                event.mods = BROOK_WM_SCROLL_FINE;
                event.x = -240;
                event.reserved = 28u << 16 | 36u;
            }
            dispatch_window_event(&surface, &event, 103);
            wire = read_wire(display, sockets[1]);
            if (version >= 5) expect(&wire, WL_POINTER_AXIS_SOURCE, 1, (uint32_t)WL_POINTER_AXIS_SOURCE_WHEEL);
            if (version >= 8) expect(&wire, WL_POINTER_AXIS_VALUE120, 2, axis, 240u);
            else if (version >= 5) expect(&wire, WL_POINTER_AXIS_DISCRETE, 2, axis, 2u);
            expect(&wire, WL_POINTER_AXIS, 3, 103u, axis, 20u * 256);
            if (version >= 5) expect(&wire, WL_POINTER_FRAME, 0);
            assert(wire.pos == wire.count);
        }
    }

    /* An exit ends only the axis that actually moved on this surface. */
    event = (struct brook_wm_event){
        .type = EVT_MOUSE_SCROLL, .mods = BROOK_WM_SCROLL_FINE | BROOK_WM_SCROLL_FINGER,
        .x = 1, .reserved = 28u << 16 | 36u,
    };
    dispatch_window_event(&surface, &event, 104);
    (void)read_wire(display, sockets[1]);
    event.type = WM_EVT_POINTER_LEFT;
    dispatch_window_event(&surface, &event, 105);
    wire = read_wire(display, sockets[1]);
    if (version >= 5) {
        expect(&wire, WL_POINTER_AXIS_SOURCE, 1, (uint32_t)WL_POINTER_AXIS_SOURCE_FINGER);
        expect(&wire, WL_POINTER_AXIS_STOP, 2, 105u, (uint32_t)WL_POINTER_AXIS_VERTICAL_SCROLL);
        expect(&wire, WL_POINTER_FRAME, 0);
    }
    expect(&wire, WL_POINTER_LEAVE, 2, g_serial - 1, 2u);
    if (version >= 5) expect(&wire, WL_POINTER_FRAME, 0);
    assert(wire.pos == wire.count && !seat->entered_surface && !seat->finger_axes);

    g_input_batch_us = 0;
    wl_display_destroy_clients(display);
    wl_display_destroy(display);
    close(sockets[1]);
}

int main(void) {
    struct brook_wm_create_out out;
    assert(wm_create_window(100, 100, "scroll", BROOK_WM_CREATE_FLAG_CSD, &out) == 0 && create_attempts == 1);
    legacy_kernel = 1;
    create_attempts = 0;
    assert(wm_create_window(100, 100, "scroll", BROOK_WM_CREATE_FLAG_CSD, &out) == 0 && create_attempts == 2);
    check_scroll(4);
    check_scroll(5);
    check_scroll(7);
    check_scroll(8);
    return 0;
}
