/* Pointer events reach subsurfaces (e.g. Chromium's WaylandBubble dialogs)
 * with surface-local coordinates, honouring input regions, stacking and the
 * implicit button grab. Production handlers and real libwayland resources. */
#include <assert.h>
#include <stdarg.h>
static long test_syscall(long,...);
#define syscall test_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall
static long test_syscall(long number,...) { (void)number;return 0; }
static struct brook_surface *surface(struct wl_client *client) {
    struct brook_surface *s=calloc(1,sizeof(*s));assert(s);surface_tree_init(s);
    s->input_region.infinite=1; // As compositor_create_surface.
    s->resource=wl_resource_create(client,&wl_surface_interface,4,0);assert(s->resource);
    wl_resource_set_implementation(s->resource,&surface_impl,s,surface_destroy_userdata);
    s->next=g_surfaces;g_surfaces=s;return s;
}
static uint32_t px[64*64];
static void pixels(struct brook_surface *s,int w,int h) {
    struct brook_image *image=image_snapshot((uint8_t *)px,w,h,w*4,WL_SHM_FORMAT_ARGB8888);assert(image);
    assert(surface_tree_commit(s,image)==1);image_unref(image);
}
struct seen { uint32_t enter_surface,button_surface,motion_surface;int32_t x,y;unsigned enters,buttons,leaves; };
static struct seen drain(struct wl_display *d,int fd,uint32_t pointer) {
    struct seen out={0};uint32_t wire[4096];wl_display_flush_clients(d);
    ssize_t bytes=recv(fd,wire,sizeof(wire),MSG_DONTWAIT);
    if (bytes<0) { assert(errno==EAGAIN);return out; }
    static uint32_t focus;
    for (size_t off=0;off<(size_t)bytes;) {
        uint32_t *e=(void *)((char *)wire+off);unsigned n=e[1]>>16,op=e[1]&65535;
        assert(n>=8 && off+n<=(size_t)bytes);
        if (e[0]==pointer) {
            if (op==WL_POINTER_ENTER) { focus=e[3];out.enter_surface=e[3];out.x=(int32_t)e[4];out.y=(int32_t)e[5];++out.enters; }
            if (op==WL_POINTER_LEAVE) { ++out.leaves;focus=0; }
            if (op==WL_POINTER_MOTION) { out.motion_surface=focus;out.x=(int32_t)e[3];out.y=(int32_t)e[4]; }
            if (op==WL_POINTER_BUTTON) { out.button_surface=focus;++out.buttons; }
        }
        off+=n;
    }
    return out;
}
static void event(struct brook_surface *root,struct brook_seat_client *sc,unsigned type,int32_t x,int32_t y) {
    struct brook_wm_event e={.type=type,.x=x,.y=y};dispatch_surface_event(root,sc,&e,1);
}
int main(void) {
    for (unsigned scale=1;scale<=2;++scale) {
        g_output_scale=scale;
        int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
        struct wl_display *display=wl_display_create();struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
        seat_bind(client,NULL,5,0);struct brook_seat_client *sc=g_seat_clients;
        seat_get_pointer(client,sc->seat,0);uint32_t pointer=wl_resource_get_id(sc->pointer);
        struct brook_surface *root=surface(client),*bubble=surface(client),*below=surface(client);
        root->wm_id=1;
        struct wl_resource *sub=wl_resource_create(client,&wl_subcompositor_interface,1,0);assert(sub);
        subcomp_get_subsurface(client,sub,0,bubble->resource,root->resource);
        subsurface_set_position(client,bubble->subsurface_resource,30,40);
        subcomp_get_subsurface(client,sub,0,below->resource,root->resource);
        subsurface_set_position(client,below->subsurface_resource,0,0);
        subsurface_place_below(client,below->subsurface_resource,root->resource);
        pixels(bubble,20,10);pixels(below,64,64);pixels(root,64,64);
        uint32_t root_id=wl_resource_get_id(root->resource),bubble_id=wl_resource_get_id(bubble->resource);
        const int32_t s=(int32_t)scale;
        // A click on the bubble enters and presses the bubble, in its own coordinates.
        event(root,sc,EVT_MOUSE_MOVE,35*s,45*s);
        struct seen got=drain(display,sockets[1],pointer);
        assert(got.enter_surface==bubble_id && got.motion_surface==bubble_id);
        assert(got.x==5*256 && got.y==5*256);
        event(root,sc,EVT_MOUSE_BTN_DOWN,35*s,45*s);
        // Implicit grab: dragging off the bubble keeps it, coordinates go negative.
        event(root,sc,EVT_MOUSE_MOVE,10*s,10*s);
        got=drain(display,sockets[1],pointer);
        assert(got.button_surface==bubble_id && !got.leaves && got.motion_surface==bubble_id);
        assert(got.x==-20*256 && got.y==-30*256);
        event(root,sc,EVT_MOUSE_BTN_UP,10*s,10*s);
        got=drain(display,sockets[1],pointer);assert(got.button_surface==bubble_id);
        // After release the next motion goes to the root (the "below" child is under it).
        event(root,sc,EVT_MOUSE_MOVE,10*s,10*s);
        got=drain(display,sockets[1],pointer);
        assert(got.leaves==1 && got.enter_surface==root_id && got.x==10*256 && got.y==10*256);
        // An empty input region makes the bubble click-through.
        struct pointer_region empty={0};struct wl_resource *rr=wl_resource_create(client,&wl_region_interface,1,0);assert(rr);
        wl_resource_set_implementation(rr,&region_impl,&empty,NULL);
        surface_set_input_region(client,bubble->resource,rr);surface_commit(client,bubble->resource);
        surface_commit(client,root->resource); // Synchronised child applies with its parent.
        event(root,sc,EVT_MOUSE_MOVE,35*s,45*s);
        got=drain(display,sockets[1],pointer);assert(!got.enters && got.motion_surface==root_id);
        // A point outside every mapped layer still goes to the root.
        event(root,sc,EVT_MOUSE_BTN_DOWN,200*s,200*s);event(root,sc,EVT_MOUSE_BTN_UP,200*s,200*s);
        got=drain(display,sockets[1],pointer);assert(got.buttons==2 && got.button_surface==root_id);
        pointer_region_clear(&empty);
        wl_client_destroy(client);wl_display_destroy(display);close(sockets[1]);
        g_seat_clients=NULL;
    }
    puts("pointer-hit-test: ok");
    return 0;
}
