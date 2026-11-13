/* Sample two fixture-owned pixels from the same leased completed frame. */
#define _GNU_SOURCE
#include "brook_debug_frame.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc,char **argv){
    if(argc!=7){fprintf(stderr,"usage: probe x1 y1 rgb1 x2 y2 rgb2\n");return 2;}
    unsigned long v[6];for(int i=0;i<6;++i){char *end=NULL;errno=0;v[i]=strtoul(argv[i+1],&end,0);if(errno || !end || *end || v[i]>0xfffffffful)return 2;}
    unsigned samples=0;
    for(unsigned attempt=0;attempt<120;++attempt){
        long token=syscall(BROOK_SYS_DEBUG_FRAME,0L,0L,0L,0L,0L,0L);if(token<=0){usleep(50000);continue;}
        struct BrookDebugFrameInfoV2 info={0};
        for(unsigned poll=0;poll<40;++poll){
            if(syscall(BROOK_SYS_DEBUG_FRAME,4L,token,(long)&info,(long)sizeof(info),0L,0L) || info.frame.state!=1)break;
            usleep(10000);
        }
        uint32_t pixels[2]={0};int ok=0;
        if(info.frame.state==2 && info.source==BROOK_DEBUG_FRAME_SOURCE_LEASED && info.frame.stride>=info.frame.width &&
           v[0]<info.frame.width && v[1]<info.frame.height && v[3]<info.frame.width && v[4]<info.frame.height){
            ++samples;int reads=0;
            for(unsigned i=0;i<2;++i){unsigned k=i*3;uint64_t offset=((uint64_t)v[k+1]*info.frame.stride+v[k])*4;
                reads+=syscall(BROOK_SYS_DEBUG_FRAME,2L,token,(long)&pixels[i],4L,(long)offset,0L)==4;}
            ok=reads==2 && (pixels[0]&0xffffffu)==(v[2]&0xffffffu) && (pixels[1]&0xffffffu)==(v[5]&0xffffffu);
        }
        (void)syscall(BROOK_SYS_DEBUG_FRAME,3L,token,0L,0L,0L,0L);
        if(ok){printf("PAIR_PIXEL_PASS samples=%u\n",samples);return 0;}usleep(50000);
    }
    printf("PAIR_PIXEL_FAIL samples=%u\n",samples);return 1;
}
