#define _GNU_SOURCE
#include "brook_debug_frame.h"
#include "wm_window_state.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct window_out {uint32_t id,stride;uint64_t pixels;};

int main(int argc,char **argv) {
    const int requireLeased=argc==2 && !strcmp(argv[1],"--require-leased");
    if(argc!=1 && !requireLeased)return 2;
    struct window_out window={0};
    if(syscall(506,160L,120L,(long)"Presentation test",(long)&window,4L,0L))return 1;
    int result=1;unsigned denied=0,ready=0,readError=0,mismatch=0,otherSource=0;BrookWindowState state={0};
    if(syscall(BROOK_SYS_WM_WINDOW_STATE,(long)window.id,0L,(long)&state,(long)sizeof(state),0L,0L) ||
       !window.pixels || window.stride<160)goto done;
    if(syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)window.id,(long)state.incarnation,200L,200L,0L,0L))goto done;
    uint32_t *pixels=(uint32_t *)(uintptr_t)window.pixels;
    for(unsigned y=0;y<120;++y)for(unsigned x=0;x<160;++x)pixels[y*window.stride+x]=0xff2468acu;
    if(syscall(508,(long)window.id,0L,0L,0L,0L,0L))goto done;
    if(syscall(BROOK_SYS_WM_WINDOW_STATE,(long)window.id,(long)state.incarnation,(long)&state,(long)sizeof(state),0L,0L))goto done;
    // A concurrent renderer can own the single capture slot. Retry for a
    // bounded interval and release every lease, including failed captures.
    for(unsigned attempt=0;attempt<80;++attempt){
        usleep(50000);
        long token=syscall(BROOK_SYS_DEBUG_FRAME,0L,0L,0L,0L,0L,0L);
        if(token<=0){++denied;continue;}
        struct BrookDebugFrameInfo info={0};struct BrookDebugFrameInfoV2 extended={0};
        for(unsigned poll=0;poll<40;++poll){
            if(requireLeased){
                if(syscall(BROOK_SYS_DEBUG_FRAME,4L,token,(long)&extended,(long)sizeof(extended),0L,0L))break;
                info=extended.frame;
            }else if(syscall(BROOK_SYS_DEBUG_FRAME,1L,token,(long)&info,(long)sizeof(info),0L,0L))break;
            if(info.state!=1)break;
            usleep(10000);
        }
        int32_t x=state.client_x+80,y=state.client_y+60;uint32_t pixel=0;
        if(info.state==2 && x>=0 && y>=0 && (uint32_t)x<info.width && (uint32_t)y<info.height &&
           info.stride>=info.width){
            ++ready;
            if(requireLeased && extended.source!=BROOK_DEBUG_FRAME_SOURCE_LEASED)++otherSource;
            else if(syscall(BROOK_SYS_DEBUG_FRAME,2L,token,(long)&pixel,4L,
                       (long)(((uint64_t)y*info.stride+(uint32_t)x)*4),0L)!=4)++readError;
            else if((pixel&0x00ffffffu)==0x002468acu)result=0;
            else ++mismatch;
        }
        (void)syscall(BROOK_SYS_DEBUG_FRAME,3L,token,0L,0L,0L,0L);
        if(!result)break;
    }
done:
    (void)syscall(507,(long)window.id,0L,0L,0L,0L,0L);
    printf("%s denied=%u ready=%u read_error=%u mismatch=%u other_source=%u\n",
           result?"DESKTOP_PRESENT_TEST_FAIL":"DESKTOP_PRESENT_TEST_PASS",denied,ready,readError,mismatch,otherSource);
    return result;
}
