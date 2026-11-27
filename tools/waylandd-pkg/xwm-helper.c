/* Private XWM worker. Never opens DISPLAY: both connections are inherited from
 * waylandd's owned Xwayland launch. XCB reply waits occur only in this process,
 * while waylandd remains free to dispatch Xwayland's Wayland requests. */
#define _GNU_SOURCE
#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/xcb_cursor.h>
#include <xcb/res.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <strings.h>
#include "xwm-wire.h"

struct window {
    struct window *next;
    struct bxwm_message state,last;
    uint64_t serial_high_water,activity;
    int scale_decided;
    xcb_window_t inferred_parent;
};
struct xwm {
    xcb_connection_t *xcb;
    xcb_window_t root,support;
    int control;
    uint64_t generation,next_identity;
    struct bxwm_stream stream;
    struct window *windows;
    xcb_atom_t serial,protocols,delete_window,take_focus,hints,transient,motif;
    xcb_atom_t utf8,name,supporting,supported,active,wm_state;
    xcb_atom_t time_property,net_state,max_h,max_v,fullscreen,moveresize,change_state,net_pid;
    xcb_timestamp_t timestamp;
    uint32_t time_counter;
    struct bxwm_message pending_focus;
    int failed;
    /* Set by waylandd (BROOK_XWM_PROTOCOL/BROOK_XWM_SCALE). Protocol 1 is the
     * original wire use; 2 adds fullscreen requests and BXWM_SCALED windows,
     * which an older waylandd would reject as malformed. */
    int protocol;
    int client_pids; /* BROOK_XWM_CLIENT_PID=1: waylandd accepts BXWM_CLIENT_PID */
    uint32_t scale,client_mask;
    const char *scaled_apps;
    uint32_t scaled_clients[64];
    unsigned scaled_client_count;
    uint64_t activity;
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
/* WM_CLASS is "instance\0class\0". List entries are comma separated and
 * match either name case-insensitively; "*" matches every client. */
static int class_listed(const char *list,const char *value,size_t length) {
    const char *names[2]={NULL,NULL};size_t lengths[2]={0,0};
    for (size_t start=0,i=0;i<2 && start<length;++i) {
        const char *end=memchr(value+start,0,length-start);
        size_t n=end?(size_t)(end-(value+start)):length-start;
        names[i]=value+start;lengths[i]=n;start+=n+1;
    }
    for (const char *p=list;p && *p;) {
        while (*p==' ' || *p==',') ++p;
        size_t n=strcspn(p,",");
        while (n && p[n-1]==' ') --n;
        if (n==1 && *p=='*') return 1;
        for (unsigned i=0;i<2;++i)
            if (n && names[i] && lengths[i]==n && !strncasecmp(names[i],p,n)) return 1;
        p+=n;
    }
    return 0;
}
static uint32_t client_key(struct xwm *wm,xcb_window_t window) { return window&~wm->client_mask; }
/* Once, when a window is first mapped: is it presented scaled by the system?
 * A listed WM_CLASS marks its whole X client, so later menus and tooltips
 * (which often carry no WM_CLASS) follow their application. */
static void decide_scale(struct xwm *wm,struct window *w) {
    if (w->scale_decided) {
        if ((w->state.flags&(BXWM_SCALED|BXWM_OVERRIDE))!=(BXWM_SCALED|BXWM_OVERRIDE)) return;
    }
    else {
        w->scale_decided=1;
        if (wm->protocol<2 || wm->scale<2 || !wm->scaled_apps || !*wm->scaled_apps) return;
        uint32_t key=client_key(wm,w->state.window);int scaled=0;
        for (unsigned i=0;i<wm->scaled_client_count;++i) if (wm->scaled_clients[i]==key) scaled=1;
        if (!scaled) {
            xcb_get_property_reply_t *reply=property(wm,w->state.window,XCB_ATOM_WM_CLASS,64);
            if (reply && reply->format==8 && xcb_get_property_value_length(reply)>0)
                scaled=class_listed(wm->scaled_apps,xcb_get_property_value(reply),
                    (size_t)xcb_get_property_value_length(reply));
            free(reply);
            if (scaled && wm->scaled_client_count<sizeof(wm->scaled_clients)/sizeof(wm->scaled_clients[0]))
                wm->scaled_clients[wm->scaled_client_count++]=key;
        }
        if (!scaled) return;
        w->state.flags|=BXWM_SCALED;
        if (!(w->state.flags&BXWM_OVERRIDE)) return;
    }
    // Each map of a scaled popup re-anchors to the client's active window.
    struct window *best=NULL;uint32_t key=client_key(wm,w->state.window);
    for (struct window *other=wm->windows;other;other=other->next)
        if (other!=w && client_key(wm,other->state.window)==key &&
            (other->state.flags&(BXWM_MAPPED|BXWM_OVERRIDE|BXWM_SCALED))==(BXWM_MAPPED|BXWM_SCALED) &&
            (!best || other->activity>best->activity)) best=other;
    w->inferred_parent=best?best->state.window:XCB_WINDOW_NONE;
}
/* The X client's process: X-Resource reports the server's own peer
 * credentials; _NET_WM_PID is the client's claim, used only as a fallback.
 * Either only lets launch feedback recognise this window's launch. */
static uint32_t client_pid(struct xwm *wm,xcb_window_t window) {
    xcb_res_client_id_spec_t spec={.client=window,.mask=XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
    xcb_res_query_client_ids_reply_t *ids=xcb_res_query_client_ids_reply(wm->xcb,
        xcb_res_query_client_ids(wm->xcb,1,&spec),NULL);
    uint32_t pid=0;
    if (ids) {
        for (xcb_res_client_id_value_iterator_t it=xcb_res_query_client_ids_ids_iterator(ids);it.rem;
             xcb_res_client_id_value_next(&it))
            if ((it.data->spec.mask&XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) &&
                xcb_res_client_id_value_value_length(it.data)==1)
                pid=*xcb_res_client_id_value_value(it.data);
        free(ids);
    }
    if (!pid && wm->net_pid) pid=property_word(wm,window,wm->net_pid,0);
    return pid<=65535?pid:0;
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
    const unsigned was_mapped=w->state.flags&BXWM_MAPPED;
    int newly_mapped=0;
    w->state.flags=(w->state.flags&(BXWM_MAXIMIZED|BXWM_MINIMIZED|BXWM_FULLSCREEN|BXWM_SCALED))|
        (attributes->override_redirect?BXWM_OVERRIDE:0)|
        (attributes->map_state!=XCB_MAP_STATE_UNMAPPED?BXWM_MAPPED:0)|BXWM_INPUT;
    free(geometry);free(attributes);
    xcb_get_property_reply_t *hints=property(wm,w->state.window,wm->hints,9);
    if (hints && hints->format==32 && xcb_get_property_value_length(hints)>=8) {
        uint32_t *values=xcb_get_property_value(hints);
        if ((values[0]&1) && !values[1]) w->state.flags&=~BXWM_INPUT;
    }
    free(hints);
    // Only an explicit, well-formed decorations=0 request suppresses native
    // chrome. Keep ordinary and malformed/no-hint windows server-decorated.
    xcb_get_property_reply_t *motif=property(wm,w->state.window,wm->motif,5);
    if (motif && motif->type==wm->motif && motif->format==32 &&
        xcb_get_property_value_length(motif)>=12) {
        uint32_t *values=xcb_get_property_value(motif);
        if ((values[0]&2) && values[2]==0) w->state.flags|=BXWM_CLIENT_CHROME;
    }
    free(motif);
    if ((w->state.flags&BXWM_MAPPED) && !was_mapped) {
        w->activity=++wm->activity;newly_mapped=1;
        decide_scale(wm,w);
    }
    uint32_t parent=property_word(wm,w->state.window,wm->transient,0);
    // A scaled client's menus and tooltips rarely name a transient parent but
    // are positioned in that client's 1x coordinates; anchor them to its
    // most recently active window so waylandd can map them consistently.
    if (!parent && (w->state.flags&(BXWM_SCALED|BXWM_OVERRIDE))==(BXWM_SCALED|BXWM_OVERRIDE))
        parent=w->inferred_parent;
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
    free(title);
    if (!publish(wm,w)) return 0;
    if (newly_mapped)
        fprintf(stderr,"[brook-xwm] map 0x%x %ux%u%+d%+d flags=0x%x parent=0x%x \"%.40s\"\n",w->state.window,
                w->state.width,w->state.height,w->state.x,w->state.y,w->state.flags,w->state.parent,w->state.title);
    if (newly_mapped && wm->client_pids && !(w->state.flags&BXWM_OVERRIDE)) {
        uint32_t pid=client_pid(wm,w->state.window);
        if (pid) {
            struct bxwm_message message=w->state;message.type=BXWM_CLIENT_PID;message.x=(int32_t)pid;
            return emit(wm,&message);
        }
    }
    return 1;
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
/* Replace the atoms in `atoms` (all present or all absent) within
 * _NET_WM_STATE. Preserve states owned by the client or other policies.
 * Refuse to rewrite a malformed or truncated property rather than silently
 * discarding it. */
static int update_net_state(struct xwm *wm,xcb_window_t window,const xcb_atom_t *atoms,size_t n,int present) {
    xcb_get_property_reply_t *reply=property(wm,window,wm->net_state,4096);
    // A client may have destroyed the X window while its configure was queued.
    if(!reply)return !xcb_connection_has_error(wm->xcb);
    if(reply->bytes_after || (reply->type!=XCB_ATOM_NONE &&
       (reply->type!=XCB_ATOM_ATOM || reply->format!=32))) {free(reply);return 1;}
    const size_t count=(size_t)xcb_get_property_value_length(reply)/sizeof(xcb_atom_t);
    xcb_atom_t *states=malloc((count+n)*sizeof(*states));
    if(!states){free(reply);return 0;}
    const xcb_atom_t *old=xcb_get_property_value(reply);size_t used=0;
    for(size_t i=0;i<count;++i) {
        int listed=0;
        for(size_t j=0;j<n;++j)if(old[i]==atoms[j])listed=1;
        if(!listed)states[used++]=old[i];
    }
    if(present && used+n>4096){free(states);free(reply);return 1;}
    if(present)for(size_t j=0;j<n;++j)states[used++]=atoms[j];
    xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,window,wm->net_state,XCB_ATOM_ATOM,32,(uint32_t)used,states);
    free(states);free(reply);return 1;
}
static int initial_fullscreen(struct xwm *wm,xcb_window_t window) {
    xcb_get_property_reply_t *reply=property(wm,window,wm->net_state,4096);int found=0;
    if (reply && reply->type==XCB_ATOM_ATOM && reply->format==32) {
        const xcb_atom_t *atoms=xcb_get_property_value(reply);
        for (int i=0;i<xcb_get_property_value_length(reply)/4;++i) if (atoms[i]==wm->fullscreen) found=1;
    }
    free(reply);return found;
}
static int command(struct xwm *wm,const struct bxwm_message *message) {
    if (!bxwm_valid(message,wm->generation,0)) return 0;
    struct window *w=find(wm,message->window);
    /* A command queued before an XID was destroyed must never affect its reuse. */
    if (!w || w->state.identity!=message->identity) return 1;
    switch (message->type) {
    case BXWM_CONFIGURE: {
        w->state.configure_serial=message->configure_serial;
        unsigned state_mask=BXWM_MAXIMIZED|BXWM_MINIMIZED|BXWM_FULLSCREEN;
        unsigned changed=(w->state.flags^message->flags)&state_mask;
        if (changed) {
            w->state.flags=(w->state.flags&~state_mask)|(message->flags&state_mask);
            const xcb_atom_t maximized[]={wm->max_h,wm->max_v};
            if((changed&BXWM_MAXIMIZED) && !update_net_state(wm,message->window,maximized,2,(message->flags&BXWM_MAXIMIZED)!=0))return 0;
            if((changed&BXWM_FULLSCREEN) && !update_net_state(wm,message->window,&wm->fullscreen,1,(message->flags&BXWM_FULLSCREEN)!=0))return 0;
            uint32_t state[]={(message->flags&BXWM_MINIMIZED)?3:1,XCB_WINDOW_NONE};
            xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,message->window,wm->wm_state,wm->wm_state,32,2,state);
        }
        uint32_t values[]={(uint32_t)message->x,(uint32_t)message->y,message->width,message->height};
        xcb_configure_window(wm->xcb,message->window,XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y|
            XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,values);
        // ICCCM 4.1.5/4.2.3: tell the client its root-relative client-area
        // geometry even when the request changed nothing on the X side (a
        // real ConfigureNotify is generated only for an actual change). X
        // windows are never reparented here, so this matches the real one.
        xcb_configure_notify_event_t notify={.response_type=XCB_CONFIGURE_NOTIFY,
            .event=message->window,.window=message->window,.above_sibling=XCB_WINDOW_NONE,
            .x=(int16_t)message->x,.y=(int16_t)message->y,.width=(uint16_t)message->width,
            .height=(uint16_t)message->height,.border_width=0,.override_redirect=0};
        xcb_send_event(wm->xcb,0,message->window,XCB_EVENT_MASK_STRUCTURE_NOTIFY,(const char *)&notify);
        break;
    }
    case BXWM_FOCUS:
        w->activity=++wm->activity;
        // Brook already raised this native window. Keep X's hit-test order
        // aligned: changing keyboard focus alone leaves an overlapping X
        // sibling on top, so pointer events can reach the wrong application.
        {
            uint32_t above=XCB_STACK_MODE_ABOVE;
            xcb_configure_window(wm->xcb,message->window,XCB_CONFIG_WINDOW_STACK_MODE,&above);
            // Keep mapped override-redirect windows (menus, dropdowns,
            // tooltips) above it, in their stacking order, as Brook keeps
            // native popups above a newly focused window. The raise arrives
            // after the click that may have opened a menu; without this the
            // menu stays visible natively but X hit-tests its area to the
            // raised window beneath, so the menu ignores the pointer.
            xcb_query_tree_reply_t *tree=xcb_query_tree_reply(wm->xcb,xcb_query_tree(wm->xcb,wm->root),NULL);
            if (tree) {
                xcb_window_t *children=xcb_query_tree_children(tree);
                for (int i=0;i<xcb_query_tree_children_length(tree);++i) {
                    struct window *popup=find(wm,children[i]);
                    if (popup && popup!=w && (popup->state.flags&(BXWM_OVERRIDE|BXWM_MAPPED))==(BXWM_OVERRIDE|BXWM_MAPPED))
                        xcb_configure_window(wm->xcb,children[i],XCB_CONFIG_WINDOW_STACK_MODE,&above);
                }
                free(tree);
            }
        }
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
        // EWMH: a withdrawn client sets _NET_WM_STATE itself before mapping
        // (SDL does this for fullscreen windows). Report the initial request,
        // including its absence, so the native side applies it before the
        // first frame and a remap without the atom leaves fullscreen.
        if (w && !refresh(wm,w)) return 0;
        if (w && wm->protocol>=2 && !(w->state.flags&BXWM_OVERRIDE)) {
            struct bxwm_message request=w->state;
            request.type=BXWM_FULLSCREEN_REQUEST;request.x=initial_fullscreen(wm,e->window);
            return emit(wm,&request);
        }
        return 1;
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
        // A fullscreen window keeps the output geometry until the native side
        // leaves fullscreen. Deny the geometry part (ICCCM 4.1.5: answer with
        // a synthetic ConfigureNotify) but still honour restacking.
        struct window *full=find(wm,e->window);
        if (full && (full->state.flags&BXWM_FULLSCREEN) &&
            (e->value_mask&(XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y|XCB_CONFIG_WINDOW_WIDTH|
                            XCB_CONFIG_WINDOW_HEIGHT|XCB_CONFIG_WINDOW_BORDER_WIDTH))) {
            xcb_configure_notify_event_t notify={.response_type=XCB_CONFIGURE_NOTIFY,
                .event=e->window,.window=e->window,.above_sibling=XCB_WINDOW_NONE,
                .x=(int16_t)full->state.x,.y=(int16_t)full->state.y,.width=(uint16_t)full->state.width,
                .height=(uint16_t)full->state.height,.border_width=0,.override_redirect=0};
            xcb_send_event(wm->xcb,0,e->window,XCB_EVENT_MASK_STRUCTURE_NOTIFY,(const char *)&notify);
            e->value_mask&=~(XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y|XCB_CONFIG_WINDOW_WIDTH|
                             XCB_CONFIG_WINDOW_HEIGHT|XCB_CONFIG_WINDOW_BORDER_WIDTH);
        }
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
#ifndef BROOK_XWM_TEST_SYNTHETIC_SERIAL
        // Xwayland delivers association events directly. XSendEvent sets the
        // high bit; ordinary X clients must not impersonate that association.
        if(e->type==wm->serial && (generic->response_type&0x80))return 1;
#endif
        w=track(wm,e->window);
        if (w && e->type==wm->serial) {
            uint64_t serial=(uint64_t)e->data.data32[0]|((uint64_t)e->data.data32[1]<<32);
            if (!serial || serial<=w->serial_high_water) return 1;
            w->state.serial=serial;w->serial_high_water=serial;
        } else if (w && !(w->state.flags&BXWM_OVERRIDE) && (w->state.flags&BXWM_MAPPED)) {
            struct bxwm_message request=w->state;
            if (e->type==wm->moveresize && e->data.data32[3]==1 && e->data.data32[2]<=8) {
                request.type=e->data.data32[2]==8?BXWM_BEGIN_MOVE:BXWM_BEGIN_RESIZE;
                request.x=(int32_t)e->data.data32[2];
            } else if (e->type==wm->change_state && e->data.data32[0]==3) request.type=BXWM_MINIMIZE;
            else if (wm->protocol>=2 && e->type==wm->net_state && e->data.data32[0]<=2 &&
                (e->data.data32[1]==wm->fullscreen || e->data.data32[2]==wm->fullscreen)) {
                request.type=BXWM_FULLSCREEN_REQUEST;request.x=(int32_t)e->data.data32[0];
            }
            else if (e->type==wm->net_state && e->data.data32[0]<=2 &&
                (e->data.data32[1]==wm->max_h || e->data.data32[1]==wm->max_v ||
                 e->data.data32[2]==wm->max_h || e->data.data32[2]==wm->max_v)) {
                request.type=BXWM_MAXIMIZE;request.x=(int32_t)e->data.data32[0];
            } else return 1;
            return emit(wm,&request);
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
/* "Apps scale themselves": the X screen stays at device pixels, so tell X
 * clients the scale the way GNOME/KDE sessions do. Xft.dpi (Qt, Xft, Tk via
 * the toolkit, Chromium) and Xcursor.size live in RESOURCE_MANAGER; GTK reads
 * Gdk/WindowScalingFactor and the unscaled font DPI from XSETTINGS. An
 * existing Xft.dpi (an earlier xrdb -merge) is left alone. */
static void xsettings_int(uint8_t *buffer,size_t *used,const char *name,int32_t value) {
    const uint16_t length=(uint16_t)strlen(name);
    buffer[*used]=0;buffer[*used+1]=0;memcpy(buffer+*used+2,&length,2);*used+=4;
    memcpy(buffer+*used,name,length);*used+=(length+3u)&~3u;
    const uint32_t serial=0;memcpy(buffer+*used,&serial,4);memcpy(buffer+*used+4,&value,4);*used+=8;
}
static int publish_scale_hints(struct xwm *wm) {
    const uint32_t dpi=96*wm->scale,cursor=24*wm->scale;
    xcb_atom_t resources=atom(wm,"RESOURCE_MANAGER");
    if (!resources) return 0;
    xcb_get_property_reply_t *old=property(wm,wm->root,resources,16384);
    const char *text=old && old->format==8?xcb_get_property_value(old):"";
    size_t length=old && old->format==8?(size_t)xcb_get_property_value_length(old):0;
    if (!memmem(text,length,"Xft.dpi:",8)) {
        char *merged=malloc(length+96);
        if (!merged) { free(old);return 0; }
        memcpy(merged,text,length);
        if (length && merged[length-1]!='\n') merged[length++]='\n';
        length+=(size_t)snprintf(merged+length,96,"Xft.dpi:\t%u\nXcursor.size:\t%u\n",dpi,cursor);
        xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,wm->root,resources,XCB_ATOM_STRING,8,(uint32_t)length,merged);
        free(merged);
    }
    free(old);
    uint8_t settings[256]={0};size_t used=12;uint32_t count=4,serial=1;
    memcpy(settings+4,&serial,4);memcpy(settings+8,&count,4); // byte 0: LSBFirst
    xsettings_int(settings,&used,"Xft/DPI",(int32_t)(dpi*1024));
    xsettings_int(settings,&used,"Gdk/WindowScalingFactor",(int32_t)wm->scale);
    xsettings_int(settings,&used,"Gdk/UnscaledDPI",96*1024);
    xsettings_int(settings,&used,"Gtk/CursorThemeSize",(int32_t)cursor);
    xcb_atom_t selection=atom(wm,"_XSETTINGS_S0"),data=atom(wm,"_XSETTINGS_SETTINGS"),manager=atom(wm,"MANAGER");
    if (!selection || !data || !manager) return 0;
    xcb_get_selection_owner_reply_t *owner=xcb_get_selection_owner_reply(wm->xcb,
        xcb_get_selection_owner(wm->xcb,selection),NULL);
    const int taken=owner && owner->owner!=XCB_WINDOW_NONE;free(owner);
    if (taken) return 1; // Another settings daemon already answers clients.
    xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,wm->support,data,data,8,(uint32_t)used,settings);
    xcb_set_selection_owner(wm->xcb,wm->support,selection,XCB_CURRENT_TIME);
    xcb_client_message_event_t announce={.response_type=XCB_CLIENT_MESSAGE,.format=32,
        .window=wm->root,.type=manager};
    announce.data.data32[0]=XCB_CURRENT_TIME;announce.data.data32[1]=selection;announce.data.data32[2]=wm->support;
    xcb_send_event(wm->xcb,0,wm->root,XCB_EVENT_MASK_STRUCTURE_NOTIFY,(const char *)&announce);
    return 1;
}
/* X windows without a cursor of their own inherit the root's, and the X
 * server's default root cursor is the core "X" glyph: rootless Xwayland
 * then shows it over most X11 windows. Give the root the desktop theme's
 * arrow, as other Xwayland window managers do. libxcb-cursor reads
 * Xcursor.theme/size from RESOURCE_MANAGER (written above for HiDPI) or
 * XCURSOR_THEME/XCURSOR_SIZE, and XCURSOR_PATH; without a theme it falls
 * back to the core cursor font's arrow, still not the "X". */
static void set_root_cursor(struct xwm *wm,xcb_screen_t *screen) {
    if (!getenv("XCURSOR_PATH")) setenv("XCURSOR_PATH","/nix/profile/share/icons",0);
    xcb_cursor_context_t *context=NULL;
    if (xcb_cursor_context_new(wm->xcb,screen,&context)<0 || !context) return;
    xcb_cursor_t cursor=xcb_cursor_load_cursor(context,"left_ptr");
    if (cursor!=XCB_CURSOR_NONE) {
        xcb_change_window_attributes(wm->xcb,wm->root,XCB_CW_CURSOR,&cursor);
        xcb_free_cursor(wm->xcb,cursor);
    }
    xcb_cursor_context_free(context);
}
static int initialize(struct xwm *wm,int wm_fd) {
    wm->xcb=xcb_connect_to_fd(wm_fd,NULL);
    if (!wm->xcb || xcb_connection_has_error(wm->xcb)) return 0;
    xcb_screen_iterator_t screens=xcb_setup_roots_iterator(xcb_get_setup(wm->xcb));
    if (!screens.rem) return 0;
    wm->root=screens.data->root;wm->client_mask=xcb_get_setup(wm->xcb)->resource_id_mask;
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
    wm->motif=atom(wm,"_MOTIF_WM_HINTS");
    wm->utf8=atom(wm,"UTF8_STRING");wm->name=atom(wm,"_NET_WM_NAME");
    wm->supporting=atom(wm,"_NET_SUPPORTING_WM_CHECK");wm->supported=atom(wm,"_NET_SUPPORTED");
    wm->active=atom(wm,"_NET_ACTIVE_WINDOW");wm->wm_state=atom(wm,"WM_STATE");
    wm->time_property=atom(wm,"_BROOK_XWM_TIME");
    wm->net_state=atom(wm,"_NET_WM_STATE");wm->max_h=atom(wm,"_NET_WM_STATE_MAXIMIZED_HORZ");
    wm->max_v=atom(wm,"_NET_WM_STATE_MAXIMIZED_VERT");wm->fullscreen=atom(wm,"_NET_WM_STATE_FULLSCREEN");wm->moveresize=atom(wm,"_NET_WM_MOVERESIZE");
    wm->change_state=atom(wm,"WM_CHANGE_STATE");wm->net_pid=atom(wm,"_NET_WM_PID");
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
    xcb_atom_t supported[]={wm->supporting,wm->name,wm->active,wm->net_state,wm->max_h,wm->max_v,
        wm->moveresize,wm->fullscreen};
    xcb_change_property(wm->xcb,XCB_PROP_MODE_REPLACE,wm->root,wm->supported,XCB_ATOM_ATOM,32,
        sizeof(supported)/sizeof(supported[0])-(wm->protocol>=2?0:1),supported);
    if (wm->scale>1 && !publish_scale_hints(wm)) return 0;
    set_root_cursor(wm,screens.data);
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
    struct xwm wm={.control=(int)control,.generation=generation,.protocol=1,.scale=1};
    const char *protocol=getenv("BROOK_XWM_PROTOCOL"),*scale=getenv("BROOK_XWM_SCALE");
    if (protocol && !strcmp(protocol,"2")) wm.protocol=2;
    const char *client_pids=getenv("BROOK_XWM_CLIENT_PID");
    wm.client_pids=client_pids && !strcmp(client_pids,"1");
    if (scale && scale[0]>='1' && scale[0]<='4' && !scale[1]) wm.scale=(uint32_t)(scale[0]-'0');
    wm.scaled_apps=getenv("BROOK_XWAYLAND_SCALED_APPS");
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
