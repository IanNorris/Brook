/* Exercise the actual commit handler and libwayland wire events. */
#include <assert.h>
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
int main(void) {
    struct brook_frame_clock phase={0};
    assert(brook_frame_clock_start(&phase,100)==16 && phase.next_ms==116);
    assert(brook_frame_clock_delay(&phase,116)==16 && phase.next_ms==132);
    assert(brook_frame_clock_delay(&phase,139)==9 && phase.next_ms==148);
    assert(brook_frame_clock_delay(&phase,200)==12 && phase.next_ms==212);
    assert(brook_frame_clock_delay(&phase,205)==7 && phase.next_ms==212);
    assert(brook_frame_clock_delay(&phase,100)==-1);
    // Cross wl_callback's 32-bit timestamp wrap without losing timer phase.
    assert(brook_frame_clock_start(&phase,UINT32_MAX-7u)==16);
    assert(brook_frame_clock_delay(&phase,(uint64_t)UINT32_MAX+12u)==13);
    assert(brook_frame_clock_start(&phase,UINT64_MAX-3)==-1);
    phase.next_ms=UINT64_MAX-7;
    assert(brook_frame_clock_delay(&phase,UINT64_MAX)==-1);
    g_trace_frames = 1;
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *display=wl_display_create();
    struct wl_client *client=wl_client_create(display,sockets[0]);
    assert(display && client);
    struct wl_event_loop *loop=wl_display_get_event_loop(display);
    g_frame_timer=wl_event_loop_add_timer(loop,frame_clock_tick,NULL);
    assert(g_frame_timer);
    assert(wl_event_source_timer_update(g_frame_timer,16)==0);
    uint64_t initial;
    assert(frame_monotonic_ms(&initial)==0 && brook_frame_clock_start(&g_frame_clock,initial)==16);
    struct brook_surface surface={0};
    surface_tree_init(&surface);
    surface.wm_id=1;g_surfaces=&surface;
    uint32_t pixel=0xff112233,vfb=0;
    surface.vfb=&vfb;surface.vfb_w=surface.vfb_h=surface.vfb_stride=1;surface.scene_disabled=1;
    struct brook_image *mapped=image_snapshot((uint8_t *)&pixel,1,1,4,WL_SHM_FORMAT_ARGB8888);assert(mapped);
    assert(surface_tree_commit(&surface,mapped));image_unref(mapped);
    surface.resource=wl_resource_create(client,&wl_surface_interface,4,2);
    assert(surface.resource);
    wl_resource_set_user_data(surface.resource,&surface);
    for(unsigned round=0;round<3;++round) {
        g_phase_frame_clock=(round==1);
        surface_frame(client,surface.resource,3+round);
        assert(wl_list_length(&surface.pending_frames)==1);
        assert(wl_event_loop_dispatch(loop,32)==0);
        wl_display_flush_clients(display);
        uint32_t wire[64];
        assert(recv(sockets[1],wire,sizeof(wire),MSG_DONTWAIT)==-1 && errno==EAGAIN);
        surface_commit(client,surface.resource);
        assert(wl_list_empty(&surface.pending_frames) && wl_list_length(&surface.committed_frames)==1);
        assert(wl_event_loop_dispatch(loop,32)==0);
        assert(wl_list_empty(&surface.committed_frames));
        wl_display_flush_clients(display);
        ssize_t bytes=recv(sockets[1],wire,sizeof(wire),MSG_DONTWAIT);
        assert(bytes>0);
        unsigned done=0;
        for(size_t i=0;i<(size_t)bytes/4;) {
            uint32_t object=wire[i],op=wire[i+1]&65535,size=wire[i+1]>>16;
            assert(size>=8 && i*4+size<=(size_t)bytes);
            if(object==3+round) {assert(op==WL_CALLBACK_DONE && size==12);++done;}
            i+=size/4;
        }
        assert(done==1);
        surface_commit(client,surface.resource);
        wl_display_flush_clients(display);
        assert(recv(sockets[1],wire,sizeof(wire),MSG_DONTWAIT)==-1 && errno==EAGAIN);
    }
    for(unsigned i=0;i<24;++i)surface_frame(client,surface.resource,6+i);
    assert(surface.trace_requests==27 && surface.trace_done==3 && !surface.trace_cancelled);
    assert(wl_list_length(&surface.pending_frames)==24);
    surface_commit(client,surface.resource);
    assert(wl_list_length(&surface.committed_frames)==24);
    surface_frame(client,surface.resource,30);
    assert(surface.trace_requests==28);
    int foreignSockets[2];
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,foreignSockets)==0);
    struct wl_client *foreign=wl_client_create(display,foreignSockets[0]);
    assert(foreign);
    struct brook_seat_client first={.client=client}, second={.client=client}, other={.client=foreign};
    first.keyboard=wl_resource_create(client,&wl_keyboard_interface,5,31);
    second.keyboard=wl_resource_create(client,&wl_keyboard_interface,5,32);
    other.keyboard=wl_resource_create(foreign,&wl_keyboard_interface,5,2);
    assert(first.keyboard && second.keyboard && other.keyboard);
    first.pointer=wl_resource_create(client,&wl_pointer_interface,5,33);
    second.pointer=wl_resource_create(client,&wl_pointer_interface,5,34);
    other.pointer=wl_resource_create(foreign,&wl_pointer_interface,5,3);
    assert(first.pointer && second.pointer && other.pointer);
    other.next=&second;second.next=&first;g_seat_clients=&other;
    /* v1 clients also require exactly one post-map activation configure, even
     * when the client binds the seat twice. */
    surface.xdg_surface=wl_resource_create(client,&xdg_surface_interface,1,35);
    surface.xdg_toplevel=wl_resource_create(client,&xdg_toplevel_interface,1,36);
    assert(surface.xdg_surface && surface.xdg_toplevel);
    surface.xdg_initial_commit_done=1;
    surface.xdg_acked=1;
    wl_resource_set_user_data(surface.xdg_surface,&surface);
    surface.vfb_w=852;surface.vfb_h=652;surface.vfb_stride=852; // CSD shadow margins
    surface.vfb=calloc(852*652,4);assert(surface.vfb);
    struct brook_image *csd=image_snapshot((uint8_t *)surface.vfb,852,652,852*4,WL_SHM_FORMAT_ARGB8888);assert(csd);
    assert(surface_tree_commit(&surface,csd));image_unref(csd);
    xdg_surface_set_window_geometry(client,surface.xdg_surface,26,26,800,600);
    assert(surface.window_geometry_pending && surface.window_geometry_w==0);
    surface_commit(client,surface.resource);
    assert(!surface.window_geometry_pending && surface.window_geometry_w==800 && surface.window_geometry_h==600);
    /* A later uncommitted geometry must not alter a state-only configure. */
    xdg_surface_set_window_geometry(client,surface.xdg_surface,26,26,640,480);
    struct brook_wm_event event={.type=WM_EVT_FOCUS_GAINED};
    dispatch_window_event(&surface,&event,100);
    assert(first.kb_focus==&surface && second.kb_focus==&surface && !other.kb_focus);
    event.type=EVT_KEY_PRESS;event.scan=0x1e;
    dispatch_window_event(&surface,&event,101);
    event.type=EVT_KEY_RELEASE;
    dispatch_window_event(&surface,&event,102);
    event.type=EVT_MOUSE_SCROLL;event.scan=1;
    dispatch_window_event(&surface,&event,103);
    wl_display_flush_clients(display);
    uint32_t keysWire[128];
    ssize_t keysBytes=recv(sockets[1],keysWire,sizeof(keysWire),MSG_DONTWAIT);
    assert(keysBytes>0);
    unsigned enters[2]={0},keys[2]={0},axes[2]={0},activations=0,configures=0;
    for(size_t i=0;i<(size_t)keysBytes/4;) {
        uint32_t object=keysWire[i],op=keysWire[i+1]&65535,size=keysWire[i+1]>>16;
        assert(size>=8 && i*4+size<=(size_t)keysBytes);
        if(object==36 && op==XDG_TOPLEVEL_CONFIGURE) {
            assert(size==24 && keysWire[i+2]==800 && keysWire[i+3]==600);
            assert(keysWire[i+4]==4 && keysWire[i+5]==XDG_TOPLEVEL_STATE_ACTIVATED);
            ++activations;
        }
        if(object==35 && op==XDG_SURFACE_CONFIGURE) {
            assert(size==12 && keysWire[i+2]==surface.next_configure_serial);++configures;
        }
        if(object==31 || object==32) {
            if(op==WL_KEYBOARD_ENTER)++enters[object-31];
            if(op==WL_KEYBOARD_KEY)++keys[object-31];
        }
        if((object==33 || object==34) && op==WL_POINTER_AXIS) {
            assert(size==20 && (int32_t)keysWire[i+4]==wl_fixed_from_int(-10));
            ++axes[object-33];
        }
        i+=size/4;
    }
    assert(surface.toplevel_activated && surface.xdg_acked);
    assert(activations==1 && configures==1);
    assert(axes[0]==1 && axes[1]==1);
    assert(enters[0]==1 && enters[1]==1 && keys[0]==2 && keys[1]==2);
    assert(recv(foreignSockets[1],keysWire,sizeof(keysWire),MSG_DONTWAIT)==-1 && errno==EAGAIN);
    event.type=WM_EVT_FOCUS_LOST;dispatch_window_event(&surface,&event,103);
    assert(!first.kb_focus && !second.kb_focus && !other.kb_focus);
    assert(!surface.toplevel_activated);
    wl_display_flush_clients(display);
    keysBytes=recv(sockets[1],keysWire,sizeof(keysWire),MSG_DONTWAIT);assert(keysBytes>0);
    unsigned deactivations=0;configures=0;
    for(size_t i=0;i<(size_t)keysBytes/4;) {
        uint32_t object=keysWire[i],op=keysWire[i+1]&65535,size=keysWire[i+1]>>16;
        assert(size>=8 && i*4+size<=(size_t)keysBytes);
        if(object==36 && op==XDG_TOPLEVEL_CONFIGURE) {assert(size==20 && keysWire[i+4]==0);++deactivations;}
        if(object==35 && op==XDG_SURFACE_CONFIGURE)++configures;
        i+=size/4;
    }
    assert(deactivations==1 && configures==1);
    /* Repeated focus-lost must not manufacture another configure. */
    dispatch_window_event(&surface,&event,104);wl_display_flush_clients(display);
    assert(recv(sockets[1],keysWire,sizeof(keysWire),MSG_DONTWAIT)==-1 && errno==EAGAIN);

    /* Native maximize must be a protocol state, and focus changes must not
     * erase it. Wine ignores normal-state size suggestions by design. */
    event=(struct brook_wm_event){.type=WM_EVT_RESIZED,.x=1280,.y=720,.reserved=WM_RESIZE_MAXIMIZED};
    dispatch_window_event(&surface,&event,105);assert(surface.toplevel_maximized);
    event.type=WM_EVT_FOCUS_GAINED;dispatch_window_event(&surface,&event,106);
    wl_display_flush_clients(display);keysBytes=recv(sockets[1],keysWire,sizeof(keysWire),MSG_DONTWAIT);assert(keysBytes>0);
    unsigned maxConfigs=0;
    for(size_t i=0;i<(size_t)keysBytes/4;) {
        unsigned size=keysWire[i+1]>>16;assert(size>=8 && i*4+size<=(size_t)keysBytes);
        if(keysWire[i]==36 && (keysWire[i+1]&65535)==XDG_TOPLEVEL_CONFIGURE) {
            assert(keysWire[i+2]==1280 && keysWire[i+3]==720);
            unsigned count=keysWire[i+4]/4;int found=0;
            for(unsigned j=0;j<count;++j)if(keysWire[i+5+j]==XDG_TOPLEVEL_STATE_MAXIMIZED)found=1;
            assert(found);++maxConfigs;
        }
        i+=size/4;
    }
    assert(maxConfigs==2);
    event=(struct brook_wm_event){.type=WM_EVT_RESIZED,.x=800,.y=600};
    dispatch_window_event(&surface,&event,107);assert(!surface.toplevel_maximized && surface.configure_size_pending);
    xdg_surface_set_window_geometry(client,surface.xdg_surface,0,0,640,480);
    xdg_surface_ack_configure(client,surface.xdg_surface,surface.configure_size_serial);
    assert(surface.configure_size_pending);surface_commit(client,surface.resource);
    assert(!surface.configure_size_pending && surface.window_geometry_w==640 && surface.window_geometry_h==480);

    wl_display_flush_clients(display);keysBytes=recv(sockets[1],keysWire,sizeof(keysWire),MSG_DONTWAIT);assert(keysBytes>0);
    event=(struct brook_wm_event){.type=WM_EVT_RESIZED,.x=900,.y=700,.reserved=WM_RESIZE_RESIZING};
    dispatch_window_event(&surface,&event,108);uint32_t firstResize=surface.configure_size_serial;
    assert(surface.toplevel_resizing && surface.configure_size_pending);
    toplevel_set_activated(&surface,0);uint32_t focusedResize=surface.configure_size_serial;
    assert(focusedResize!=firstResize);
    xdg_surface_ack_configure(client,surface.xdg_surface,firstResize);surface_commit(client,surface.resource);
    assert(surface.toplevel_resizing && surface.configure_size_pending); // superseded ACK cannot finish
    event.x=960;event.y=720;dispatch_window_event(&surface,&event,109);
    xdg_surface_ack_configure(client,surface.xdg_surface,focusedResize);surface_commit(client,surface.resource);
    assert(surface.toplevel_resizing && surface.configure_size_pending);
    uint32_t finalResize=surface.configure_size_serial;
    xdg_surface_ack_configure(client,surface.xdg_surface,finalResize);
    xdg_surface_set_window_geometry(client,surface.xdg_surface,0,0,800,600); // valid smaller accepted size
    assert(surface.toplevel_resizing);surface_commit(client,surface.resource);
    assert(!surface.toplevel_resizing && !surface.configure_size_pending && surface.next_configure_serial!=finalResize);
    wl_display_flush_clients(display);keysBytes=recv(sockets[1],keysWire,sizeof(keysWire),MSG_DONTWAIT);assert(keysBytes>0);
    unsigned resizeConfigs=0;
    const unsigned expectedW[]={900,900,960,800},expectedH[]={700,700,720,600};
    for(size_t i=0;i<(size_t)keysBytes/4;) {
        unsigned size=keysWire[i+1]>>16;assert(size>=8 && i*4+size<=(size_t)keysBytes);
        if(keysWire[i]==36 && (keysWire[i+1]&65535)==XDG_TOPLEVEL_CONFIGURE) {
            assert(resizeConfigs<4 && keysWire[i+2]==expectedW[resizeConfigs] && keysWire[i+3]==expectedH[resizeConfigs]);
            unsigned count=keysWire[i+4]/4;int resizing=0;
            for(unsigned j=0;j<count;++j)if(keysWire[i+5+j]==XDG_TOPLEVEL_STATE_RESIZING)resizing=1;
            assert(resizing==(resizeConfigs<3));++resizeConfigs;
        }
        i+=size/4;
    }
    assert(resizeConfigs==4);

    g_seat_clients=NULL;wl_client_destroy(foreign);close(foreignSockets[1]);
    wl_client_destroy(client);
    assert(surface.trace_done==3 && surface.trace_cancelled==25);
    assert(wl_list_empty(&surface.pending_frames) && wl_list_empty(&surface.committed_frames));
    bt_state_set(&surface.tree.current,NULL);bt_state_set(&surface.tree.latest,NULL);
    free(surface.vfb);free(surface.composite);
    g_surfaces=NULL;wl_display_destroy(display);close(sockets[1]);
    return 0;
}
