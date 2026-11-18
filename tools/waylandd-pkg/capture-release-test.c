/* Real Wayland events: only proved-safe capture results may release a dmabuf. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
static int test_fprintf(FILE *stream,const char *format,...);
#define fprintf test_fprintf
static long test_syscall(long number, ...);
#define syscall test_syscall
static int test_ioctl(int fd,unsigned long request,...);
#define ioctl test_ioctl
#ifdef BROOK_DIRECT_TEST
#define BROOK_WAYLAND_VULKAN_COMPOSITOR 1
#define brook_vk_surface_create test_surface_create
#define brook_vk_surface_compose test_surface_compose
#define brook_vk_surface_compose_external test_surface_compose_external
#define brook_vk_surface_device test_surface_device
#endif
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef ioctl
#undef syscall
#undef fprintf
#ifdef BROOK_DIRECT_TEST
static int resize_calls;static long resize_w,resize_h;static uint32_t resize_vfb[16];
static int direct_status=1,direct_calls,window_queries,window_stale,direct_mutate_commit,peer_error,source_begins,source_tags,source_begin_error;
BrookVkSurface *test_surface_create(int software){(void)software;return (BrookVkSurface *)(uintptr_t)1;}
const char *test_surface_device(const BrookVkSurface *s){(void)s;return "test";}
BrookVkResult test_surface_compose(BrookVkSurface *s,uint32_t w,uint32_t h,const struct BrookVkSurfaceLayer *l,size_t n,uint32_t *out,size_t cap)
{(void)s;(void)w;(void)h;(void)l;(void)n;(void)out;(void)cap;return BROOK_VK_REJECTED;}
BrookVkResult test_surface_compose_external(BrookVkSurface *s,uint32_t w,uint32_t h,const struct BrookVkSurfaceExternalLayer *l,size_t n,uint32_t *out,size_t cap)
{(void)s;assert(n==1 && l->fd>=0 && l->syncFd>=0 && cap>=(size_t)w*h);++direct_calls;
 if(direct_mutate_commit){assert(g_surfaces && g_surfaces->tree.current);++g_surfaces->tree.current->commit;}
 for(size_t i=0;i<(size_t)w*h;++i)out[i]=0xff112233;return (BrookVkResult)direct_status;}
#endif
static int test_fprintf(FILE *stream,const char *format,...)
{ (void)stream;(void)format;errno=ENOSPC;return -1; }
static long result;
static int result_errno, calls;
static unsigned intel_fail_request;
static int try_error,snapshot_error,snapshot_status=1,mock_fence_fd=-1,gem_create_ok,gem_creates,gem_closes;
static int test_ioctl(int fd,unsigned long request,...)
{
    (void)fd;
    if((request&255u)==intel_fail_request){errno=EIO;return -1;}
    if(request==0xc0383e04u){
        va_list args;va_start(args,request);unsigned char *info=va_arg(args,void *);va_end(args);
        memcpy(info+32,&snapshot_status,sizeof(snapshot_status));return 0;
    }
    if((request&255u)==0x5b && gem_create_ok){
        va_list args;va_start(args,request);struct {uint64_t size;uint32_t handle,pad;} *c=va_arg(args,void *);va_end(args);
        c->handle=77;c->size=4096;++gem_creates;return 0;
    }
    if((request&255u)==0x09){++gem_closes;return 0;}
    if((request&255u)==0x2d){
        va_list args;va_start(args,request);struct drm_prime_handle *prime=va_arg(args,void *);va_end(args);
        prime->fd=dup(g_drm_fd);return prime->fd<0?-1:0;
    }
    return 0;
}
static long test_syscall(long number, ...)
{
    va_list args;va_start(args,number);
#ifdef BROOK_DIRECT_TEST
    if(number==BROOK_SYS_WM_BEGIN_SOURCE_WRITE){va_end(args);++source_begins;if(source_begin_error){errno=source_begin_error;return -1;}return 0;}
    if(number==BROOK_SYS_WM_PUBLISH_SOURCE_COMMIT){
        long window=va_arg(args,long),incarnation=va_arg(args,long),commit=va_arg(args,long);va_end(args);
        assert(window==1 && incarnation==77 && commit>0);++source_tags;return 0;
    }
    if(number==BROOK_SYS_WM_RESIZE_VFB){
        long window=va_arg(args,long),w=va_arg(args,long),h=va_arg(args,long);
        struct brook_wm_create_out *out=(void *)(uintptr_t)va_arg(args,long);va_end(args);
        ++resize_calls;resize_w=w;resize_h=h;
        *out=(struct brook_wm_create_out){(uint32_t)window,(uint32_t)w,(uintptr_t)resize_vfb};return 0;
    }
    if(number==BROOK_SYS_GPU_WINDOW_PEER_RECOVERY){va_end(args);if(peer_error){errno=peer_error;return -1;}return 0;}
    if(number==BROOK_SYS_WM_WINDOW_STATE){
        long window=va_arg(args,long),expected=va_arg(args,long);BrookWindowState *state=(void *)(uintptr_t)va_arg(args,long);
        (void)va_arg(args,long);va_end(args);++window_queries;
        if(window_stale && expected){errno=ESTALE;return -1;}
        *state=(BrookWindowState){.version=BROOK_WINDOW_STATE_VERSION,.window=(uint32_t)window,.incarnation=77,.width=1,.height=1};return 0;
    }
#endif
    if(number==BROOK_SYS_GPU_BUFFER_SNAPSHOT_FENCE){va_end(args);if(snapshot_error){errno=snapshot_error;return -1;}return dup(mock_fence_fd>=0?mock_fence_fd:g_drm_fd);}
    if(number==BROOK_SYS_GPU_BUFFER_TRY_CPU_READ){va_end(args);if(try_error){errno=try_error;return -1;}return 0;}
    if(number==BROOK_SYS_WM_SIGNAL_DIRTY || number==BROOK_SYS_WM_DESTROY_WINDOW){va_end(args);return 0;}
    if (number==0xB06) { va_end(args);return open("/dev/null",O_RDWR); }
    assert(number==0xB07);(void)va_arg(args,long);long op=va_arg(args,long);va_end(args);
    if (op!=SCENE_CTL_CAPTURE_DMABUF) return 0;
    ++calls;errno=result_errno;return result;
}
static void check_intel_fence(int fail_status)
{
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct brook_dmabuf_buffer *db=calloc(1,sizeof(*db));assert(db);
    db->refs=1;db->width=db->height=4;db->stride=16;db->bo_handle=123;
    struct wl_resource *resource=wl_resource_create(client,&wl_buffer_interface,1,2);assert(resource);
    wl_resource_set_implementation(resource,&dmabuf_buffer_impl,db,dmabuf_buffer_destroy);
    struct brook_buffer *buffer=buffer_hold(resource);assert(buffer);
    g_drm_fd=open("/dev/zero",O_RDWR);assert(g_drm_fd>=0);
    int ready_pipe[2];assert(!pipe(ready_pipe));mock_fence_fd=ready_pipe[0];
    assert(!setenv("BROOK_WAYLAND_SNAPSHOT_FENCE","1",1));
    struct brook_image *image=intel_dmabuf_defer(buffer);assert(image && buffer->uses==1);
    snapshot_error=EAGAIN;intel_pending_poll();assert(image->pending_cpu==1 && buffer->uses==1);
    snapshot_error=fail_status==2?ENOSYS:0;
    if(fail_status!=2){
        intel_pending_poll();assert(image->pending_cpu==1 && buffer->uses==1);
        assert(write(ready_pipe[1],"x",1)==1);
    }
    snapshot_status=fail_status?-5:1;intel_pending_poll();assert(!g_intel_pending_count);
    if(fail_status)assert(image->pending_cpu==2 && image->uncertain && buffer->uncertain && buffer->uses==1);
    else assert(!image->pending_cpu && image->pixels && buffer->uses==0);
    wl_display_flush_clients(display);uint32_t wire[256];ssize_t n=recv(sockets[1],wire,sizeof(wire),MSG_DONTWAIT);
    int releases=0,errors=0;if(n>0)for(size_t i=0;i<(size_t)n/4;){
        uint32_t id=wire[i],op=wire[i+1]&65535,size=wire[i+1]>>16;assert(size>=8 && i*4+size<=(size_t)n);
        if(id==2 && op==WL_BUFFER_RELEASE)++releases;
        if(id==1 && op==WL_DISPLAY_ERROR)++errors;
        i+=size/4;
    }
    assert(releases==!fail_status && errors==!!fail_status);
    image_unref(image);buffer_drop(buffer);close(g_drm_fd);g_drm_fd=-1;
    close(ready_pipe[0]);close(ready_pipe[1]);mock_fence_fd=-1;
    snapshot_error=0;snapshot_status=1;unsetenv("BROOK_WAYLAND_SNAPSHOT_FENCE");
    wl_client_destroy(client);close(sockets[1]);wl_display_destroy(display);
}
static void check_intel_deferred(int final_error,int destroy_early)
{
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct brook_dmabuf_buffer *db=calloc(1,sizeof(*db));assert(db);
    db->refs=1;db->width=db->height=4;db->stride=16;db->bo_handle=123;
    struct wl_resource *resource=wl_resource_create(client,&wl_buffer_interface,1,2);assert(resource);
    wl_resource_set_implementation(resource,&dmabuf_buffer_impl,db,dmabuf_buffer_destroy);
    struct brook_buffer *buffer=buffer_hold(resource);assert(buffer);
    g_drm_fd=open("/dev/zero",O_RDWR);assert(g_drm_fd>=0);
    struct brook_image *image=intel_dmabuf_defer(buffer);assert(image && buffer->uses==1);
    if(destroy_early)wl_resource_destroy(resource);
    try_error=EAGAIN;intel_pending_poll();assert(image->pending_cpu==1 && buffer->uses==1);
    wl_display_flush_clients(display);uint32_t wire[256];
    assert(recv(sockets[1],wire,sizeof(wire),MSG_DONTWAIT)<0 && errno==EAGAIN);
    try_error=final_error;intel_pending_poll();assert(!g_intel_pending_count);
    if(final_error){assert(image->pending_cpu==2 && image->uncertain && buffer->uncertain && buffer->uses==1);}
    else {assert(!image->pending_cpu && image->pixels && buffer->uses==0);}
    wl_display_flush_clients(display);ssize_t n=recv(sockets[1],wire,sizeof(wire),MSG_DONTWAIT);
    int releases=0,errors=0;if(n>0)for(size_t i=0;i<(size_t)n/4;){
        uint32_t id=wire[i],op=wire[i+1]&65535,size=wire[i+1]>>16;
        assert(size>=8 && i*4+size<=(size_t)n);
        if(id==2 && op==WL_BUFFER_RELEASE)++releases;
        if(id==1 && op==WL_DISPLAY_ERROR)++errors;
        i+=size/4;
    }
    assert(releases==(!final_error && !destroy_early));
    assert(errors==(final_error && !destroy_early));
    image_unref(image);buffer_drop(buffer);close(g_drm_fd);g_drm_fd=-1;try_error=0;
    wl_client_destroy(client);close(sockets[1]);wl_display_destroy(display);
}
static void check_intel_deferred_surface(void)
{
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct brook_dmabuf_buffer *db=calloc(1,sizeof(*db));assert(db);
    db->refs=1;db->width=db->height=1;db->stride=4;db->bo_handle=123;
    struct wl_resource *resource=wl_resource_create(client,&wl_buffer_interface,1,2);assert(resource);
    wl_resource_set_implementation(resource,&dmabuf_buffer_impl,db,dmabuf_buffer_destroy);
    struct brook_surface surface={0};surface_tree_init(&surface);
    surface.resource=wl_resource_create(client,&wl_surface_interface,4,3);assert(surface.resource);
    wl_resource_set_user_data(surface.resource,&surface);
    uint32_t vfb=0xff112233;surface.wm_id=1;surface.vfb=&vfb;
    surface.vfb_w=surface.vfb_h=surface.vfb_stride=1;surface.scene_disabled=1;g_surfaces=&surface;
    g_drm_fd=open("/dev/zero",O_RDWR);assert(g_drm_fd>=0);g_drm_intel=1;
    assert(!setenv("BROOK_WAYLAND_INTEL_ASYNC","1",1));
    surface_attach(client,surface.resource,resource,0,0);
    surface_commit(client,surface.resource);assert(g_intel_pending_count==1 && vfb==0xff112233);
    try_error=EAGAIN;intel_pending_poll();assert(g_intel_pending_count==1 && vfb==0xff112233);
    try_error=0;intel_pending_poll();assert(!g_intel_pending_count && vfb==0);
    wl_display_flush_clients(display);uint32_t wire[32];ssize_t n=recv(sockets[1],wire,sizeof(wire),MSG_DONTWAIT);
    assert(n>0 && wire[0]==2 && (wire[1]&65535)==WL_BUFFER_RELEASE);
    bt_state_set(&surface.tree.current,NULL);bt_state_set(&surface.tree.latest,NULL);
    g_surfaces=NULL;g_drm_intel=0;close(g_drm_fd);g_drm_fd=-1;unsetenv("BROOK_WAYLAND_INTEL_ASYNC");
    wl_client_destroy(client);close(sockets[1]);wl_display_destroy(display);
}
static void check_superseded_intel_image(void)
{
    struct brook_dmabuf_buffer db={.width=1,.height=1,.stride=4,.bo_handle=123};
    struct brook_buffer buffer={.refs=1,.dmabuf=&db};
    g_drm_fd=open("/dev/zero",O_RDWR);assert(g_drm_fd>=0);
    int fence[2];assert(!pipe(fence));mock_fence_fd=fence[0];
    assert(!setenv("BROOK_WAYLAND_SNAPSHOT_FENCE","1",1));
    struct brook_image *image=intel_dmabuf_defer(&buffer);assert(image && buffer.uses==1);
    image_unref(image);
    intel_pending_poll();assert(g_intel_pending_count==1 && buffer.uses==1);
    assert(write(fence[1],"x",1)==1);
    const uint64_t discarded=g_intel_discarded;
    try_error=EAGAIN; /* A discarded image must never attempt a CPU transition. */
    intel_pending_poll();
    assert(!g_intel_pending_count && !buffer.uses && !buffer.uncertain && buffer.refs==1);
    assert(g_intel_discarded==discarded+1);
    try_error=0;mock_fence_fd=-1;close(fence[0]);close(fence[1]);
    close(g_drm_fd);g_drm_fd=-1;unsetenv("BROOK_WAYLAND_SNAPSHOT_FENCE");
}
#ifdef BROOK_DIRECT_TEST
static void check_gpu_window_offer(int mode)
{
    int clientSockets[2],channel[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,clientSockets));
    assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,channel));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,clientSockets[0]);assert(client);
    struct brook_dmabuf_buffer *db=calloc(1,sizeof(*db));assert(db);
    db->refs=1;db->width=db->height=1;db->stride=4;db->bo_handle=123;db->fourcc=0x34325258;
    struct wl_resource *resource=wl_resource_create(client,&wl_buffer_interface,1,2);assert(resource);
    wl_resource_set_implementation(resource,&dmabuf_buffer_impl,db,dmabuf_buffer_destroy);
    struct brook_surface surface={0};surface_tree_init(&surface);
    surface.resource=wl_resource_create(client,&wl_surface_interface,4,3);assert(surface.resource);
    wl_resource_set_user_data(surface.resource,&surface);
    surface.xdg_toplevel=wl_resource_create(client,&xdg_toplevel_interface,1,4);assert(surface.xdg_toplevel);
    surface.xdg_acked=surface.xdg_initial_commit_done=1;
    uint32_t vfb=0;surface.wm_id=1;surface.vfb=&vfb;surface.vfb_w=surface.vfb_h=surface.vfb_stride=1;
    surface.scene_disabled=1;g_surfaces=&surface;
    char path[]="/tmp/brook-offer-test-XXXXXX";g_drm_fd=mkstemp(path);assert(g_drm_fd>=0);unlink(path);assert(!ftruncate(g_drm_fd,4096));
    g_drm_intel=1;g_gpu_window_channel=channel[0];window_stale=0;
    assert(!setenv("BROOK_WAYLAND_INTEL_ASYNC","1",1));assert(!setenv("BROOK_WAYLAND_SNAPSHOT_FENCE","1",1));
    assert(!setenv("BROOK_WAYLAND_GPU_SOCKET","/tmp/fake-consumer",1));
    surface_attach(client,surface.resource,resource,0,0);surface_commit(client,surface.resource);
    struct brook_view *view=surface.tree.current->value;assert(view && view->image && view->image->source);
    struct brook_buffer *buffer=view->image->source;++buffer->refs;
    intel_pending_poll();assert(g_intel_pending_count==1 && view->image->offer_state==1 && buffer->uses==1);
    struct BrookGpuWindowPacket offer={0};int fds[2];assert(brook_gpu_window_receive(channel[1],&offer,fds)==1);
    assert(offer.type==BROOK_GPU_WINDOW_OFFER && offer.identity.window==1 && offer.identity.incarnation==77 && offer.identity.commit);
    close(fds[0]);close(fds[1]);
    wl_display_flush_clients(display);uint32_t wire[256];assert(recv(clientSockets[1],wire,sizeof(wire),MSG_DONTWAIT)<0 && errno==EAGAIN);
    if(mode){close(channel[1]);peer_error=mode==1?EIO:EAGAIN;}
    else assert(brook_gpu_window_send_safe(channel[1],&offer.identity)==1);
    if(mode==2){intel_pending_poll();assert(g_intel_pending_count==1 && buffer->uses==1 && !buffer->uncertain);peer_error=0;}
    intel_pending_poll();assert(!g_intel_pending_count);
    if(mode==1)assert(buffer->uncertain && buffer->uses==1 && view->image->pending_cpu==2);
    else assert(!buffer->uncertain && !buffer->uses && !view->image->pending_cpu);
    if(!mode){
        const uint32_t previous=vfb;const int tags=source_tags;source_begin_error=EBUSY;
        assert(surface_repaint(&surface) && vfb==previous && source_tags==tags);
        source_begin_error=0;
        /* Later CPU/SHM scenes can supersede a direct source even without an
         * offer identity, provided the current root view was composed. */
        memset(&view->image->offer_identity,0,sizeof(view->image->offer_identity));
        assert(surface_repaint(&surface) && source_tags==tags+1);
    }
    wl_display_flush_clients(display);ssize_t bytes=recv(clientSockets[1],wire,sizeof(wire),MSG_DONTWAIT);
    int releases=0,errors=0;if(bytes>0)for(size_t i=0;i<(size_t)bytes/4;){
        uint32_t id=wire[i],op=wire[i+1]&65535,size=wire[i+1]>>16;assert(size>=8 && i*4+size<=(size_t)bytes);
        if(id==2 && op==WL_BUFFER_RELEASE)++releases;
        if(id==1 && op==WL_DISPLAY_ERROR)++errors;
        i+=size/4;
    }
    assert(releases==(mode!=1) && errors==(mode==1));
    bt_state_set(&surface.tree.current,NULL);bt_state_set(&surface.tree.latest,NULL);buffer_drop(buffer);
    free(surface.composite);g_surfaces=NULL;g_drm_intel=0;close(g_drm_fd);g_drm_fd=-1;
    if(!mode){close(channel[1]);gpu_window_channel_lost();gpu_window_reap_dead();}
    peer_error=0;
    unsetenv("BROOK_WAYLAND_INTEL_ASYNC");unsetenv("BROOK_WAYLAND_SNAPSHOT_FENCE");unsetenv("BROOK_WAYLAND_GPU_SOCKET");
    wl_client_destroy(client);close(clientSockets[1]);wl_display_destroy(display);
}
/* Version 2: frame callbacks follow PRESENTED, a superseded source is
 * released without a CPU copy, and pixels are taken only on demand. */
