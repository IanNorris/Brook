/* Actual frontend adapter with a deterministic scene-service boundary. */
#include <assert.h>
#include <stdarg.h>
static long test_syscall(long,...);
#define syscall test_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall
struct image_slot { uint32_t pixel,w,h;int live; };
struct scene_slot { uint64_t next;struct image_slot images[64]; };
static struct scene_slot scenes[1024];
static unsigned captures,clones;
static int uncertain_capture;
static long present_result;
static uint32_t producer=0xff00ff00,screen[12];
static long test_syscall(long number,...) {
    va_list ap;va_start(ap,number);
    if (number==BROOK_SYS_WM_DESTROY_WINDOW) { va_end(ap);return 0; }
    if (number==0xB06) {
        int fd=open("/dev/null",O_RDWR);assert(fd>=0 && fd<1024);
        for (unsigned i=0;i<64;++i) assert(!scenes[fd].images[i].live);
        memset(&scenes[fd],0,sizeof(scenes[fd]));scenes[fd].next=1;va_end(ap);return fd;
    }
    assert(number==0xB07);
    int fd=(int)va_arg(ap,long);long op=va_arg(ap,long);void *arg=(void *)va_arg(ap,long);
    size_t size=(size_t)va_arg(ap,long);va_end(ap);assert(fd>=0 && fd<1024);
    struct scene_slot *scene=&scenes[fd];
    if (op==SCENE_CTL_RELEASE) {
        uint64_t v=(uintptr_t)arg;assert(!size && v<64 && scene->images[v].live);
        scene->images[v].live=0;return 0;
    }
    if (op==SCENE_CTL_PRESENT) {
        if (present_result) return present_result;
        const struct ScenePresentDesc *desc=arg;assert(size==sizeof(*desc) && desc->width==4 && desc->height==3);
        struct SceneLayerDesc *layers=(void *)(uintptr_t)desc->layers;memset(screen,0,sizeof(screen));
        for (unsigned i=0;i<desc->count;++i) {
            const struct SceneLayerDesc *l=&layers[i];assert(l->version<64 && scene->images[l->version].live);
            for (int y=0;y<3;++y) for (int x=0;x<4;++x)
                if (x>=l->x && y>=l->y && x<(int64_t)l->x+l->w && y<(int64_t)l->y+l->h)
                    screen[y*4+x]=pixel_over(scene->images[l->version].pixel,screen[y*4+x]);
        }
        return 0;
    }
    if (op==SCENE_CTL_CAPTURE_DMABUF && uncertain_capture) { ++captures;errno=EIO;return -1; }
    uint64_t v=scene->next++;assert(v<64);struct image_slot *out=&scene->images[v];out->live=1;
    if (op==SCENE_CTL_UPLOAD) {
        const struct SceneUploadDesc *d=arg;assert(size==sizeof(*d));
        out->pixel=*(uint32_t *)(uintptr_t)d->pixels;out->w=d->width;out->h=d->height;
    } else if (op==SCENE_CTL_CAPTURE_DMABUF) {
        const struct SceneDmabufCaptureDesc *d=arg;assert(size==sizeof(*d) && d->drmFd==42 && d->gem==7);
        assert(d->fourcc==BROOK_DMABUF_ARGB8888 && d->stride==4 && d->offset==0 && d->flags==0 && d->modifier==0);
        ++captures;out->pixel=producer;out->w=d->width;out->h=d->height;
    } else if (op==SCENE_CTL_CLONE) {
        const struct SceneCloneDesc *d=arg;assert(size==sizeof(*d));
        assert(d->sourceFd>=0 && d->sourceFd<1024 && d->version<64 && scenes[d->sourceFd].images[d->version].live);
        ++clones;*out=scenes[d->sourceFd].images[d->version];
    } else assert(!"unexpected scene operation");
    return (long)v;
}
static struct brook_surface *surface(struct wl_client *client) {
    struct brook_surface *s=calloc(1,sizeof(*s));assert(s);surface_tree_init(s);
    s->resource=wl_resource_create(client,&wl_surface_interface,4,0);assert(s->resource);
    wl_resource_set_implementation(s->resource,&surface_impl,s,surface_destroy_userdata);
    s->next=g_surfaces;g_surfaces=s;return s;
}
static void pixels(struct brook_surface *s,uint32_t color,int w,int h) {
    uint32_t px[12];for (int i=0;i<w*h;++i) px[i]=color;
    struct brook_image *image=image_snapshot((uint8_t *)px,w,h,w*4,WL_SHM_FORMAT_ARGB8888);assert(image);
    assert(surface_tree_commit(s,image));image_unref(image);
    if (!bt_effective_sync(&s->tree)) surface_repaint(s);
}
static unsigned releases(struct wl_display *display,int socket,uint32_t id) {
    wl_display_flush_clients(display);uint32_t wire[512];unsigned count=0;
    for (;;) {
        ssize_t bytes=recv(socket,wire,sizeof(wire),MSG_DONTWAIT);
        if (bytes<0) { assert(errno==EAGAIN);return count; }
        assert(bytes>0);
        for (size_t i=0;i<(size_t)bytes/4;) {
            unsigned size=wire[i+1]>>16;assert(size>=8 && i*4+size<=(size_t)bytes);
            if (wire[i]==id && (wire[i+1]&65535)==WL_BUFFER_RELEASE) ++count;
            i+=size/4;
        }
    }
}
int main(void) {
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_display *display=wl_display_create();struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct wl_resource *sc=wl_resource_create(client,&wl_subcompositor_interface,1,0);assert(sc);
    struct brook_surface *r=surface(client),*p=surface(client),*c=surface(client);
    uint32_t vfb[12]={0};r->wm_id=1;r->vfb=vfb;r->vfb_w=4;r->vfb_h=3;r->vfb_stride=4;g_drm_fd=42;
    subcomp_get_subsurface(client,sc,0,p->resource,r->resource);
    subcomp_get_subsurface(client,sc,0,c->resource,p->resource);
    subsurface_set_position(client,p->subsurface_resource,1,1);
    struct brook_dmabuf_buffer *db=calloc(1,sizeof(*db));assert(db);
    db->refs=1;db->width=db->height=1;db->stride=4;db->fourcc=0x34325241;db->bo_handle=7;db->gres=999;
    struct wl_resource *buffer=wl_resource_create(client,&wl_buffer_interface,1,0);assert(buffer);
    wl_resource_set_implementation(buffer,&dmabuf_buffer_impl,db,dmabuf_buffer_destroy);
    uint32_t id=wl_resource_get_id(buffer);
    surface_frame(client,c->resource,0);surface_attach(client,c->resource,buffer,0,0);surface_commit(client,c->resource);
    pixels(p,0,2,2); // P captures A before root is mapped.
    surface_frame(client,c->resource,0);surface_attach(client,c->resource,buffer,0,0);surface_commit(client,c->resource);
    assert(!captures && !releases(display,sockets[1],id));
    pixels(r,0xff112233,4,3);
    assert(captures==1 && screen[5]==0xff00ff00 && !releases(display,sockets[1],id));
    assert(wl_list_length(&c->committed_frames)==1 && wl_list_length(&c->waiting_frames)==1);
    struct brook_image *cached=((struct brook_view *)c->tree.cached->value)->image;
    assert(cached->source && cached->source->uses==1); // A cannot release B's producer.
    surface_commit(client,p->resource);surface_commit(client,r->resource);
    assert(captures==2 && screen[5]==0xff00ff00 && releases(display,sockets[1],id)==1);
    assert(wl_list_length(&c->committed_frames)==2 && wl_list_empty(&c->waiting_frames));
    producer=0xffff0000;surface_repaint(r);assert(captures==2 && screen[5]==0xff00ff00);
    present_result=-116;assert(surface_render(r) && screen[5]==0xff00ff00);
    present_result=0;assert(surface_render(r) && screen[5]==0xff00ff00 && captures==2);
    // Viewport-only commits share the completed image, without recapturing or
    // releasing a producer again (including synchronized child snapshots).
    struct wl_resource *manager=wl_resource_create(client,&wp_viewporter_interface,1,0);assert(manager);
    struct brook_image *storage=((struct brook_view *)c->tree.current->value)->image;
    viewporter_get_viewport(client,manager,0,c->resource);assert(c->viewport);
    viewport_set_destination(client,c->viewport,2,1);surface_commit(client,c->resource);
    assert(((struct brook_view *)c->tree.cached->value)->image==storage);
    assert(((struct brook_view *)c->tree.current->value)->geometry.w==1);
    surface_commit(client,p->resource);surface_commit(client,r->resource);
    assert(captures==2 && !releases(display,sockets[1],id) && screen[5]==0xff00ff00 && screen[6]==0xff00ff00);
    viewport_destroy(client,c->viewport);surface_commit(client,c->resource);
    surface_commit(client,p->resource);surface_commit(client,r->resource);
    assert(captures==2 && screen[5]==0xff00ff00 && screen[6]==0xff112233);
    // Retained images migrate after the old root window disappears, without
    // reading a released producer or publishing the new root prematurely.
    surface_unmap(r);r->wm_id=1;r->vfb=vfb;r->vfb_w=4;r->vfb_h=3;r->vfb_stride=4;
    surface_repaint(r);assert(clones==3 && captures==2 && screen[5]==0xff00ff00);
    surface_destroy(client,r->resource);surface_destroy(client,p->resource);surface_destroy(client,c->resource);
    assert(!g_surfaces);
    for (unsigned fd=0;fd<1024;++fd) for (unsigned v=0;v<64;++v) assert(!scenes[fd].images[v].live);
    // One uncertain use suppresses release across another cached use of the
    // same buffer, even when both protocol states are later destroyed.
    r=surface(client);p=surface(client);c=surface(client);
    r->wm_id=1;r->vfb=vfb;r->vfb_w=4;r->vfb_h=3;r->vfb_stride=4;
    subcomp_get_subsurface(client,sc,0,p->resource,r->resource);
    subcomp_get_subsurface(client,sc,0,c->resource,p->resource);
    surface_attach(client,c->resource,buffer,0,0);surface_commit(client,c->resource);pixels(p,0,2,2);
    surface_attach(client,c->resource,buffer,0,0);surface_commit(client,c->resource);
    uncertain_capture=1;pixels(r,0xff112233,4,3);
    assert(!releases(display,sockets[1],id));
    struct brook_image *waiting=((struct brook_view *)c->tree.cached->value)->image;
    assert(waiting->source->uncertain && waiting->source->uses==2);
    surface_destroy(client,r->resource);surface_destroy(client,p->resource);surface_destroy(client,c->resource);
    assert(!releases(display,sockets[1],id) && !g_surfaces);
    wl_client_destroy(client);wl_display_destroy(display);close(sockets[1]);
    puts("SCENE_SURFACE_PASS deferred dmabuf children, shared-buffer A/P/B release, callback eligibility, immutable pixels, cross-scene migration, cleanup");
}
