#define _GNU_SOURCE
#include "brook_debug_frame.h"
#include "desktop_layers_abi.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Checks a captured full-width band against independent chunk reads, without
 * exporting its contents. Times only the packed operation on each lease. */
static uint64_t now_ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(void){
    for(unsigned sample=0;sample<8;++sample){
        long token=-1;
        for(unsigned i=0;i<100 && token<=0;++i){token=syscall(BROOK_SYS_DESKTOP_OVERLAY,0,0,0,0,0,0);if(token<=0)usleep(20000);}
        if(token<=0){fprintf(stderr,"overlay request failed %d\n",errno);return 1;}
        struct BrookDebugFrameInfo info={0};
        for(unsigned i=0;i<500;++i){if(syscall(BROOK_SYS_DESKTOP_OVERLAY,1,token,&info,sizeof(info),0,0))return 2;if(info.state!=1)break;usleep(10000);}
        struct BrookDesktopOverlayBands bands={0};
        if(info.state!=2 || syscall(BROOK_SYS_DESKTOP_OVERLAY,4,token,&bands,sizeof(bands),0,0))return 3;
        unsigned index=0;while(index<bands.count && (bands.bands[index].x || bands.bands[index].width!=info.width))++index;
        if(index==bands.count)return 4;
        const struct BrookDesktopOverlayBand *b=&bands.bands[index];const size_t bytes=(size_t)b->width*b->height*4;
        uint8_t *actual=malloc(bytes),*reference=malloc(bytes);if(!actual||!reference)return 5;
        const uint64_t start=now_ns();long n=syscall(BROOK_SYS_DESKTOP_OVERLAY,BROOK_DESKTOP_OVERLAY_READ_BAND,token,actual,bytes,index,0);const uint64_t end=now_ns();
        if(n!=(long)bytes || !start || end<start)return 6;
        for(size_t pos=0;pos<bytes;){size_t chunk=bytes-pos<BROOK_DEBUG_FRAME_CHUNK?bytes-pos:BROOK_DEBUG_FRAME_CHUNK;
            if(syscall(BROOK_SYS_DESKTOP_OVERLAY,2,token,reference+pos,chunk,(uint64_t)b->y*info.width*4+pos,0)!=(long)chunk)return 7;
            pos+=chunk;}
        if(memcmp(actual,reference,bytes))return 8;
        if(syscall(BROOK_SYS_DESKTOP_OVERLAY,3,token,0,0,0,0))return 9;
        free(actual);free(reference);printf("OVERLAY_BAND_SAMPLE bytes=%zu ms=%llu\n",bytes,(unsigned long long)((end-start)/1000000ull));
    }
    puts("OVERLAY_BAND_PROBE_PASS");return 0;
}
