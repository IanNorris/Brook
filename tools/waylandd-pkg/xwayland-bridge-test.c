/* Actual Wayland/bridge/retained-surface adapter with only the kernel ABI mocked.
 * This is not a proof of the real GPU or native kernel paths (VM gates remain). */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include "wm-window-state.h"
static long bridge_syscall(long number,...);
#define syscall bridge_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall
static BrookWindowState native[64];
static uint32_t pixels[64][64*64],next_window,created,destroyed,resized,placed;
static long bridge_syscall(long number,...) {
    va_list ap;va_start(ap,number);long result=0;
    if (number==BROOK_SYS_WM_CREATE_WINDOW) {
        long width=va_arg(ap,long),height=va_arg(ap,long);(void)va_arg(ap,long);
        struct brook_wm_create_out *out=(void *)va_arg(ap,long);long flags=va_arg(ap,long);
        uint32_t id=++next_window;assert(id<64 && width<=64 && height<=64);++created;
        native[id]=(BrookWindowState){.version=1,.window=id,.incarnation=100+id,
            .client_x=40,.client_y=80,.width=width,.height=height,
            .flags=BROOK_WINDOW_STATE_VISIBLE|((flags&1)?BROOK_WINDOW_STATE_CLIENT_CHROME:0)};
        *out=(struct brook_wm_create_out){id,64,(uintptr_t)pixels[id]};
    } else if (number==BROOK_SYS_WM_DESTROY_WINDOW) {
        long id=va_arg(ap,long);assert(id>0 && id<64);native[id].version=0;++destroyed;
    } else if (number==BROOK_SYS_WM_WINDOW_STATE) {
        long id=va_arg(ap,long);uint64_t identity=va_arg(ap,long);
        BrookWindowState *out=(void *)va_arg(ap,long);long size=va_arg(ap,long);
        assert(id>0 && id<64 && size==sizeof(*out));
        if (!native[id].version) { errno=ENOENT;result=-1; }
        else if (identity && identity!=native[id].incarnation) { errno=ESTALE;result=-1; }
        else *out=native[id];
    } else if (number==BROOK_SYS_WM_PLACE_WINDOW) {
        long id=va_arg(ap,long);uint64_t identity=va_arg(ap,long);
        long x=va_arg(ap,long),y=va_arg(ap,long);assert(native[id].incarnation==identity);
        native[id].client_x=x;native[id].client_y=y;++placed;
    } else if (number==BROOK_SYS_WM_RESIZE_VFB) {
        long id=va_arg(ap,long),width=va_arg(ap,long),height=va_arg(ap,long);
        struct brook_wm_create_out *out=(void *)va_arg(ap,long);
        assert(id>0 && id<64 && width<=64 && height<=64);++resized;
        native[id].width=width;native[id].height=height;
        *out=(struct brook_wm_create_out){id,64,(uintptr_t)pixels[id]};
    } else if (number==0xB06) { errno=ENOSYS;result=-1; }
    va_end(ap);return result;
}
static struct wl_client *new_client(int *peer) {
    int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));
    struct wl_client *client=wl_client_create(g_display,fds[0]);assert(client);*peer=fds[1];return client;
}
static struct brook_surface *new_surface(struct wl_client *client,uint64_t serial) {
    struct wl_resource *compositor=wl_resource_create(client,&wl_compositor_interface,4,0);assert(compositor);
    compositor_create_surface(client,compositor,0);struct brook_surface *s=g_surfaces;
    if (serial) {
        struct wl_resource *shell=wl_resource_create(client,&xwayland_shell_v1_interface,1,0);assert(shell);
        xwayland_get_surface(client,shell,0,s->resource);
        xwayland_set_serial(client,s->xwayland_surface,(uint32_t)serial,(uint32_t)(serial>>32));
        surface_commit(client,s->resource);
    }
    return s;
}
static void image(struct brook_surface *s,unsigned width,unsigned height) {
    uint32_t source[64*64];for (unsigned i=0;i<width*height;++i) source[i]=0xff123456;
    struct brook_image *image=image_snapshot((const uint8_t *)source,width,height,width*4,WL_SHM_FORMAT_ARGB8888);assert(image);
    assert(surface_tree_commit(s,image)>0);image_unref(image);assert(surface_repaint(s));
}
static struct bxwm_message command_from_peer(int fd,struct bxwm_stream *stream,uint32_t type) {
    struct bxwm_message message;assert(bxwm_receive(fd,stream,&message)==1);
    assert(message.type==type && bxwm_valid(&message,g_xwayland_generation,0));return message;
}
int main(void) {
    g_display=wl_display_create();assert(g_display && xwayland_shell_init(g_display));
    int peer,wayland_peer,other_peer;int control[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,control));
    g_xwm_fd=control[0];peer=control[1];struct bxwm_stream rx={0};
    struct wl_client *other=new_client(&other_peer);struct brook_surface *unrelated=new_surface(other,0);
    struct wl_client *client=new_client(&wayland_peer);assert(xwayland_attach_client(client));
    struct bxwm_message state={.version=BXWM_VERSION,.type=BXWM_WINDOW,
        .generation=g_xwayland_generation,.identity=1,.serial=55,.window=123,.width=2,.height=2,.flags=BXWM_MAPPED|BXWM_INPUT};
    assert(xwayland_accept_message(&state));assert(g_xwindows && !g_xwindows->surface);
    struct brook_surface *s=new_surface(client,55);assert(s->xwindow && !s->xdg_acked && !s->wm_id);
    image(s,2,2);assert(s->wm_id && created==1 && pixels[s->wm_id][0]==0xff123456);
    struct bxwm_message configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);
    assert(configure.x==40 && configure.y==80 && configure.configure_serial==1);
    state.configure_serial=configure.configure_serial;state.x=40;state.y=80;assert(xwayland_accept_message(&state));
    // Native drag/resize sends new geometry; an older X acknowledgement cannot
    // revert it or let an old-size buffer resize the native window backwards.
    uint32_t id=s->wm_id;native[id].client_x=100;native[id].client_y=120;native[id].width=3;
    xwayland_native_sync(s,0);configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);
    assert(configure.configure_serial==2 && configure.width==3);
    unsigned before=resized;assert(xwayland_accept_message(&state));assert(native[id].width==3 && native[id].client_x==100);
    assert(surface_repaint(s) && resized==before);image(s,3,2);assert(s->vfb_w==3 && native[id].width==3);
    state.configure_serial=2;state.x=100;state.y=120;state.width=3;assert(xwayland_accept_message(&state));
    state.x=110;state.width=4;assert(xwayland_accept_message(&state));assert(native[id].client_x==110 && native[id].width==4);
    image(s,4,2);
    assert(surface_tree_commit(s,NULL)>0 && surface_repaint(s));
    assert(!s->wm_id && s->xwindow && !s->xwindow->native_valid && destroyed==1);
    image(s,4,2);id=s->wm_id;assert(created==2 && !g_xwm_failed);
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);assert(configure.configure_serial==3);
    state.configure_serial=3;assert(xwayland_accept_message(&state));
    // Second independently managed X window, transient to the first's identity.
    struct bxwm_message menu=state;menu.window=124;menu.identity=2;menu.serial=56;
    menu.configure_serial=0;menu.parent=123;menu.parent_identity=1;menu.x=115;menu.y=125;
    menu.width=2;menu.height=2;menu.flags=BXWM_MAPPED|BXWM_OVERRIDE;
    struct brook_surface *popup=new_surface(client,56);assert(!popup->xwindow);
    assert(xwayland_accept_message(&menu));image(popup,2,2);
    assert(popup->wm_id!=id && created==3 && (native[popup->wm_id].flags&BROOK_WINDOW_STATE_CLIENT_CHROME));
    command_from_peer(peer,&rx,BXWM_CONFIGURE);
    xwayland_window_event(s,WM_EVT_FOCUS_GAINED);command_from_peer(peer,&rx,BXWM_FOCUS);
    xwayland_window_event(s,WM_EVT_CLOSE_REQUESTED);command_from_peer(peer,&rx,BXWM_CLOSE);
    struct bxwm_message dead=state;dead.type=BXWM_DESTROY;dead.identity=999;
    assert(xwayland_accept_message(&dead) && s->wm_id==id);
    state.flags=0;assert(xwayland_accept_message(&state));assert(!s->wm_id && destroyed==2);
    wl_resource_destroy(s->resource);
    // Keep a mapped native toplevel, not just an unconfigured wl_surface,
    // through retirement. Its window identity and retained pixels must stay.
    struct wl_resource *base=wl_resource_create(other,&xdg_wm_base_interface,1,0);assert(base);
    wm_base_get_xdg_surface(other,base,0,unrelated->resource);
    xdg_surface_get_toplevel(other,unrelated->xdg_surface,0);
    surface_commit(other,unrelated->resource);
    assert(unrelated->next_configure_serial);
    xdg_surface_ack_configure(other,unrelated->xdg_surface,unrelated->next_configure_serial);
    image(unrelated,2,2);
    uint32_t native_id=unrelated->wm_id;assert(native_id && created==4);
    uint64_t native_incarnation=native[native_id].incarnation;
    uint64_t first_generation=g_xwayland_generation;
    // Helper EOF retires the complete owned generation, including the popup,
    // without disconnecting an unrelated native Wayland client.
    close(peer);xwayland_control_ready(g_xwm_fd,WL_EVENT_READABLE,NULL);assert(g_xwm_failed);
    xwayland_pump();assert(!g_xwayland_client && !g_xwindows && g_xwm_fd<0);
    assert(destroyed==3 && g_surfaces==unrelated && unrelated->resource);
    assert(unrelated->wm_id==native_id && native[native_id].incarnation==native_incarnation);
    assert(pixels[native_id][0]==0xff123456 && surface_repaint(unrelated));
    close(wayland_peer);
    // A replacement server may reuse XIDs, identities and serials. A delayed
    // message from the retired generation must not bind to its new window.
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,control));
    g_xwm_fd=control[0];peer=control[1];memset(&rx,0,sizeof(rx));
    client=new_client(&wayland_peer);assert(xwayland_attach_client(client));
    assert(g_xwayland_generation>first_generation);
    state.flags=BXWM_MAPPED|BXWM_INPUT;state.configure_serial=0;
    assert(!xwayland_accept_message(&state) && !g_xwindows);
    state.generation=g_xwayland_generation;state.width=2;state.height=2;
    assert(xwayland_accept_message(&state));s=new_surface(client,55);image(s,2,2);
    assert(s->wm_id && s->wm_id!=native_id && created==5);
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);
    assert(configure.generation==g_xwayland_generation && configure.configure_serial==1);
    struct bxwm_message stale=state;stale.generation=first_generation;stale.type=BXWM_DESTROY;
    assert(!xwayland_accept_message(&stale) && s->wm_id && g_xwindows);
    assert(unrelated->wm_id==native_id && pixels[native_id][0]==0xff123456);
    // Server disconnect takes the other teardown route through the exact
    // wl_client destroy listener. It must have the same isolation guarantee.
    wl_client_destroy(client);close(wayland_peer);close(peer);
    assert(!g_xwayland_client && !g_xwindows && g_xwm_fd<0 && destroyed==4);
    assert(g_surfaces==unrelated && unrelated->wm_id==native_id);
    assert(native[native_id].incarnation==native_incarnation && surface_repaint(unrelated));
    wl_client_destroy(other);close(other_peer);assert(!g_surfaces && destroyed==5);
    wl_display_destroy(g_display);
    puts("XWAYLAND_BRIDGE_PASS association orders, independent windows, geometry ACK ordering, stale frames, focus/close, EOF/server-loss isolation, mapped native survival and generation reuse");
    return 0;
}
