/* Exercise the production params state machine, before any DRM import. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <wayland-server.h>
static unsigned errors;
static uint32_t last_error;
static void test_protocol_error(struct wl_resource *resource,uint32_t error,const char *format,...) {
    (void)resource;(void)format;++errors;last_error=error;
}
#define wl_resource_post_error test_protocol_error
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef wl_resource_post_error
static int plane_fd(void) {
    int fd=memfd_create("params-layout",0);assert(fd>=0);assert(ftruncate(fd,4096)==0);return fd;
}
int main(void) {
    int sockets[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    for (unsigned i=0;i<11;++i) {
        struct brook_dmabuf_params params={.fd=-1};
        struct wl_resource *resource=wl_resource_create(client,&zwp_linux_buffer_params_v1_interface,4,0);assert(resource);
        wl_resource_set_user_data(resource,&params);
        errors=0;int fd=plane_fd();
        if(i==0) {
            params_add(client,resource,fd,1,0,16,0,0);
            assert(errors==1 && last_error==ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX && params.fd==-1);
            assert(fcntl(fd,F_GETFD)==-1 && errno==EBADF);
        } else {
            params_add(client,resource,fd,0,0,16,0,0);assert(!errors && params.fd==fd);
            if (i==1) {
                int second=plane_fd();params_add(client,resource,second,0,0,16,0,0);
                assert(errors==1 && last_error==ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET && params.fd==fd);
                assert(fcntl(second,F_GETFD)==-1 && errno==EBADF);
            } else {
                int w=4,h=4;uint32_t format=BROOK_DMABUF_XRGB8888,flags=0;
                if(i==2)w=0;
                if(i==3)h=-1;
                if(i==4)format=0;
                if(i==5)params.stride=15;
                if(i==6)params.offset=1;
                if(i==7)params.modifier=1;
                if(i==8)flags=1;
                if(i==9)params.used=1;
                if(i==10){close(params.fd);params.fd=-1;}
                unsigned before_info=errors;
                assert(!params_make_buffer(client,resource,0,w,h,format,flags));
                assert(errors==before_info+1);
                const uint32_t expected=i==2||i==3?ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS:
                    i==4||i==7||i==8?ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT:
                    i==5||i==6?ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS:
                    i==9?ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED:ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE;
                assert(last_error==expected);
                if(i!=9)assert(params.fd==-1);
                /* create, including a failed attempt, consumes the params. */
                errors=0;int extra=plane_fd();params_add(client,resource,extra,0,0,16,0,0);
                assert(errors==1 && last_error==ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED);
                assert(fcntl(extra,F_GETFD)==-1 && errno==EBADF);
            }
        }
        if(params.fd>=0)close(params.fd);
        wl_resource_destroy(resource);
    }
    wl_client_destroy(client);wl_display_destroy(display);close(sockets[1]);
    puts("DMABUF_PARAMS_PASS planes/duplicate/consumption/layout/flags/fd-ownership");
}
