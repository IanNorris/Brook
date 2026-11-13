#define _GNU_SOURCE
#include "desktop_layers_abi.h"
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int call(unsigned op,struct BrookDesktopLayers *layers){
    return (int)syscall(BROOK_SYS_DESKTOP_LAYERS,op,layers,sizeof(*layers),0,0,0);
}
struct window_out {uint32_t id,stride;uint64_t pixels;};
int main(int argc,char **argv){
    int create=argc==2 && !strcmp(argv[1],"--create-window");
    if(argc!=1 && !create)return 2;
    struct window_out window={0};
    if(create){
        if(syscall(506,320L,200L,(long)"Desktop capture fixture",(long)&window,4L,0L)<0 || !window.pixels || window.stride<320)return 1;
        uint32_t *pixels=(uint32_t *)(uintptr_t)window.pixels;
        for(unsigned y=0;y<200;++y)for(unsigned x=0;x<320;++x)pixels[y*window.stride+x]=0xff000000u|(y<<8)|x;
        if(syscall(508,(long)window.id,0L,0L,0L,0L,0L)<0)return 1;
    }
    struct BrookDesktopLayers scene={.version=BROOK_DESKTOP_LAYERS_VERSION};
    if(call(BROOK_DESKTOP_LAYERS_QUERY,&scene)){
        fprintf(stderr,"DESKTOP_LAYERS_QUERY_FAIL errno=%d\n",errno);return errno==95?77:1;
    }
    if(scene.count>BROOK_DESKTOP_LAYER_LIMIT)return 1;
    fprintf(stderr,"DESKTOP_LAYERS_QUERY count=%u\n",scene.count);
    for(unsigned i=0;i<scene.count;++i){
        if(!scene.layers[i].bytes || scene.layers[i].bytes>64ull*1024*1024)return 1;
        void *pixels=malloc(scene.layers[i].bytes);if(!pixels)return 1;
        scene.layers[i].pixels=(uintptr_t)pixels;scene.layers[i].capacity=scene.layers[i].bytes;
    }
    if(call(BROOK_DESKTOP_LAYERS_CAPTURE,&scene)){
        fprintf(stderr,"DESKTOP_LAYERS_CAPTURE_FAIL errno=%d\n",errno);return errno==11?75:1;
    }
    uint64_t checksum=1469598103934665603ull;
    int verified=!create;
    for(unsigned i=0;i<scene.count;++i){
        const struct BrookDesktopLayer *l=&scene.layers[i];
        const unsigned char *p=(const void *)(uintptr_t)l->pixels;
        if(create && l->window==window.id){
            if(l->width!=320 || l->height!=200 || l->stride<320)return 1;
            const uint32_t *pixels=(const void *)p;
            for(unsigned y=0;y<200;++y)for(unsigned x=0;x<320;++x)
                if(pixels[y*l->stride+x]!=(0xff000000u|(y<<8)|x))return 1;
            verified=1;
        }
        for(uint64_t n=0;n<l->bytes;++n)checksum=(checksum^p[n])*1099511628211ull;
        fprintf(stderr,"DESKTOP_LAYER window=%u position=%d,%d size=%ux%u stride=%u bytes=%" PRIu64 " generation=%" PRIu64 "\n",
                l->window,l->clientX,l->clientY,l->width,l->height,l->stride,l->bytes,l->generation);
        free((void *)(uintptr_t)l->pixels);
    }
    if(!verified)return 1;
    if(create && syscall(507,(long)window.id,0L,0L,0L,0L,0L)<0)return 1;
    printf("DESKTOP_LAYERS_PASS count=%u checksum=%016" PRIx64 "\n",scene.count,checksum);return 0;
}
