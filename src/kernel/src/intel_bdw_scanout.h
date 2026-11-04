#pragma once
#include <stdint.h>
namespace brook {
struct BdwGopPtePlan { uint64_t firstIndex=0,lastIndex=0,firstAddress=0,lastAddress=0; };
struct BootProtocol;
struct BdwGopObservation { bool inventory=false,planned=false;BdwGopPtePlan plan{}; };
void BdwGopObserveConfigure(const BootProtocol* boot);
const BdwGopObservation& BdwGopObserveRead();
// The extents supplied here must be independently verified by the caller.
// This performs arithmetic only and grants no MMIO or allocation rights.
inline bool BdwPlanGopPtes(uint64_t bar0,uint64_t bar0Bytes,uint64_t bar2,uint64_t bar2Bytes,
                           uint8_t msac,uint16_t ggc,uint64_t framebuffer,uint32_t width,
                           uint32_t height,uint32_t stride,BdwGopPtePlan& out) {
    constexpr uint64_t MiB=1024ull*1024, page=4096;
    if(!bar0 || !bar2 || (bar0&(16*MiB-1)) || (bar2&(256*MiB-1)) ||
       bar0Bytes!=16*MiB || bar2Bytes!=256*MiB || (msac&0x1f)!=1 || msac==0xff || ggc==0xffff ||
       ((ggc>>6)&3)!=3 || !width || !height || uint64_t(width)*4>stride || !framebuffer)return false;
    if(bar0>~uint64_t{0}-bar0Bytes || bar2>~uint64_t{0}-bar2Bytes || framebuffer<bar2)return false;
    if(bar0<bar2+bar2Bytes && bar2<bar0+bar0Bytes)return false;
    const uint64_t bytes=uint64_t(stride)*height;
    const uint64_t offset=framebuffer-bar2;
    if(!bytes || offset>=bar2Bytes || bytes>bar2Bytes-offset)return false;
    const uint64_t first=offset/page,last=(offset+bytes-1)/page;
    constexpr uint64_t gttOffset=8*MiB,gttSize=8*MiB;
    if(first>=gttSize/8 || last>=gttSize/8)return false;
    BdwGopPtePlan result{first,last,bar0+gttOffset+first*8,bar0+gttOffset+last*8};
    out=result;return true;
}
}
