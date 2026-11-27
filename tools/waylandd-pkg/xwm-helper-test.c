#define _GNU_SOURCE
#include <assert.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include "xwm-wire.h"
static struct bxwm_stream stream;
static xcb_window_t never_present;
static uint64_t now(void) { struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static xcb_atom_t atom(xcb_connection_t *c,const char *name) {
    xcb_intern_atom_reply_t *r=xcb_intern_atom_reply(c,xcb_intern_atom(c,0,strlen(name),name),NULL);
    assert(r);xcb_atom_t result=r->atom;free(r);return result;
}
static struct bxwm_message receive(int fd,uint32_t type,xcb_window_t window,uint64_t serial,unsigned width,unsigned height) {
    uint64_t deadline=now()+5000;struct bxwm_message m;
    while (now()<deadline) {
        int result=bxwm_receive(fd,&stream,&m);assert(result>=0);
        if (result) {
            assert(bxwm_valid(&m,7,1));
            assert(m.type!=BXWM_WINDOW || m.window!=never_present);
            // Without waylandd's protocol 2 the helper must keep to the
            // original messages, which an older waylandd validates.
            if (!getenv("BROOK_XWM_PROTOCOL")) assert(m.type!=BXWM_FULLSCREEN_REQUEST && !(m.flags&BXWM_SCALED));
            if (m.type==type && (!window || m.window==window) && (!serial || m.serial==serial) &&
                (!width || m.width==width) && (!height || m.height==height) &&
                (type!=BXWM_WINDOW || (m.flags&BXWM_MAPPED))) return m;
        } else { struct pollfd p={fd,POLLIN,0};assert(poll(&p,1,100)>=0); }
    }
    fprintf(stderr,"XWM helper event timeout type=%u\n",type);abort();
}
static void send_command(int fd,struct bxwm_message m,uint32_t type) {
    m.type=type;assert(bxwm_enqueue(&stream,&m));assert(bxwm_flush(fd,&stream)==1);
}
static xcb_timestamp_t wait_protocol(xcb_connection_t *c,xcb_atom_t protocols,xcb_atom_t expected) {
    uint64_t deadline=now()+5000;
    while (now()<deadline) {
        xcb_generic_event_t *e=xcb_poll_for_event(c);
        if (e) {
            int found=(e->response_type&127)==XCB_CLIENT_MESSAGE &&
                ((xcb_client_message_event_t *)e)->type==protocols &&
                ((xcb_client_message_event_t *)e)->data.data32[0]==expected;
            xcb_timestamp_t stamp=found?((xcb_client_message_event_t *)e)->data.data32[1]:0;
            free(e);if (found) return stamp;
        } else { struct pollfd p={xcb_get_file_descriptor(c),POLLIN,0};assert(poll(&p,1,100)>=0); }
    }
    fprintf(stderr,"XWM helper protocol timeout\n");abort();
}
/* Wait for a ConfigureNotify on window; synthetic selects SendEvent copies. */
static xcb_configure_notify_event_t wait_configure(xcb_connection_t *c,xcb_window_t window,int synthetic,
                                                   unsigned width,unsigned height) {
    uint64_t deadline=now()+5000;
    while (now()<deadline) {
        xcb_generic_event_t *e=xcb_poll_for_event(c);
        if (e) {
            xcb_configure_notify_event_t n=*(xcb_configure_notify_event_t *)e;
            int found=(e->response_type&127)==XCB_CONFIGURE_NOTIFY && n.window==window &&
                (!synthetic || (e->response_type&0x80)) && n.width==width && n.height==height;
            free(e);if (found) return n;
        } else { struct pollfd p={xcb_get_file_descriptor(c),POLLIN,0};assert(poll(&p,1,100)>=0); }
    }
    fprintf(stderr,"XWM helper ConfigureNotify timeout\n");abort();
}
static int state_has(xcb_connection_t *c,xcb_window_t window,xcb_atom_t net_state,xcb_atom_t wanted) {
    xcb_get_property_reply_t *r=xcb_get_property_reply(c,xcb_get_property(c,0,window,net_state,XCB_ATOM_ATOM,0,64),NULL);
    int found=0;assert(r);
    for (int i=0;i<xcb_get_property_value_length(r)/4;++i) if (((xcb_atom_t *)xcb_get_property_value(r))[i]==wanted) found=1;
    free(r);return found;
}
/* The XWM gives the root window the theme's left_ptr; without it rootless
 * Xwayland shows the server's "X" glyph over every X window that defines
 * no cursor. Read the cursor shown over the bare root back through XFixes. */
static void check_root_cursor(xcb_connection_t *c,xcb_screen_t *screen,const char *expect) {
    unsigned width,height,xhot,yhot;
    assert(sscanf(expect,"%ux%u+%u+%u",&width,&height,&xhot,&yhot)==4);
    free(xcb_xfixes_query_version_reply(c,xcb_xfixes_query_version(c,4,0),NULL));
    xcb_warp_pointer(c,XCB_WINDOW_NONE,screen->root,0,0,0,0,
                     (int16_t)(screen->width_in_pixels-5),(int16_t)(screen->height_in_pixels-5));
    uint64_t deadline=now()+5000;int matched=0;
    while (!matched && now()<deadline) {
        xcb_xfixes_get_cursor_image_reply_t *image=xcb_xfixes_get_cursor_image_reply(c,
            xcb_xfixes_get_cursor_image(c),NULL);
        assert(image);
        matched=image->width==width && image->height==height && image->xhot==xhot && image->yhot==yhot &&
            xcb_xfixes_get_cursor_image_cursor_image_length(image)==(int)(width*height) &&
            xcb_xfixes_get_cursor_image_cursor_image(image)[0]==0xff00c000u;
        free(image);
        if (!matched) usleep(10000);
    }
    assert(matched);
}
int main(int argc,char **argv) {
    /* Runner supplies a newly created headless Xvfb display, never DISPLAY. */
    assert(argc==3);int control=atoi(argv[2]);
    struct bxwm_message ready=receive(control,BXWM_READY,0,0,0,0);
    xcb_connection_t *c=xcb_connect(argv[1],NULL);assert(c && !xcb_connection_has_error(c));
    xcb_screen_t *screen=xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    assert(screen->root==ready.window);
    if (getenv("BROOK_XWM_EXPECT_ROOT_CURSOR")) check_root_cursor(c,screen,getenv("BROOK_XWM_EXPECT_ROOT_CURSOR"));
    xcb_window_t input=xcb_generate_id(c);
    never_present=input;
    xcb_create_window(c,0,input,screen->root,0,0,1,1,0,XCB_WINDOW_CLASS_INPUT_ONLY,0,0,NULL);
    xcb_map_window(c,input);xcb_flush(c);
    uint64_t deadline=now()+5000;int input_mapped=0;
    while (now()<deadline && !input_mapped) {
        xcb_get_window_attributes_reply_t *attributes=xcb_get_window_attributes_reply(c,xcb_get_window_attributes(c,input),NULL);
        assert(attributes);input_mapped=attributes->map_state!=XCB_MAP_STATE_UNMAPPED;free(attributes);
        if (!input_mapped) { struct pollfd pause={control,POLLIN,0};assert(poll(&pause,1,10)>=0); }
    }
    assert(input_mapped);xcb_destroy_window(c,input);xcb_flush(c);
    xcb_atom_t protocols=atom(c,"WM_PROTOCOLS"),close=atom(c,"WM_DELETE_WINDOW"),focus=atom(c,"WM_TAKE_FOCUS");
    xcb_atom_t serial=atom(c,"WL_SURFACE_SERIAL");xcb_atom_t supported[]={close,focus};
    xcb_window_t id=xcb_generate_id(c);
    xcb_create_window(c,XCB_COPY_FROM_PARENT,id,screen->root,10,20,320,200,3,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,0,NULL);
    xcb_atom_t motif=atom(c,"_MOTIF_WM_HINTS");uint8_t malformed_hint[3]={2,0,0};
    xcb_change_property(c,XCB_PROP_MODE_REPLACE,id,motif,XCB_ATOM_STRING,8,3,malformed_hint);
    xcb_change_property(c,XCB_PROP_MODE_REPLACE,id,protocols,XCB_ATOM_ATOM,32,2,supported);
    xcb_map_window(c,id);xcb_flush(c);
    struct bxwm_message initial=receive(control,BXWM_WINDOW,id,0,320,200);
    xcb_client_message_event_t association={.response_type=XCB_CLIENT_MESSAGE,.format=32,.window=id,.type=serial};
    association.data.data32[0]=123;
    if(getenv("BROOK_XWM_EXPECT_SYNTHETIC_REJECTION")) {
        xcb_void_cookie_t sent=xcb_send_event_checked(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&association);
        assert(!xcb_request_check(c,sent));
        initial.width=321;send_command(control,initial,BXWM_CONFIGURE);
        struct bxwm_message unchanged=receive(control,BXWM_WINDOW,id,0,321,200);
        assert(!unchanged.serial && unchanged.identity==initial.identity);
        send_command(control,unchanged,BXWM_FOCUS);assert(wait_protocol(c,protocols,focus));
        xcb_disconnect(c);puts("XWM_SYNTHETIC_SERIAL_REJECT_PASS");return 0;
    }
    xcb_send_event(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&association);
    xcb_flush(c);struct bxwm_message mapped=receive(control,BXWM_WINDOW,id,123,0,0);
    assert(mapped.identity==initial.identity && !(mapped.flags&BXWM_CLIENT_CHROME));
    // Stale, zero and duplicate events must not retire the session or unpair.
    const uint32_t stale[]={0,122,123};
    for(size_t i=0;i<sizeof(stale)/sizeof(stale[0]);++i) {
        association.data.data32[0]=stale[i];
        xcb_void_cookie_t sent=xcb_send_event_checked(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&association);
        assert(!xcb_request_check(c,sent));
    }
    xcb_void_cookie_t visibility=xcb_unmap_window_checked(c,id);assert(!xcb_request_check(c,visibility));
    visibility=xcb_map_window_checked(c,id);assert(!xcb_request_check(c,visibility));
    struct bxwm_message remapped=receive(control,BXWM_WINDOW,id,0,320,200);
    assert(remapped.identity==mapped.identity && !remapped.serial);
    association.data.data32[0]=123;
    xcb_void_cookie_t replay=xcb_send_event_checked(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&association);
    assert(!xcb_request_check(c,replay));
    remapped.width=321;send_command(control,remapped,BXWM_CONFIGURE);
    remapped=receive(control,BXWM_WINDOW,id,0,321,200);assert(!remapped.serial);
    association.data.data32[0]=124;
    replay=xcb_send_event_checked(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&association);
    assert(!xcb_request_check(c,replay));
    mapped=receive(control,BXWM_WINDOW,id,124,321,200);assert(mapped.identity==initial.identity);
    xcb_get_geometry_reply_t *border=xcb_get_geometry_reply(c,xcb_get_geometry(c,id),NULL);
    assert(border && !border->border_width);free(border);
    uint32_t requested_border=4;
    xcb_configure_window(c,id,XCB_CONFIG_WINDOW_BORDER_WIDTH,&requested_border);xcb_flush(c);
    mapped.x=40;mapped.y=50;mapped.width=640;mapped.height=360;
    send_command(control,mapped,BXWM_CONFIGURE);
    struct bxwm_message configured=receive(control,BXWM_WINDOW,id,124,640,360);
    assert(configured.x==40 && configured.y==50);
    border=xcb_get_geometry_reply(c,xcb_get_geometry(c,id),NULL);assert(border && !border->border_width);free(border);
    send_command(control,configured,BXWM_FOCUS);assert(wait_protocol(c,protocols,focus));
    xcb_get_input_focus_reply_t *focused=xcb_get_input_focus_reply(c,xcb_get_input_focus(c),NULL);
    assert(focused && focused->focus==id);free(focused);
    // A globally active client receives a real timestamp but takes focus itself.
    xcb_window_t global=xcb_generate_id(c);
    xcb_create_window(c,XCB_COPY_FROM_PARENT,global,screen->root,1,1,200,100,0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,0,NULL);
    xcb_change_property(c,XCB_PROP_MODE_REPLACE,global,protocols,XCB_ATOM_ATOM,32,2,supported);
    uint32_t hints[9]={1,0};xcb_atom_t wm_hints=atom(c,"WM_HINTS");
    xcb_change_property(c,XCB_PROP_MODE_REPLACE,global,wm_hints,wm_hints,32,9,hints);
    uint32_t decorations[5]={2,0,0,0,0};
    xcb_change_property(c,XCB_PROP_MODE_REPLACE,global,motif,motif,32,5,decorations);
    xcb_map_window(c,global);xcb_flush(c);
    struct bxwm_message active=receive(control,BXWM_WINDOW,global,0,200,100);
    assert(!(active.flags&BXWM_INPUT) && (active.flags&BXWM_CLIENT_CHROME));
    send_command(control,active,BXWM_FOCUS);xcb_timestamp_t stamp=wait_protocol(c,protocols,focus);assert(stamp);
    focused=xcb_get_input_focus_reply(c,xcb_get_input_focus(c),NULL);assert(focused && focused->focus==id);free(focused);
    xcb_set_input_focus(c,XCB_INPUT_FOCUS_PARENT,global,stamp);
    focused=xcb_get_input_focus_reply(c,xcb_get_input_focus(c),NULL);assert(focused && focused->focus==global);free(focused);
    // Native activation raises a window as well as focusing it. If the X root
    // stack is left unchanged, Xwayland can send clicks to a different sibling
    // from the one Brook actually paints on top (Steam/Friends reproducer).
    send_command(control,configured,BXWM_FOCUS);assert(wait_protocol(c,protocols,focus));
    xcb_query_tree_reply_t *tree=xcb_query_tree_reply(c,xcb_query_tree(c,screen->root),NULL);
    assert(tree && xcb_query_tree_children_length(tree)>0);
    int children=xcb_query_tree_children_length(tree);
    assert(xcb_query_tree_children(tree)[children-1]==id);free(tree);
    xcb_atom_t net_state=atom(c,"_NET_WM_STATE"),max_h=atom(c,"_NET_WM_STATE_MAXIMIZED_HORZ"),
        max_v=atom(c,"_NET_WM_STATE_MAXIMIZED_VERT"),moveresize=atom(c,"_NET_WM_MOVERESIZE");
    xcb_client_message_event_t request={.response_type=XCB_CLIENT_MESSAGE,.format=32,.window=id,.type=net_state};
    request.data.data32[0]=2;request.data.data32[1]=max_h;request.data.data32[2]=max_v;
    xcb_send_event(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&request);
    xcb_flush(c);struct bxwm_message action=receive(control,BXWM_MAXIMIZE,id,0,0,0);assert(action.x==2 && action.identity==configured.identity);
    request.type=moveresize;memset(&request.data,0,sizeof(request.data));request.data.data32[2]=8;request.data.data32[3]=1;
    xcb_send_event(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&request);
    xcb_flush(c);action=receive(control,BXWM_BEGIN_MOVE,id,0,0,0);assert(action.identity==configured.identity);
    request.data.data32[2]=0;
    xcb_send_event(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&request);
    xcb_flush(c);action=receive(control,BXWM_BEGIN_RESIZE,id,0,0,0);assert(action.x==0 && action.identity==configured.identity);
    request.type=atom(c,"WM_CHANGE_STATE");memset(&request.data,0,sizeof(request.data));request.data.data32[0]=3;
    xcb_send_event(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&request);
    xcb_flush(c);action=receive(control,BXWM_MINIMIZE,id,0,0,0);assert(action.identity==configured.identity);
    // Native window-state changes must retain unrelated EWMH state atoms.
    xcb_atom_t foreign=atom(c,"_NET_WM_STATE_ABOVE");
    xcb_void_cookie_t property_cookie=xcb_change_property_checked(c,XCB_PROP_MODE_REPLACE,id,net_state,XCB_ATOM_ATOM,32,1,&foreign);
    assert(!xcb_request_check(c,property_cookie));
    struct bxwm_message native_state=configured;native_state.flags|=BXWM_MAXIMIZED|BXWM_MINIMIZED;
    send_command(control,native_state,BXWM_CONFIGURE);
    native_state=receive(control,BXWM_WINDOW,id,124,640,360);
    assert((native_state.flags&(BXWM_MAXIMIZED|BXWM_MINIMIZED))==(BXWM_MAXIMIZED|BXWM_MINIMIZED));
    xcb_get_property_reply_t *state_reply=xcb_get_property_reply(c,xcb_get_property(c,0,id,net_state,XCB_ATOM_ATOM,0,4),NULL);
    assert(state_reply && state_reply->format==32 && xcb_get_property_value_length(state_reply)==12);
    assert(((xcb_atom_t *)xcb_get_property_value(state_reply))[0]==foreign &&
        ((xcb_atom_t *)xcb_get_property_value(state_reply))[1]==max_h &&
        ((xcb_atom_t *)xcb_get_property_value(state_reply))[2]==max_v);free(state_reply);
    send_command(control,configured,BXWM_CONFIGURE);
    native_state=receive(control,BXWM_WINDOW,id,124,640,360);
    assert(!(native_state.flags&(BXWM_MAXIMIZED|BXWM_MINIMIZED)));
    state_reply=xcb_get_property_reply(c,xcb_get_property(c,0,id,net_state,XCB_ATOM_ATOM,0,4),NULL);
    assert(state_reply && state_reply->format==32 && xcb_get_property_value_length(state_reply)==4 &&
        ((xcb_atom_t *)xcb_get_property_value(state_reply))[0]==foreign);free(state_reply);
    const char malformed_state[]="client-data";
    property_cookie=xcb_change_property_checked(c,XCB_PROP_MODE_REPLACE,id,net_state,XCB_ATOM_STRING,8,sizeof(malformed_state),malformed_state);
    assert(!xcb_request_check(c,property_cookie));
    native_state=configured;native_state.flags|=BXWM_MAXIMIZED;
    send_command(control,native_state,BXWM_CONFIGURE);receive(control,BXWM_WINDOW,id,124,640,360);
    state_reply=xcb_get_property_reply(c,xcb_get_property(c,0,id,net_state,XCB_GET_PROPERTY_TYPE_ANY,0,8),NULL);
    assert(state_reply && state_reply->type==XCB_ATOM_STRING && state_reply->format==8 &&
        xcb_get_property_value_length(state_reply)==sizeof(malformed_state) &&
        !memcmp(xcb_get_property_value(state_reply),malformed_state,sizeof(malformed_state)));free(state_reply);
    send_command(control,configured,BXWM_CONFIGURE);receive(control,BXWM_WINDOW,id,124,640,360);
    xcb_atom_t axis_state[]={foreign,max_h};
    property_cookie=xcb_change_property_checked(c,XCB_PROP_MODE_REPLACE,id,net_state,XCB_ATOM_ATOM,32,2,axis_state);
    assert(!xcb_request_check(c,property_cookie));
    native_state=configured;native_state.flags|=BXWM_MINIMIZED;
    send_command(control,native_state,BXWM_CONFIGURE);receive(control,BXWM_WINDOW,id,124,640,360);
    state_reply=xcb_get_property_reply(c,xcb_get_property(c,0,id,net_state,XCB_ATOM_ATOM,0,4),NULL);
    assert(state_reply && xcb_get_property_value_length(state_reply)==8 &&
        !memcmp(xcb_get_property_value(state_reply),axis_state,sizeof(axis_state)));free(state_reply);
    send_command(control,configured,BXWM_CONFIGURE);receive(control,BXWM_WINDOW,id,124,640,360);
    xcb_atom_t many[4095];for(size_t i=0;i<4095;++i)many[i]=foreign;
    property_cookie=xcb_change_property_checked(c,XCB_PROP_MODE_REPLACE,id,net_state,XCB_ATOM_ATOM,32,4095,many);
    assert(!xcb_request_check(c,property_cookie));
    native_state=configured;native_state.flags|=BXWM_MAXIMIZED;
    send_command(control,native_state,BXWM_CONFIGURE);receive(control,BXWM_WINDOW,id,124,640,360);
    state_reply=xcb_get_property_reply(c,xcb_get_property(c,0,id,net_state,XCB_ATOM_ATOM,0,4096),NULL);
    assert(state_reply && !state_reply->bytes_after && xcb_get_property_value_length(state_reply)==sizeof(many) &&
        !memcmp(xcb_get_property_value(state_reply),many,sizeof(many)));free(state_reply);
    send_command(control,configured,BXWM_CONFIGURE);receive(control,BXWM_WINDOW,id,124,640,360);
    // Reparenting away retires the native identity. Returning creates a new one.
    xcb_reparent_window(c,global,id,0,0);xcb_flush(c);
    struct bxwm_message retired=receive(control,BXWM_DESTROY,global,0,0,0);assert(retired.identity==active.identity);
    xcb_reparent_window(c,global,screen->root,1,1);xcb_map_window(c,global);xcb_flush(c);
    struct bxwm_message returned=receive(control,BXWM_WINDOW,global,0,200,100);assert(returned.identity!=active.identity);
    send_command(control,configured,BXWM_CLOSE);wait_protocol(c,protocols,close);
    xcb_destroy_window(c,id);xcb_flush(c);
    struct bxwm_message destroyed=receive(control,BXWM_DESTROY,id,0,0,0);
    assert(destroyed.identity==initial.identity);
    /* Reuse the numeric XID. A queued configure with the old identity is ignored. */
    xcb_create_window(c,XCB_COPY_FROM_PARENT,id,screen->root,2,3,300,180,0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,0,NULL);
    xcb_change_property(c,XCB_PROP_MODE_REPLACE,id,protocols,XCB_ATOM_ATOM,32,2,supported);
    xcb_map_window(c,id);xcb_flush(c);
    struct bxwm_message replacement=receive(control,BXWM_WINDOW,id,0,300,180);
    assert(replacement.identity!=initial.identity);
    send_command(control,configured,BXWM_CONFIGURE);
    send_command(control,replacement,BXWM_FOCUS);wait_protocol(c,protocols,focus);
    xcb_get_geometry_reply_t *geometry=xcb_get_geometry_reply(c,xcb_get_geometry(c,id),NULL);
    assert(geometry && geometry->width==300 && geometry->height==180);free(geometry);
    replacement.x=11;replacement.y=12;replacement.width=301;replacement.height=181;
    send_command(control,replacement,BXWM_CONFIGURE);
    receive(control,BXWM_WINDOW,id,0,301,181);
    geometry=xcb_get_geometry_reply(c,xcb_get_geometry(c,id),NULL);
    assert(geometry && geometry->width==301 && geometry->height==181);free(geometry);
    if (!getenv("BROOK_XWM_PROTOCOL")) {
        xcb_disconnect(c);puts("XWM_HELPER_PASS protocol 1");return 0;
    }
    // EWMH fullscreen. The WM advertises it; a property set before mapping and
    // later client messages become native requests; the native answer is
    // published in _NET_WM_STATE with a synthetic root-relative ConfigureNotify;
    // geometry requests are refused while fullscreen.
    xcb_atom_t fullscreen=atom(c,"_NET_WM_STATE_FULLSCREEN");
    xcb_get_property_reply_t *supported_reply=xcb_get_property_reply(c,xcb_get_property(c,0,screen->root,
        atom(c,"_NET_SUPPORTED"),XCB_ATOM_ATOM,0,64),NULL);
    int advertised=0;assert(supported_reply);
    for (int i=0;i<xcb_get_property_value_length(supported_reply)/4;++i)
        if (((xcb_atom_t *)xcb_get_property_value(supported_reply))[i]==fullscreen) advertised=1;
    free(supported_reply);assert(advertised);
    xcb_window_t game=xcb_generate_id(c);uint32_t structure=XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_create_window(c,XCB_COPY_FROM_PARENT,game,screen->root,0,0,640,360,0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,XCB_CW_EVENT_MASK,&structure);
    xcb_change_property(c,XCB_PROP_MODE_REPLACE,game,net_state,XCB_ATOM_ATOM,32,1,&fullscreen);
    xcb_map_window(c,game);xcb_flush(c);
    // The mapped state is published before the request it qualifies.
    struct bxwm_message full=receive(control,BXWM_WINDOW,game,0,640,360);
    struct bxwm_message wanted=receive(control,BXWM_FULLSCREEN_REQUEST,game,0,0,0);assert(wanted.x==1);
    full.x=0;full.y=0;full.width=800;full.height=600;full.flags|=BXWM_FULLSCREEN;full.configure_serial=1;
    send_command(control,full,BXWM_CONFIGURE);
    xcb_configure_notify_event_t notify=wait_configure(c,game,1,800,600);assert(!notify.x && !notify.y);
    full=receive(control,BXWM_WINDOW,game,0,800,600);assert(full.flags&BXWM_FULLSCREEN);
    assert(state_has(c,game,net_state,fullscreen));
    uint32_t smaller[]={10,20,320,200};
    xcb_configure_window(c,game,XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y|XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,smaller);
    xcb_flush(c);notify=wait_configure(c,game,1,800,600);assert(!notify.x && !notify.y);
    geometry=xcb_get_geometry_reply(c,xcb_get_geometry(c,game),NULL);
    assert(geometry && geometry->width==800 && geometry->height==600 && !geometry->x && !geometry->y);free(geometry);
    xcb_client_message_event_t toggle={.response_type=XCB_CLIENT_MESSAGE,.format=32,.window=game,.type=net_state};
    toggle.data.data32[0]=0;toggle.data.data32[1]=fullscreen;
    xcb_send_event(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&toggle);
    xcb_flush(c);wanted=receive(control,BXWM_FULLSCREEN_REQUEST,game,0,0,0);assert(wanted.x==0);
    full.flags&=~BXWM_FULLSCREEN;full.x=30;full.y=40;full.width=640;full.height=360;full.configure_serial=2;
    send_command(control,full,BXWM_CONFIGURE);notify=wait_configure(c,game,1,640,360);
    assert(notify.x==30 && notify.y==40);receive(control,BXWM_WINDOW,game,0,640,360);
    assert(!state_has(c,game,net_state,fullscreen));
    // Unfullscreened windows get their geometry requests again.
    xcb_configure_window(c,game,XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y|XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,smaller);
    xcb_flush(c);receive(control,BXWM_WINDOW,game,0,320,200);
    toggle.data.data32[0]=2;toggle.data.data32[1]=0;toggle.data.data32[2]=fullscreen;
    xcb_send_event(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&toggle);
    xcb_flush(c);wanted=receive(control,BXWM_FULLSCREEN_REQUEST,game,0,0,0);assert(wanted.x==2);
    // A remap without the atom reports that absence.
    xcb_unmap_window(c,game);xcb_map_window(c,game);xcb_flush(c);
    wanted=receive(control,BXWM_FULLSCREEN_REQUEST,game,0,0,0);assert(wanted.x==0);
    xcb_destroy_window(c,game);
    // Scale hints for "apps scale themselves" (BROOK_XWM_SCALE=2).
    xcb_get_property_reply_t *resources=xcb_get_property_reply(c,xcb_get_property(c,0,screen->root,
        atom(c,"RESOURCE_MANAGER"),XCB_ATOM_STRING,0,4096),NULL);
    assert(resources && memmem(xcb_get_property_value(resources),xcb_get_property_value_length(resources),
        "Xft.dpi:\t192\n",13) && memmem(xcb_get_property_value(resources),
        xcb_get_property_value_length(resources),"Xcursor.size:\t48\n",17));free(resources);
    xcb_get_selection_owner_reply_t *owner=xcb_get_selection_owner_reply(c,
        xcb_get_selection_owner(c,atom(c,"_XSETTINGS_S0")),NULL);
    assert(owner && owner->owner!=XCB_WINDOW_NONE);
    xcb_atom_t settings_atom=atom(c,"_XSETTINGS_SETTINGS");
    xcb_get_property_reply_t *settings=xcb_get_property_reply(c,xcb_get_property(c,0,owner->owner,
        settings_atom,settings_atom,0,256),NULL);free(owner);
    assert(settings && settings->format==8);
    const uint8_t *bytes=xcb_get_property_value(settings);size_t length=(size_t)xcb_get_property_value_length(settings);
    uint32_t count;memcpy(&count,bytes+8,4);assert(!bytes[0] && count==4);
    int32_t scaling=0,font_dpi=0,unscaled=0;
    for (size_t at=12,i=0;i<count;++i) {
        uint16_t name_length;memcpy(&name_length,bytes+at+2,2);assert(bytes[at]==0);
        const char *name=(const char *)bytes+at+4;at+=4+((name_length+3u)&~3u)+4;
        int32_t value;memcpy(&value,bytes+at,4);at+=4;assert(at<=length);
        if (name_length==23 && !memcmp(name,"Gdk/WindowScalingFactor",23)) scaling=value;
        if (name_length==7 && !memcmp(name,"Xft/DPI",7)) font_dpi=value;
        if (name_length==15 && !memcmp(name,"Gdk/UnscaledDPI",15)) unscaled=value;
    }
    free(settings);assert(scaling==2 && font_dpi==192*1024 && unscaled==96*1024);
    // Per-app "scaled by the system": a listed WM_CLASS marks its X client;
    // that client's later override-redirect popup follows and is anchored to
    // its active window. Another client stays native.
    xcb_connection_t *legacy=xcb_connect(argv[1],NULL);assert(legacy && !xcb_connection_has_error(legacy));
    xcb_window_t old_app=xcb_generate_id(legacy);
    xcb_create_window(legacy,XCB_COPY_FROM_PARENT,old_app,screen->root,50,60,200,100,0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,0,NULL);
    static const char class_name[]="legacy-app\0Brook-Scaled-Test";
    xcb_change_property(legacy,XCB_PROP_MODE_REPLACE,old_app,XCB_ATOM_WM_CLASS,XCB_ATOM_STRING,8,sizeof(class_name),class_name);
    xcb_map_window(legacy,old_app);xcb_flush(legacy);
    struct bxwm_message scaled=receive(control,BXWM_WINDOW,old_app,0,200,100);assert(scaled.flags&BXWM_SCALED);
    xcb_window_t popup=xcb_generate_id(legacy);uint32_t override=1;
    xcb_create_window(legacy,XCB_COPY_FROM_PARENT,popup,screen->root,70,90,40,30,0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,XCB_CW_OVERRIDE_REDIRECT,&override);
    xcb_map_window(legacy,popup);xcb_flush(legacy);
    struct bxwm_message menu=receive(control,BXWM_WINDOW,popup,0,40,30);
    assert((menu.flags&(BXWM_SCALED|BXWM_OVERRIDE))==(BXWM_SCALED|BXWM_OVERRIDE));
    assert(menu.parent==old_app && menu.parent_identity==scaled.identity);
    xcb_window_t native_app=xcb_generate_id(c);
    xcb_create_window(c,XCB_COPY_FROM_PARENT,native_app,screen->root,5,5,120,80,0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,0,NULL);
    static const char other_class[]="modern\0Modern";
    xcb_change_property(c,XCB_PROP_MODE_REPLACE,native_app,XCB_ATOM_WM_CLASS,XCB_ATOM_STRING,8,sizeof(other_class),other_class);
    xcb_map_window(c,native_app);xcb_flush(c);
    struct bxwm_message native=receive(control,BXWM_WINDOW,native_app,0,120,80);assert(!(native.flags&BXWM_SCALED));
    xcb_disconnect(legacy);
    xcb_disconnect(c);
    puts("XWM_HELPER_PASS isolated X server: InputOnly, geometry, focus/stack, client chrome/actions, native state, EWMH fullscreen, scale hints, scaled clients, close, reparent and XID reuse");
    return 0;
}
