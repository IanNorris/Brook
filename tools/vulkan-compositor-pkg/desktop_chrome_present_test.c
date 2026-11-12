/* Compare only this fixture window's decoration pixels in guest memory. */
#define _GNU_SOURCE
#include "desktop_layers_abi.h"
#include "brook_debug_frame.h"
#include "wm_window_state.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
struct window_out {uint32_t id,stride;uint64_t pixels;};
static int check_frame(const struct window_out *w,unsigned *samples,int requirePartial){
    for(unsigned attempt=0;attempt<120;++attempt){
        struct BrookDesktopLayers wire={.version=BROOK_DESKTOP_LAYERS_VERSION};wire.layers[0].window=w->id;
        if(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_QUERY,&wire,sizeof(wire),0,0,0)){usleep(50000);continue;}
        struct BrookDesktopLayer *l=&wire.layers[0];if(!l->bytes || l->width<8 || l->height<8)return -1;
        uint32_t *chrome=malloc(l->bytes);if(!chrome)return -1;l->pixels=(uintptr_t)chrome;l->capacity=l->bytes;
        if(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_CAPTURE,&wire,sizeof(wire),0,0,0)){free(chrome);usleep(50000);continue;}
        BrookWindowState state={0};if(syscall(BROOK_SYS_WM_WINDOW_STATE,(long)w->id,0L,(long)&state,(long)sizeof(state),0L,0L)){free(chrome);return -1;}
        int32_t left=state.client_x-l->clientX,top=state.client_y-l->clientY;
        int32_t right=left+(int32_t)state.width,bottom=top+(int32_t)state.height;
        if(left<1 || top<1 || right>=(int32_t)l->width || bottom>=(int32_t)l->height){free(chrome);return -1;}
        int32_t x[4]={left+(right-left)/2,left+(right-left)/2,left/2,right+((int32_t)l->width-right)/2};
        int32_t y[4]={top/2,bottom+((int32_t)l->height-bottom)/2,top+(bottom-top)/2,top+(bottom-top)/2};
        long token=syscall(BROOK_SYS_DEBUG_FRAME,0L,0L,0L,0L,0L,0L);if(token<=0){free(chrome);usleep(50000);continue;}
        struct BrookDebugFrameInfoV3 extended={0};struct BrookDebugFrameInfoV2 *info=&extended.v2;
        for(unsigned p=0;p<40;++p){
            long rc=requirePartial?syscall(BROOK_SYS_DEBUG_FRAME,5L,token,(long)&extended,(long)sizeof(extended),0L,0L):
                syscall(BROOK_SYS_DEBUG_FRAME,4L,token,(long)info,(long)sizeof(*info),0L,0L);
            if(rc || info->frame.state!=1)break;usleep(10000);}
        int good=info->frame.state==2 && info->source==BROOK_DEBUG_FRAME_SOURCE_LEASED &&
            (!requirePartial || extended.presentation_mode==BROOK_DEBUG_FRAME_PRESENT_PARTIAL);
        for(unsigned i=0;good && i<4;++i){int32_t px=l->clientX+x[i],py=l->clientY+y[i];uint32_t got=0;
            if(px<0 || py<0 || (uint32_t)px>=info->frame.width || (uint32_t)py>=info->frame.height ||
               (chrome[(uint64_t)y[i]*l->stride+x[i]]>>24)!=255 ||
               syscall(BROOK_SYS_DEBUG_FRAME,2L,token,(long)&got,4L,(long)(((uint64_t)py*info->frame.stride+(uint32_t)px)*4),0L)!=4 ||
               (got&0xffffffu)!=(chrome[(uint64_t)y[i]*l->stride+x[i]]&0xffffffu))good=0;
        }
        (void)syscall(BROOK_SYS_DEBUG_FRAME,3L,token,0L,0L,0L,0L);free(chrome);++*samples;
        if(good)return 0;usleep(50000);
    }
    return -1;
}
int main(int argc,char **argv){
    if(argc>2 || (argc==2 && strcmp(argv[1],"--require-partial")))return 2;
    const int requirePartial=argc==2;
    struct window_out w={0};BrookWindowState state={0};unsigned samples=0;int result=1;
    if(syscall(506,320L,200L,(long)"Chrome presentation fixture",(long)&w,4L,0L) ||
       syscall(BROOK_SYS_WM_WINDOW_STATE,(long)w.id,0L,(long)&state,(long)sizeof(state),0L,0L))goto done;
    for(unsigned i=0;i<3;++i){
        if(syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)w.id,(long)state.incarnation,250L+(long)i*120,180L+(long)i*80,0L,0L) ||
           check_frame(&w,&samples,requirePartial))goto done;
    }
    const unsigned widths[]={600,160,1000},heights[]={360,120,600};
    for(unsigned i=0;i<3;++i){
        struct window_out resized={0};
        if(syscall(511,(long)w.id,(long)widths[i],(long)heights[i],(long)&resized,0L,0L) ||
           resized.id!=w.id || !resized.pixels || resized.stride<widths[i])goto done;
        w=resized;
        if(check_frame(&w,&samples,requirePartial))goto done;
    }
    printf("CHROME_PRESENT_PASS positions=3 resizes=3 samples=%u require_partial=%d\n",samples,requirePartial);result=0;
done:
    if(w.id)(void)syscall(507,(long)w.id,0L,0L,0L,0L,0L);
    if(result)fprintf(stderr,"CHROME_PRESENT_FAIL samples=%u\n",samples);return result;
}