static unsigned count_events(int socket,uint32_t object,uint32_t opcode){
    uint32_t wire[512];ssize_t bytes=recv(socket,wire,sizeof(wire),MSG_DONTWAIT);unsigned found=0;
    if(bytes>0)for(size_t i=0;i<(size_t)bytes/4;){
        uint32_t id=wire[i],op=wire[i+1]&65535,size=wire[i+1]>>16;assert(size>=8 && i*4+size<=(size_t)bytes);
        if(id==object && op==opcode)++found;
        assert(!(id==1 && op==WL_DISPLAY_ERROR));
        i+=size/4;
    }
    return found;
}
static struct wl_resource *retained_test_buffer(struct wl_client *client,uint32_t id){
    struct brook_dmabuf_buffer *db=calloc(1,sizeof(*db));assert(db);
    db->refs=1;db->width=db->height=1;db->stride=4;db->bo_handle=123;db->fourcc=0x34325258;
    struct wl_resource *resource=wl_resource_create(client,&wl_buffer_interface,1,id);assert(resource);
    wl_resource_set_implementation(resource,&dmabuf_buffer_impl,db,dmabuf_buffer_destroy);
    return resource;
}
static void check_gpu_window_retained(void)
{
    int clientSockets[2],channel[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,clientSockets));
    assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,channel));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,clientSockets[0]);assert(client);
    struct wl_resource *resources[2];struct brook_buffer *held[2];
    /* New object ids must be allocated in order: 2 buffer, 3 surface,
     * 4 toplevel, 5 buffer, then the two frame callbacks. */
    resources[0]=retained_test_buffer(client,2);
    struct brook_surface surface={0};surface_tree_init(&surface);
    surface.resource=wl_resource_create(client,&wl_surface_interface,4,3);assert(surface.resource);
    wl_resource_set_user_data(surface.resource,&surface);
    surface.xdg_toplevel=wl_resource_create(client,&xdg_toplevel_interface,1,4);assert(surface.xdg_toplevel);
    resources[1]=retained_test_buffer(client,5);
    surface.xdg_acked=surface.xdg_initial_commit_done=1;
    const uint32_t sentinel=0xff445566;
    uint32_t vfb=sentinel;surface.wm_id=1;surface.vfb=&vfb;surface.vfb_w=surface.vfb_h=surface.vfb_stride=1;
    surface.scene_disabled=1;g_surfaces=&surface;
    char path[]="/tmp/brook-retain-test-XXXXXX";g_drm_fd=mkstemp(path);assert(g_drm_fd>=0);unlink(path);assert(!ftruncate(g_drm_fd,4096));
    g_drm_intel=1;g_gpu_window_channel=channel[0];window_stale=0;
    assert(!setenv("BROOK_WAYLAND_INTEL_ASYNC","1",1));assert(!setenv("BROOK_WAYLAND_SNAPSHOT_FENCE","1",1));
    assert(!setenv("BROOK_WAYLAND_GPU_SOCKET","/tmp/fake-consumer",1));
    struct brook_image *images[2];struct BrookGpuWindowPacket offers[2];
    for(unsigned i=0;i<2;++i){
        surface_frame(client,surface.resource,6+i);
        surface_attach(client,surface.resource,resources[i],0,0);surface_commit(client,surface.resource);
        struct brook_view *view=surface.tree.current->value;images[i]=view->image;held[i]=view->image->source;++held[i]->refs;
        assert(wl_list_length(&surface.committed_frames)==1);
        intel_pending_poll();assert(images[i]->offer_state==1 && held[i]->uses==1 && images[i]->pending_cpu==1);
        int fds[2];assert(brook_gpu_window_receive(channel[1],&offers[i],fds)==1);close(fds[0]);close(fds[1]);
        assert(offers[i].version==BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN);
        /* Offered and not yet presented: the clock withholds the callback
         * until the renderer reports it, or until the fallback delay. */
        const uint32_t now=g_now_ms();assert(!image_callbacks_due(images[i],now));
        assert(image_callbacks_due(images[i],now+GPU_CALLBACK_FALLBACK_MS));
        wl_display_flush_clients(display);assert(!count_events(clientSockets[1],6+i,WL_CALLBACK_DONE));
        assert(brook_gpu_window_send_reply(channel[1],BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN,BROOK_GPU_WINDOW_PRESENTED,&offers[i].identity)==1);
        if(i)assert(brook_gpu_window_send_reply(channel[1],BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN,BROOK_GPU_WINDOW_SAFE,&offers[0].identity)==1);
        intel_pending_poll();
        assert(images[i]->gpu_presented && wl_list_empty(&surface.committed_frames) && image_callbacks_due(images[i],now));
        wl_display_flush_clients(display);
        /* The presented source stays retained; the superseded one is
         * released without a CPU copy into the window buffer. */
        uint32_t wire[512];ssize_t bytes=recv(clientSockets[1],wire,sizeof(wire),MSG_DONTWAIT);assert(bytes>0);
        unsigned done=0,released[2]={0};
        for(size_t k=0;k<(size_t)bytes/4;){uint32_t id=wire[k],op=wire[k+1]&65535,size=wire[k+1]>>16;
            assert(size>=8 && k*4+size<=(size_t)bytes);assert(!(id==1 && op==WL_DISPLAY_ERROR));
            if(id==6+i && op==WL_CALLBACK_DONE)++done;
            if((id==2 || id==5) && op==WL_BUFFER_RELEASE)++released[id==5];
            k+=size/4;}
        assert(done==1 && released[1]==0 && released[0]==(unsigned)i);
        assert(vfb==sentinel && held[i]->uses==1);
        if(i)assert(!held[0]->uses);
    }
    /* A metadata-only commit of the presented source is paced by the clock. */
    assert(image_callbacks_due(images[1],g_now_ms()));
    /* A scene the GPU source cannot represent takes pixels on demand while
     * the renderer still holds the source; release still waits for SAFE. */
    const int tags=source_tags;
    intel_image_want_pixels(images[1]);intel_pending_poll();
    assert(images[1]->offer_state==1 && !images[1]->pending_cpu && images[1]->pixels && held[1]->uses==1);
    assert(vfb!=sentinel && source_tags==tags+1 && g_intel_retained_snapshots>=1);
    wl_display_flush_clients(display);assert(!count_events(clientSockets[1],5,WL_BUFFER_RELEASE));
    vfb=sentinel;
    assert(brook_gpu_window_send_reply(channel[1],BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN,BROOK_GPU_WINDOW_SAFE,&offers[1].identity)==1);
    intel_pending_poll();assert(!g_intel_pending_count && !held[1]->uses && vfb==sentinel);
    wl_display_flush_clients(display);assert(count_events(clientSockets[1],5,WL_BUFFER_RELEASE)==1);
    bt_state_set(&surface.tree.current,NULL);bt_state_set(&surface.tree.latest,NULL);buffer_drop(held[0]);buffer_drop(held[1]);
    free(surface.composite);g_surfaces=NULL;g_drm_intel=0;close(g_drm_fd);g_drm_fd=-1;
    close(channel[1]);gpu_window_channel_lost();gpu_window_reap_dead();
    unsetenv("BROOK_WAYLAND_INTEL_ASYNC");unsetenv("BROOK_WAYLAND_SNAPSHOT_FENCE");unsetenv("BROOK_WAYLAND_GPU_SOCKET");
    wl_client_destroy(client);close(clientSockets[1]);wl_display_destroy(display);
}
/* Version 3: a root DMA-BUF plus a DMA-BUF subsurface is offered as one
 * layered commit (no CPU readback), with a publish number newer than the
 * single offer before it; SAFE of the older single offer does not release
 * the root buffer the layered commit still holds; SAFE of the layered commit
 * releases both. */
