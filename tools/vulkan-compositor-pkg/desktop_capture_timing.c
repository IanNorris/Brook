/* Timed desktop capture using a fixture-owned surface. Never exports pixels. */
#define _GNU_SOURCE
#include "desktop_layers_abi.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
struct window_out {uint32_t id,stride;uint64_t pixels;};
static uint64_t ns(void){struct timespec t={0};return clock_gettime(CLOCK_MONOTONIC,&t)?0:(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(void){
    struct window_out w={0};struct BrookDesktopLayers scene={.version=BROOK_DESKTOP_LAYERS_VERSION};
    int result=1;const char *stage="create";unsigned completed=0,retries=0;
    if(syscall(506,1600L,900L,(long)"Capture timing fixture",(long)&w,4L,0L) || !w.pixels || w.stride<1600)goto done;
    uint32_t *pixels=(void *)(uintptr_t)w.pixels;
    for(unsigned y=0;y<900;++y)for(unsigned x=0;x<1600;++x)pixels[(size_t)y*w.stride+x]=0xff000000u|((y&255u)<<8)|(x&255u);
    if(syscall(508,(long)w.id,0L,0L,0L,0L,0L))goto done;
    stage="query";
    if(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_QUERY,&scene,sizeof(scene),0L,0L,0L) || scene.count>BROOK_DESKTOP_LAYER_LIMIT)goto done;
    for(unsigned i=0;i<scene.count;++i){
        if(!scene.layers[i].bytes || scene.layers[i].bytes>64ull*1024*1024)goto done;
        void *p=malloc(scene.layers[i].bytes);if(!p)goto done;
        scene.layers[i].pixels=(uintptr_t)p;scene.layers[i].capacity=scene.layers[i].bytes;
    }
    stage="capture";
    for(unsigned i=0;i<20 && completed<8;++i){
        uint64_t start=ns();int rc=(int)syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_CAPTURE,&scene,sizeof(scene),0L,0L,0L);uint64_t end=ns();
        if(rc){if(errno==EAGAIN){++retries;continue;}goto done;}
        int found=0;for(unsigned j=0;j<scene.count;++j)if(scene.layers[j].window==w.id){
            const struct BrookDesktopLayer *l=&scene.layers[j];const uint32_t *p=(const void *)(uintptr_t)l->pixels;
            if(l->width!=1600 || l->height!=900 || l->stride<1600 || p[100*l->stride+100]!=0xff006464u ||
               p[899*l->stride+1599]!=(0xff000000u|((899u&255u)<<8)|(1599u&255u)))goto done;
            found=1;break;
        }
        if(!found || !start || end<start)goto done;
        printf("CAPTURE_TIMING sample=%u us=%lu\n",completed,(unsigned long)((end-start)/1000));++completed;
    }
    result=completed==8?0:1;
done:
    for(unsigned i=0;i<scene.count && i<BROOK_DESKTOP_LAYER_LIMIT;++i)free((void *)(uintptr_t)scene.layers[i].pixels);
    if(w.id)(void)syscall(507,(long)w.id,0L,0L,0L,0L,0L);
    printf("%s completed=%u retries=%u stage=%s\n",result?"CAPTURE_TIMING_FAIL":"CAPTURE_TIMING_PASS",completed,retries,stage);return result;
}
