/* Exercise actual Wayland resources and wire events without a Brook VM. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include "text-input-v3-server-protocol.h"
struct brook_surface { struct wl_resource *resource; };
struct brook_seat_client {
    struct wl_resource *seat;
    struct brook_surface *kb_focus;
    struct brook_seat_client *next;
};
static struct brook_seat_client *g_seat_clients;
#include "text-input-v3.h"

int main(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *display = wl_display_create();
    struct wl_client *client = wl_client_create(display, sockets[0]);
    assert(display && client);
    struct brook_seat_client seat = {0};
    seat.seat = wl_resource_create(client, &wl_seat_interface, 5, 2);
    struct brook_surface surface = {wl_resource_create(client, &wl_surface_interface, 4, 3)};
    assert(seat.seat && surface.resource);
    g_seat_clients = &seat;
    text_input_manager_bind(client, NULL, 1, 4);
    text_input_get(client, NULL, 5, seat.seat);
    struct brook_text_input *t = g_text_inputs;
    text_input_enable(client, t->resource); /* No focused surface. */
    text_input_commit(client, t->resource);
    assert(!t->enabled);
    seat.kb_focus = &surface;
    text_input_focus(&seat, &surface);
    text_input_enable(client, t->resource);
    assert(!t->enabled); /* Pending enable is not active. */
    text_input_commit(client, t->resource);
    assert(t->enabled && t->commits == 2);
    wl_display_flush_clients(display);
    uint32_t wire[128];
    ssize_t bytes = recv(sockets[1], wire, sizeof(wire), MSG_DONTWAIT);
    assert(bytes > 0);
    unsigned enters = 0;
    for (size_t i = 0; i < (size_t)bytes / 4;) {
        uint32_t object = wire[i], opcode = wire[i+1] & 65535, size = wire[i+1] >> 16;
        assert(size >= 8 && i * 4 + size <= (size_t)bytes);
        if (object == 5) {
            /* Activation itself must not insert text or send unmatched done. */
            assert(opcode == ZWP_TEXT_INPUT_V3_ENTER);
            ++enters;
        }
        i += size / 4;
    }
    assert(enters == 1);
    text_input_disable(client, t->resource);
    text_input_commit(client, t->resource);
    assert(!t->enabled && t->commits == 3);
    text_input_enable(client, t->resource); text_input_commit(client, t->resource);
    assert(t->enabled);
    text_input_focus(&seat, NULL); seat.kb_focus = NULL;
    assert(!t->enabled);
    seat.kb_focus = &surface; text_input_focus(&seat, &surface);
    assert(!t->enabled); /* Focus does not restore enable. */
    text_input_remove_seat(&seat);
    assert(!t->seat && !t->focus);
    wl_resource_destroy(surface.resource);
    wl_client_destroy(client);
    assert(!g_text_inputs);
    wl_display_destroy(display); close(sockets[1]);
    return 0;
}
