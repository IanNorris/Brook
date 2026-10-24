#define _GNU_SOURCE
#include <assert.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <xcb/xcb.h>
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
int main(int argc,char **argv) {
    /* Runner supplies a newly created headless Xvfb display, never DISPLAY. */
    assert(argc==3);int control=atoi(argv[2]);
    struct bxwm_message ready=receive(control,BXWM_READY,0,0,0,0);
    xcb_connection_t *c=xcb_connect(argv[1],NULL);assert(c && !xcb_connection_has_error(c));
    xcb_screen_t *screen=xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    assert(screen->root==ready.window);
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
    xcb_send_event(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,(const char *)&association);
    xcb_flush(c);struct bxwm_message mapped=receive(control,BXWM_WINDOW,id,123,0,0);
    assert(mapped.identity==initial.identity && !(mapped.flags&BXWM_CLIENT_CHROME));
    xcb_get_geometry_reply_t *border=xcb_get_geometry_reply(c,xcb_get_geometry(c,id),NULL);
    assert(border && !border->border_width);free(border);
    uint32_t requested_border=4;
    xcb_configure_window(c,id,XCB_CONFIG_WINDOW_BORDER_WIDTH,&requested_border);xcb_flush(c);
    mapped.x=40;mapped.y=50;mapped.width=640;mapped.height=360;
    send_command(control,mapped,BXWM_CONFIGURE);
    struct bxwm_message configured=receive(control,BXWM_WINDOW,id,123,640,360);
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
    struct bxwm_message native_state=configured;native_state.flags|=BXWM_MAXIMIZED|BXWM_MINIMIZED;
    send_command(control,native_state,BXWM_CONFIGURE);
    native_state=receive(control,BXWM_WINDOW,id,123,640,360);
    assert((native_state.flags&(BXWM_MAXIMIZED|BXWM_MINIMIZED))==(BXWM_MAXIMIZED|BXWM_MINIMIZED));
    xcb_get_property_reply_t *state_reply=xcb_get_property_reply(c,xcb_get_property(c,0,id,net_state,XCB_ATOM_ATOM,0,2),NULL);
    assert(state_reply && state_reply->format==32 && xcb_get_property_value_length(state_reply)==8);
    assert(((xcb_atom_t *)xcb_get_property_value(state_reply))[0]==max_h &&
        ((xcb_atom_t *)xcb_get_property_value(state_reply))[1]==max_v);free(state_reply);
    send_command(control,configured,BXWM_CONFIGURE);
    native_state=receive(control,BXWM_WINDOW,id,123,640,360);
    assert(!(native_state.flags&(BXWM_MAXIMIZED|BXWM_MINIMIZED)));
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
    xcb_disconnect(c);
    puts("XWM_HELPER_PASS isolated X server: InputOnly, geometry, focus/stack, client chrome/actions, native state, close, reparent and XID reuse");
    return 0;
}