static void check_gpu_window_layers(void)
{
    int clientSockets[2],channel[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,clientSockets));
    assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,channel));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,clientSockets[0]);assert(client);
    struct wl_resource *rootBuffer=retained_test_buffer(client,2);
    struct brook_surface root={0};surface_tree_init(&root);
    root.resource=wl_resource_create(client,&wl_surface_interface,4,3);assert(root.resource);
    wl_resource_set_user_data(root.resource,&root);
    root.xdg_toplevel=wl_resource_create(client,&xdg_toplevel_interface,1,4);assert(root.xdg_toplevel);
    struct wl_resource *childBuffer=retained_test_buffer(client,5);
    struct brook_surface *child=calloc(1,sizeof(*child));assert(child);surface_tree_init(child);
    child->resource=wl_resource_create(client,&wl_surface_interface,4,6);assert(child->resource);
    wl_resource_set_implementation(child->resource,&surface_impl,child,surface_destroy_userdata);
    struct wl_resource *sc=wl_resource_create(client,&wl_subcompositor_interface,1,7);assert(sc);
    root.xdg_acked=root.xdg_initial_commit_done=1;
    uint32_t vfb=0xff445566;root.wm_id=1;root.vfb=&vfb;root.vfb_w=root.vfb_h=root.vfb_stride=1;
    root.scene_disabled=1;root.next=child;g_surfaces=&root;
    char path[]="/tmp/brook-layers-test-XXXXXX";g_drm_fd=mkstemp(path);assert(g_drm_fd>=0);unlink(path);assert(!ftruncate(g_drm_fd,4096));
    g_drm_intel=1;g_gpu_window_channel=channel[0];window_stale=0;
    assert(!setenv("BROOK_WAYLAND_INTEL_ASYNC","1",1));assert(!setenv("BROOK_WAYLAND_SNAPSHOT_FENCE","1",1));
    assert(!setenv("BROOK_WAYLAND_GPU_SOCKET","/tmp/fake-consumer",1));
    const uint64_t groupsBefore=g_gpu_group_offers,readbackBefore=g_layer_readback_bytes;
    /* 1. Root alone: a version 2 single offer. */
    surface_attach(client,root.resource,rootBuffer,0,0);surface_commit(client,root.resource);
    struct brook_image *rootImage=((struct brook_view *)root.tree.current->value)->image;
    struct brook_buffer *rootHeld=rootImage->source;++rootHeld->refs;
    intel_pending_poll();assert(rootImage->offer_state==1);
    struct BrookGpuWindowPacket single;int fds[2];assert(brook_gpu_window_receive(channel[1],&single,fds)==1);close(fds[0]);close(fds[1]);
    assert(single.version==BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN && single.identity.commit==root.publish_commit);
    assert(brook_gpu_window_send_reply(channel[1],BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN,BROOK_GPU_WINDOW_PRESENTED,&single.identity)==1);
    intel_pending_poll();
    /* The window's VFB is still an older size (it resized while layered):
     * the layered offer must resize it to the root's committed size, as a
     * single offer does, or the renderer clips the layers to the old size. */
    root.vfb_w=root.vfb_h=3;resize_calls=0;
    /* 2. A synchronized subsurface with its own DMA-BUF joins on the next root commit. */
    subcomp_get_subsurface(client,sc,8,child->resource,root.resource);assert(child->tree.parent==&root.tree);
    surface_attach(client,child->resource,childBuffer,0,0);surface_commit(client,child->resource);
    surface_commit(client,root.resource);
    struct brook_draw draws[64];unsigned n=0;assert(surface_layers(&root,draws,&n) && n==2);
    struct brook_image *childImage=child->tree.current?((struct brook_view *)child->tree.current->value)->image:NULL;assert(childImage);
    struct brook_buffer *childHeld=childImage->source;++childHeld->refs;
    intel_pending_poll();
    struct BrookGpuWindowLayersPacket layered;int lfds[16];unsigned nfds=0;
    {int got=brook_gpu_window_receive_layers(channel[1],&layered,lfds,&nfds);
     if(got!=1){printf("LAYERS_DEBUG got=%d errno=%d groups=%llu wait=%llu shape=%llu buffer=%llu upload=%llu full=%llu offer_layers=%llu no_channel=%llu meta=%llu child_state=%d child_pending=%d child_fence=%d root_state=%d n=%u\n",
        got,errno,(unsigned long long)g_gpu_group_offers,(unsigned long long)g_gpu_group_wait,(unsigned long long)g_gpu_group_skip_shape,
        (unsigned long long)g_gpu_group_skip_buffer,(unsigned long long)g_gpu_group_skip_upload,(unsigned long long)g_gpu_group_full,
        (unsigned long long)g_gpu_offer_layers,(unsigned long long)g_gpu_offer_no_channel,(unsigned long long)g_gpu_offer_metadata,
        childImage->offer_state,childImage->pending_cpu,childImage->producer_fence,rootImage->offer_state,n);fflush(stdout);}
     assert(got==1);}
    for(unsigned i=0;i<nfds;++i)close(lfds[i]);
    assert(layered.header.version==BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS && layered.count==2 && nfds==4);
    assert(layered.header.identity.commit>single.identity.commit && layered.header.identity.commit==root.publish_commit);
    assert(layered.layers[1].dstX==0 && layered.layers[1].dstWidth==1 && layered.layers[1].srcWidth==1);
    assert(g_gpu_group_offers==groupsBefore+1 && rootImage->group_refs==1 && childImage->group_refs==1);
    assert(resize_calls==1 && resize_w==1 && resize_h==1 && root.vfb_w==1 && root.vfb_h==1 && root.vfb==resize_vfb);
    assert(childImage->offer_state==1 && rootImage->offer_state==1 && g_layer_readback_bytes==readbackBefore);
    /* Never presented: frame callbacks are still released after the fallback delay. */
    /* Any pending image, whatever its offer state, is due after the delay from queueing. */
    {const int saved=childImage->offer_state;childImage->offer_state=3;
     assert(!image_callbacks_due(childImage,childImage->queued_ms) &&
            image_callbacks_due(childImage,childImage->queued_ms+GPU_CALLBACK_FALLBACK_MS));childImage->offer_state=saved;}
    {const uint32_t now=g_now_ms();assert(!image_callbacks_due(childImage,now));
     assert(image_callbacks_due(childImage,now+GPU_CALLBACK_FALLBACK_MS) && image_callbacks_due(rootImage,now+GPU_CALLBACK_FALLBACK_MS));}
    /* 3. SAFE for the superseded single offer keeps the root held by the group. */
    assert(brook_gpu_window_send_reply(channel[1],BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN,BROOK_GPU_WINDOW_SAFE,&single.identity)==1);
    intel_pending_poll();assert(rootImage->offer_state==1 && rootHeld->uses==1 && g_gpu_window_channel>=0);
    /* 4. PRESENTED, then SAFE of the layered commit releases its group. */
    assert(brook_gpu_window_send_reply(channel[1],BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS,BROOK_GPU_WINDOW_PRESENTED,&layered.header.identity)==1);
    intel_pending_poll();assert(childImage->gpu_presented && rootImage->gpu_presented && g_gpu_group_presented>=1);
    /* 4b. The window can no longer be offered as layers (a transformed
     * subsurface): both layers held by the presented group are read back and
     * composed on the CPU, instead of the renderer showing that group forever. */
    {struct brook_view *cv=child->tree.current->value;const uint64_t snapshots=g_intel_retained_snapshots;
     cv->geometry.transform=1;assert(surface_render(&root));
     assert(childImage->want_pixels || childImage->pending_cpu==0);
     for(int k=0;k<4 && (childImage->pending_cpu || rootImage->pending_cpu);++k)intel_pending_poll();
     assert(!childImage->pending_cpu && !rootImage->pending_cpu && g_intel_retained_snapshots==snapshots+2);
     assert(childImage->pending_fd>=0 && rootImage->pending_fd>=0 && childImage->offer_state==1);
     cv->geometry.transform=0;}
    assert(brook_gpu_window_send_reply(channel[1],BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS,BROOK_GPU_WINDOW_SAFE,&layered.header.identity)==1);
    intel_pending_poll();assert(g_gpu_window_channel>=0 && !rootImage->group_refs && !childImage->group_refs && g_gpu_group_safe>=1);
    /* The group's SAFE retires the read-back images too: their client
     * buffers are released (fds closed), not left waiting forever. */
    intel_pending_poll();assert(childImage->pending_fd<0 && rootImage->pending_fd<0);
    for(unsigned i=0;i<GPU_GROUPS;++i)assert(!g_gpu_groups[i].live);
    bt_state_set(&child->tree.current,NULL);bt_state_set(&child->tree.latest,NULL);
    bt_state_set(&root.tree.current,NULL);bt_state_set(&root.tree.latest,NULL);
    buffer_drop(rootHeld);buffer_drop(childHeld);
    free(root.composite);g_surfaces=NULL;g_drm_intel=0;close(g_drm_fd);g_drm_fd=-1;
    close(channel[1]);gpu_window_channel_lost();gpu_window_reap_dead();
    unsetenv("BROOK_WAYLAND_INTEL_ASYNC");unsetenv("BROOK_WAYLAND_SNAPSHOT_FENCE");unsetenv("BROOK_WAYLAND_GPU_SOCKET");
    wl_client_destroy(client);close(clientSockets[1]);wl_display_destroy(display);
}
/* Version 3 with a CPU-pixel layer: the SHM-style root is copied once into
 * a GEM buffer and offered with the DMA-BUF subsurface; the GEM copy is
 * freed with its image. */
