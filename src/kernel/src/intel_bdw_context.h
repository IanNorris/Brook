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
inline bool BdwContext64Valid(const BdwContext64& c,uint64_t contextBytes) {
    constexpr uint64_t limit=uint64_t{1}<<32, dmaLimit=uint64_t{1}<<39;
    if(!contextBytes || contextBytes>limit || (c.contextGgtt&4095) || c.contextGgtt>=limit || c.contextGgtt>limit-contextBytes ||
       (c.ringGgtt&4095) || c.ringGgtt>=limit || c.ringBytes<4096 || c.ringBytes>2*1024*1024 ||
       (c.ringBytes&(c.ringBytes-1)) || c.ringBytes>limit-c.ringGgtt ||
       (c.contextGgtt<c.ringGgtt+c.ringBytes && c.ringGgtt<c.contextGgtt+contextBytes) ||
       !c.tailBytes || (c.tailBytes&7) || c.tailBytes>=c.ringBytes ||
       !c.pml4Dma || (c.pml4Dma&4095) || c.pml4Dma>=dmaLimit ||
       !c.contextId || c.contextId>0x1fffff)return false;
    return true;
}
inline bool BdwBuildBcsContext64(const BdwContext64& c,uint32_t (&image)[2048],uint64_t& descriptor) {
    if(!BdwContext64Valid(c,8192))return false;
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

// Initial private register template for the Gen8 render engine. A real
// backend must allocate/pin the full 20-page context and establish the GPU's
// render default state, workarounds, GGTT ownership, PPGTT and cache policy
// before this can be used. This does not grant or submit engine work.
inline bool BdwBuildRcsContext64(const BdwContext64& c,uint32_t (&image)[20*1024],uint64_t& descriptor) {
    if(!BdwContext64Valid(c,20*4096))return false;
    for(auto& word:image)word=0;
    auto* reg=image+1024;
    constexpr uint32_t first[]={0x2244,0x2034,0x2030,0x2038,0x203c,0x2168,0x2140,
                                0x2110,0x211c,0x2114,0x2118,0x21c0,0x21c4,0x21c8};
    constexpr uint32_t second[]={0x23a8,0x228c,0x2288,0x2284,0x2280,0x227c,0x2278,0x2274,0x2270};
    reg[1]=0x1100101b;for(unsigned i=0;i<14;++i)reg[2+2*i]=first[i];
    reg[33]=0x11001011;for(unsigned i=0;i<9;++i)reg[34+2*i]=second[i];
    reg[65]=0x11000001;reg[66]=0x20c8;reg[68]=0x05000000;
    reg[3]=0x000f0009;reg[5]=0;reg[7]=c.tailBytes;reg[9]=uint32_t(c.ringGgtt);
    reg[11]=(c.ringBytes-4096)|1;reg[49]=uint32_t(c.pml4Dma>>32);reg[51]=uint32_t(c.pml4Dma);
    descriptor=(uint64_t(c.contextId)<<32)|c.contextGgtt|0x13d;
    return true;
}

// Rebase a *verified, retired* Gen8 RCS default-context snapshot onto a new
// ring and PPGTT. This does not validate GPU execution, allocate or publish
// memory, or apply the separately owned indirect workaround batch. The caller
// must prove snapshot provenance and establish all of those guarantees. The
// existing context image is preserved except for the first status page and
// the kernel-controlled register offsets and per-context values.
inline bool BdwBuildRcsContextFromDefault(const BdwContext64& c,const uint32_t (&saved)[20*1024],
                                          uint32_t (&image)[20*1024],uint64_t& descriptor) {
    if(!BdwContext64Valid(c,20*4096))return false;
    const uintptr_t src=reinterpret_cast<uintptr_t>(saved),dst=reinterpret_cast<uintptr_t>(image);
    if(src>~uintptr_t{0}-sizeof(saved) || dst>~uintptr_t{0}-sizeof(image) ||
       (src<dst+sizeof(image) && dst<src+sizeof(saved)))return false;
    for(unsigned i=0;i<20*1024;++i)image[i]=i<1024?0:saved[i];
    auto* reg=image+1024;
    constexpr uint32_t first[]={0x2244,0x2034,0x2030,0x2038,0x203c,0x2168,0x2140,
                                0x2110,0x211c,0x2114,0x2118,0x21c0,0x21c4,0x21c8};
    constexpr uint32_t second[]={0x23a8,0x228c,0x2288,0x2284,0x2280,0x227c,0x2278,0x2274,0x2270};
    // Regenerate the command headers and register offsets, but retain saved
    // values except where they belong to the old context's address space.
    reg[1]=0x1100101b;for(unsigned i=0;i<14;++i)reg[2+2*i]=first[i];
    reg[33]=0x11001011;for(unsigned i=0;i<9;++i)reg[34+2*i]=second[i];
    reg[65]=0x11000001;reg[66]=0x20c8;
    reg[3]=0x000f0008; // context restore enabled for a saved default image
    reg[5]=0;reg[7]=c.tailBytes;reg[9]=uint32_t(c.ringGgtt);reg[11]=(c.ringBytes-4096)|1;
    reg[25]=0;reg[27]=0;reg[29]=0; // old per-context/indirect GGTT batch pointers
    reg[35]=0; // context timestamp
    for(unsigned i=37;i<=47;i+=2)reg[i]=0; // unused PDP1..PDP3
    reg[49]=uint32_t(c.pml4Dma>>32);reg[51]=uint32_t(c.pml4Dma);
    reg[0x59]=(reg[0x59]&~0x100u)|0x01000000u; // clear STOP_RING with write mask
    descriptor=(uint64_t(c.contextId)<<32)|c.contextGgtt|0x13d;
    return true;
}
}
