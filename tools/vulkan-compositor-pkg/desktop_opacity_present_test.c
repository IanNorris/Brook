/* Read only two pixels from each in-guest completed frame: no frame export.
 * Wait for convergence, since a completed frame may be one frame behind. */
#define _GNU_SOURCE
#include "brook_debug_frame.h"
#include "wm_window_state.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
struct window_out {uint32_t id,stride;uint64_t pixels;};
static int fill(const struct window_out *w,uint32_t color){
    if(!w->pixels || w->stride<160)return -1;
    uint32_t *pixels=(uint32_t *)(uintptr_t)w->pixels;
    for(unsigned y=0;y<120;++y)for(unsigned x=0;x<160;++x)pixels[y*w->stride+x]=color;
    return (int)syscall(508,(long)w->id,0L,0L,0L,0L,0L);
}
static int place(const struct window_out *w,BrookWindowState *state,long x,long y){
    if(syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)w->id,(long)state->incarnation,x,y,0L,0L))return -1;
    return (int)syscall(BROOK_SYS_WM_WINDOW_STATE,(long)w->id,(long)state->incarnation,(long)state,(long)sizeof(*state),0L,0L);
}
static int expect_pair(const BrookWindowState *a,uint32_t ca,uint32_t cb,unsigned *samples,int requirePartial){
    for(unsigned attempt=0;attempt<120;++attempt){
        usleep(50000);long token=syscall(BROOK_SYS_DEBUG_FRAME,0L,0L,0L,0L,0L,0L);if(token<=0)continue;
        struct BrookDebugFrameInfoV3 extended={0};struct BrookDebugFrameInfoV2 *info=&extended.v2;
        for(unsigned poll=0;poll<40;++poll){
            long rc=requirePartial?syscall(BROOK_SYS_DEBUG_FRAME,5L,token,(long)&extended,(long)sizeof(extended),0L,0L):
                syscall(BROOK_SYS_DEBUG_FRAME,4L,token,(long)info,(long)sizeof(*info),0L,0L);
            if(rc || info->frame.state!=1)break;
            usleep(10000);
        }
        uint32_t pixels[2]={0,0};int ok=0;
        const int32_t x[2]={a->client_x+20,a->client_x+120},y[2]={a->client_y+60,a->client_y+60};
        if(info->frame.state==2 && info->source==BROOK_DEBUG_FRAME_SOURCE_LEASED &&
           (!requirePartial || extended.presentation_mode==BROOK_DEBUG_FRAME_PRESENT_PARTIAL) && info->frame.stride>=info->frame.width &&
           x[0]>=0 && y[0]>=0 && x[1]>=0 && y[1]>=0 && (uint32_t)x[0]<info->frame.width &&
           (uint32_t)x[1]<info->frame.width && (uint32_t)y[0]<info->frame.height && (uint32_t)y[1]<info->frame.height){
            ++*samples;
            int reads=0;for(unsigned i=0;i<2;++i)
                reads+=syscall(BROOK_SYS_DEBUG_FRAME,2L,token,(long)&pixels[i],4L,
                    (long)(((uint64_t)y[i]*info->frame.stride+(uint32_t)x[i])*4),0L)==4;
            ok=reads==2 && (pixels[0]&0xffffffu)==(ca&0xffffffu) && (pixels[1]&0xffffffu)==(cb&0xffffffu);
        }
        (void)syscall(BROOK_SYS_DEBUG_FRAME,3L,token,0L,0L,0L,0L);if(ok)return 0;
    }
    return -1;
}
static uint32_t blended(uint32_t below,uint32_t above,unsigned opacity){
    uint32_t out=0xff000000u;
    for(unsigned shift=0;shift<24;shift+=8){
        unsigned a=(above>>shift)&255u,b=(below>>shift)&255u;
        out|=((a*opacity+b*(255u-opacity)+127u)/255u)<<shift;
    }
    return out;
}
static int opacity(const struct window_out *w,unsigned value){
    return (int)syscall(0xB00,(long)w->id,1L,(long)value,0L,0L,0L);
}
static int blur(const struct window_out *w,unsigned value){
    return (int)syscall(0xB00,(long)w->id,2L,0L,(long)value,0L,0L);
}
/* Source metadata only; no unrelated desktop pixels leave the guest. */
static int expect_source(unsigned wanted){
    for(unsigned attempt=0;attempt<120;++attempt){
        usleep(50000);long token=syscall(BROOK_SYS_DEBUG_FRAME,0L,0L,0L,0L,0L,0L);if(token<=0)continue;
        struct BrookDebugFrameInfoV2 info={0};
        for(unsigned poll=0;poll<40;++poll){
            if(syscall(BROOK_SYS_DEBUG_FRAME,4L,token,(long)&info,(long)sizeof(info),0L,0L) || info.frame.state!=1)break;
            usleep(10000);
        }
        (void)syscall(BROOK_SYS_DEBUG_FRAME,3L,token,0L,0L,0L,0L);
        if(info.frame.state==2 && info.source==wanted)return 0;
    }
    return -1;
}
int main(int argc,char **argv){
    if(argc>2 || (argc==2 && strcmp(argv[1],"--require-partial")))return 2;
    const int requirePartial=argc==2;
    const uint32_t red=0xffd02020u,blue=0xff2060d0u;
    struct window_out a={0},b={0};BrookWindowState sa={0},sb={0};unsigned samples=0;int result=1;const char *stage="create";
    if(syscall(506,160L,120L,(long)"Opacity fixture",(long)&a,4L,0L) ||
       syscall(506,160L,120L,(long)"Opacity fixture",(long)&b,4L,0L))goto done;
    if(a.id==b.id || syscall(BROOK_SYS_WM_WINDOW_STATE,(long)a.id,0L,(long)&sa,(long)sizeof(sa),0L,0L) ||
       syscall(BROOK_SYS_WM_WINDOW_STATE,(long)b.id,0L,(long)&sb,(long)sizeof(sb),0L,0L) ||
       !sa.incarnation || !sb.incarnation || sa.incarnation==sb.incarnation)goto done;
    stage="opaque";
    if(place(&a,&sa,300,200) || place(&b,&sb,380,200) || fill(&a,red) || fill(&b,blue) ||
       expect_pair(&sa,red,blue,&samples,requirePartial))goto done;
    stage="translucent";
    if(opacity(&b,128) || expect_pair(&sa,red,blended(red,blue,128),&samples,requirePartial))goto done;
    stage="transparent";
    if(opacity(&b,0) || expect_pair(&sa,red,red,&samples,requirePartial))goto done;
    stage="restore";
    if(opacity(&b,255) || expect_pair(&sa,red,blue,&samples,requirePartial))goto done;
    stage="move";
    if(place(&b,&sb,900,200) || expect_pair(&sa,red,red,&samples,requirePartial))goto done;
    stage="fallback";
    if(blur(&b,1) || expect_source(BROOK_DEBUG_FRAME_SOURCE_FRONT))goto done;
    stage="resume";
    if(blur(&b,0) || expect_pair(&sa,red,red,&samples,requirePartial))goto done;
    result=0;
done:
    if(b.id)(void)blur(&b,0);
    if(a.id)(void)syscall(507,(long)a.id,0L,0L,0L,0L,0L);
    if(b.id)(void)syscall(507,(long)b.id,0L,0L,0L,0L,0L);
    printf("%s samples=%u stage=%s require_partial=%d\n",result?"OPACITY_PRESENT_FAIL":"OPACITY_PRESENT_PASS",samples,stage,requirePartial);return result;
}