static void check_gpu_window_layers_shm(void)
{
    int clientSockets[2],channel[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,clientSockets));
    assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,channel));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,clientSockets[0]);assert(client);
    struct brook_surface root={0};surface_tree_init(&root);
    root.resource=wl_resource_create(client,&wl_surface_interface,4,2);assert(root.resource);
    wl_resource_set_user_data(root.resource,&root);
    root.xdg_toplevel=wl_resource_create(client,&xdg_toplevel_interface,1,3);assert(root.xdg_toplevel);
    struct wl_resource *childBuffer=retained_test_buffer(client,4);
    struct brook_surface *child=calloc(1,sizeof(*child));assert(child);surface_tree_init(child);
    child->resource=wl_resource_create(client,&wl_surface_interface,4,5);assert(child->resource);
    wl_resource_set_implementation(child->resource,&surface_impl,child,surface_destroy_userdata);
    struct wl_resource *sc=wl_resource_create(client,&wl_subcompositor_interface,1,6);assert(sc);
    root.xdg_acked=root.xdg_initial_commit_done=1;
    uint32_t vfb=0xff445566;root.wm_id=1;root.vfb=&vfb;root.vfb_w=root.vfb_h=root.vfb_stride=1;
    root.scene_disabled=1;root.next=child;g_surfaces=&root;
    char path[]="/tmp/brook-layers-shm-XXXXXX";g_drm_fd=mkstemp(path);assert(g_drm_fd>=0);unlink(path);assert(!ftruncate(g_drm_fd,4096));
    g_drm_intel=1;g_gpu_window_channel=channel[0];window_stale=0;gem_create_ok=1;gem_creates=gem_closes=0;
    assert(!setenv("BROOK_WAYLAND_INTEL_ASYNC","1",1));assert(!setenv("BROOK_WAYLAND_SNAPSHOT_FENCE","1",1));
    assert(!setenv("BROOK_WAYLAND_GPU_SOCKET","/tmp/fake-consumer",1));
    subcomp_get_subsurface(client,sc,7,child->resource,root.resource);
    uint32_t px=0x80402010;struct brook_image *rootImage=image_snapshot((uint8_t *)&px,1,1,4,WL_SHM_FORMAT_ARGB8888);assert(rootImage);
    assert(surface_tree_commit(&root,rootImage));image_unref(rootImage);
    surface_attach(client,child->resource,childBuffer,0,0);surface_commit(client,child->resource);
    surface_commit(client,root.resource);
    struct brook_image *childImage=((struct brook_view *)child->tree.current->value)->image;
    struct brook_buffer *childHeld=childImage->source;++childHeld->refs;
    intel_pending_poll();
    struct BrookGpuWindowLayersPacket layered;int lfds[16];unsigned nfds=0;
    assert(brook_gpu_window_receive_layers(channel[1],&layered,lfds,&nfds)==1 && layered.count==2 && nfds==4);
    for(unsigned i=0;i<nfds;++i)close(lfds[i]);
    assert(layered.layers[0].fourcc==0x34325241 && !(layered.layers[0].flags&BROOK_GPU_WINDOW_LAYER_OPAQUE));
    assert(gem_creates==1 && rootImage->gem_handle==77 && rootImage->gem_fd>=0 && rootImage->gem_fence>=0);
    uint32_t copied=0;assert(pread(g_drm_fd,&copied,4,0)==4 && copied==0x80402010);
    unsigned extra=0;struct BrookGpuWindowLayersPacket more;
    while(brook_gpu_window_receive_layers(channel[1],&more,lfds,&nfds)==1){for(unsigned i=0;i<nfds;++i)close(lfds[i]);++extra;}
    assert(!extra);
    /* SAFE while the window still shows these layers: the DMA-BUF child is
     * snapshotted and released, and the window is re-offered from CPU pixels
     * (both layers now GEM copies). Its SAFE then releases everything. */
    assert(brook_gpu_window_send_reply(channel[1],BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS,BROOK_GPU_WINDOW_SAFE,&layered.header.identity)==1);
    intel_pending_poll();
    assert(brook_gpu_window_receive_layers(channel[1],&more,lfds,&nfds)==1 && more.count==2);
    for(unsigned i=0;i<nfds;++i)close(lfds[i]);
    assert(more.header.identity.commit>layered.header.identity.commit && gem_creates==2 && !childHeld->uses);
    assert(brook_gpu_window_send_reply(channel[1],BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS,BROOK_GPU_WINDOW_SAFE,&more.header.identity)==1);
    intel_pending_poll();assert(!rootImage->group_refs && !childImage->group_refs);
    assert(brook_gpu_window_receive_layers(channel[1],&more,lfds,&nfds)==0);
    bt_state_set(&child->tree.current,NULL);bt_state_set(&child->tree.latest,NULL);
    bt_state_set(&root.tree.current,NULL);bt_state_set(&root.tree.latest,NULL);
    assert(gem_closes==2); /* both images and their GEM copies are gone */
    buffer_drop(childHeld);
    free(root.composite);g_surfaces=NULL;g_drm_intel=0;gem_create_ok=0;close(g_drm_fd);g_drm_fd=-1;
    close(channel[1]);gpu_window_channel_lost();gpu_window_reap_dead();
    unsetenv("BROOK_WAYLAND_INTEL_ASYNC");unsetenv("BROOK_WAYLAND_SNAPSHOT_FENCE");unsetenv("BROOK_WAYLAND_GPU_SOCKET");
    wl_client_destroy(client);close(clientSockets[1]);wl_display_destroy(display);
}
/* An ARGB DMA-BUF whose opaque region covers the surface is offered as a
 * one-layer version 3 commit with the opaque flag; without the region it is
 * an ordinary version 2 offer. */
