/* Production tree adapter: commit dependencies, pixels, callbacks and lifetime. */
#include <assert.h>
#include <stdarg.h>
static long test_syscall(long,...);
#define syscall test_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall
static unsigned sourceBegins,sourceTags;static uint64_t sourceCommit;
static long test_syscall(long number,...){
    va_list args;va_start(args,number);
    if(number==BROOK_SYS_WM_WINDOW_STATE){long window=va_arg(args,long);(void)va_arg(args,long);
        BrookWindowState *state=(void *)(uintptr_t)va_arg(args,long);*state=(BrookWindowState){.version=BROOK_WINDOW_STATE_VERSION,.window=(uint32_t)window,.incarnation=77};
        va_end(args);return 0;}
    if(number==BROOK_SYS_WM_BEGIN_SOURCE_WRITE){++sourceBegins;va_end(args);return 0;}
    if(number==BROOK_SYS_WM_PUBLISH_SOURCE_COMMIT){long window=va_arg(args,long),incarnation=va_arg(args,long);
        long commit=va_arg(args,long);assert(window==1 && incarnation==77 && commit>0);sourceCommit=(uint64_t)commit;++sourceTags;va_end(args);return 0;}
    va_end(args);return 0;
}
static struct brook_surface *make_surface(struct wl_client *client) {
    struct brook_surface *s=calloc(1,sizeof(*s));assert(s);surface_tree_init(s);
    s->resource=wl_resource_create(client,&wl_surface_interface,4,0);assert(s->resource);
    wl_resource_set_implementation(s->resource,&surface_impl,s,surface_destroy_userdata);
    s->next=g_surfaces;g_surfaces=s;return s;
}
static void snapshot(struct brook_surface *s,uint32_t color,int w,int h) {
    uint32_t px[12];assert(w*h<=12);for (int i=0;i<w*h;++i) px[i]=color;
    struct brook_image *image=image_snapshot((uint8_t *)px,w,h,w*4,WL_SHM_FORMAT_ARGB8888);assert(image);
    assert(surface_tree_commit(s,image));image_unref(image);
    if (!bt_effective_sync(&s->tree)) surface_repaint(s);
}
static void associate(struct wl_client *c,struct wl_resource *sc,struct brook_surface *child,struct brook_surface *parent) {
    subcomp_get_subsurface(c,sc,0,child->resource,parent->resource);assert(child->tree.parent==&parent->tree);
}
int main(void) {
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_display *display=wl_display_create();struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct wl_resource *sc=wl_resource_create(client,&wl_subcompositor_interface,1,0);assert(sc);
    struct brook_surface *r=make_surface(client),*p=make_surface(client),*c=make_surface(client);
    uint32_t vfb[12]={0};r->wm_id=1;r->vfb=vfb;r->vfb_w=4;r->vfb_h=3;r->vfb_stride=4;r->scene_disabled=1;
    assert(!setenv("BROOK_WAYLAND_GPU_SOCKET","/tmp/brook-retained-test",1));
    associate(client,sc,p,r);associate(client,sc,c,p);
    subsurface_set_position(client,p->subsurface_resource,1,1);
    snapshot(c,0xffff0000,1,1);snapshot(p,0xffabcdef,2,2);
    snapshot(c,0xff0000ff,1,1);snapshot(r,0xff102030,4,3);
    assert(vfb[5]==0xffff0000 && vfb[0]==0xff102030); // captured A, not newer B
    assert(sourceBegins && sourceTags && sourceCommit==r->tree.current->commit);
    assert(c->tree.cached && ((struct brook_view *)c->tree.cached->value)->image->pixels[0]==0xff0000ff);
    snapshot(r,0xff102030,4,3);assert(vfb[5]==0xffff0000);
    surface_commit(client,p->resource);surface_commit(client,r->resource);assert(vfb[5]==0xff0000ff);
    subsurface_place_below(client,c->subsurface_resource,p->resource);
    surface_commit(client,c->resource);assert(vfb[5]==0xff0000ff); // stacking is parent-owned
    surface_commit(client,p->resource);surface_commit(client,r->resource);assert(vfb[5]==0xffabcdef);
    subsurface_place_above(client,c->subsurface_resource,p->resource);
    snapshot(c,0x80800000,1,1);surface_commit(client,p->resource);surface_commit(client,r->resource);
    uint32_t expected=pixel_straight(pixel_over(0x80800000,0xffabcdef));assert(vfb[5]==expected);
    for (unsigned i=0;i<100;++i) { surface_repaint(r);assert(vfb[5]==expected); }
    // Parent position commits atomically; nested offsets clip only at the root.
    subsurface_set_position(client,c->subsurface_resource,-1,-1);
    surface_commit(client,c->resource);assert(vfb[0]==0xff102030);
    surface_commit(client,p->resource);surface_commit(client,r->resource);
    assert(vfb[0]==pixel_straight(pixel_over(0x80800000,0xff102030)));
    // A NULL image hides its subtree only when the synchronized cache applies.
    surface_attach(client,p->resource,NULL,0,0);surface_commit(client,p->resource);
    assert(vfb[0]!=0xff102030);surface_commit(client,r->resource);assert(vfb[0]==0xff102030);
    snapshot(p,0xffabcdef,2,2);surface_commit(client,r->resource);
    subsurface_set_desync(client,c->subsurface_resource);
    snapshot(c,0xff00ff00,1,1);assert(vfb[0]!=0xff00ff00); // inherited sync remains
    subsurface_set_desync(client,p->subsurface_resource);assert(vfb[0]==0xff00ff00);
    assert(sourceCommit==r->tree.current->commit && sourceTags==sourceBegins);
    // Destroying a role unmaps immediately; old snapshots retain safe dead nodes.
    struct wl_resource *role=c->subsurface_resource;wl_resource_destroy(role);assert(vfb[0]==0xff102030 && c->role==2);
    associate(client,sc,c,p);snapshot(c,0xff0000ff,1,1);surface_commit(client,p->resource);
    assert(vfb[5]==0xff0000ff);
    role=c->subsurface_resource;surface_destroy(client,c->resource);wl_resource_destroy(role);
    assert(vfb[5]==0xffabcdef);
    role=p->subsurface_resource;surface_destroy(client,r->resource);
    assert(!p->tree.parent && !p->subsurface_parent);wl_resource_destroy(role);surface_destroy(client,p->resource);
    assert(!g_surfaces);wl_client_destroy(client);wl_display_destroy(display);close(sockets[1]);
    unsetenv("BROOK_WAYLAND_GPU_SOCKET");
    puts("RETAINED_SURFACE_PASS exact A/B dependencies, parent stacking/position, alpha, inherited desync, NULL mapping, detach/deep lifetime");
}
