#pragma once
#include <stdint.h>
#include "intel_bdw_render_wa.h"

namespace brook {
// Private RCS ring image. The caller must own and publish the ring, context,
// page tables and completion page, validate commands and maintain all DMA
// mappings until verified retirement or reset. PPGTT selection does not by
// itself establish command or memory isolation.
inline bool BdwBuildRenderRing(uint64_t batch,uint64_t completion,uint32_t nonce,
                               bool ppgtt,bool gt3,uint32_t (&ring)[56]) {
    const uint64_t limit=ppgtt?(uint64_t{1}<<48):(uint64_t{1}<<32);
    if((batch&7) || batch>=limit || !completion || (completion&0x27) ||
       completion>(uint64_t{1}<<32)-8 || !nonce)return false;
    constexpr uint32_t pc=0x7a000004,stall=1u<<20,tlb=1u<<18,qw=1u<<14,index=1u<<21;
    constexpr uint32_t invalidate=stall|tlb|qw|index|(1u<<11)|(1u<<10)|(1u<<4)|(1u<<3)|(1u<<2);
    constexpr uint32_t flush=stall|tlb|(1u<<12)|(1u<<5)|1u;
    constexpr uint32_t write=stall|qw|(1u<<24)|(1u<<7);
    const uint32_t initial[6]={pc,invalidate,0xd0,0,0,0};
    const uint32_t barrier[6]={pc,stall,0,0,0,0};
    uint32_t contextWa[20]{};BdwBuildRenderContextWa(gt3,contextWa);
    // Gen8: bit 8 selects the per-process graphics address space. The ring
    // itself remains privileged; it writes the breadcrumb via GGTT only.
    const uint32_t finish[18]={0x04000000,ppgtt?0x18800101u:0x18800001u,
        uint32_t(batch),uint32_t(batch>>32),pc,flush,0,0,0,0,
        pc,write,uint32_t(completion),0,nonce,0,0x04000001,0};
    unsigned cursor=0;
    for(auto word:initial)ring[cursor++]=word;
    for(auto word:barrier)ring[cursor++]=word;
    for(auto word:contextWa)ring[cursor++]=word;
    for(auto word:barrier)ring[cursor++]=word;
    for(auto word:finish)ring[cursor++]=word;
    return true;
}
}