static void check_gpu_window_opaque(void)
{
    int clientSockets[2],channel[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,clientSockets));
    assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,channel));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,clientSockets[0]);assert(client);
    struct wl_resource *buffers[2]={retained_test_buffer(client,2),NULL};
    ((struct brook_dmabuf_buffer *)wl_resource_get_user_data(buffers[0]))->fourcc=0x34325241;
    struct brook_surface root={0};surface_tree_init(&root);
    root.resource=wl_resource_create(client,&wl_surface_interface,4,3);assert(root.resource);
    wl_resource_set_user_data(root.resource,&root);
    root.xdg_toplevel=wl_resource_create(client,&xdg_toplevel_interface,1,4);assert(root.xdg_toplevel);
    buffers[1]=retained_test_buffer(client,5);
    ((struct brook_dmabuf_buffer *)wl_resource_get_user_data(buffers[1]))->fourcc=0x34325241;
    /* Chrome's region: its initial window size, larger than the surface. */
    struct pointer_rect rect={0,0,1050,880,1};struct pointer_region region={&rect,1,0};
    struct wl_resource *reg=wl_resource_create(client,&wl_region_interface,1,6);assert(reg);wl_resource_set_user_data(reg,&region);
    root.xdg_acked=root.xdg_initial_commit_done=1;
    uint32_t vfb=0;root.wm_id=1;root.vfb=&vfb;root.vfb_w=root.vfb_h=root.vfb_stride=1;
    root.scene_disabled=1;g_surfaces=&root;
    char path[]="/tmp/brook-opaque-test-XXXXXX";g_drm_fd=mkstemp(path);assert(g_drm_fd>=0);unlink(path);assert(!ftruncate(g_drm_fd,4096));
    g_drm_intel=1;g_gpu_window_channel=channel[0];window_stale=0;
    assert(!setenv("BROOK_WAYLAND_INTEL_ASYNC","1",1));assert(!setenv("BROOK_WAYLAND_SNAPSHOT_FENCE","1",1));
    assert(!setenv("BROOK_WAYLAND_GPU_SOCKET","/tmp/fake-consumer",1));
    struct brook_buffer *held[2];struct BrookGpuWindowLayersPacket got;int fds[16];unsigned nfds=0;
    for(unsigned i=0;i<2;++i){
        if(i)surface_set_opaque_region(client,root.resource,reg);
        surface_attach(client,root.resource,buffers[i],0,0);surface_commit(client,root.resource);
        struct brook_view *view=root.tree.current->value;held[i]=view->image->source;++held[i]->refs;
        assert(view->opaque==(int)i);
        intel_pending_poll();
        assert(brook_gpu_window_receive_layers(channel[1],&got,fds,&nfds)==1 && nfds==2);
        for(unsigned k=0;k<nfds;++k)close(fds[k]);
        if(!i)assert(got.header.version==BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN && !got.count);
        else assert(got.header.version==BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS && got.count==1 &&
                    (got.layers[0].flags&BROOK_GPU_WINDOW_LAYER_OPAQUE) && got.layers[0].dstWidth==1);
        assert(brook_gpu_window_send_reply(channel[1],got.header.version,BROOK_GPU_WINDOW_SAFE,&got.header.identity)==1);
        intel_pending_poll();assert(g_gpu_window_channel>=0);
    }
    /* Coverage is in logical surface units, clamped to the surface. */
    {struct brook_surface t={0};t.opaque_set=1;t.opaque_w=1050;t.opaque_h=880;
     assert(surface_opaque_covers(&t,832,642) && surface_opaque_covers(&t,1050,880) && !surface_opaque_covers(&t,1051,880));
     t.opaque_x=1;assert(!surface_opaque_covers(&t,832,642));t.opaque_x=-5;assert(surface_opaque_covers(&t,832,642));
     t.opaque_set=0;assert(!surface_opaque_covers(&t,832,642));}
    /* A later NULL region clears it for the next commit. */
    surface_set_opaque_region(client,root.resource,NULL);surface_commit(client,root.resource);
    assert(!root.opaque_set && ((struct brook_view *)root.tree.current->value)->opaque==0);
    wl_resource_set_user_data(reg,NULL);
    bt_state_set(&root.tree.current,NULL);bt_state_set(&root.tree.latest,NULL);buffer_drop(held[0]);buffer_drop(held[1]);
    free(root.composite);g_surfaces=NULL;g_drm_intel=0;close(g_drm_fd);g_drm_fd=-1;
    close(channel[1]);gpu_window_channel_lost();gpu_window_reap_dead();
    unsetenv("BROOK_WAYLAND_INTEL_ASYNC");unsetenv("BROOK_WAYLAND_SNAPSHOT_FENCE");unsetenv("BROOK_WAYLAND_GPU_SOCKET");
    wl_client_destroy(client);close(clientSockets[1]);wl_display_destroy(display);
}
/* A buffer drawn scaled (here a 1x1 XRGB buffer with a 2x2 viewport
 * destination, as a 640x480 game on a 200% output) is offered directly as
 * a one-layer version 3 commit with both rectangles and the opaque flag,
 * instead of being composed into the 2x2 VFB by the provider. */
