/* Owned legacy framebuffer: inspect only our known pixel in completed frames. */
#define _GNU_SOURCE
#include "brook_debug_frame.h"
#include "desktop_layers_abi.h"
#include "wm_window_state.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void){struct timespec t={0};return clock_gettime(CLOCK_MONOTONIC,&t)?0:(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static int expect(const BrookWindowState *w,uint32_t rgb,unsigned *samples){
    for(unsigned attempt=0;attempt<120;++attempt){
        usleep(50000);long token=syscall(BROOK_SYS_DEBUG_FRAME,0L,0L,0L,0L,0L,0L);if(token<=0)continue;
        struct BrookDebugFrameInfoV2 info={0};
        for(unsigned poll=0;poll<40;++poll){
            if(syscall(BROOK_SYS_DEBUG_FRAME,4L,token,(long)&info,(long)sizeof(info),0L,0L) || info.frame.state!=1)break;
            usleep(10000);
        }
        int32_t x=w->client_x+100,y=w->client_y+100;uint32_t pixel=0;int ok=0;
        if(info.frame.state==2 && info.source==BROOK_DEBUG_FRAME_SOURCE_LEASED && info.frame.stride>=info.frame.width &&
           x>=0 && y>=0 && (uint32_t)x<info.frame.width && (uint32_t)y<info.frame.height){
            ++*samples;
            ok=syscall(BROOK_SYS_DEBUG_FRAME,2L,token,(long)&pixel,4L,
                (long)(((uint64_t)y*info.frame.stride+(uint32_t)x)*4),0L)==4 && (pixel&0xffffffu)==rgb;
        }
        (void)syscall(BROOK_SYS_DEBUG_FRAME,3L,token,0L,0L,0L,0L);if(ok)return 0;
    }
    return -1;
}
int main(void){
    const uint32_t colors[]={0xd02020u,0x20c040u,0x2060d0u,0xb030c0u};
    int fd=-1,result=1;uint32_t *pixels=MAP_FAILED;size_t bytes=0;unsigned samples=0;const char *stage="open";
    BrookWindowState w={0};struct fb_var_screeninfo var={0};
    fd=open("/dev/fb0",O_RDWR);if(fd<0 || ioctl(fd,FBIOGET_VSCREENINFO,&var) || var.xres!=640 || var.yres!=480)goto done;
    bytes=(size_t)var.xres*var.yres*4;pixels=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(pixels==MAP_FAILED)goto done;
    stage="identity";
    for(unsigned id=1;id<=32;++id)if(!syscall(BROOK_SYS_WM_WINDOW_STATE,(long)id,0L,(long)&w,(long)sizeof(w),0L,0L))break;
    if(!w.window || !w.incarnation || syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)w.window,(long)w.incarnation,300L,200L,0L,0L) ||
       syscall(BROOK_SYS_WM_WINDOW_STATE,(long)w.window,(long)w.incarnation,(long)&w,(long)sizeof(w),0L,0L))goto done;
    struct BrookDesktopLayers layers={.version=BROOK_DESKTOP_LAYERS_VERSION};
    if(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_QUERY,&layers,sizeof(layers),0L,0L,0L))goto done;
    int legacy=0;for(unsigned i=0;i<layers.count;++i)if(layers.layers[i].window==w.window &&
        layers.layers[i].incarnation==w.incarnation && (layers.layers[i].flags&BROOK_DESKTOP_LAYER_LEGACY_OPAQUE))legacy=1;
    if(!legacy)goto done;
    stage="frames";
    for(unsigned pass=0;pass<sizeof(colors)/sizeof(colors[0]);++pass){
        for(size_t i=0;i<bytes/4;++i)pixels[i]=((uint32_t)(i*17u+pass*61u)&255u)<<24|colors[pass];
        uint64_t start=now_ns();char marker=0;if(write(fd,&marker,1)!=1 || expect(&w,colors[pass],&samples))goto done;
        uint64_t end=now_ns();printf("LEGACY_FRAME pass=%u elapsed_ms=%lu\n",pass,(unsigned long)((end-start)/1000000ull));
    }
    result=0;
done:
    if(pixels!=MAP_FAILED)munmap(pixels,bytes);if(fd>=0)close(fd);
    printf("%s samples=%u stage=%s\n",result?"LEGACY_PRESENT_FAIL":"LEGACY_PRESENT_PASS",samples,stage);return result;
}
