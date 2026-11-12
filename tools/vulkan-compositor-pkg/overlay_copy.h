#pragma once
#include "brook_debug_frame.h"
#include "desktop_layers_abi.h"
#include <stdint.h>
#include <errno.h>
#include <unistd.h>

// Copy an immutable captured band into tightly packed rows. Full-width bands
// are contiguous in the snapshot and can use the existing bounded chunk API.
static int desktop_overlay_copy_band(long token,uint32_t index,void *destination,uint32_t width,uint32_t height,
                                     const struct BrookDesktopOverlayBand *b){
    if(!destination || !b || !b->width || !b->height || b->x>=width || b->y>=height ||
       b->width>width-b->x || b->height>height-b->y)return -1;
    uint64_t rowBytes=(uint64_t)b->width*4;
    if(rowBytes>BROOK_DEBUG_FRAME_CHUNK)return -1;
    const uint64_t packedBytes=rowBytes*b->height;
    long copied=syscall(BROOK_SYS_DESKTOP_OVERLAY,BROOK_DESKTOP_OVERLAY_READ_BAND,token,destination,packedBytes,index,0);
    if(copied==(long)packedBytes)return 0;
    if(copied>=0 || (errno!=EINVAL && errno!=ENOSYS))return -1;
    if(!b->x && b->width==width){
        uint64_t base=(uint64_t)b->y*width*4,total=rowBytes*b->height;
        for(uint64_t offset=0;offset<total;){
            uint64_t n=total-offset<BROOK_DEBUG_FRAME_CHUNK?total-offset:BROOK_DEBUG_FRAME_CHUNK;
            if(syscall(BROOK_SYS_DESKTOP_OVERLAY,2,token,(uint8_t*)destination+offset,n,base+offset,0)!=(long)n)return -1;
            offset+=n;
        }
    }else for(uint32_t row=0;row<b->height;++row){
        uint64_t offset=((uint64_t)(b->y+row)*width+b->x)*4;
        if(syscall(BROOK_SYS_DESKTOP_OVERLAY,2,token,(uint8_t*)destination+rowBytes*row,rowBytes,offset,0)!=(long)rowBytes)return -1;
    }
    return 0;
}