static void check_gpu_window_scaled(void)
{
    int clientSockets[2],channel[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,clientSockets));
    assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,channel));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,clientSockets[0]);assert(client);
    struct wl_resource *buffer=retained_test_buffer(client,2);
    struct brook_surface root={0};surface_tree_init(&root);
    root.resource=wl_resource_create(client,&wl_surface_interface,4,3);assert(root.resource);
    wl_resource_set_user_data(root.resource,&root);
    root.xdg_toplevel=wl_resource_create(client,&xdg_toplevel_interface,1,4);assert(root.xdg_toplevel);
    root.xdg_acked=root.xdg_initial_commit_done=1;
    root.geometry.destination_set=1;root.geometry.dw=root.geometry.dh=2;
    uint32_t vfb[4]={0};root.wm_id=1;root.vfb=vfb;root.vfb_w=root.vfb_h=root.vfb_stride=2;
    root.scene_disabled=1;g_surfaces=&root;
    char path[]="/tmp/brook-scaled-test-XXXXXX";g_drm_fd=mkstemp(path);assert(g_drm_fd>=0);unlink(path);assert(!ftruncate(g_drm_fd,4096));
    g_drm_intel=1;g_gpu_window_channel=channel[0];window_stale=0;
    assert(!setenv("BROOK_WAYLAND_INTEL_ASYNC","1",1));assert(!setenv("BROOK_WAYLAND_SNAPSHOT_FENCE","1",1));
    assert(!setenv("BROOK_WAYLAND_GPU_SOCKET","/tmp/fake-consumer",1));
    const uint64_t before=g_gpu_scaled_offers,geometryBefore=g_gpu_offer_geometry;
    surface_attach(client,root.resource,buffer,0,0);surface_commit(client,root.resource);
    struct brook_view *view=root.tree.current->value;struct brook_buffer *held=view->image->source;++held->refs;
    assert(view->geometry.w==2 && view->geometry.h==2 && view->image->width==1);
    intel_pending_poll();
    struct BrookGpuWindowLayersPacket got;int fds[16];unsigned nfds=0;
    assert(brook_gpu_window_receive_layers(channel[1],&got,fds,&nfds)==1 && nfds==2);
    for(unsigned k=0;k<nfds;++k)close(fds[k]);
    assert(got.header.version==BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS && got.count==1);
    const struct BrookGpuWindowLayer *l=&got.layers[0];
    assert(l->width==1 && l->height==1 && l->srcX==0 && l->srcY==0 && l->srcWidth==1 && l->srcHeight==1 &&
           l->dstX==0 && l->dstY==0 && l->dstWidth==2 && l->dstHeight==2 && (l->flags&BROOK_GPU_WINDOW_LAYER_OPAQUE));
    assert(g_gpu_scaled_offers==before+1 && g_gpu_offer_geometry==geometryBefore);
    assert(brook_gpu_window_send_reply(channel[1],got.header.version,BROOK_GPU_WINDOW_SAFE,&got.header.identity)==1);
    intel_pending_poll();assert(g_gpu_window_channel>=0);
    bt_state_set(&root.tree.current,NULL);bt_state_set(&root.tree.latest,NULL);buffer_drop(held);
    free(root.composite);g_surfaces=NULL;g_drm_intel=0;close(g_drm_fd);g_drm_fd=-1;
    close(channel[1]);gpu_window_channel_lost();gpu_window_reap_dead();
    unsetenv("BROOK_WAYLAND_INTEL_ASYNC");unsetenv("BROOK_WAYLAND_SNAPSHOT_FENCE");unsetenv("BROOK_WAYLAND_GPU_SOCKET");
    wl_client_destroy(client);close(clientSockets[1]);wl_display_destroy(display);
}
/* A rootless Xwayland window (role 4, no xdg_toplevel) paired with its
 * mapped X window is offered to the renderer like a native toplevel:
 * glamor's DMA-BUF window buffer (or a Present flip's pixmap) becomes the
 * window's GPU source instead of a CPU readback into the VFB. An unpaired
 * surface and a buffer still waiting for a native resize stay on the CPU
 * path. */
