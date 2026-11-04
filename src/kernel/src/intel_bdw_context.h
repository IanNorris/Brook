#pragma once
#include <stdint.h>
namespace brook {
// Fresh, restore-inhibited Broadwell non-render context in legacy 64-bit
// addressing mode. These are GPU-visible addresses; the caller must own,
// pin and publish the mappings and a valid populated PML4 before submission.
// The engine-wide HWS/CSB is separate from this per-context 8 KiB image.
struct BdwContext64 {
    uint64_t contextGgtt=0,ringGgtt=0,pml4Dma=0;
    uint32_t ringBytes=0,tailBytes=0,contextId=0;
};
inline bool BdwBuildBcsContext64(const BdwContext64& c,uint32_t (&image)[2048],uint64_t& descriptor) {
    constexpr uint64_t limit=uint64_t{1}<<32, dmaLimit=uint64_t{1}<<39;
    if((c.contextGgtt&4095) || c.contextGgtt>=limit || c.contextGgtt>limit-8192 ||
       (c.ringGgtt&4095) || c.ringGgtt>=limit || c.ringBytes<4096 || c.ringBytes>2*1024*1024 ||
       (c.ringBytes&(c.ringBytes-1)) || c.ringBytes>limit-c.ringGgtt ||
       (c.contextGgtt<c.ringGgtt+c.ringBytes && c.ringGgtt<c.contextGgtt+8192) ||
       !c.tailBytes || (c.tailBytes&7) || c.tailBytes>=c.ringBytes ||
       !c.pml4Dma || (c.pml4Dma&4095) || c.pml4Dma>=dmaLimit ||
       !c.contextId || c.contextId>0x1fffff)return false;
    // Full validation precedes output changes. Initial image is private RAM,
    // not an instruction to rewrite a live or saved context.
    for(auto& word:image)word=0;
    auto* reg=image+1024;
    constexpr uint32_t first[]={0x22244,0x22034,0x22030,0x22038,0x2203c,0x22168,0x22140,0x22110,0x2211c,0x22114,0x22118};
    constexpr uint32_t second[]={0x223a8,0x2228c,0x22288,0x22284,0x22280,0x2227c,0x22278,0x22274,0x22270};
    reg[1]=0x11000015;for(unsigned i=0;i<11;++i)reg[2+2*i]=first[i];
    reg[33]=0x11000011;for(unsigned i=0;i<9;++i)reg[34+2*i]=second[i];
    reg[65]=0x11000003;reg[66]=0x22200;reg[68]=0x22028;reg[70]=0x05000000;
    reg[3]=0x000f0009;reg[5]=0;reg[7]=c.tailBytes;reg[9]=uint32_t(c.ringGgtt);
    reg[11]=(c.ringBytes-4096)|1;reg[49]=uint32_t(c.pml4Dma>>32);reg[51]=uint32_t(c.pml4Dma);
    descriptor=(uint64_t(c.contextId)<<32)|c.contextGgtt|0x13d;
    return true;
}
}
