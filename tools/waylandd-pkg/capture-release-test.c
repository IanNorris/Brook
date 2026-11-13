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
static int try_error,snapshot_error,snapshot_status=1,mock_fence_fd=-1;
static int test_ioctl(int fd,unsigned long request,...)
{
    (void)fd;
    if((request&255u)==intel_fail_request){errno=EIO;return -1;}
    if(request==0xc0383e04u){
        va_list args;va_start(args,request);unsigned char *info=va_arg(args,void *);va_end(args);
        memcpy(info+32,&snapshot_status,sizeof(snapshot_status));return 0;
    }
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
    if(number==BROOK_SYS_WM_SIGNAL_DIRTY){va_end(args);return 0;}
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
