/* Exercise the actual compositor adapter, not a parallel association model. */
#define _GNU_SOURCE
#include <assert.h>
static long test_syscall(long number, ...) { (void)number;return 0; }
#define syscall test_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall

static struct wl_client *client_new(struct wl_display *display,int *peer) {
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    *peer=sockets[1];return client;
}
static struct brook_surface *surface_new(struct wl_client *client) {
    struct wl_resource *compositor=wl_resource_create(client,&wl_compositor_interface,4,0);
    assert(compositor);compositor_create_surface(client,compositor,0);
    struct brook_surface *s=g_surfaces;assert(s && !s->role);return s;
}
static struct wl_resource *shell_new(struct wl_client *client) {
    struct wl_resource *shell=wl_resource_create(client,&xwayland_shell_v1_interface,1,0);
    assert(shell);return shell;
}
static void associate(struct wl_client *client,struct wl_resource *shell,struct brook_surface *s,uint64_t serial) {
    xwayland_get_surface(client,shell,0,s->resource);assert(s->xwayland_surface);
    xwayland_set_serial(client,s->xwayland_surface,(uint32_t)serial,(uint32_t)(serial>>32));
}
static void expect_error(struct wl_client *client,int peer,uint32_t code) {
    wl_client_flush(client);
    uint32_t wire[512];ssize_t bytes=recv(peer,wire,sizeof(wire),MSG_DONTWAIT);
    assert(bytes>=20 && wire[0]==1 && (wire[1]&65535)==WL_DISPLAY_ERROR);
    assert(wire[3]==code);
}
static void protocol_errors(void) {
    for (unsigned which=0;which<6;++which) {
        struct wl_display *display=wl_display_create();assert(display);
        assert(xwayland_shell_init(display));int peer;
        struct wl_client *client=client_new(display,&peer);
        if (!which) {
            xwayland_shell_bind(client,NULL,1,0);
            expect_error(client,peer,WL_DISPLAY_ERROR_IMPLEMENTATION);
        } else {
            assert(xwayland_attach_client(client));
            struct wl_resource *shell=shell_new(client);
            struct brook_surface *s=surface_new(client);
            if (which==1) {
                s->role=1;xwayland_get_surface(client,shell,0,s->resource);
                expect_error(client,peer,XWAYLAND_SHELL_V1_ERROR_ROLE);
            } else {
                associate(client,shell,s,2);
                if (which==2 || which==3) {
                    xwayland_set_serial(client,s->xwayland_surface,which==2?0:2,0);
                    expect_error(client,peer,XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL);
                } else if (which==4) {
                    surface_commit(client,s->resource);
                    xwayland_set_serial(client,s->xwayland_surface,3,0);
                    surface_commit(client,s->resource);
                    expect_error(client,peer,XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED);
                } else {
                    wl_resource_destroy(s->xwayland_surface);
                    xwayland_get_surface(client,shell,0,s->resource);
                    expect_error(client,peer,XWAYLAND_SHELL_V1_ERROR_ROLE);
                }
            }
        }
        wl_client_destroy(client);close(peer);wl_display_destroy(display);
        assert(!g_xwayland_client && !g_surfaces);
    }
}
int main(void) {
    int peer,other_peer;
    struct wl_display *display=wl_display_create();assert(display);
    assert(xwayland_shell_init(display));
    struct wl_client *client=client_new(display,&peer),*other=client_new(display,&other_peer);
    assert(!xwayland_global_filter(client,g_xwayland_shell_global,NULL));
    assert(xwayland_attach_client(client));
    uint64_t first_generation=g_xwayland_generation;
    assert(!xwayland_attach_client(other));
    assert(xwayland_global_filter(client,g_xwayland_shell_global,NULL));
    assert(!xwayland_global_filter(other,g_xwayland_shell_global,NULL));
    struct wl_resource *shell=shell_new(client);
    struct brook_surface *a=surface_new(client),*b=surface_new(client),*native=surface_new(other);
    native->role=1;
    associate(client,shell,a,UINT64_C(0x100000001));
    assert(!xwayland_surface_by_serial(first_generation,UINT64_C(0x100000001)));
    /* Replacing pending state before a commit is permitted. A serial consumes
     * its unique value even when it is replaced before association. */
    xwayland_set_serial(client,a->xwayland_surface,2,1);
    surface_commit(client,a->resource);
    assert(!a->xdg_acked && a->xwayland_serial==UINT64_C(0x100000002));
    assert(xwayland_surface_by_serial(first_generation,a->xwayland_serial)==a);
    assert(!xwayland_surface_by_serial(first_generation,UINT64_C(0x100000001)));
    assert(!xwayland_surface_by_serial(first_generation+1,a->xwayland_serial));
    wl_resource_destroy(a->xwayland_surface);
    assert(!a->xwayland_surface && xwayland_surface_by_serial(first_generation,a->xwayland_serial)==a);
    associate(client,shell,b,UINT64_C(0x100000003));
    wl_resource_destroy(b->xwayland_surface);
    surface_commit(client,b->resource); /* pending serial survives role object */
    assert(xwayland_surface_by_serial(first_generation,UINT64_C(0x100000003))==b);
    wl_resource_destroy(a->resource);
    assert(!xwayland_surface_by_serial(first_generation,UINT64_C(0x100000002)));
    wl_client_destroy(client);close(peer);
    assert(!g_xwayland_client && g_surfaces==native);
    assert(!xwayland_surface_by_serial(first_generation,UINT64_C(0x100000003)));
    /* Same serial in a replacement server is a different namespace. Native
     * clients and their surfaces survive retiring the old server. */
    client=client_new(display,&peer);assert(xwayland_attach_client(client));
    assert(g_xwayland_generation!=first_generation);
    shell=shell_new(client);a=surface_new(client);associate(client,shell,a,1);
    surface_commit(client,a->resource);
    assert(xwayland_surface_by_serial(g_xwayland_generation,1)==a);
    assert(!xwayland_surface_by_serial(first_generation,1));
    wl_client_destroy(client);close(peer);wl_client_destroy(other);close(other_peer);
    assert(!g_surfaces);wl_display_destroy(display);
    protocol_errors();
    puts("XWAYLAND_SHELL_PASS exact client, commit ordering, role-object lifetime, surface destruction, server generation");
    return 0;
}
