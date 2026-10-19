/* Exercise production import ownership with deterministic DRM and allocation failures. */
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
static int test_ioctl(int fd, unsigned long request, ...);
static void *test_calloc(size_t count, size_t size);
#define ioctl test_ioctl
#define calloc test_calloc
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef calloc
#undef ioctl

static uint32_t live[8];
static unsigned closes, info_calls;
static int fail_info, fail_alloc;

static void *test_calloc(size_t count, size_t size)
{
    if (fail_alloc) { fail_alloc = 0; errno = ENOMEM; return NULL; }
    return calloc(count, size);
}
static int test_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 42);
    va_list args; va_start(args, request); void *arg = va_arg(args, void *); va_end(args);
    if (request == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
        struct drm_prime_handle *ph = arg;
        uint32_t resource = ph->fd == 10 || ph->fd == 11 ? 100 : ph->fd == 20 ? 200 : 300;
        for (unsigned i = 1; i < 8; ++i) if (live[i] == resource) { ph->handle = i; return 0; }
        for (unsigned i = 1; i < 8; ++i) if (!live[i]) { live[i] = resource; ph->handle = i; return 0; }
        assert(!"mock GEM table exhausted");
    }
    if (request == DRM_IOCTL_VIRTGPU_RESOURCE_INFO) {
        struct drm_virtgpu_resource_info *ri = arg;
        assert(ri->bo_handle < 8 && live[ri->bo_handle]);
        ++info_calls;
        if (fail_info) { errno = EIO; return -1; }
        ri->res_handle = live[ri->bo_handle]; return 0;
    }
    if (request == DRM_IOCTL_GEM_CLOSE) {
        struct drm_gem_close *gc = arg;
        assert(gc->handle < 8 && live[gc->handle]);
        live[gc->handle] = 0; ++closes; return 0;
    }
    assert(!"unexpected ioctl"); return -1;
}
static struct wl_resource *buffer(struct wl_client *client, unsigned id, int prime)
{
    struct brook_dmabuf_buffer *db = calloc(1, sizeof(*db)); assert(db);
    db->refs = 1;
    db->gres = dmabuf_resolve_gres(prime, &db->bo_handle); assert(db->gres && db->bo_handle);
    struct wl_resource *resource = wl_resource_create(client, &wl_buffer_interface, 1, id); assert(resource);
    wl_resource_set_implementation(resource, &dmabuf_buffer_impl, db, dmabuf_buffer_destroy);
    return resource;
}
int main(void)
{
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *display = wl_display_create(); assert(display);
    struct wl_client *client = wl_client_create(display, sockets[0]); assert(client);
    g_drm_fd = 42;
    struct wl_resource *a1 = buffer(client, 2, 10), *a2 = buffer(client, 3, 11);
    assert(closes == 0 && info_calls == 1 && g_dmabuf_imports->refs == 2);
    uint32_t a_bo = ((struct brook_dmabuf_buffer *)wl_resource_get_user_data(a1))->bo_handle;
    assert(a_bo == ((struct brook_dmabuf_buffer *)wl_resource_get_user_data(a2))->bo_handle);
    wl_resource_destroy(a1); assert(closes == 0 && live[a_bo] == 100);
    struct wl_resource *b = buffer(client, 4, 20);
    uint32_t b_bo = ((struct brook_dmabuf_buffer *)wl_resource_get_user_data(b))->bo_handle;
    assert(b_bo != a_bo && live[b_bo] == 200);
    wl_resource_destroy(a2); assert(closes == 1 && !live[a_bo] && live[b_bo] == 200);
    struct wl_resource *a3 = buffer(client, 5, 10);
    assert(((struct brook_dmabuf_buffer *)wl_resource_get_user_data(a3))->bo_handle == a_bo);
    wl_resource_destroy(b); assert(closes == 2 && live[a_bo] == 100);
    wl_resource_destroy(a3); assert(closes == 3 && !g_dmabuf_imports);
    uint32_t bo = 123;
    fail_info = 1;
    assert(dmabuf_resolve_gres(30, &bo) == 0 && bo == 0);
    fail_info = 0;
    assert(closes == 4 && !g_dmabuf_imports);
    fail_alloc = 1; bo = 123;
    assert(dmabuf_resolve_gres(30, &bo) == 0 && bo == 0);
    assert(closes == 5 && !g_dmabuf_imports);
    /* Pending attachments keep the GEM alias after protocol destruction.
     * A new import must not reuse that live alias; replacing the attachment
     * releases it exactly once without touching the unrelated new import. */
    struct brook_surface surface={0};
    surface.resource=wl_resource_create(client,&wl_surface_interface,4,0);assert(surface.resource);
    wl_resource_set_user_data(surface.resource,&surface);
    struct wl_resource *pending=buffer(client,6,10);
    unsigned before=closes;
    uint32_t held_bo=((struct brook_dmabuf_buffer *)wl_resource_get_user_data(pending))->bo_handle;
    surface_attach(client,surface.resource,pending,0,0);
    wl_resource_destroy(pending);
    assert(surface.pending_buffer && !surface.pending_buffer->resource);
    assert(surface.pending_buffer->dmabuf->refs==1 && closes==before && live[held_bo]==100);
    struct wl_resource *other=buffer(client,7,20);
    uint32_t other_bo=((struct brook_dmabuf_buffer *)wl_resource_get_user_data(other))->bo_handle;
    assert(other_bo!=held_bo);
    surface_attach(client,surface.resource,NULL,0,0);
    assert(!surface.pending_buffer && closes==before+1 && !live[held_bo] && live[other_bo]==200);
    wl_resource_destroy(other);assert(closes==before+2);
    wl_resource_destroy(surface.resource);
    /* Destroying the surface with a still-pending attachment is balanced in
     * both resource-destruction orders, including a deferred final GEM close. */
    for (int early=0;early<2;++early) {
        struct brook_surface *dying=calloc(1,sizeof(*dying));assert(dying);
        surface_tree_init(dying);
        dying->resource=wl_resource_create(client,&wl_surface_interface,4,0);assert(dying->resource);
        wl_resource_set_implementation(dying->resource,&surface_impl,dying,surface_destroy_userdata);
        struct wl_resource *attached=buffer(client,0,10);
        uint32_t attached_bo=((struct brook_dmabuf_buffer *)wl_resource_get_user_data(attached))->bo_handle;
        unsigned old_closes=closes;
        surface_attach(client,dying->resource,attached,0,0);
        if (early) wl_resource_destroy(attached);
        wl_resource_destroy(dying->resource);
        if (!early) { assert(closes==old_closes && live[attached_bo]==100);wl_resource_destroy(attached); }
        assert(closes==old_closes+1 && !live[attached_bo]);
    }
    /* Client disconnect must release one shared handle exactly once. */
    buffer(client, 6, 10); buffer(client, 7, 11);
    wl_client_destroy(client);
    assert(closes == 10 && !g_dmabuf_imports);
    for (unsigned i = 1; i < 8; ++i) assert(!live[i]);
    wl_display_destroy(display); close(sockets[1]);
    puts("DMABUF_IMPORT_OWNERSHIP_PASS shared/reuse/info-failure/OOM/disconnect");
    return 0;
}
