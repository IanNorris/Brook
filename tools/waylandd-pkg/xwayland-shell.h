#ifndef BROOK_XWAYLAND_SHELL_H
#define BROOK_XWAYLAND_SHELL_H
#include "xwayland-shell-v1-server-protocol.h"

/* The launcher must attach the exact wl_client made from its private socket
 * pair. This interface stays invisible and unusable until that happens. No UID
 * or client-supplied name grants authority. Serial state belongs to wl_surface,
 * independently of the shorter-lived xwayland_surface protocol object. */
static struct wl_client *g_xwayland_client;
static struct wl_global *g_xwayland_shell_global;
static struct wl_listener g_xwayland_client_destroy;
static uint64_t g_xwayland_generation, g_xwayland_last_serial;

static void xwayland_client_destroyed(struct wl_listener *listener,void *data) {
    (void)data;
    wl_list_remove(&listener->link);wl_list_init(&listener->link);
    g_xwayland_client=NULL;g_xwayland_last_serial=0;
    xwayland_generation_lost();
    /* Resource destroy order is unspecified. Retire native windows before a
     * replacement server can get an association; native Wayland clients stay. */
    for (struct brook_surface *s=g_surfaces;s;s=s->next) {
        if (s->role!=4 || s->xwayland_generation!=g_xwayland_generation) continue;
        surface_unmap(s);s->xwayland_serial=s->xwayland_pending_serial=0;
    }
}
static int xwayland_attach_client(struct wl_client *client) {
    if (!client || g_xwayland_client || g_xwayland_generation==UINT64_MAX) return 0;
    ++g_xwayland_generation;g_xwayland_last_serial=0;g_xwayland_client=client;
    g_xwayland_client_destroy.notify=xwayland_client_destroyed;
    wl_client_add_destroy_listener(client,&g_xwayland_client_destroy);
    return 1;
}
static bool xwayland_global_filter(const struct wl_client *client,
                                  const struct wl_global *global,void *data) {
    (void)data;
    return global!=g_xwayland_shell_global || client==g_xwayland_client;
}
static void xwayland_object_destroy(struct wl_client *client,struct wl_resource *resource) {
    (void)client;wl_resource_destroy(resource);
}
static void xwayland_surface_object_destroyed(struct wl_resource *resource) {
    struct brook_surface *s=wl_resource_get_user_data(resource);
    if (s && s->xwayland_surface==resource) s->xwayland_surface=NULL;
    /* Existing and pending surface state deliberately survives this object. */
}
static void xwayland_set_serial(struct wl_client *client,struct wl_resource *resource,
                                uint32_t lo,uint32_t hi) {
    struct brook_surface *s=wl_resource_get_user_data(resource);
    if (!s) return;
    uint64_t serial=(uint64_t)lo|((uint64_t)hi<<32);
    if (client!=g_xwayland_client || !serial || serial<=g_xwayland_last_serial) {
        wl_resource_post_error(resource,XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL,
                               "Xwayland serial must be nonzero, unique and increasing");return;
    }
    g_xwayland_last_serial=serial;s->xwayland_pending_serial=serial;
}
static const struct xwayland_surface_v1_interface brook_xwayland_surface_impl={
    .set_serial=xwayland_set_serial,.destroy=xwayland_object_destroy
};
static void xwayland_get_surface(struct wl_client *client,struct wl_resource *resource,
                                 uint32_t id,struct wl_resource *surface) {
    struct brook_surface *s=wl_resource_get_user_data(surface);
    if (client!=g_xwayland_client || !s || wl_resource_get_client(surface)!=client) {
        wl_client_post_implementation_error(client,"unauthorized Xwayland surface");return;
    }
    if (s->role) {
        wl_resource_post_error(resource,XWAYLAND_SHELL_V1_ERROR_ROLE,"surface already has a role");return;
    }
    struct wl_resource *role=wl_resource_create(client,&xwayland_surface_v1_interface,1,id);
    if (!role) { wl_client_post_no_memory(client);return; }
    s->role=4;s->xwayland_generation=g_xwayland_generation;s->xwayland_surface=role;
    wl_resource_set_implementation(role,&brook_xwayland_surface_impl,s,xwayland_surface_object_destroyed);
}
static const struct xwayland_shell_v1_interface brook_xwayland_shell_impl={
    .destroy=xwayland_object_destroy,.get_xwayland_surface=xwayland_get_surface
};
static void xwayland_shell_bind(struct wl_client *client,void *data,uint32_t version,uint32_t id) {
    (void)data;(void)version;
    if (client!=g_xwayland_client) {
        wl_client_post_implementation_error(client,"Xwayland shell is private to the owned server");return;
    }
    struct wl_resource *r=wl_resource_create(client,&xwayland_shell_v1_interface,1,id);
    if (!r) { wl_client_post_no_memory(client);return; }
    wl_resource_set_implementation(r,&brook_xwayland_shell_impl,NULL,NULL);
}
static int xwayland_shell_init(struct wl_display *display) {
    g_xwayland_shell_global=wl_global_create(display,&xwayland_shell_v1_interface,1,NULL,xwayland_shell_bind);
    if (!g_xwayland_shell_global) return 0;
    wl_display_set_global_filter(display,xwayland_global_filter,NULL);return 1;
}
static int xwayland_surface_commit(struct brook_surface *s) {
    if (s->role!=4 || !s->xwayland_pending_serial) return 1;
    if (s->xwayland_serial) {
        if (s->xwayland_surface)
            wl_resource_post_error(s->xwayland_surface,XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED,
                                   "surface association may only be committed once");
        else wl_client_post_implementation_error(wl_resource_get_client(s->resource),
                                                 "surface association may only be committed once");
        return 0;
    }
    s->xwayland_serial=s->xwayland_pending_serial;s->xwayland_pending_serial=0;
    return 1;
}
static struct brook_surface *xwayland_surface_by_serial(uint64_t generation,uint64_t serial) {
    if (!g_xwayland_client || !serial || generation!=g_xwayland_generation) return NULL;
    for (struct brook_surface *s=g_surfaces;s;s=s->next)
        if (s->role==4 && s->xwayland_generation==generation && s->xwayland_serial==serial)
            return s;
    return NULL;
}
#endif
