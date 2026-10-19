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
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall
#undef fprintf
static int test_fprintf(FILE *stream,const char *format,...)
{ (void)stream;(void)format;errno=ENOSPC;return -1; }
static long result;
static int result_errno, calls;
static long test_syscall(long number, ...)
{
    va_list args;va_start(args,number);
    if (number==0xB06) { va_end(args);return open("/dev/null",O_RDWR); }
    assert(number==0xB07);(void)va_arg(args,long);long op=va_arg(args,long);va_end(args);
    if (op!=SCENE_CTL_CAPTURE_DMABUF) return 0;
    ++calls;errno=result_errno;return result;
}
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
int main(void)
{
    check(7,0,1,0);check(-116,0,1,0);check(-1,EINVAL,1,0);
    check(-EIO,0,0,0);check(-1,EIO,0,0);
    check(7,0,1,1);check(-1,EIO,0,1);
    puts("CAPTURE_RELEASE_PASS published/discarded release; raw/libc uncertain errors never release");
}
