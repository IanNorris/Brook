/* Private XWM worker. Never opens DISPLAY: both connections are inherited from
 * waylandd's owned Xwayland launch. XCB reply waits occur only in this process,
 * while waylandd remains free to dispatch Xwayland's Wayland requests. */
#define _GNU_SOURCE
#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include "xwm-wire.h"

struct window {
    struct window *next;
    struct bxwm_message state,last;
};
struct xwm {
    xcb_connection_t *xcb;
    xcb_window_t root,support;
    int control;
    uint64_t generation,next_identity;
    struct bxwm_stream stream;
    struct window *windows;
    xcb_atom_t serial,protocols,delete_window,take_focus,hints,transient;
    xcb_atom_t utf8,name,supporting,supported,active,wm_state;
    xcb_atom_t time_property;
    xcb_timestamp_t timestamp;
    uint32_t time_counter;
    struct bxwm_message pending_focus;
    int failed;
};
static xcb_atom_t atom(struct xwm *wm,const char *name) {
    xcb_intern_atom_reply_t *reply=xcb_intern_atom_reply(wm->xcb,
        xcb_intern_atom(wm->xcb,0,(uint16_t)strlen(name),name),NULL);
    if (!reply) return XCB_ATOM_NONE;
    xcb_atom_t result=reply->atom;free(reply);return result;
}
static xcb_get_property_reply_t *property(struct xwm *wm,xcb_window_t window,xcb_atom_t name,uint32_t words) {
    return xcb_get_property_reply(wm->xcb,xcb_get_property(wm->xcb,0,window,name,XCB_GET_PROPERTY_TYPE_ANY,0,words),NULL);
}
static uint32_t property_word(struct xwm *wm,xcb_window_t window,xcb_atom_t name,uint32_t fallback) {
    xcb_get_property_reply_t *reply=property(wm,window,name,1);uint32_t result=fallback;
    if (reply && reply->format==32 && xcb_get_property_value_length(reply)>=4)
        memcpy(&result,xcb_get_property_value(reply),4);
    free(reply);return result;
}
static int has_protocol(struct xwm *wm,xcb_window_t window,xcb_atom_t protocol) {
    xcb_get_property_reply_t *reply=property(wm,window,wm->protocols,256);int found=0;
    if (reply && reply->format==32) {
        uint32_t *values=xcb_get_property_value(reply);
        for (int i=0;i<xcb_get_property_value_length(reply)/4;++i) if (values[i]==protocol) found=1;
    }
    free(reply);return found;
}
static struct window *find(struct xwm *wm,xcb_window_t id) {
    for (struct window *w=wm->windows;w;w=w->next) if (w->state.window==id) return w;
    return NULL;
}
static int emit(struct xwm *wm,const struct bxwm_message *message) {
    int result=bxwm_enqueue(&wm->stream,message) && bxwm_flush(wm->control,&wm->stream)>=0;
    if (!result) wm->failed=1;
    return result;
}
static int publish(struct xwm *wm,struct window *w) {
    if (!memcmp(&w->state,&w->last,sizeof(w->state))) return 1;
    if (!emit(wm,&w->state)) return 0;
    w->last=w->state;return 1;
}
static int refresh(struct xwm *wm,struct window *w) {
    xcb_get_geometry_reply_t *geometry=xcb_get_geometry_reply(wm->xcb,xcb_get_geometry(wm->xcb,w->state.window),NULL);
    xcb_get_window_attributes_reply_t *attributes=xcb_get_window_attributes_reply(wm->xcb,
        xcb_get_window_attributes(wm->xcb,w->state.window),NULL);
    if (!geometry || !attributes) { free(geometry);free(attributes);return 1; }
    if (geometry->border_width) {
        uint32_t zero=0;
        xcb_configure_window(wm->xcb,w->state.window,XCB_CONFIG_WINDOW_BORDER_WIDTH,&zero);
    }
    w->state.x=geometry->x;w->state.y=geometry->y;
    w->state.width=geometry->width;w->state.height=geometry->height;
    w->state.flags=(attributes->override_redirect?BXWM_OVERRIDE:0)|
        (attributes->map_state!=XCB_MAP_STATE_UNMAPPED?BXWM_MAPPED:0)|BXWM_INPUT;
    free(geometry);free(attributes);
    xcb_get_property_reply_t *hints=property(wm,w->state.window,wm->hints,9);
    if (hints && hints->format==32 && xcb_get_property_value_length(hints)>=8) {
        uint32_t *values=xcb_get_property_value(hints);
        if ((values[0]&1) && !values[1]) w->state.flags&=~BXWM_INPUT;
    }
    free(hints);
    uint32_t parent=property_word(wm,w->state.window,wm->transient,0);
    if (parent!=w->state.parent) w->state.parent_identity=0;
    w->state.parent=parent;
    struct window *parent_window=find(wm,parent);
    if (!w->state.parent_identity && parent_window) w->state.parent_identity=parent_window->state.identity;
    xcb_get_property_reply_t *title=property(wm,w->state.window,wm->name,16);
    if (!title || title->format!=8 || !xcb_get_property_value_length(title)) {
        free(title);title=property(wm,w->state.window,XCB_ATOM_WM_NAME,16);
    }
    memset(w->state.title,0,sizeof(w->state.title));
    if (title && title->format==8) {
        size_t length=(size_t)xcb_get_property_value_length(title);
        if (length>=sizeof(w->state.title)) length=sizeof(w->state.title)-1;
        memcpy(w->state.title,xcb_get_property_value(title),length);
    }
    free(title);return publish(wm,w);
}
static struct window *track(struct xwm *wm,xcb_window_t id) {
    struct window *w=find(wm,id);if (w) return w;
    if (!id || id==wm->root || id==wm->support || wm->next_identity==UINT64_MAX) return NULL;
    xcb_query_tree_reply_t *tree=xcb_query_tree_reply(wm->xcb,xcb_query_tree(wm->xcb,id),NULL);
    if (!tree || tree->parent!=wm->root) { free(tree);return NULL; }free(tree);
    xcb_get_window_attributes_reply_t *attributes=xcb_get_window_attributes_reply(wm->xcb,xcb_get_window_attributes(wm->xcb,id),NULL);
    if (!attributes || attributes->_class==XCB_WINDOW_CLASS_INPUT_ONLY) { free(attributes);return NULL; }
    free(attributes);
    w=calloc(1,sizeof(*w));if (!w) { wm->failed=1;return NULL; }
    w->state=(struct bxwm_message){.version=BXWM_VERSION,.type=BXWM_WINDOW,
        .generation=wm->generation,.identity=++wm->next_identity,.window=id};
    w->next=wm->windows;wm->windows=w;
    for (struct window *child=w->next;child;child=child->next) {
        if (child->state.parent==id && !child->state.parent_identity) {
            child->state.parent_identity=w->state.identity;
            if (!publish(wm,child)) { errno=ENOBUFS;return NULL; }
        }
    }
    uint32_t mask=XCB_EVENT_MASK_PROPERTY_CHANGE|XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(wm->xcb,id,XCB_CW_EVENT_MASK,&mask);
    return w;
}
static void protocol_message(struct xwm *wm,xcb_window_t window,xcb_atom_t protocol,xcb_timestamp_t timestamp) {
    xcb_client_message_event_t event={.response_type=XCB_CLIENT_MESSAGE,.format=32,
        .window=window,.type=wm->protocols};
    event.data.data32[0]=protocol;event.data.data32[1]=timestamp;
    xcb_send_event(wm->xcb,0,window,XCB_EVENT_MASK_NO_EVENT,(const char *)&event);
}
static int command(struct xwm *wm,const struct bxwm_message *message) {
    if (!bxwm_valid(message,wm->generation,0)) return 0;
    struct window *w=find(wm,message->window);
    /* A command queued before an XID was destroyed must never affect its reuse. */
    if (!w || w->state.identity!=message->identity) return 1;
    switch (message->type) {
    case BXWM_CONFIGURE: {
        w->state.configure_serial=message->configure_serial;
        uint32_t values[]={(uint32_t)message->x,(uint32_t)message->y,message->width,message->height};
        xcb_configure_window(wm->xcb,message->window,XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y|
            XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,values);break;
    }
    case BXWM_FOCUS:
        // Obtain an X-server timestamp asynchronously. Brook monotonic time is
        // not in the X server's clock domain, and WM_TAKE_FOCUS forbids zero.
        wm->pending_focus=*message;++wm->time_counter;
        xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,wm->support,wm->time_property,XCB_ATOM_INTEGER,32,1,&wm->time_counter);
        break;
    case BXWM_CLOSE:
        if (has_protocol(wm,message->window,wm->delete_window)) protocol_message(wm,message->window,wm->delete_window,wm->timestamp);
        else xcb_kill_client(wm->xcb,message->window);
        break;
    default:return 0;
    }
    return xcb_flush(wm->xcb)>0;
}
static int event(struct xwm *wm,xcb_generic_event_t *generic) {
    unsigned type=generic->response_type&127;
    struct window *w=NULL;
    switch (type) {
    case XCB_CREATE_NOTIFY: {
        xcb_create_notify_event_t *e=(void *)generic;
        if (e->parent==wm->root) w=track(wm,e->window);
        break;
    }
    case XCB_MAP_REQUEST: {
        xcb_map_request_event_t *e=(void *)generic;w=track(wm,e->window);
        if (w) {
            uint32_t state[]={1,XCB_WINDOW_NONE};
            xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,e->window,wm->wm_state,wm->wm_state,32,2,state);
        }
        // InputOnly windows do not get native presentation, but still need
        // their ordinary X11 map request honored (grabs/focus/event routing).
        xcb_map_window(wm->xcb,e->window);
        break;
    }
    case XCB_MAP_NOTIFY:w=track(wm,((xcb_map_notify_event_t *)generic)->window);break;
    case XCB_UNMAP_NOTIFY: {
        w=find(wm,((xcb_unmap_notify_event_t *)generic)->window);
        if (w) {
            w->state.flags&=~BXWM_MAPPED;
            if (!publish(wm,w)) return 0;
            w->state.serial=0;
        }
        return 1;
    }
    case XCB_CONFIGURE_REQUEST: {
        xcb_configure_request_event_t *e=(void *)generic;
        uint32_t values[7];unsigned n=0;
        if (e->value_mask&XCB_CONFIG_WINDOW_X) values[n++]=(uint32_t)e->x;
        if (e->value_mask&XCB_CONFIG_WINDOW_Y) values[n++]=(uint32_t)e->y;
        if (e->value_mask&XCB_CONFIG_WINDOW_WIDTH) values[n++]=e->width;
        if (e->value_mask&XCB_CONFIG_WINDOW_HEIGHT) values[n++]=e->height;
        if (e->value_mask&XCB_CONFIG_WINDOW_BORDER_WIDTH) values[n++]=0;
        if (e->value_mask&XCB_CONFIG_WINDOW_SIBLING) values[n++]=e->sibling;
        if (e->value_mask&XCB_CONFIG_WINDOW_STACK_MODE) values[n++]=e->stack_mode;
        xcb_configure_window(wm->xcb,e->window,e->value_mask,values);
        w=track(wm,e->window);break;
    }
    case XCB_CONFIGURE_NOTIFY:w=find(wm,((xcb_configure_notify_event_t *)generic)->window);break;
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *e=(void *)generic;
        if (e->window==wm->support && e->atom==wm->time_property && e->time) {
            wm->timestamp=e->time;
            struct bxwm_message focus=wm->pending_focus;wm->pending_focus.identity=0;
            w=find(wm,focus.window);
            if (focus.identity && w && w->state.identity==focus.identity && (w->state.flags&BXWM_MAPPED)) {
                if (w->state.flags&BXWM_INPUT)
                    xcb_set_input_focus(wm->xcb,XCB_INPUT_FOCUS_PARENT,focus.window,e->time);
                if (has_protocol(wm,focus.window,wm->take_focus)) protocol_message(wm,focus.window,wm->take_focus,e->time);
                xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,wm->root,wm->active,XCB_ATOM_WINDOW,32,1,&focus.window);
                xcb_flush(wm->xcb);
            }
            return 1;
        }
        w=find(wm,e->window);
        if (w && e->atom==wm->transient) w->state.parent_identity=0;
        break;
    }
    case XCB_REPARENT_NOTIFY: {
        xcb_reparent_notify_event_t *e=(void *)generic;
        if (e->parent==wm->root) { w=track(wm,e->window);break; }
        struct window **link=&wm->windows;
        while (*link && (*link)->state.window!=e->window) link=&(*link)->next;
        if (!*link) return 1;
        w=*link;struct bxwm_message message=w->state;message.type=BXWM_DESTROY;
        *link=w->next;free(w);return emit(wm,&message);
    }
    case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t *e=(void *)generic;
        if (e->format!=32) return 1;
        w=track(wm,e->window);
        if (w && e->type==wm->serial) {
            uint64_t serial=(uint64_t)e->data.data32[0]|((uint64_t)e->data.data32[1]<<32);
            if (!serial || (w->state.serial && serial<=w->state.serial)) return 0;
            w->state.serial=serial;
        } else return 1;
        break;
    }
    case XCB_DESTROY_NOTIFY: {
        xcb_window_t id=((xcb_destroy_notify_event_t *)generic)->window;
        struct window **link=&wm->windows;
        while (*link && (*link)->state.window!=id) link=&(*link)->next;
        if (!*link) return 1;
        w=*link;struct bxwm_message message=w->state;message.type=BXWM_DESTROY;
        *link=w->next;free(w);return emit(wm,&message);
    }
    default:return 1;
    }
    return !w || refresh(wm,w);
}
static int initialize(struct xwm *wm,int wm_fd) {
    wm->xcb=xcb_connect_to_fd(wm_fd,NULL);
    if (!wm->xcb || xcb_connection_has_error(wm->xcb)) return 0;
    xcb_screen_iterator_t screens=xcb_setup_roots_iterator(xcb_get_setup(wm->xcb));
    if (!screens.rem) return 0;
    wm->root=screens.data->root;
    // Rootless Xwayland only creates Wayland surfaces for manually redirected
    // X windows. Synthetic serial events in an Xvfb control cannot prove this.
    xcb_composite_query_version_reply_t *composite=xcb_composite_query_version_reply(wm->xcb,
        xcb_composite_query_version(wm->xcb,0,4),NULL);
    if (!composite || (!composite->major_version && composite->minor_version<1)) {
        free(composite);return 0;
    }
    free(composite);
    xcb_generic_error_t *redirect_error=xcb_request_check(wm->xcb,
        xcb_composite_redirect_subwindows_checked(wm->xcb,wm->root,XCB_COMPOSITE_REDIRECT_MANUAL));
    if (redirect_error) { free(redirect_error);return 0; }
    wm->serial=atom(wm,"WL_SURFACE_SERIAL");wm->protocols=atom(wm,"WM_PROTOCOLS");
    wm->delete_window=atom(wm,"WM_DELETE_WINDOW");wm->take_focus=atom(wm,"WM_TAKE_FOCUS");
    wm->hints=atom(wm,"WM_HINTS");wm->transient=atom(wm,"WM_TRANSIENT_FOR");
    wm->utf8=atom(wm,"UTF8_STRING");wm->name=atom(wm,"_NET_WM_NAME");
    wm->supporting=atom(wm,"_NET_SUPPORTING_WM_CHECK");wm->supported=atom(wm,"_NET_SUPPORTED");
    wm->active=atom(wm,"_NET_ACTIVE_WINDOW");wm->wm_state=atom(wm,"WM_STATE");
    wm->time_property=atom(wm,"_BROOK_XWM_TIME");
    if (!wm->serial || !wm->wm_state) return 0;
    uint32_t mask=XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY|XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_generic_error_t *error=xcb_request_check(wm->xcb,xcb_change_window_attributes_checked(wm->xcb,wm->root,XCB_CW_EVENT_MASK,&mask));
    if (error) { free(error);return 0; }
    wm->support=xcb_generate_id(wm->xcb);
    uint32_t support_events=XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(wm->xcb,XCB_COPY_FROM_PARENT,wm->support,wm->root,0,0,1,1,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,XCB_COPY_FROM_PARENT,XCB_CW_EVENT_MASK,&support_events);
    xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,wm->root,wm->supporting,XCB_ATOM_WINDOW,32,1,&wm->support);
    xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,wm->support,wm->supporting,XCB_ATOM_WINDOW,32,1,&wm->support);
    xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,wm->support,wm->name,wm->utf8,8,9,"Brook XWM");
    xcb_atom_t supported[]={wm->supporting,wm->name};
    xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,wm->root,wm->supported,XCB_ATOM_ATOM,32,2,supported);
    xcb_flush(wm->xcb);
    struct bxwm_message ready={.version=BXWM_VERSION,.type=BXWM_READY,.generation=wm->generation,
        .window=wm->root,.width=screens.data->width_in_pixels,.height=screens.data->height_in_pixels};
    return emit(wm,&ready);
}
static int number(const char *text,uint64_t *out) {
    if (!text || !*text) return 0;
    for (const char *p=text;*p;++p) if (*p<'0' || *p>'9') return 0;
    errno=0;char *end;*out=strtoull(text,&end,10);return !errno && !*end;
}
int main(int argc,char **argv) {
    uint64_t wm_fd,control,generation;
    if (argc!=4 || !number(argv[1],&wm_fd) || !number(argv[2],&control) || !number(argv[3],&generation) ||
        wm_fd>INT_MAX || control>INT_MAX || wm_fd==control || !generation ||
        fcntl((int)wm_fd,F_GETFD)<0 || fcntl((int)control,F_GETFD)<0) return 2;
    signal(SIGPIPE,SIG_IGN);
    struct xwm wm={.control=(int)control,.generation=generation};
    int success=initialize(&wm,(int)wm_fd);
    while (success && !wm.failed && !xcb_connection_has_error(wm.xcb)) {
        xcb_generic_event_t *e;
        unsigned work=0;
        while (work<64 && (e=xcb_poll_for_event(wm.xcb))) { ++work;success=event(&wm,e);free(e);if (!success) break; }
        if (!success || wm.failed) { success=0;break; }
        struct bxwm_message message;int result;
        unsigned commands=0;result=0;
        while (commands<64 && (result=bxwm_receive(wm.control,&wm.stream,&message))>0) {
            ++commands;
            if (!command(&wm,&message)) { success=0;break; }
        }
        if (result<0 || !success || bxwm_flush(wm.control,&wm.stream)<0) { success=0;break; }
        if (xcb_flush(wm.xcb)<=0) { success=0;break; }
        // Synchronous replies above may have read other X events into XCB's
        // userspace queue. Service one before sleeping on the now-empty fd.
        if ((e=xcb_poll_for_queued_event(wm.xcb))) {
            success=event(&wm,e);free(e);continue;
        }
        if (work==64 || commands==64) continue;
        struct pollfd fds[]={{xcb_get_file_descriptor(wm.xcb),POLLIN,0},
            {wm.control,POLLIN|(wm.stream.count?POLLOUT:0),0}};
        if (poll(fds,2,-1)<0 && errno!=EINTR) { success=0;break; }
        if ((fds[0].revents|fds[1].revents)&(POLLERR|POLLHUP|POLLNVAL)) { success=0;break; }
    }
    if (wm.failed || (wm.xcb && xcb_connection_has_error(wm.xcb))) success=0;
    while (wm.windows) { struct window *w=wm.windows;wm.windows=w->next;free(w); }
    if (wm.xcb) xcb_disconnect(wm.xcb);
    close(wm.control);return success?0:1;
}
