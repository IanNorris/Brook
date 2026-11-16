/* Actual Wayland/bridge/retained-surface adapter with only the kernel ABI mocked.
 * This is not a proof of the real GPU or native kernel paths (VM gates remain). */
#define _GNU_SOURCE
#include <assert.h>
#include <poll.h>
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
static uint32_t create_flags[64],decoration_changes,moves,edge_resizes;
static long bridge_syscall(long number,...) {
    va_list ap;va_start(ap,number);long result=0;
    if (number==BROOK_SYS_WM_CREATE_WINDOW) {
        long width=va_arg(ap,long),height=va_arg(ap,long);(void)va_arg(ap,long);
        struct brook_wm_create_out *out=(void *)va_arg(ap,long);long flags=va_arg(ap,long);
        uint32_t id=++next_window;assert(id<64 && width<=64 && height<=64);++created;
        create_flags[id]=(uint32_t)flags;
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
    } else if (number==BROOK_SYS_WM_SET_DECORATION_MODE) {
        long id=va_arg(ap,long),csd=va_arg(ap,long);++decoration_changes;
        if (csd) native[id].flags|=BROOK_WINDOW_STATE_CLIENT_CHROME;
        else native[id].flags&=~BROOK_WINDOW_STATE_CLIENT_CHROME;
    } else if (number==BROOK_SYS_WM_BEGIN_MOVE || number==BROOK_SYS_WM_BEGIN_RESIZE) {
        long id=va_arg(ap,long);assert(native[id].version);
        if (number==BROOK_SYS_WM_BEGIN_MOVE) ++moves;
        else { assert(va_arg(ap,long)==(BROOK_WM_EDGE_TOP|BROOK_WM_EDGE_LEFT));++edge_resizes; }
        // A missing active mouse press is an ordinary declined request, not a
        // reason to disconnect every X client.
        errno=EINVAL;result=-1;
    } else if (number==BROOK_SYS_WM_SET_MAXIMIZED) {
        long id=va_arg(ap,long),enable=va_arg(ap,long);assert(enable==0 || enable==1);
        if (enable) native[id].flags|=BROOK_WINDOW_STATE_MAXIMIZED;
        else native[id].flags&=~BROOK_WINDOW_STATE_MAXIMIZED;
    } else if (number==BROOK_SYS_WM_SET_MINIMIZED) {
        long id=va_arg(ap,long);native[id].flags|=BROOK_WINDOW_STATE_MINIMIZED;
    } else if (number==BROOK_SYS_WM_GET_SCREEN_INFO) {
        uint32_t *out=(void *)va_arg(ap,long);out[0]=48;out[1]=40;
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
static void no_command(int fd,struct bxwm_stream *stream) {
    struct bxwm_message message;assert(bxwm_receive(fd,stream,&message)==0);
}
int main(int argc,char **argv) {
    (void)argv;
    // This executable also serves as a long-lived fake server and helper for
    // the restart test. Parent termination uses the normal SIGTERM action.
    if(argc>1) {
        const char *report=getenv("BROOK_TEST_XWAYLAND_REPORT_FD");
        if(report && !strcmp(argv[1],":0")) {
            int shm=0,glamor=0,extension=0,iglx=0,dumb=0;
            for(int i=2;i<argc;++i) {
                if(!strcmp(argv[i],"-shm"))shm=1;
                if(!strcmp(argv[i],"-glamor") && i+1<argc && !strcmp(argv[i+1],"gl"))glamor=1;
                if(!strcmp(argv[i],"+extension") && i+1<argc && !strcmp(argv[i+1],"GLX"))extension=1;
                if(!strcmp(argv[i],"+iglx"))iglx=1;
                if(!strcmp(argv[i],"-dumbSched"))dumb=1;
            }
            const char *mode=getenv("BROOK_XWAYLAND_SOFTWARE_GLX");
            int software=mode && !strcmp(mode,"1");
            const char *drivers=getenv("LIBGL_DRIVERS_PATH"),*always=getenv("LIBGL_ALWAYS_SOFTWARE");
            char result=software ? (shm && !glamor && extension && iglx && dumb && drivers &&
                !strcmp(drivers,"/nix/profile/lib/dri") && always && !strcmp(always,"1")?'S':'X') :
                (!shm && glamor && !extension && !iglx && !dumb?'D':'X');
            if(write(atoi(report),&result,1)!=1)_exit(2);
        }
        for(;;)pause();
    }
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
    state.flags|=BXWM_CLIENT_CHROME;assert(xwayland_accept_message(&state));
    assert(decoration_changes==1 && (native[id].flags&BROOK_WINDOW_STATE_CLIENT_CHROME));
    state.flags&=~BXWM_CLIENT_CHROME;assert(xwayland_accept_message(&state));
    assert(decoration_changes==2 && !(native[id].flags&BROOK_WINDOW_STATE_CLIENT_CHROME));
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
    state.flags=BXWM_MAPPED|BXWM_INPUT|BXWM_CLIENT_CHROME;state.configure_serial=0;
    assert(!xwayland_accept_message(&state) && !g_xwindows);
    state.generation=g_xwayland_generation;state.width=2;state.height=2;
    assert(xwayland_accept_message(&state));s=new_surface(client,55);image(s,2,2);
    assert(s->wm_id && s->wm_id!=native_id && created==5);
    assert((create_flags[s->wm_id]&BROOK_WM_CREATE_FLAG_CSD) &&
        !(create_flags[s->wm_id]&BROOK_WM_CREATE_FLAG_NO_FOCUS));
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);
    assert(configure.generation==g_xwayland_generation && configure.configure_serial==1);
    struct bxwm_message stale=state;stale.generation=first_generation;stale.type=BXWM_DESTROY;
    assert(!xwayland_accept_message(&stale) && s->wm_id && g_xwindows);
    struct bxwm_message request=state;request.type=BXWM_BEGIN_MOVE;request.identity+=100;
    assert(xwayland_accept_message(&request) && !moves);
    request.identity=state.identity;assert(xwayland_accept_message(&request) && moves==1 && !g_xwm_failed);
    request.type=BXWM_BEGIN_RESIZE;request.x=0;
    assert(xwayland_accept_message(&request) && edge_resizes==1 && !g_xwm_failed);
    request.type=BXWM_MAXIMIZE;request.x=1;assert(xwayland_accept_message(&request));
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);assert(configure.flags&BXWM_MAXIMIZED);
    request.x=2;assert(xwayland_accept_message(&request));
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);assert(!(configure.flags&BXWM_MAXIMIZED));
    request.type=BXWM_MINIMIZE;assert(xwayland_accept_message(&request));
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);assert(configure.flags&BXWM_MINIMIZED);
    // EWMH fullscreen from a maximized window: no chrome, output origin and
    // exactly the output size (SDL assumes both); maximize returns on exit.
    native[s->wm_id].flags&=~BROOK_WINDOW_STATE_MINIMIZED;
    xwayland_native_sync(s,0);command_from_peer(peer,&rx,BXWM_CONFIGURE);
    request.type=BXWM_MAXIMIZE;request.x=1;assert(xwayland_accept_message(&request));
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);assert(configure.flags&BXWM_MAXIMIZED);
    uint32_t fid=s->wm_id;int32_t windowed_x=native[fid].client_x,windowed_y=native[fid].client_y;
    uint32_t windowed_w=native[fid].width,windowed_h=native[fid].height;
    request.type=BXWM_FULLSCREEN_REQUEST;request.x=1;assert(xwayland_accept_message(&request));
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);
    assert((configure.flags&BXWM_FULLSCREEN) && !(configure.flags&BXWM_MAXIMIZED));
    assert(configure.x==0 && configure.y==0 && configure.width==48 && configure.height==40);
    assert(native[fid].client_x==0 && native[fid].client_y==0 && native[fid].width==48 && native[fid].height==40);
    assert((native[fid].flags&BROOK_WINDOW_STATE_CLIENT_CHROME) && s->vfb_w==48 && s->xwindow->target_width==48);
    // Old-size frames are not stretched into the fullscreen extent.
    unsigned fullscreen_resizes=resized;image(s,windowed_w,windowed_h);assert(resized==fullscreen_resizes && s->vfb_w==48);
    // Fullscreen owns geometry: maximize/move requests and a chrome hint change are ignored.
    request.type=BXWM_MAXIMIZE;request.x=2;assert(xwayland_accept_message(&request));no_command(peer,&rx);
    request.type=BXWM_BEGIN_MOVE;unsigned moves_before=moves;assert(xwayland_accept_message(&request) && moves==moves_before);
    unsigned decorations_before=decoration_changes;state.flags&=~BXWM_CLIENT_CHROME;
    assert(xwayland_accept_message(&state) && decoration_changes==decorations_before);
    request.type=BXWM_FULLSCREEN_REQUEST;request.x=2;assert(xwayland_accept_message(&request));
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);
    assert(!(configure.flags&BXWM_FULLSCREEN) && (configure.flags&BXWM_MAXIMIZED));
    assert(native[fid].client_x==windowed_x && native[fid].client_y==windowed_y &&
           native[fid].width==windowed_w && native[fid].height==windowed_h);
    assert(!(native[fid].flags&BROOK_WINDOW_STATE_CLIENT_CHROME));
    request.type=BXWM_FULLSCREEN_REQUEST;request.x=0;assert(xwayland_accept_message(&request));no_command(peer,&rx);
    // A request made before the X window is presented applies on its first
    // native sync: chrome-less creation and a single fullscreen configure.
    struct bxwm_message game=state;game.window=125;game.identity=3;game.serial=57;game.configure_serial=0;
    game.flags=BXWM_MAPPED|BXWM_INPUT;game.x=0;game.y=0;game.width=2;game.height=2;
    assert(xwayland_accept_message(&game));
    struct bxwm_message wanted=game;wanted.type=BXWM_FULLSCREEN_REQUEST;wanted.x=1;
    assert(xwayland_accept_message(&wanted));
    struct brook_surface *full=new_surface(client,57);image(full,2,2);
    assert(full->wm_id && (create_flags[full->wm_id]&BROOK_WM_CREATE_FLAG_CSD));
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);
    assert(configure.window==125 && (configure.flags&BXWM_FULLSCREEN) && configure.width==48 && configure.height==40 &&
           !configure.x && !configure.y);
    no_command(peer,&rx);
    image(full,48,40);assert(full->vfb_w==48 && !full->xwindow->target_width);
    // "Scaled by the system": a BXWM_SCALED window keeps 1x X geometry at its
    // native client origin and is presented at the output scale. Pointer
    // input arrives in 1x surface units; popups map through their anchor.
    g_output_scale=2;
    struct bxwm_message legacy=state;legacy.window=126;legacy.identity=4;legacy.serial=58;legacy.configure_serial=0;
    legacy.flags=BXWM_MAPPED|BXWM_INPUT|BXWM_SCALED;legacy.x=50;legacy.y=90;legacy.width=10;legacy.height=8;
    assert(xwayland_accept_message(&legacy));
    struct brook_surface *old_app=new_surface(client,58);image(old_app,10,8);
    uint32_t lid=old_app->wm_id;assert(lid && old_app->vfb_w==20 && old_app->vfb_h==16);
    assert(native[lid].client_x==50 && native[lid].client_y==90 && native[lid].width==20 && native[lid].height==16);
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);
    assert(configure.x==50 && configure.y==90 && configure.width==10 && configure.height==8);
    assert(surface_pointer_fixed(old_app,20)==wl_fixed_from_int(10));
    legacy.configure_serial=configure.configure_serial;assert(xwayland_accept_message(&legacy));
    // An odd native resize snaps to whole 1x pixels before the X configure.
    native[lid].width=25;native[lid].height=17;xwayland_native_sync(old_app,0);
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);
    assert(configure.width==12 && configure.height==8 && native[lid].width==24 && native[lid].height==16);
    image(old_app,10,8);assert(old_app->vfb_w==24); // stale 1x size is held back
    legacy.configure_serial=configure.configure_serial;legacy.width=12;assert(xwayland_accept_message(&legacy));
    image(old_app,12,8);assert(old_app->vfb_w==24 && !old_app->xwindow->target_width);
    // The client resizes itself in 1x units.
    legacy.width=14;assert(xwayland_accept_message(&legacy));assert(native[lid].width==28 && old_app->xwindow->target_width==28);
    image(old_app,14,8);assert(old_app->vfb_w==28);
    struct bxwm_message popup_menu=legacy;popup_menu.window=127;popup_menu.identity=5;popup_menu.serial=59;
    popup_menu.configure_serial=0;popup_menu.flags=BXWM_MAPPED|BXWM_OVERRIDE|BXWM_SCALED;
    popup_menu.parent=126;popup_menu.parent_identity=4;popup_menu.x=55;popup_menu.y=93;popup_menu.width=4;popup_menu.height=3;
    assert(xwayland_accept_message(&popup_menu));
    struct brook_surface *scaled_popup=new_surface(client,59);image(scaled_popup,4,3);
    uint32_t pid=scaled_popup->wm_id;
    assert(native[pid].client_x==60 && native[pid].client_y==96 && native[pid].width==8 && native[pid].height==6);
    configure=command_from_peer(peer,&rx,BXWM_CONFIGURE);
    assert(configure.window==127 && configure.x==55 && configure.y==93 && configure.width==4 && configure.height==3);
    // Moving the popup in X units moves it twice as far natively.
    popup_menu.configure_serial=configure.configure_serial;popup_menu.x=57;assert(xwayland_accept_message(&popup_menu));
    assert(native[pid].client_x==64);
    g_output_scale=1;
    assert(unrelated->wm_id==native_id && pixels[native_id][0]==0xff123456);
    // Server disconnect takes the other teardown route through the exact
    // wl_client destroy listener. It must have the same isolation guarantee.
    wl_client_destroy(client);close(wayland_peer);close(peer);
    assert(!g_xwayland_client && !g_xwindows && g_xwm_fd<0 && destroyed==7);
    assert(g_surfaces==unrelated && unrelated->wm_id==native_id);
    assert(native[native_id].incarnation==native_incarnation && surface_repaint(unrelated));
    // Exercise real fork/exec retirement and replacement while the unrelated
    // native surface remains mapped. No replacement while either child lives.
    int report[2];assert(!pipe(report));char report_fd[24];
    snprintf(report_fd,sizeof(report_fd),"%d",report[1]);
    assert(!setenv("BROOK_TEST_XWAYLAND_REPORT_FD",report_fd,1));
    unsetenv("BROOK_XWAYLAND_SOFTWARE_GLX");unsetenv("BROOK_XWAYLAND_DUMB_SCHED");
    xwayland_restart_configure("/proc/self/exe","/proc/self/exe",":0");
    assert(xwayland_start(g_xrestart_server,g_xrestart_helper,g_xrestart_display));
    assert(g_xwayland_pid>0 && g_xwm_pid>0 && g_xwayland_client);
    struct pollfd ready={.fd=report[0],.events=POLLIN};char launch=0;
    assert(poll(&ready,1,3000)==1 && read(report[0],&launch,1)==1 && launch=='D');
    uint64_t old_generation=g_xwayland_generation;
    // A stopped child cannot act on SIGTERM; it must not pin recovery forever.
    assert(!kill(g_xwm_pid,SIGSTOP));int stopped=0;
    assert(waitpid(g_xwm_pid,&stopped,WUNTRACED)==g_xwm_pid && WIFSTOPPED(stopped));
    xwayland_generation_lost();assert(g_xrestart_pending);
    g_xrestart_at=g_now_ms();
    xwayland_pump();assert(g_xwayland_generation==old_generation && g_xwm_pid>0);
    g_xretire_started=g_now_ms()-3000;g_shutdown=1;
    for(unsigned tries=0;tries<300 && (g_xwayland_pid || g_xwm_pid);++tries) {
        xwayland_pump();usleep(10000);
    }
    assert(!g_xwayland_pid && !g_xwm_pid && g_xwayland_generation==old_generation);
    assert(!setenv("BROOK_XWAYLAND_SOFTWARE_GLX","1",1));
    assert(!setenv("BROOK_XWAYLAND_DUMB_SCHED","1",1));
    g_shutdown=0;
    for(unsigned tries=0;tries<300 && g_xwayland_generation==old_generation;++tries) {
        xwayland_pump();usleep(10000);
    }
    assert(g_xwayland_generation==old_generation+1 && g_xwayland_pid>0 && g_xwm_pid>0);
    ready.revents=0;launch=0;
    assert(poll(&ready,1,3000)==1 && read(report[0],&launch,1)==1 && launch=='S');
    assert(unrelated->wm_id==native_id && native[native_id].incarnation==native_incarnation);
    assert(!kill(g_xwm_pid,SIGSTOP));
    assert(waitpid(g_xwm_pid,&stopped,WUNTRACED)==g_xwm_pid && WIFSTOPPED(stopped));
    uint64_t last_generation=g_xwayland_generation;assert(xwayland_shutdown());
    assert(!g_xwayland_pid && !g_xwm_pid && !g_xrestart_pending);
    xwayland_pump();assert(g_xwayland_generation==last_generation);
    close(report[0]);close(report[1]);unsetenv("BROOK_TEST_XWAYLAND_REPORT_FD");
    unsetenv("BROOK_XWAYLAND_SOFTWARE_GLX");unsetenv("BROOK_XWAYLAND_DUMB_SCHED");
    wl_client_destroy(other);close(other_peer);assert(!g_surfaces && destroyed==8);
    wl_display_destroy(g_display);
    puts("XWAYLAND_BRIDGE_PASS association orders, independent windows, geometry ACK ordering, stale frames, focus/close, EOF/server-loss isolation, mapped native survival, generation reuse, EWMH fullscreen and scaled windows");
    return 0;
}
