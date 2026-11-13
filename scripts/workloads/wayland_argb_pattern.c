/* Static ARGB Wayland fixture for comparing completed desktop pixels. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

struct state {struct wl_compositor *compositor;struct wl_shm *shm;struct xdg_wm_base *wm;int configured;};
static void ping(void *data,struct xdg_wm_base *wm,uint32_t serial){(void)data;xdg_wm_base_pong(wm,serial);}
static const struct xdg_wm_base_listener wm_listener={.ping=ping};
static void global(void *data,struct wl_registry *reg,uint32_t id,const char *iface,uint32_t version){
    struct state *s=data;(void)version;
    if(!strcmp(iface,"wl_compositor"))s->compositor=wl_registry_bind(reg,id,&wl_compositor_interface,4);
    else if(!strcmp(iface,"wl_shm"))s->shm=wl_registry_bind(reg,id,&wl_shm_interface,1);
    else if(!strcmp(iface,"xdg_wm_base")){s->wm=wl_registry_bind(reg,id,&xdg_wm_base_interface,1);xdg_wm_base_add_listener(s->wm,&wm_listener,s);}
}
static void remove_global(void *data,struct wl_registry *reg,uint32_t id){(void)data;(void)reg;(void)id;}
static const struct wl_registry_listener registry_listener={global,remove_global};
static void configured(void *data,struct xdg_surface *surface,uint32_t serial){struct state *s=data;xdg_surface_ack_configure(surface,serial);s->configured=1;}
static const struct xdg_surface_listener surface_listener={configured};
static void top_configure(void *d,struct xdg_toplevel *t,int32_t w,int32_t h,struct wl_array *states){(void)d;(void)t;(void)w;(void)h;(void)states;}
static void top_close(void *d,struct xdg_toplevel *t){(void)d;(void)t;}
static const struct xdg_toplevel_listener top_listener={.configure=top_configure,.close=top_close};
int main(void){
    struct state s={0};struct wl_display *display=wl_display_connect(NULL);if(!display)return 1;
    struct wl_registry *registry=wl_display_get_registry(display);wl_registry_add_listener(registry,&registry_listener,&s);
    if(wl_display_roundtrip(display)<0 || !s.compositor || !s.shm || !s.wm)return 2;
    struct wl_surface *surface=wl_compositor_create_surface(s.compositor);
    struct xdg_surface *xdg=xdg_wm_base_get_xdg_surface(s.wm,surface);xdg_surface_add_listener(xdg,&surface_listener,&s);
    struct xdg_toplevel *top=xdg_surface_get_toplevel(xdg);xdg_toplevel_add_listener(top,&top_listener,&s);
    xdg_toplevel_set_title(top,"Brook ARGB validation");wl_surface_commit(surface);
    while(!s.configured)if(wl_display_dispatch(display)<0)return 3;
    const int width=320,height=240,stride=width*4,size=stride*height;
    char path[]="/tmp/brook-argb-pattern-XXXXXX";int fd=mkstemp(path);if(fd<0)return 4;unlink(path);
    if(ftruncate(fd,size))return 5;
    uint32_t *pixels=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(pixels==MAP_FAILED)return 6;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        const uint32_t colors[4]={0xffd02020u,0xff2060d0u,0x80800000u,0x00000000u};
        pixels[(size_t)y*width+x]=colors[(x>=width/2)+(y>=height/2)*2];
    }
    struct wl_shm_pool *pool=wl_shm_create_pool(s.shm,fd,size);struct wl_buffer *buffer=wl_shm_pool_create_buffer(pool,0,width,height,stride,WL_SHM_FORMAT_ARGB8888);
    wl_surface_set_buffer_scale(surface,2);wl_surface_attach(surface,buffer,0,0);wl_surface_damage(surface,0,0,160,120);wl_surface_commit(surface);
    if(wl_display_roundtrip(display)<0)return 7;
    puts("ARGB_PATTERN_READY");fflush(stdout);sleep(90);
    wl_buffer_destroy(buffer);wl_shm_pool_destroy(pool);munmap(pixels,size);close(fd);xdg_toplevel_destroy(top);xdg_surface_destroy(xdg);wl_surface_destroy(surface);wl_display_flush(display);wl_display_disconnect(display);
    puts("ARGB_PATTERN_DONE");return 0;
}