static void check_gpu_window_xwayland(void)
{
    int clientSockets[2],channel[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,clientSockets));
    assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,channel));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,clientSockets[0]);assert(client);
    struct wl_resource *first=retained_test_buffer(client,2);
    struct brook_surface root={0};surface_tree_init(&root);
    root.resource=wl_resource_create(client,&wl_surface_interface,4,3);assert(root.resource);
    wl_resource_set_user_data(root.resource,&root);
    struct brook_xwindow xw={0};xw.state.flags=BXWM_MAPPED;xw.state.serial=9;xw.state.width=xw.state.height=1;
    xw.surface=&root;xw.native_valid=1;
    root.role=4;root.xwindow=&xw;root.xwayland_serial=9;root.xwayland_generation=g_xwayland_generation;g_xwm_failed=0;
    uint32_t vfb=0;root.wm_id=1;root.vfb=&vfb;root.vfb_w=root.vfb_h=root.vfb_stride=1;
    root.scene_disabled=1;g_surfaces=&root;
    char path[]="/tmp/brook-xwl-test-XXXXXX";g_drm_fd=mkstemp(path);assert(g_drm_fd>=0);unlink(path);assert(!ftruncate(g_drm_fd,4096));
    g_drm_intel=1;g_gpu_window_channel=channel[0];window_stale=0;
    assert(!setenv("BROOK_WAYLAND_INTEL_ASYNC","1",1));assert(!setenv("BROOK_WAYLAND_SNAPSHOT_FENCE","1",1));
    assert(!setenv("BROOK_WAYLAND_GPU_SOCKET","/tmp/fake-consumer",1));
    /* Paired and mapped: one GPU offer, no CPU copy, released after SAFE. */
    const uint64_t offers=g_gpu_xwayland_offers,noRoot=g_gpu_offer_no_root,ready=g_intel_ready;
    surface_attach(client,root.resource,first,0,0);surface_commit(client,root.resource);
    struct brook_view *view=root.tree.current->value;struct brook_buffer *held=view->image->source;++held->refs;
    intel_pending_poll();
    assert(view->image->offer_state==1 && g_gpu_xwayland_offers==offers+1 && g_gpu_offer_no_root==noRoot);
    struct BrookGpuWindowPacket offer={0};int fds[2];assert(brook_gpu_window_receive(channel[1],&offer,fds)==1);
    assert(offer.type==BROOK_GPU_WINDOW_OFFER && offer.identity.window==1 && offer.width==1 && offer.fourcc==0x34325258);
    close(fds[0]);close(fds[1]);
    assert(brook_gpu_window_send_safe(channel[1],&offer.identity)==1);
    intel_pending_poll();assert(!g_intel_pending_count && !held->uses);(void)ready;
    wl_display_flush_clients(display);assert(count_events(clientSockets[1],2,WL_BUFFER_RELEASE)==1);
    bt_state_set(&root.tree.current,NULL);bt_state_set(&root.tree.latest,NULL);buffer_drop(held);
    /* A native resize to 2x2 is pending: the 1x1 buffer is not offered. */
    const uint64_t waits=g_gpu_xwayland_resize_wait;xw.target_width=xw.target_height=2;
    struct wl_resource *second=retained_test_buffer(client,4);
    surface_attach(client,root.resource,second,0,0);surface_commit(client,root.resource);
    view=root.tree.current->value;held=view->image->source;++held->refs;
    intel_pending_poll();
    assert(g_gpu_xwayland_resize_wait==waits+1 && g_gpu_xwayland_offers==offers+1 && view->image->offer_state==4);
    assert(brook_gpu_window_receive(channel[1],&offer,fds)==0);
    intel_pending_poll();assert(!g_intel_pending_count);
    bt_state_set(&root.tree.current,NULL);bt_state_set(&root.tree.latest,NULL);buffer_drop(held);
    xw.target_width=xw.target_height=0;
    /* Unpaired (serial mismatch): not a window root, so no offer. */
    xw.state.serial=10;
    struct wl_resource *third=retained_test_buffer(client,5);
    surface_attach(client,root.resource,third,0,0);surface_commit(client,root.resource);
    view=root.tree.current->value;held=view->image->source;++held->refs;
    intel_pending_poll();
    assert(g_gpu_offer_no_root==noRoot+1 && g_gpu_xwayland_offers==offers+1);
    assert(brook_gpu_window_receive(channel[1],&offer,fds)==0);
    intel_pending_poll();assert(!g_intel_pending_count);
    bt_state_set(&root.tree.current,NULL);bt_state_set(&root.tree.latest,NULL);buffer_drop(held);
    root.xwindow=NULL;
    free(root.composite);g_surfaces=NULL;g_drm_intel=0;close(g_drm_fd);g_drm_fd=-1;
    close(channel[1]);gpu_window_channel_lost();gpu_window_reap_dead();
    unsetenv("BROOK_WAYLAND_INTEL_ASYNC");unsetenv("BROOK_WAYLAND_SNAPSHOT_FENCE");unsetenv("BROOK_WAYLAND_GPU_SOCKET");
    wl_client_destroy(client);close(clientSockets[1]);wl_display_destroy(display);
}
/* Chromium asks for client-side decoration and then draws a translucent
 * frame, so its window is never opaque. waylandd answers SERVER_SIDE for
 * listed app_ids, also when the app_id arrives after the request. */
