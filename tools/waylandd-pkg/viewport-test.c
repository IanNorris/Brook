/* Production protocol handlers, immutable commits and CPU pixel sampling. */
#include <assert.h>
#include <stdarg.h>
static long test_syscall(long,...);
#define syscall test_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall
static uint32_t cursor_pixels[64*64],cursor_w,cursor_h;
static int32_t cursor_x,cursor_y;
static long test_syscall(long number,...) {
    if(number==BROOK_SYS_WM_SET_CURSOR_IMAGE) {
        va_list ap;va_start(ap,number);const uint32_t *pixels=(void *)va_arg(ap,long);
        cursor_w=va_arg(ap,long);cursor_h=va_arg(ap,long);cursor_x=va_arg(ap,long);cursor_y=va_arg(ap,long);va_end(ap);
        assert(cursor_w<=64 && cursor_h<=64);if(pixels)memcpy(cursor_pixels,pixels,cursor_w*cursor_h*4);
    }
    return 0;
}
static struct brook_surface *surface(struct wl_client *client) {
    struct brook_surface *s=calloc(1,sizeof(*s));assert(s);surface_tree_init(s);
    s->resource=wl_resource_create(client,&wl_surface_interface,4,0);assert(s->resource);
    wl_resource_set_implementation(s->resource,&surface_impl,s,surface_destroy_userdata);
    s->next=g_surfaces;g_surfaces=s;return s;
}
static void commit_pixels(struct brook_surface *s,const uint32_t *px,int w,int h) {
    struct brook_image *image=image_snapshot((const uint8_t *)px,w,h,w*4,WL_SHM_FORMAT_ARGB8888);assert(image);
    assert(surface_tree_commit(s,image)==1);image_unref(image);
}
static struct brook_view *current(struct brook_surface *s) { return s->tree.current->value; }
static void protocol_error(unsigned scenario) {
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_display *display=wl_display_create();struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct brook_surface *s=surface(client);
    struct wl_resource *manager=wl_resource_create(client,&wp_viewporter_interface,1,0);assert(manager);
    viewporter_get_viewport(client,manager,0,s->resource);
    struct wl_resource *vp=s->viewport,*error_object=vp;uint32_t expected=WP_VIEWPORT_ERROR_BAD_VALUE;
    switch(scenario) {
    case 0:viewport_set_source(client,vp,-256,0,-256,-256);break;
    case 1:viewport_set_destination(client,vp,0,1);break;
    case 2:viewport_set_destination(client,vp,-1,1);break;
    case 3:viewporter_get_viewport(client,manager,0,s->resource);error_object=manager;expected=WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS;break;
    case 4:surface_destroy(client,s->resource);viewport_set_source(client,vp,0,0,256,256);expected=WP_VIEWPORT_ERROR_NO_SURFACE;break;
    case 5:surface_set_buffer_scale(client,s->resource,0);error_object=s->resource;expected=WL_SURFACE_ERROR_INVALID_SCALE;break;
    case 6:surface_set_buffer_transform(client,s->resource,8);error_object=s->resource;expected=WL_SURFACE_ERROR_INVALID_TRANSFORM;break;
    case 7:viewport_set_source(client,vp,0,0,128,256);assert(surface_tree_commit(s,NULL)==-1);expected=WP_VIEWPORT_ERROR_BAD_SIZE;break;
    case 8:{
        uint32_t px=0;struct brook_image *image=image_snapshot((const uint8_t *)&px,1,1,4,WL_SHM_FORMAT_ARGB8888);assert(image);
        viewport_set_source(client,vp,256,0,256,256);
        assert(surface_tree_commit(s,NULL)==1); // No out-of-buffer error without a buffer.
        assert(surface_tree_commit(s,image)==-1);image_unref(image);expected=WP_VIEWPORT_ERROR_OUT_OF_BUFFER;break;
    }
    case 9:{
        uint32_t px[6]={0};struct brook_image *image=image_snapshot((const uint8_t *)px,3,2,12,WL_SHM_FORMAT_ARGB8888);assert(image);
        surface_set_buffer_scale(client,s->resource,2);assert(surface_tree_commit(s,image)==-1);
        image_unref(image);error_object=s->resource;expected=WL_SURFACE_ERROR_INVALID_SIZE;break;
    }
    default:assert(0);
    }
    uint32_t object=wl_resource_get_id(error_object);wl_display_flush_clients(display);
    uint32_t wire[512];ssize_t bytes=recv(sockets[1],wire,sizeof(wire),MSG_DONTWAIT);assert(bytes>0);
    unsigned errors=0;
    for(size_t i=0;i<(size_t)bytes/4;) {
        unsigned size=wire[i+1]>>16;assert(size>=8 && i*4+size<=(size_t)bytes);
        if(wire[i]==1 && (wire[i+1]&65535)==WL_DISPLAY_ERROR) {assert(wire[i+2]==object && wire[i+3]==expected);++errors;}
        i+=size/4;
    }
    assert(errors==1);wl_client_destroy(client);assert(!g_surfaces);wl_display_destroy(display);close(sockets[1]);
}
int main(void) {
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_display *display=wl_display_create();struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct wl_resource *vpman=wl_resource_create(client,&wp_viewporter_interface,1,0);assert(vpman);
    struct wl_resource *sc=wl_resource_create(client,&wl_subcompositor_interface,1,0);assert(sc);
    struct brook_surface *root=surface(client),*child=surface(client);
    uint32_t screen[12]={0};root->wm_id=1;root->vfb=screen;root->vfb_w=4;root->vfb_h=3;root->vfb_stride=4;root->scene_disabled=1;
    const uint32_t grid[]={0xff000001,0xff000002,0xff000003,0xff000004,0xff000005,0xff000006};
    uint32_t black[12];for(unsigned i=0;i<12;++i)black[i]=0xff000000;
    subcomp_get_subsurface(client,sc,0,child->resource,root->resource);
    viewporter_get_viewport(client,vpman,0,child->resource);assert(child->viewport);
    struct wl_resource *vp=child->viewport;
    viewport_set_source(client,vp,256,0,2*256,2*256);
    viewport_set_destination(client,vp,2,2);
    commit_pixels(child,grid,3,2);commit_pixels(root,black,4,3);
    struct brook_view *first=current(child);view_ref(first);
    assert(first->geometry.sx==256 && first->geometry.w==2);
    assert(surface_render(root));assert(screen[0]==grid[1] && screen[1]==grid[2] && screen[4]==grid[4]);
    // Cached metadata B stays hidden until the parent commits.
    viewport_set_source(client,vp,0,0,256,2*256);
    viewport_set_destination(client,vp,1,2);surface_commit(client,child->resource);
    assert(current(child)==first);
    assert(screen[0]==grid[1] && screen[1]==grid[2]);
    surface_commit(client,root->resource);
    assert(current(child)->geometry.w==1 && current(child)->image==first->image);
    assert(screen[0]==grid[0] && screen[1]==black[1] && screen[4]==grid[3]);
    assert(first->geometry.sx==256 && first->geometry.w==2);view_unref(first);
    // Viewport destruction is pending until commit, and can be recreated.
    viewport_destroy(client,vp);assert(!child->viewport && current(child)->geometry.w==1);
    surface_commit(client,child->resource);surface_commit(client,root->resource);
    assert(current(child)->geometry.w==3 && screen[2]==grid[2]);
    viewporter_get_viewport(client,vpman,0,child->resource);vp=child->viewport;
    viewport_set_source(client,vp,128,0,2*256,2*256);viewport_set_destination(client,vp,2,2);
    surface_commit(client,child->resource);surface_commit(client,root->resource);
    assert(screen[0]==grid[1] && screen[1]==grid[2]);
    // Unsetting source and destination restores the scale/transform mapping.
    viewport_set_source(client,vp,-256,-256,-256,-256);viewport_set_destination(client,vp,-1,-1);
    surface_set_buffer_transform(client,child->resource,1);
    surface_commit(client,child->resource);surface_commit(client,root->resource);
    assert(current(child)->geometry.w==2 && current(child)->geometry.h==3);
    assert(screen[0]==grid[3] && screen[1]==grid[0] && screen[8]==grid[5]);
    surface_attach(client,child->resource,NULL,0,0);surface_commit(client,child->resource);surface_commit(client,root->resource);
    assert(!child->tree.current->value && screen[0]==black[0]);
    // A viewport may outlive its surface, without retaining a dangling pointer.
    surface_destroy(client,child->resource);assert(!wl_resource_get_user_data(vp));viewport_destroy(client,vp);
    struct brook_surface *cursor=surface(client);cursor->is_cursor=1;cursor->role=3;cursor->cursor_hot_x=1;cursor->cursor_hot_y=2;
    cursor->geometry.transform=1;commit_pixels(cursor,grid,3,2);surface_cursor_commit(cursor);
    assert(cursor_w==2 && cursor_h==3 && cursor_x==1 && cursor_y==2);
    assert(cursor_pixels[0]==grid[3] && cursor_pixels[1]==grid[0] && cursor_pixels[4]==grid[5]);
    cursor->geometry.transform=2;surface_cursor_commit(cursor);assert(cursor_w==2 && cursor_pixels[0]==grid[3]);
    surface_commit(client,cursor->resource);assert(cursor_w==3 && cursor_h==2 && cursor_pixels[0]==grid[5]);
    uint32_t wide[130];for(unsigned i=0;i<130;++i)wide[i]=0xff000000|i;
    cursor->geometry.transform=0;cursor->geometry.destination_set=1;cursor->geometry.dw=65;cursor->geometry.dh=1;
    commit_pixels(cursor,wide,130,1);surface_cursor_commit(cursor);
    assert(cursor_w==64 && cursor_h==1);
    for(unsigned x=0;x<64;++x)assert(cursor_pixels[x]==wide[(2*x+1)*130/(2*64)]);
    surface_destroy(client,cursor->resource);
    surface_destroy(client,root->resource);assert(!g_surfaces);
    wl_client_destroy(client);wl_display_destroy(display);close(sockets[1]);
    for(unsigned scenario=0;scenario<10;++scenario)protocol_error(scenario);
    puts("VIEWPORT_PASS cropped/scaled pixels, immutable cached geometry, metadata-only storage, destroy/recreate, fractional source, transform, NULL mapping and detached lifetime");
}
