/* Two windows owned by this process: a native red window and a legacy blue
 * framebuffer. Sample only their overlapping pixel in completed partial
 * frames. The legacy source is explicitly opaque regardless of its alpha. */
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
#include <unistd.h>

struct window_out {uint32_t id,stride;uint64_t pixels;};
static int place(BrookWindowState *w,long x,long y){
    return syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)w->window,(long)w->incarnation,x,y,0L,0L) ||
           syscall(BROOK_SYS_WM_WINDOW_STATE,(long)w->window,(long)w->incarnation,(long)w,(long)sizeof(*w),0L,0L);
}
static int expect(const BrookWindowState *w,uint32_t color,unsigned *samples){
    for(unsigned attempt=0;attempt<120;++attempt){
        usleep(50000);long token=syscall(BROOK_SYS_DEBUG_FRAME,0L,0L,0L,0L,0L,0L);if(token<=0)continue;
        struct BrookDebugFrameInfoV3 info={0};
        for(unsigned poll=0;poll<40;++poll){
            if(syscall(BROOK_SYS_DEBUG_FRAME,5L,token,(long)&info,(long)sizeof(info),0L,0L) || info.v2.frame.state!=1)break;
            usleep(10000);
        }
        uint32_t pixel=0;int ok=0;int32_t x=w->client_x+80,y=w->client_y+60;
        if(info.v2.frame.state==2 && info.v2.source==BROOK_DEBUG_FRAME_SOURCE_LEASED &&
           info.presentation_mode==BROOK_DEBUG_FRAME_PRESENT_PARTIAL && x>=0 && y>=0 &&
           (uint32_t)x<info.v2.frame.width && (uint32_t)y<info.v2.frame.height){
            ++*samples;
            ok=syscall(BROOK_SYS_DEBUG_FRAME,2L,token,(long)&pixel,4L,
                (long)(((uint64_t)y*info.v2.frame.stride+(uint32_t)x)*4),0L)==4 && (pixel&0xffffffu)==color;
        }
        (void)syscall(BROOK_SYS_DEBUG_FRAME,3L,token,0L,0L,0L,0L);if(ok)return 0;
    }
    return -1;
}
static uint32_t blend(uint32_t below,uint32_t above,unsigned opacity){
    uint32_t out=0;for(unsigned shift=0;shift<24;shift+=8){
        unsigned a=(above>>shift)&255u,b=(below>>shift)&255u;
        out|=((a*opacity+b*(255u-opacity)+127u)/255u)<<shift;
    }return out;
}
int main(void){
    const uint32_t red=0xd02020u,blue=0x2060d0u;struct window_out lower={0};
    BrookWindowState lo={0},hi={0};struct fb_var_screeninfo var={0};
    int fd=-1,result=1;uint32_t *legacy=MAP_FAILED;size_t bytes=0;unsigned samples=0;const char *stage="native";
    if(syscall(506,160L,120L,(long)"Occlusion lower",(long)&lower,4L,0L) ||
       syscall(BROOK_SYS_WM_WINDOW_STATE,(long)lower.id,0L,(long)&lo,(long)sizeof(lo),0L,0L) ||
       place(&lo,350,250) || !lower.pixels || lower.stride<160)goto done;
    for(unsigned y=0;y<120;++y)for(unsigned x=0;x<160;++x)
        ((uint32_t *)(uintptr_t)lower.pixels)[y*lower.stride+x]=0xff000000u|red;
    if(syscall(508,(long)lower.id,0L,0L,0L,0L,0L))goto done;
    stage="legacy";fd=open("/dev/fb0",O_RDWR);
    if(fd<0 || ioctl(fd,FBIOGET_VSCREENINFO,&var) || var.xres!=640 || var.yres!=480)goto done;
    bytes=(size_t)var.xres*var.yres*4;legacy=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(legacy==MAP_FAILED)goto done;
    struct BrookDesktopLayers layers={.version=BROOK_DESKTOP_LAYERS_VERSION};
    if(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_QUERY,&layers,sizeof(layers),0L,0L,0L))goto done;
    for(unsigned i=0;i<layers.count;++i)if(layers.layers[i].window!=lower.id &&
        (layers.layers[i].flags&BROOK_DESKTOP_LAYER_LEGACY_OPAQUE) &&
        !syscall(BROOK_SYS_WM_WINDOW_STATE,(long)layers.layers[i].window,(long)layers.layers[i].incarnation,
                 (long)&hi,(long)sizeof(hi),0L,0L))break;
    if(!hi.window || place(&hi,280,180))goto done;
    for(size_t i=0;i<bytes/4;++i)legacy[i]=((uint32_t)i&255u)<<24|blue;
    char marker=0;if(write(fd,&marker,1)!=1)goto done;
    stage="covered";if(expect(&lo,blue,&samples))goto done;
    stage="translucent";if(syscall(0xB00,(long)hi.window,1L,128L,0L,0L,0L) || expect(&lo,blend(red,blue,128),&samples))goto done;
    stage="restore";if(syscall(0xB00,(long)hi.window,1L,255L,0L,0L,0L) || expect(&lo,blue,&samples))goto done;
    stage="expose";if(place(&hi,1000,200) || expect(&lo,red,&samples))goto done;
    result=0;
done:
    if(legacy!=MAP_FAILED)munmap(legacy,bytes);if(fd>=0)close(fd);
    if(lower.id)(void)syscall(507,(long)lower.id,0L,0L,0L,0L,0L);
    printf("%s samples=%u stage=%s\n",result?"OCCLUSION_PRESENT_FAIL":"OCCLUSION_PRESENT_PASS",samples,stage);return result;
}
