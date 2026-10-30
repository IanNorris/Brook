/* Production logical/physical boundaries, pixels and protocol events at 2x. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
static long test_syscall(long,...);
#define syscall test_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall
static uint32_t framebuffer[48],cursor_pixels[4096],cursor_width,cursor_height;
static int32_t hotspot_x,hotspot_y;
static uint32_t native_width,native_height,presents;
static long test_syscall(long number,...) {
    if(number==0xB06){errno=EOPNOTSUPP;return -1;}
    va_list ap;va_start(ap,number);
    if(number==0xB04){long op=va_arg(ap,long);va_end(ap);return op==0?200:100;}
    if (number==BROOK_SYS_WM_CREATE_WINDOW || number==BROOK_SYS_WM_RESIZE_VFB) {
        if (number==BROOK_SYS_WM_RESIZE_VFB) (void)va_arg(ap,long);
        native_width=va_arg(ap,long);native_height=va_arg(ap,long);
        if (number==BROOK_SYS_WM_CREATE_WINDOW) (void)va_arg(ap,long);
        struct brook_wm_create_out *out=(void *)va_arg(ap,long);
        assert(native_width==8 && native_height==6);
        *out=(struct brook_wm_create_out){1,8,(uintptr_t)framebuffer};
    } else if (number==BROOK_SYS_WM_SET_CURSOR_IMAGE) {
        const uint32_t *px=(void *)va_arg(ap,long);
        cursor_width=va_arg(ap,long);cursor_height=va_arg(ap,long);
        hotspot_x=va_arg(ap,long);hotspot_y=va_arg(ap,long);
        assert(cursor_width*cursor_height<=4096);
        if(px)memcpy(cursor_pixels,px,cursor_width*cursor_height*4);
    } else if (number==0xB07) {
        (void)va_arg(ap,long);long op=va_arg(ap,long);const void *arg=(void *)va_arg(ap,long);
        if (op==SCENE_CTL_PRESENT) {
            const struct ScenePresentDesc *p=arg;
            assert(p->width==8 && p->height==6 && p->count==2);
            const struct SceneLayerDesc *l=(void *)(uintptr_t)p->layers;
            assert(l[0].w==8 && l[0].h==6 && l[0].sw==8*256 && l[0].sh==6*256);
            assert(l[1].x==2 && l[1].y==2 && l[1].w==4 && l[1].h==4);
            ++presents;
        } else assert(op==SCENE_CTL_RELEASE);
    }
    va_end(ap);return 0;
}
static struct brook_surface *make_surface(struct wl_client *client) {
    struct brook_surface *s=calloc(1,sizeof(*s));assert(s);surface_tree_init(s);
    s->resource=wl_resource_create(client,&wl_surface_interface,4,0);assert(s->resource);
    wl_resource_set_implementation(s->resource,&surface_impl,s,surface_destroy_userdata);
    s->next=g_surfaces;g_surfaces=s;return s;
}
static void commit_pixels(struct brook_surface *s,uint32_t *pixels,int w,int h) {
    struct brook_image *image=image_snapshot((void *)pixels,w,h,w*4,WL_SHM_FORMAT_ARGB8888);assert(image);
    assert(surface_tree_commit(s,image)==1);image_unref(image);
}
int main(void) {
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    struct wl_display *d=wl_display_create();assert(d);
    struct wl_client *client=wl_client_create(d,pair[0]);assert(client);
    output_scale_init();assert(g_output_scale==2);
    struct brook_surface *root=make_surface(client),*child=make_surface(client),*cursor=make_surface(client);
    root->xdg_toplevel=wl_resource_create(client,&xdg_toplevel_interface,1,0);
    root->xdg_surface=wl_resource_create(client,&xdg_surface_interface,1,0);
    root->xdg_acked=1;root->scene_disabled=1;
    struct wl_resource *sub=wl_resource_create(client,&wl_subcompositor_interface,1,0);
    subcomp_get_subsurface(client,sub,0,child->resource,root->resource);
    subsurface_set_position(client,child->subsurface_resource,1,1);
    surface_set_buffer_scale(client,root->resource,2);
    uint32_t background[48],overlay[4]={0xff110001,0xff220002,0xff330003,0xff440004};
    for(unsigned i=0;i<48;++i)background[i]=0xff000000|i;
    commit_pixels(child,overlay,2,2);commit_pixels(root,background,8,6);
    assert(surface_render(root));assert(native_width==8 && native_height==6);
    /* HiDPI source pixels survive exactly; low-resolution children scale at
     * the presentation boundary, with logical subsurface offsets doubled. */
    assert(framebuffer[0]==background[0] && framebuffer[1]==background[1]);
    assert(framebuffer[2+2*8]==overlay[0] && framebuffer[3+3*8]==overlay[0]);
    assert(framebuffer[4+2*8]==overlay[1] && framebuffer[2+4*8]==overlay[2]);
    root->vfb_w=1;assert(surface_window(root,root->tree.current->value));
    assert(native_width==8 && native_height==6);
    for(int p=-3;p<=3;++p)assert(surface_pointer_fixed(root,p)==p*128);
    assert(pointer_fixed_floor(surface_pointer_fixed(root,-1))==-1);
    struct pointer_rect rect={0,0,1,1,1};struct pointer_region region={&rect,1,0};
    assert(!pointer_region_contains(&region,pointer_fixed_floor(surface_pointer_fixed(root,-1)),0));
    assert(pointer_region_contains(&region,pointer_fixed_floor(surface_pointer_fixed(root,1)),0));
    assert(!pointer_region_contains(&region,pointer_fixed_floor(surface_pointer_fixed(root,2)),0));
    struct brook_surface x11={.role=4};surface_tree_init(&x11);
    assert(surface_output_scale(&x11)==1 && surface_pointer_fixed(&x11,3)==3*256);
    cursor->is_cursor=1;cursor->role=3;cursor->cursor_hot_x=1;cursor->cursor_hot_y=1;
    surface_set_buffer_scale(client,cursor->resource,2);commit_pixels(cursor,background,8,6);
    surface_cursor_commit(cursor);
    assert(cursor_width==8 && cursor_height==6 && hotspot_x==2 && hotspot_y==2);
    assert(!memcmp(cursor_pixels,background,sizeof(background)));
    cursor->cursor_output_scale=1;surface_cursor_commit(cursor);
    assert(cursor_width==4 && cursor_height==3 && hotspot_x==1 && hotspot_y==1);
    /* Exercise the real pointer dispatcher and decode the wire, including a
     * button before motion. Raw relative motion is intentionally unscaled. */
    struct brook_seat_client seat={.client=client,.cursor_visible=1};
    seat.pointer=wl_resource_create(client,&wl_pointer_interface,5,0);assert(seat.pointer);
    struct brook_wm_event event={.type=EVT_MOUSE_BTN_DOWN,.x=3,.y=-1};
    dispatch_surface_event(root,&seat,&event,42);
    event.type=EVT_MOUSE_MOVE;event.x=1;event.y=3;dispatch_surface_event(root,&seat,&event,43);
    struct brook_relative_pointer relative={.seat=&seat,.pointer=seat.pointer};
    relative.resource=wl_resource_create(client,&zwp_relative_pointer_v1_interface,1,0);assert(relative.resource);
    g_relative_pointers=&relative;
    event.type=EVT_MOUSE_RELATIVE;event.x=3;event.y=-1;dispatch_surface_event(root,&seat,&event,43);
    g_relative_pointers=NULL;
    wl_display_flush_clients(d);uint32_t wire[256];ssize_t bytes=recv(pair[1],wire,sizeof(wire),MSG_DONTWAIT);assert(bytes>0);
    unsigned entered=0,motion=0,raw=0;
    for(size_t i=0;i<(size_t)bytes/4;) {
        uint32_t size=wire[i+1]>>16;assert(size>=8 && i*4+size<=(size_t)bytes);
        if(wire[i]==wl_resource_get_id(seat.pointer)) {
            if((wire[i+1]&65535)==WL_POINTER_ENTER) {assert((int32_t)wire[i+4]==384 && (int32_t)wire[i+5]==-128);++entered;}
            if((wire[i+1]&65535)==WL_POINTER_MOTION) {assert(wire[i+3]==128 && wire[i+4]==384);++motion;}
        }
        if(wire[i]==wl_resource_get_id(relative.resource)) {
            assert((int32_t)wire[i+4]==768 && (int32_t)wire[i+5]==-256);
            assert((int32_t)wire[i+6]==768 && (int32_t)wire[i+7]==-256);++raw;
        }
        i+=size/4;
    }
    assert(entered==1 && motion==1 && raw==1);
    struct brook_wm_event resize={.type=WM_EVT_RESIZED,.x=1600,.y=1200};
    dispatch_surface_event(root,NULL,&resize,44);
    assert(root->configure_width==800 && root->configure_height==600);
    /* The scene service receives the same physical destinations as the CPU
     * renderer, while source rectangles remain in original buffer pixels. */
    struct brook_scene_owner *owner=calloc(1,sizeof(*owner));assert(owner);owner->refs=3;owner->fd=open("/dev/null",O_RDONLY);assert(owner->fd>=0);
    root->scene=owner;
    struct brook_image *a=((struct brook_view *)root->tree.current->value)->image;
    struct brook_image *b=((struct brook_view *)child->tree.current->value)->image;
    a->owner=b->owner=owner;a->version=1;b->version=2;
    assert(surface_render(root) && presents==1);
    struct brook_view *oversized=child->tree.current->value;
    uint32_t saved_width=oversized->geometry.w;oversized->geometry.w=10000;
    assert(!surface_render(root) && presents==1);
    oversized->geometry.w=saved_width;
    wl_client_destroy(client);assert(!g_surfaces);close(pair[1]);wl_display_destroy(d);
    puts("DISPLAY_SCALE_PASS 2x native buffers, exact HiDPI pixels, subsurfaces, scene rectangles, resize, fractional absolute input, negative region edges, cursors and X11 pixel contract");
    return 0;
}
