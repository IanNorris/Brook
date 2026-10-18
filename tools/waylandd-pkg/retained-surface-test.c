/* Pixel and lifetime regressions against the production server implementation. */
#include <assert.h>
#define main brook_waylandd_main
#include "waylandd.c"
#undef main

static struct brook_surface *make_surface(struct wl_client *client, unsigned id) {
    struct brook_surface *s = calloc(1, sizeof(*s));
    assert(s);
    wl_list_init(&s->pending_frames); wl_list_init(&s->committed_frames);
    s->resource = wl_resource_create(client, &wl_surface_interface, 4, id);
    assert(s->resource);
    wl_resource_set_implementation(s->resource, &surface_impl, s, surface_destroy_userdata);
    s->next = g_surfaces; g_surfaces = s;
    return s;
}
static void snapshot(struct brook_surface *s, uint32_t *px, int w, int h) {
    assert(surface_snapshot(s, (uint8_t *)px, w, h, w * 4, WL_SHM_FORMAT_ARGB8888));
}
int main(void) {
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *display = wl_display_create(); assert(display);
    struct wl_client *client = wl_client_create(display, sockets[0]); assert(client);
    struct wl_resource *subcomp = wl_resource_create(client, &wl_subcompositor_interface, 1, 2);
    assert(subcomp);
    struct brook_surface *root = make_surface(client, 3), *child = make_surface(client, 4);
    uint32_t vfb[12] = {0}; root->vfb = vfb; root->vfb_w = 4; root->vfb_h = 3; root->vfb_stride = 4;
    uint32_t base[12]; for (unsigned i = 0; i < 12; ++i) base[i] = 0xff204060;
    uint32_t video[4] = {0xffabcdef,0xffabcdef,0xffabcdef,0xffabcdef};
    subcomp_get_subsurface(client, subcomp, 5, child->resource, root->resource);
    subsurface_set_position(client, child->subsurface_resource, 1, 1);
    /* A child may commit before its root has mapped. Release/reuse of the
     * client's original pixels must not alter the retained image. */
    snapshot(child, video, 2, 2); video[0] = 0;
    snapshot(root, base, 4, 3); surface_repaint(root);
    assert(vfb[5] == 0xffabcdef && vfb[0] == 0xff204060);
    for (unsigned i = 0; i < 12; ++i) base[i] = 0xff102030;
    snapshot(root, base, 4, 3); surface_repaint(root);
    assert(vfb[5] == 0xffabcdef && vfb[0] == 0xff102030);
    surface_commit(client, root->resource);
    surface_commit(client, child->resource);
    assert(root->image && child->image && vfb[5] == 0xffabcdef);
    struct brook_surface *upper = make_surface(client, 6);
    subcomp_get_subsurface(client, subcomp, 7, upper->resource, root->resource);
    subsurface_set_position(client, upper->subsurface_resource, 1, 1);
    uint32_t blue = 0xff0000ff; snapshot(upper, &blue, 1, 1);
    surface_repaint(root); assert(vfb[5] == blue);
    snapshot(root, base, 4, 3); surface_repaint(root); assert(vfb[5] == blue);
    struct wl_resource *upper_role = upper->subsurface_resource;
    surface_destroy(client, upper->resource);
    assert(vfb[5] == 0xffabcdef);
    wl_resource_destroy(upper_role);
    /* Repeated alpha composition must not accumulate darkening, and ARGB
     * channels are already premultiplied, including the output alpha. */
    uint32_t alpha = 0x80800000; snapshot(child, &alpha, 1, 1);
    uint32_t expected = pixel_straight(pixel_over(alpha, base[5]));
    assert(expected == 0xff881018);
    for (unsigned i = 0; i < 300; ++i) { surface_repaint(root); assert(vfb[5] == expected); }
    assert(vfb[6] == base[6]); /* shrinking a child clears its old extent */
    assert(pixel_straight(pixel_over(0x80800000, 0x80000080)) == 0xc0aa0055);
    /* Nested negative offsets clip at the root, not the intermediate image. */
    struct brook_surface *grand = make_surface(client, 8);
    subcomp_get_subsurface(client, subcomp, 9, grand->resource, child->resource);
    uint32_t green = 0xff00ff00; snapshot(grand, &green, 1, 1);
    subsurface_set_position(client, grand->subsurface_resource, -1, -1);
    surface_repaint(root); assert(vfb[0] == green);
    subsurface_set_position(client, child->subsurface_resource, -1, -1);
    surface_repaint(root); assert(vfb[0] == base[0] && vfb[5] == base[5]);
    /* Invalid replacement leaves the last committed pixels intact. */
    assert(!surface_snapshot(child, (uint8_t *)&green, 2, 1, 4, WL_SHM_FORMAT_ARGB8888));
    assert(child->image_w == 1 && child->image[0] == alpha);
    subsurface_set_position(client, child->subsurface_resource, 1, 1);
    surface_attach(client, child->resource, NULL, 0, 0);
    surface_commit(client, child->resource);
    assert(!child->image && vfb[0] == base[0] && vfb[5] == base[5]);
    snapshot(child, &alpha, 1, 1); surface_repaint(root);
    /* Destruction of a live child restores its parent's pixels. A later role
     * destruction must not dereference the freed child. */
    struct wl_resource *role = grand->subsurface_resource;
    surface_destroy(client, grand->resource); wl_resource_destroy(role);
    assert(vfb[0] == base[0]);
    role = child->subsurface_resource;
    wl_resource_destroy(role); /* same path used on client disconnect */
    assert(!child->subsurface_resource && !child->subsurface_parent && !root->children);
    assert(vfb[5] == base[5]);
    /* Orphan -> role destruction -> child destruction was a use-after-free. */
    subcomp_get_subsurface(client, subcomp, 10, child->resource, root->resource);
    role = child->subsurface_resource;
    surface_destroy(client, root->resource);
    assert(!child->subsurface_parent && child->subsurface_resource == role);
    wl_resource_destroy(role);
    assert(!child->subsurface_resource);
    surface_destroy(client, child->resource);
    assert(!g_surfaces);
    wl_display_destroy(display); close(sockets[1]);
    puts("RETAINED_SURFACE_PASS parent-repaint alpha clip resize detach lifetime");
}
