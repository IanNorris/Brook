#ifndef BROOK_XWAYLAND_XWM_H
#define BROOK_XWAYLAND_XWM_H
struct brook_xwindow {
    struct brook_xwindow *next;
    struct bxwm_message state;
    struct brook_surface *surface;
    BrookWindowState native;
    int native_valid,initial_placement;
    uint64_t native_sequence;
    uint32_t target_width,target_height;
};
static struct brook_xwindow *g_xwindows;
static struct bxwm_stream g_xwm_stream;
static struct wl_event_source *g_xwm_source;
static int g_xwm_fd=-1,g_xwm_failed,g_xwm_retiring,g_xwm_ready;
static pid_t g_xwayland_pid,g_xwm_pid;
static uint32_t g_xwm_last_sync;
static uint32_t g_xwm_started_ms;
static int g_xdisplay_fd=-1,g_xdisplay_number,g_xdisplay_ready;
static char g_xdisplay_reply[16];
static size_t g_xdisplay_received;

static struct brook_xwindow *xwayland_window(uint32_t xid,uint64_t identity) {
    for (struct brook_xwindow *w=g_xwindows;w;w=w->next)
        if (w->state.window==xid && (!identity || w->state.identity==identity)) return w;
    return NULL;
}
static int xwayland_surface_ready(const struct brook_surface *s) {
    return s->role==4 && s->xwindow && s->xwindow->surface==s &&
        s->xwayland_generation==g_xwayland_generation && !g_xwm_failed &&
        (s->xwindow->state.flags&BXWM_MAPPED) && s->xwindow->state.serial==s->xwayland_serial;
}
static unsigned xwayland_surface_flags(const struct brook_surface *s) {
    /* Override-redirect menus/tooltips keep their own pixels and do not steal
     * native keyboard focus. Normal X11 windows get movable Brook chrome. */
    if (!s->xwindow) return 0;
    unsigned flags=s->xwindow->state.flags;
    return ((flags&(BXWM_OVERRIDE|BXWM_CLIENT_CHROME))?BROOK_WM_CREATE_FLAG_CSD:0)|
        ((flags&BXWM_OVERRIDE)?BROOK_WM_CREATE_FLAG_NO_FOCUS:0);
}
static void xwayland_native_unmapped(struct brook_surface *s) {
    if (!s->xwindow) return;
    s->xwindow->native_valid=0;s->xwindow->initial_placement=0;
    s->xwindow->target_width=s->xwindow->target_height=0;
}
static void xwayland_surface_detach(struct brook_surface *s) {
    if (s->xwindow) {
        s->xwindow->surface=NULL;s->xwindow->native_valid=0;
        s->xwindow->target_width=s->xwindow->target_height=0;s->xwindow=NULL;
    }
}
static void xwayland_unpair(struct brook_xwindow *w) {
    if (w->surface) {
        struct brook_surface *s=w->surface;xwayland_surface_detach(s);surface_unmap(s);
    }
}
static void xwayland_generation_lost(void) {
    if (g_xwm_retiring) return;
    g_xwm_retiring=1;g_xwm_ready=0;
    if (g_xwm_source) { wl_event_source_remove(g_xwm_source);g_xwm_source=NULL; }
    if (g_xwm_fd>=0) { close(g_xwm_fd);g_xwm_fd=-1; }
    if (g_xdisplay_fd>=0) { close(g_xdisplay_fd);g_xdisplay_fd=-1; }
    g_xdisplay_ready=0;g_xdisplay_received=0;
    while (g_xwindows) {
        struct brook_xwindow *w=g_xwindows;g_xwindows=w->next;xwayland_unpair(w);free(w);
    }
    memset(&g_xwm_stream,0,sizeof(g_xwm_stream));
    if (g_xwayland_pid>0) kill(g_xwayland_pid,SIGTERM);
    if (g_xwm_pid>0) kill(g_xwm_pid,SIGTERM);
    // This callback can also be entered from the client's destroy listener.
    if (g_xwayland_client) wl_client_destroy(g_xwayland_client);
    g_xwm_retiring=0;
}
static int xwayland_send(struct brook_xwindow *w,uint32_t type,const BrookWindowState *geometry) {
    if (g_xwm_fd<0 || g_xwm_failed) return 0;
    struct bxwm_message message=w->state;message.type=type;
    if (geometry) {
        message.flags&=~(BXWM_MAXIMIZED|BXWM_MINIMIZED);
        if (geometry->flags&BROOK_WINDOW_STATE_MAXIMIZED) message.flags|=BXWM_MAXIMIZED;
        if (geometry->flags&BROOK_WINDOW_STATE_MINIMIZED) message.flags|=BXWM_MINIMIZED;
        message.x=geometry->client_x;message.y=geometry->client_y;
        message.width=geometry->width;message.height=geometry->height;
        if (w->native_sequence==UINT64_MAX) { g_xwm_failed=1;return 0; }
        message.configure_serial=++w->native_sequence;
    }
    if (!bxwm_valid(&message,g_xwayland_generation,0) ||
        !bxwm_enqueue(&g_xwm_stream,&message) || bxwm_flush(g_xwm_fd,&g_xwm_stream)<0) {
        g_xwm_failed=1;return 0;
    }
    if (g_xwm_source) wl_event_source_fd_update(g_xwm_source,WL_EVENT_READABLE|
        (g_xwm_stream.count?WL_EVENT_WRITABLE:0));
    return 1;
}
static void xwayland_native_sync(struct brook_surface *s,int initial) {
    if (!s->wm_id || !xwayland_surface_ready(s)) return;
    struct brook_xwindow *w=s->xwindow;BrookWindowState geometry={0};
    if (syscall(BROOK_SYS_WM_WINDOW_STATE,(long)s->wm_id,
        (long)(w->native_valid?w->native.incarnation:0),(long)&geometry,(long)sizeof(geometry),0L,0L)<0 ||
        geometry.version!=BROOK_WINDOW_STATE_VERSION || !geometry.incarnation) {
        g_xwm_failed=1;return;
    }
    if (initial && !w->initial_placement) {
        w->initial_placement=1;
        // Let unspecified normal (0,0) placement use native cascading. Menus
        // use X's absolute client coordinates, preserving pointer alignment.
        if ((w->state.flags&BXWM_OVERRIDE) || w->state.x>0 || w->state.y>0) {
            int32_t x=w->state.x,y=w->state.y;
            struct brook_xwindow *parent=xwayland_window(w->state.parent,w->state.parent_identity);
            if (w->state.parent_identity && parent && parent!=w && parent->native_valid) {
                x=parent->native.client_x+(w->state.x-parent->state.x);
                y=parent->native.client_y+(w->state.y-parent->state.y);
            }
            if (!(w->state.flags&BXWM_OVERRIDE)) {
                if (x<40) x=40;
                if (y<80) y=80;
            }
            if (syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)s->wm_id,(long)geometry.incarnation,
                (long)x,(long)y,0L,0L)<0) { g_xwm_failed=1;return; }
            geometry.client_x=x;geometry.client_y=y;
        }
    }
    int changed=!w->native_valid || geometry.client_x!=w->native.client_x ||
        geometry.client_y!=w->native.client_y || geometry.width!=w->native.width || geometry.height!=w->native.height ||
        ((geometry.flags^w->native.flags)&(BROOK_WINDOW_STATE_MAXIMIZED|BROOK_WINDOW_STATE_MINIMIZED));
    if (w->native_valid && (geometry.width!=w->native.width || geometry.height!=w->native.height)) {
        w->target_width=geometry.width;w->target_height=geometry.height;
    }
    w->native=geometry;w->native_valid=1;
    if (changed) xwayland_send(w,BXWM_CONFIGURE,&geometry);
}
static void xwayland_surface_created(struct brook_surface *s) {
    xwayland_native_sync(s,1);
}
static int xwayland_accept_view(struct brook_surface *s,uint32_t width,uint32_t height) {
    struct brook_xwindow *w=s->xwindow;
    if (w && !w->native_valid && (width!=w->state.width || height!=w->state.height)) return 0;
    if (!w || !w->target_width) return 1;
    if (width!=w->target_width || height!=w->target_height) return 0;
    w->target_width=w->target_height=0;return 1;
}
static void xwayland_apply_client_geometry(struct brook_xwindow *w,const struct bxwm_message *message) {
    struct brook_surface *s=w->surface;
    if (!s || !s->wm_id || !w->native_valid || message->configure_serial!=w->native_sequence) return;
    if (message->x!=w->native.client_x || message->y!=w->native.client_y) {
        if (syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)s->wm_id,(long)w->native.incarnation,
            (long)message->x,(long)message->y,0L,0L)<0) { g_xwm_failed=1;return; }
        w->native.client_x=message->x;w->native.client_y=message->y;
    }
    if (message->width!=w->native.width || message->height!=w->native.height) {
        struct brook_wm_create_out out={0};
        if (wm_resize_vfb(s->wm_id,message->width,message->height,&out) || !out.vfb_user) {
            g_xwm_failed=1;return;
        }
        s->vfb=(uint32_t *)(uintptr_t)out.vfb_user;s->vfb_stride=out.vfb_stride;
        s->vfb_w=message->width;s->vfb_h=message->height;
        w->native.width=message->width;w->native.height=message->height;
        w->target_width=message->width;w->target_height=message->height;
    }
}
static void xwayland_pair_surfaces(void) {
    if (!g_xwayland_client || g_xwm_failed) return;
    for (struct brook_xwindow *w=g_xwindows;w;w=w->next) {
        if (!(w->state.flags&BXWM_MAPPED) || !w->state.serial) { xwayland_unpair(w);continue; }
        struct brook_surface *s=xwayland_surface_by_serial(g_xwayland_generation,w->state.serial);
        if (!s) continue;
        if (w->surface && w->surface!=s) xwayland_unpair(w);
        if (s->xwindow && s->xwindow!=w) { g_xwm_failed=1;return; }
        if (!w->surface) {
            w->surface=s;s->xwindow=w;w->native_valid=0;w->initial_placement=0;
            snprintf(s->title,sizeof(s->title),"%s",w->state.title[0]?w->state.title:"X11 application");
            if (!surface_repaint(s)) g_xwm_failed=1;
        }
    }
}
static int xwayland_accept_message(const struct bxwm_message *message) {
    if (!bxwm_valid(message,g_xwayland_generation,1)) return 0;
    if (message->type==BXWM_READY) { g_xwm_ready=1;return 1; }
    if (message->type==BXWM_ERROR) return 0;
    struct brook_xwindow **link=&g_xwindows;
    while (*link && (*link)->state.window!=message->window) link=&(*link)->next;
    struct brook_xwindow *w=*link;
    if (message->type==BXWM_DESTROY) {
        if (w && w->state.identity==message->identity) { *link=w->next;xwayland_unpair(w);free(w); }
        return 1;
    }
    if (message->type==BXWM_BEGIN_MOVE || message->type==BXWM_BEGIN_RESIZE ||
        message->type==BXWM_MAXIMIZE || message->type==BXWM_MINIMIZE) {
        if (!w || w->state.identity!=message->identity || !w->surface || !w->surface->wm_id ||
            !w->native_valid || !(w->state.flags&BXWM_MAPPED) || (w->state.flags&BXWM_OVERRIDE)) return 1;
        uint32_t id=w->surface->wm_id;
        if (message->type==BXWM_BEGIN_MOVE) (void)wm_begin_move(id);
        if (message->type==BXWM_BEGIN_RESIZE) {
            static const uint32_t edges[]={BROOK_WM_EDGE_TOP|BROOK_WM_EDGE_LEFT,BROOK_WM_EDGE_TOP,
                BROOK_WM_EDGE_TOP|BROOK_WM_EDGE_RIGHT,BROOK_WM_EDGE_RIGHT,
                BROOK_WM_EDGE_BOTTOM|BROOK_WM_EDGE_RIGHT,BROOK_WM_EDGE_BOTTOM,
                BROOK_WM_EDGE_BOTTOM|BROOK_WM_EDGE_LEFT,BROOK_WM_EDGE_LEFT};
            (void)wm_begin_resize(id,edges[message->x]);
        }
        if (message->type==BXWM_MINIMIZE) (void)wm_set_minimized(id);
        if (message->type==BXWM_MAXIMIZE) {
            xwayland_native_sync(w->surface,0);
            int enable=message->x==2?!(w->native.flags&BROOK_WINDOW_STATE_MAXIMIZED):message->x;
            (void)wm_set_maximized(id,enable);
        }
        xwayland_native_sync(w->surface,0);return !g_xwm_failed;
    }
    if (w && w->state.identity!=message->identity) {
        if (message->identity<w->state.identity) return 1;
        *link=w->next;xwayland_unpair(w);free(w);w=NULL;
    }
    if (!w) {
        w=calloc(1,sizeof(*w));if (!w) return 0;
        w->next=g_xwindows;g_xwindows=w;
    } else if (w->state.serial!=message->serial || !(message->flags&BXWM_MAPPED)) xwayland_unpair(w);
    xwayland_apply_client_geometry(w,message);
    unsigned old_flags=w->state.flags;
    w->state=*message;
    if (w->surface && w->surface->wm_id &&
        ((old_flags^message->flags)&BXWM_CLIENT_CHROME) && !(message->flags&BXWM_OVERRIDE)) {
        if (wm_set_decoration_mode(w->surface->wm_id,(message->flags&BXWM_CLIENT_CHROME)!=0)<0) {
            g_xwm_failed=1;return 0;
        }
        xwayland_native_sync(w->surface,0);
    }
    if (w->surface && strcmp(w->surface->title,message->title)) {
        snprintf(w->surface->title,sizeof(w->surface->title),"%s",message->title);
        if (w->surface->wm_id) wm_set_title(w->surface->wm_id,message->title);
    }
    xwayland_pair_surfaces();
    if (w->surface && (!w->surface->wm_id || w->target_width))
        if (!surface_repaint(w->surface)) g_xwm_failed=1;
    return !g_xwm_failed;
}
static int xwayland_control_ready(int fd,uint32_t mask,void *data) {
    (void)data;
    if (mask&(WL_EVENT_HANGUP|WL_EVENT_ERROR)) { g_xwm_failed=1;return 0; }
    if (mask&WL_EVENT_READABLE) {
        struct bxwm_message message;int result=0;unsigned work=0;
        while (work++<64 && (result=bxwm_receive(fd,&g_xwm_stream,&message))>0)
            if (!xwayland_accept_message(&message)) { g_xwm_failed=1;break; }
        if (result<0) g_xwm_failed=1;
    }
    if (bxwm_flush(fd,&g_xwm_stream)<0) g_xwm_failed=1;
    if (g_xwm_source) wl_event_source_fd_update(g_xwm_source,WL_EVENT_READABLE|
        (g_xwm_stream.count?WL_EVENT_WRITABLE:0));
    return 0;
}
static void xwayland_window_event(struct brook_surface *s,unsigned type) {
    if (!xwayland_surface_ready(s)) return;
    if (type==WM_EVT_CLOSE_REQUESTED) xwayland_send(s->xwindow,BXWM_CLOSE,NULL);
    if (type==WM_EVT_FOCUS_GAINED) xwayland_send(s->xwindow,BXWM_FOCUS,NULL);
    if (type==WM_EVT_RESIZED) xwayland_native_sync(s,0);
}
static void xwayland_reap(pid_t *pid) {
    if (*pid<=0) return;
    int status;pid_t done=waitpid(*pid,&status,WNOHANG);
    if (done==*pid || (done<0 && errno==ECHILD)) { *pid=0;if (g_xwm_fd>=0) g_xwm_failed=1; }
}
static void xwayland_pump(void) {
    xwayland_reap(&g_xwayland_pid);xwayland_reap(&g_xwm_pid);
    if (g_xdisplay_fd>=0 && !g_xdisplay_ready) {
        ssize_t n=recv(g_xdisplay_fd,g_xdisplay_reply+g_xdisplay_received,
            sizeof(g_xdisplay_reply)-1-g_xdisplay_received,MSG_DONTWAIT);
        if (n>0) {
            g_xdisplay_received+=(size_t)n;g_xdisplay_reply[g_xdisplay_received]=0;
            char *newline=strchr(g_xdisplay_reply,'\n');
            if (newline) {
                *newline=0;
                if (!*g_xdisplay_reply || strspn(g_xdisplay_reply,"0123456789")!=strlen(g_xdisplay_reply) ||
                    atoi(g_xdisplay_reply)!=g_xdisplay_number) g_xwm_failed=1;
                else g_xdisplay_ready=1;
                close(g_xdisplay_fd);g_xdisplay_fd=-1;
            } else if (g_xdisplay_received==sizeof(g_xdisplay_reply)-1) g_xwm_failed=1;
        } else if (!n || (errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK)) g_xwm_failed=1;
    }
    if (g_xwm_fd>=0 && (!g_xwm_ready || !g_xdisplay_ready) && (uint32_t)(g_now_ms()-g_xwm_started_ms)>15000) g_xwm_failed=1;
    if (g_xwm_failed) {
        fprintf(stderr,"[waylandd] owned Xwayland generation retired after connection failure\n");
        xwayland_generation_lost();g_xwm_failed=0;return;
    }
    uint32_t now=g_now_ms();
    if ((uint32_t)(now-g_xwm_last_sync)<50) return;
    g_xwm_last_sync=now;
    for (struct brook_xwindow *w=g_xwindows;w;w=w->next) if (w->surface) xwayland_native_sync(w->surface,0);
}
static int xwayland_inherit_fd(int fd) { return fcntl(fd,F_SETFD,0)>=0; }
static int xwayland_start(const char *server,const char *helper,const char *display) {
    if (g_xwayland_client || g_xwm_fd>=0 || g_xwayland_pid || g_xwm_pid) return 0;
    int wayland[2]={-1,-1},wm[2]={-1,-1},control[2]={-1,-1},display_pipe[2]={-1,-1};
    if (socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,wayland) ||
        socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,wm) ||
        socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,control) ||
        socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,display_pipe)) goto fail;
    struct wl_client *client=wl_client_create(g_display,wayland[0]);
    if (!client) goto fail;
    wayland[0]=-1;
    if (!xwayland_attach_client(client)) { wl_client_destroy(client);goto fail; }
    g_xwm_failed=0;g_xwm_ready=0;
    g_xwm_started_ms=g_now_ms();
    g_xdisplay_fd=display_pipe[0];display_pipe[0]=-1;
    g_xdisplay_number=atoi(display+1);g_xdisplay_ready=0;g_xdisplay_received=0;
    g_xwm_fd=control[0];control[0]=-1;
    g_xwm_source=wl_event_loop_add_fd(wl_display_get_event_loop(g_display),g_xwm_fd,WL_EVENT_READABLE,xwayland_control_ready,NULL);
    if (!g_xwm_source) goto fail;
    pid_t child=fork();
    if (child<0) goto fail;
    if (!child) {
        char wayland_fd[24],wm_fd[24],display_fd[24];
        snprintf(wayland_fd,sizeof(wayland_fd),"%d",wayland[1]);snprintf(wm_fd,sizeof(wm_fd),"%d",wm[1]);
        snprintf(display_fd,sizeof(display_fd),"%d",display_pipe[1]);
        if (!xwayland_inherit_fd(wayland[1]) || !xwayland_inherit_fd(wm[1]) || !xwayland_inherit_fd(display_pipe[1])) _exit(126);
        setenv("WAYLAND_SOCKET",wayland_fd,1);
        // -displayfd supplies readiness and avoids Xserver's root-only -nolock
        // option, so the owned server can run as the ordinary desktop user.
        execl(server,server,display,"-rootless","-wm",wm_fd,"-displayfd",display_fd,"-nolisten","tcp","-noreset","-ac","-glamor","gl",(char *)NULL);
        _exit(127);
    }
    g_xwayland_pid=child;
    child=fork();if (child<0) goto fail;
    if (!child) {
        char wm_fd[24],control_fd[24],generation[32];
        snprintf(wm_fd,sizeof(wm_fd),"%d",wm[0]);snprintf(control_fd,sizeof(control_fd),"%d",control[1]);
        snprintf(generation,sizeof(generation),"%llu",(unsigned long long)g_xwayland_generation);
        if (!xwayland_inherit_fd(wm[0]) || !xwayland_inherit_fd(control[1])) _exit(126);
        execl(helper,helper,wm_fd,control_fd,generation,(char *)NULL);_exit(127);
    }
    g_xwm_pid=child;
    close(wayland[1]);close(wm[0]);close(wm[1]);close(control[1]);close(display_pipe[1]);return 1;
fail:
    for (unsigned i=0;i<2;++i) {
        if (wayland[i]>=0) close(wayland[i]);
        if (wm[i]>=0) close(wm[i]);
        if (control[i]>=0) close(control[i]);
        if (display_pipe[i]>=0) close(display_pipe[i]);
    }
    g_xwm_failed=1;xwayland_generation_lost();return 0;
}
#endif