static void check_decoration_policy(void)
{
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct brook_surface a={0},b={0};surface_tree_init(&a);surface_tree_init(&b);
    struct wl_resource *da=wl_resource_create(client,&zxdg_toplevel_decoration_v1_interface,1,2);assert(da);
    wl_resource_set_implementation(da,&deco_impl,&a,deco_resource_destroyed);
    struct wl_resource *db=wl_resource_create(client,&zxdg_toplevel_decoration_v1_interface,1,3);assert(db);
    wl_resource_set_implementation(db,&deco_impl,&b,deco_resource_destroyed);
    struct wl_resource *ta=wl_resource_create(client,&xdg_toplevel_interface,1,4);assert(ta);wl_resource_set_user_data(ta,&a);
    /* Default: the client's CLIENT_SIDE request is honoured. */
    unsetenv("BROOK_WAYLAND_SSD_APPS");
    snprintf(b.app_id,sizeof(b.app_id),"chromium-browser");
    deco_set_mode(client,db,ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);assert(b.deco_csd==1);b.app_id[0]=0;
    assert(!setenv("BROOK_WAYLAND_SSD_APPS","chromium,google-chrome",1));
    /* a: request first, app_id later (Chromium's order) -> server side. */
    deco_set_mode(client,da,ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);assert(a.deco_csd==1 && a.deco_resource==da);
    xdg_toplevel_set_app_id(client,ta,"chromium-browser");assert(a.deco_csd==0);
    /* b: an unlisted app keeps client-side decoration. */
    snprintf(b.app_id,sizeof(b.app_id),"org.gnome.Calculator");
    deco_set_mode(client,db,ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);assert(b.deco_csd==1);
    /* An empty list disables the policy. */
    assert(!setenv("BROOK_WAYLAND_SSD_APPS","",1));
    deco_set_mode(client,da,ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);assert(a.deco_csd==1);
    assert(!setenv("BROOK_WAYLAND_SSD_APPS","org.gnome",1));
    deco_set_mode(client,db,ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);assert(b.deco_csd==0);
    unsetenv("BROOK_WAYLAND_SSD_APPS");
    wl_resource_destroy(da);assert(!a.deco_resource);
    wl_resource_set_user_data(db,NULL);
    wl_client_destroy(client);close(sockets[1]);wl_display_destroy(display);
}
static void check_intel_direct(int mode)
{
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct brook_dmabuf_buffer *db=calloc(1,sizeof(*db));assert(db);
    db->refs=1;db->width=db->height=1;db->stride=4;db->bo_handle=123;db->fourcc=0x34325258;
    struct wl_resource *resource=wl_resource_create(client,&wl_buffer_interface,1,2);assert(resource);
    wl_resource_set_implementation(resource,&dmabuf_buffer_impl,db,dmabuf_buffer_destroy);
    struct brook_surface surface={0};surface_tree_init(&surface);
    surface.resource=wl_resource_create(client,&wl_surface_interface,4,3);assert(surface.resource);
    wl_resource_set_user_data(surface.resource,&surface);
    surface.xdg_toplevel=wl_resource_create(client,&xdg_toplevel_interface,1,4);assert(surface.xdg_toplevel);
    surface.xdg_acked=surface.xdg_initial_commit_done=1;
    uint32_t vfb=0xffabcdef;surface.wm_id=1;surface.vfb=&vfb;
    surface.vfb_w=surface.vfb_h=surface.vfb_stride=1;surface.scene_disabled=1;g_surfaces=&surface;
    char path[]="/tmp/brook-direct-test-XXXXXX";g_drm_fd=mkstemp(path);assert(g_drm_fd>=0);unlink(path);assert(!ftruncate(g_drm_fd,4096));
    g_drm_intel=1;direct_status=mode==1?BROOK_VK_UNCERTAIN:BROOK_VK_COMPLETE;direct_calls=0;window_queries=0;window_stale=mode==2;direct_mutate_commit=mode==3;
    assert(!setenv("BROOK_WAYLAND_INTEL_ASYNC","1",1));assert(!setenv("BROOK_WAYLAND_SNAPSHOT_FENCE","1",1));
    assert(!setenv("BROOK_WAYLAND_DIRECT_SOURCE","1",1));
    surface_attach(client,surface.resource,resource,0,0);surface_commit(client,surface.resource);
    struct brook_view *view=surface.tree.current->value;assert(view && view->image && view->image->source);
    struct brook_buffer *buffer=view->image->source;++buffer->refs;
    uint64_t published=g_intel_direct;intel_pending_poll();assert(direct_calls==1 && !g_intel_pending_count);
    assert(g_intel_direct-published==(uint64_t)(mode==0));
    if(mode==1)assert(buffer->uncertain && buffer->uses==1 && view->image->pending_cpu==2);
    else assert(!buffer->uncertain && !buffer->uses && !view->image->pending_cpu);
    assert(window_queries==((mode==1 || mode==3)?1:2));
    wl_display_flush_clients(display);uint32_t wire[256];ssize_t bytes=recv(sockets[1],wire,sizeof(wire),MSG_DONTWAIT);
    int releases=0,errors=0;if(bytes>0)for(size_t i=0;i<(size_t)bytes/4;){
        uint32_t id=wire[i],op=wire[i+1]&65535,size=wire[i+1]>>16;assert(size>=8 && i*4+size<=(size_t)bytes);
        if(id==2 && op==WL_BUFFER_RELEASE)++releases;
        if(id==1 && op==WL_DISPLAY_ERROR)++errors;
        i+=size/4;
    }
    assert(releases==(mode!=1) && errors==(mode==1));
    bt_state_set(&surface.tree.current,NULL);bt_state_set(&surface.tree.latest,NULL);buffer_drop(buffer);
    free(surface.composite);g_surfaces=NULL;g_drm_intel=0;close(g_drm_fd);g_drm_fd=-1;
    unsetenv("BROOK_WAYLAND_INTEL_ASYNC");unsetenv("BROOK_WAYLAND_SNAPSHOT_FENCE");unsetenv("BROOK_WAYLAND_DIRECT_SOURCE");
    wl_client_destroy(client);close(sockets[1]);wl_display_destroy(display);
}
#endif
static void check(long rc,int error,int safe,int destroy_early)
{
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct brook_dmabuf_buffer *db=calloc(1,sizeof(*db));assert(db);
    db->refs=1;db->width=db->height=4;db->gres=65536;
    struct wl_resource *buffer=wl_resource_create(client,&wl_buffer_interface,1,2);assert(buffer);
    wl_resource_set_implementation(buffer,&dmabuf_buffer_impl,db,dmabuf_buffer_destroy);
    const uint32_t buffer_id=wl_resource_get_id(buffer);
    struct brook_surface surface={0};
    surface_tree_init(&surface);
    surface.resource=wl_resource_create(client,&wl_surface_interface,4,0);assert(surface.resource);
    wl_resource_set_user_data(surface.resource,&surface);
    surface.xdg_toplevel=wl_resource_create(client,&xdg_toplevel_interface,1,0);assert(surface.xdg_toplevel);
    surface.xdg_initial_commit_done=surface.xdg_acked=1;surface.wm_id=1;uint32_t vfb[16];surface.vfb=vfb;surface.vfb_w=surface.vfb_h=surface.vfb_stride=4;surface_attach(client,surface.resource,buffer,0,0);
    if (destroy_early) { wl_resource_destroy(buffer);assert(!surface.pending_buffer->resource); }
    result=rc;result_errno=error;calls=0;
    surface_commit(client,surface.resource);
    assert(calls==1 && !surface.pending_buffer);
    bt_state_set(&surface.tree.current,NULL);bt_state_set(&surface.tree.latest,NULL);
    scene_owner_unref(surface.scene);surface.scene=NULL;
    wl_display_flush_clients(display);
    uint32_t wire[256];ssize_t n=recv(sockets[1],wire,sizeof(wire),MSG_DONTWAIT);
    if (n<0) { assert(destroy_early && rc>0 && errno==EAGAIN);n=0; }
    else assert(n>0);
    int releases=0,errors=0;
    for (size_t i=0;i<(size_t)n/4;) {
        uint32_t id=wire[i],op=wire[i+1]&65535,size=wire[i+1]>>16;
        assert(size>=8 && i*4+size<=(size_t)n);
        if (id==buffer_id && op==WL_BUFFER_RELEASE) ++releases;
        if (id==1 && op==WL_DISPLAY_ERROR) ++errors;
        i+=size/4;
    }
    assert(errors==(rc<0));
    if (!safe || destroy_early) assert(!releases);
    else if (rc>0) assert(releases==1);
    else assert(releases<=1); // Fatal protocol errors may suppress later events.
    wl_client_destroy(client);close(sockets[1]);wl_display_destroy(display);
}
static void check_intel_release(unsigned fail_request,int uncertain)
{
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct brook_dmabuf_buffer *db=calloc(1,sizeof(*db));assert(db);
    db->refs=1;db->width=db->height=4;db->stride=16;db->bo_handle=123;
    struct wl_resource *resource=wl_resource_create(client,&wl_buffer_interface,1,2);assert(resource);
    wl_resource_set_implementation(resource,&dmabuf_buffer_impl,db,dmabuf_buffer_destroy);
    struct brook_surface surface={0};surface_tree_init(&surface);
    surface.resource=wl_resource_create(client,&wl_surface_interface,4,0);assert(surface.resource);
    wl_resource_set_user_data(surface.resource,&surface);
    surface.xdg_toplevel=wl_resource_create(client,&xdg_toplevel_interface,1,0);assert(surface.xdg_toplevel);
    surface.xdg_initial_commit_done=surface.xdg_acked=1;surface.wm_id=1;
    surface_attach(client,surface.resource,resource,0,0);assert(surface.pending_buffer);
    g_drm_intel=1;intel_fail_request=fail_request;
    surface_commit(client,surface.resource);
    assert(surface.pending_buffer && surface.pending_buffer->uncertain==uncertain && surface.pending_buffer->uses==0);
    wl_display_flush_clients(display);
    uint32_t wire[256];ssize_t n=recv(sockets[1],wire,sizeof(wire),MSG_DONTWAIT);assert(n>0);
    int releases=0,errors=0;for(size_t i=0;i<(size_t)n/4;){
        uint32_t id=wire[i],op=wire[i+1]&65535,size=wire[i+1]>>16;
        assert(size>=8 && i*4+size<=(size_t)n);
        if(id==2 && op==WL_BUFFER_RELEASE)++releases;
        if(id==1 && op==WL_DISPLAY_ERROR){assert(size>=20);
            assert(wire[i+3]==(uncertain?WL_DISPLAY_ERROR_IMPLEMENTATION:WL_DISPLAY_ERROR_NO_MEMORY));++errors;}
        i+=size/4;
    }
    assert(releases==(uncertain?0:1) && errors==1);
    buffer_drop(surface.pending_buffer);surface.pending_buffer=NULL;
    bt_state_set(&surface.tree.current,NULL);bt_state_set(&surface.tree.latest,NULL);
    wl_client_destroy(client);close(sockets[1]);wl_display_destroy(display);
    g_drm_intel=0;intel_fail_request=0;
}
int main(void)
{
    struct GpuWindowTiming timing={0};
    assert(!setenv("BROOK_WAYLAND_GPU_TIMING","1",1));
    gpu_window_timing_add(&timing,100,140);gpu_window_timing_add(&timing,0xfffffff0u,5);
    gpu_window_timing_add(&timing,100,99);assert(timing.count==2 && timing.sum==61 && timing.max==40);
    assert(gpu_window_timing_p80_bound(&timing)==50);
    unsetenv("BROOK_WAYLAND_GPU_TIMING");
    check(7,0,1,0);check(-116,0,1,0);check(-1,EINVAL,1,0);
    check(-EIO,0,0,0);check(-1,EIO,0,0);
    check(7,0,1,1);check(-1,EIO,0,1);
    check_intel_release(0x6c,1); /* wait does not establish retirement */
    check_intel_release(0x5f,1); /* transition did not complete */
    check_intel_release(0x64,0); /* retired, mapping setup failed */
    check_intel_deferred(0,0);check_intel_deferred(0,1);
    check_intel_deferred(EIO,0);check_intel_deferred(ENOSYS,0);
    check_intel_fence(0);check_intel_fence(1);check_intel_fence(2);
    check_superseded_intel_image();
    check_intel_deferred_surface();
#ifdef BROOK_DIRECT_TEST
    source_begins=source_tags=0;check_gpu_window_offer(0);
    assert(source_begins>0 && source_tags>0);
    check_gpu_window_offer(1);check_gpu_window_offer(2);
    check_gpu_window_retained();
    check_gpu_window_layers();
    check_gpu_window_layers_shm();
    check_gpu_window_opaque();
    check_gpu_window_scaled();
    check_gpu_window_xwayland();
    check_decoration_policy();
    check_intel_direct(0);check_intel_direct(2);check_intel_direct(3);check_intel_direct(1);
#endif
    {
        struct brook_dmabuf_buffer db={.width=4,.height=4,.stride=16,.bo_handle=123};
        g_drm_fd=open("/dev/zero",O_RDWR);assert(g_drm_fd>=0);int uncertain=1;
        struct brook_image *image=intel_dmabuf_snapshot(&db,&uncertain);
        assert(image && !uncertain && image->width==4 && image->height==4);
        image_unref(image);close(g_drm_fd);g_drm_fd=-1;
    }
    puts("CAPTURE_RELEASE_PASS published/discarded release; raw/libc uncertain errors never release");
}
