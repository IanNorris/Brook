/* Initial mapping, activation and resize against real libwayland SHM buffers. */
#define _GNU_SOURCE
#include <assert.h>
#include <wayland-client.h>
#define main brook_waylandd_main
#include "waylandd.c"
#undef main

static struct wl_shm *client_shm;
static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    (void)data; (void)version;
    if (!strcmp(interface, "wl_shm"))
        client_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
}
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {registry_global, registry_remove};

static void requests(struct wl_display *connection, struct wl_event_loop *loop) {
    assert(wl_display_flush(connection) >= 0);
    assert(wl_event_loop_dispatch(loop, 0) == 0);
}
static uint32_t configure_serial(struct wl_display *display, int fd, uint32_t object) {
    wl_display_flush_clients(display);
    uint32_t wire[256];
    ssize_t bytes = recv(fd, wire, sizeof(wire), MSG_DONTWAIT);
    assert(bytes > 0);
    uint32_t serial = 0;
    unsigned count = 0;
    for (size_t i = 0; i < (size_t)bytes / 4;) {
        uint32_t id = wire[i], op = wire[i+1] & 65535, size = wire[i+1] >> 16;
        assert(size >= 8 && i * 4 + size <= (size_t)bytes);
        if (id == object && op == XDG_SURFACE_CONFIGURE) {
            assert(size == 12); serial = wire[i+2]; ++count;
        }
        i += size / 4;
    }
    assert(count == 1);
    return serial;
}
int main(void) {
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *display = wl_display_create(); assert(display);
    assert(wl_display_init_shm(display) == 0);
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    struct wl_client *client = wl_client_create(display, sockets[0]); assert(client);
    struct wl_display *connection = wl_display_connect_to_fd(sockets[1]); assert(connection);
    struct wl_registry *registry = wl_display_get_registry(connection); assert(registry);
    assert(wl_registry_add_listener(registry, &registry_listener, NULL) == 0);
    requests(connection, loop); wl_display_flush_clients(display);
    assert(wl_display_dispatch(connection) >= 0 && client_shm);
    requests(connection, loop);

    int fd = memfd_create("configure-surface", 0); assert(fd >= 0);
    assert(ftruncate(fd, 16) == 0);
    uint32_t *pixels = mmap(NULL, 16, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(pixels != MAP_FAILED);
    struct wl_shm_pool *pool = wl_shm_create_pool(client_shm, fd, 16); assert(pool);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 2, 2, 8, WL_SHM_FORMAT_XRGB8888);
    assert(buffer); requests(connection, loop);
    struct wl_resource *server_buffer = wl_client_get_object(client, wl_proxy_get_id((struct wl_proxy *)buffer));
    assert(server_buffer && wl_shm_buffer_get(server_buffer));

    struct brook_surface surface = {0};
    wl_list_init(&surface.pending_frames); wl_list_init(&surface.committed_frames);
    surface.resource = wl_resource_create(client, &wl_surface_interface, 4, 0);
    surface.xdg_surface = wl_resource_create(client, &xdg_surface_interface, 1, 0);
    surface.xdg_toplevel = wl_resource_create(client, &xdg_toplevel_interface, 1, 0);
    assert(surface.resource && surface.xdg_surface && surface.xdg_toplevel);
    wl_resource_set_user_data(surface.resource, &surface);
    wl_resource_set_user_data(surface.xdg_surface, &surface);
    uint32_t vfb[4] = {0};
    surface.wm_id = 1; surface.vfb = vfb;
    surface.vfb_w = surface.vfb_h = surface.vfb_stride = 2;
    g_serial = 1;
    surface_commit(client, surface.resource);
    uint32_t initial = configure_serial(display, sockets[1], wl_resource_get_id(surface.xdg_surface));
    assert(!surface.xdg_acked);
    for (unsigned i = 0; i < 4; ++i) pixels[i] = 0x00112233;
    surface_attach(client, surface.resource, server_buffer, 0, 0);
    surface_commit(client, surface.resource);
    assert(!surface.xdg_acked && !vfb[0] && !vfb[3]);
    xdg_surface_ack_configure(client, surface.xdg_surface, initial);
    surface_attach(client, surface.resource, server_buffer, 0, 0);
    surface_commit(client, surface.resource);
    assert(vfb[0] == 0xff112233 && vfb[3] == 0xff112233);

    toplevel_set_activated(&surface, 1);
    uint32_t active = configure_serial(display, sockets[1], wl_resource_get_id(surface.xdg_surface));
    assert(active != initial); /* Old independent allocators both emitted 1. */
    /* A mapped client may keep submitting while a newer configure is pending. */
    for (unsigned i = 0; i < 4; ++i) pixels[i] = 0x00445566;
    surface_attach(client, surface.resource, server_buffer, 0, 0);
    surface_commit(client, surface.resource);
    assert(surface.xdg_acked && vfb[0] == 0xff445566 && vfb[3] == 0xff445566);
    xdg_surface_ack_configure(client, surface.xdg_surface, active);
    struct brook_wm_event resize = {.type = WM_EVT_RESIZED, .x = 2, .y = 2};
    dispatch_window_event(&surface, &resize, 0);
    uint32_t resized = configure_serial(display, sockets[1], wl_resource_get_id(surface.xdg_surface));
    assert(resized != initial && resized != active && surface.xdg_acked);
    for (unsigned i = 0; i < 4; ++i) pixels[i] = 0x00778899;
    surface_attach(client, surface.resource, server_buffer, 0, 0);
    surface_commit(client, surface.resource);
    assert(vfb[0] == 0xff778899 && vfb[3] == 0xff778899);
    xdg_surface_ack_configure(client, surface.xdg_surface, resized);

    /* Initial popup and reposition serials share the same allocator too. */
    struct brook_surface popup = {0};
    wl_list_init(&popup.pending_frames); wl_list_init(&popup.committed_frames);
    popup.resource = wl_resource_create(client, &wl_surface_interface, 4, 0);
    popup.xdg_surface = wl_resource_create(client, &xdg_surface_interface, 3, 0);
    popup.xdg_popup = wl_resource_create(client, &xdg_popup_interface, 3, 0);
    assert(popup.resource && popup.xdg_surface && popup.xdg_popup);
    wl_resource_set_user_data(popup.resource, &popup);
    wl_resource_set_user_data(popup.xdg_popup, &popup);
    surface_commit(client, popup.resource);
    uint32_t popup_initial = configure_serial(display, sockets[1], wl_resource_get_id(popup.xdg_surface));
    assert(popup_initial > resized);
    xdg_popup_reposition(client, popup.xdg_popup, NULL, 1);
    uint32_t repositioned = configure_serial(display, sockets[1], wl_resource_get_id(popup.xdg_surface));
    assert(repositioned > popup_initial);

    free(surface.image); free(surface.composite);
    munmap(pixels, 16); close(fd);
    wl_display_disconnect(connection); wl_client_destroy(client); wl_display_destroy(display);
    return 0;
}
