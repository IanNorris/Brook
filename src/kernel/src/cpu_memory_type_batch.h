#pragma once
#include <stdint.h>
#include "cpu.h"
#include "framebuffer_memory_type.h"
#include "mtrr_page_probe.h"

namespace brook {
// Batched form of CpuObserveMemoryTypes(page,page) for the GPU critical-section
// audits. The single-page call reads the whole MTRR/PAT/CR0 configuration
// twice per page (about 70 MSR reads and 12 CPUIDs); an audit of 13 MMIO pages
// repeated that 13 times on every GPU-wait yield with interrupts off.
//
// Guarantee is unchanged: one configuration snapshot is read before decoding
// and again after, both must be identical (MTRRs, PAT, CR0.CD/NW), and every
// page's result is MtrrObservePage over that stable snapshot, exactly what the
// single call computes for that page. Any failure returns false and the caller
// falls back to the per-page call.
//
// Hw provides: cr0(), pat(), cpu(), mtrrs(FramebufferMtrrs&) -> bool.
template<class Hw>
bool CpuObserveMemoryTypesBatchWith(Hw& hw,const uint64_t* pages,unsigned count,CpuMemoryTypeObservation* out) {
    if(!pages || !out || !count)return false;
    constexpr uint64_t cacheBits=(1ull<<30)|(1ull<<29);
    FramebufferMtrrs before{},after{};
    const uint64_t cr0=hw.cr0();
    if(!hw.mtrrs(before))return false;
    const uint64_t pat=hw.pat();
    const uint32_t cpu=hw.cpu();
    if(!hw.mtrrs(after) || !FramebufferMtrrsEqual(before,after) ||
       ((cr0^hw.cr0())&cacheBits) || hw.pat()!=pat)return false;
    for(unsigned i=0;i<count;++i){
        CpuMemoryTypeObservation r{};
        r.cpu=cpu;r.patSlot0=uint8_t(pat&255);
        r.cacheDisabled=(cr0&(1ull<<30))!=0;r.notWriteThrough=(cr0&(1ull<<29))!=0;
        if(!MtrrObservePage(before,pages[i],r.page0) || !MtrrObservePage(before,pages[i],r.page1))return false;
        out[i]=r;
    }
    return true;
}
}
// Kernel implementation (cpu.cpp): interrupts disabled for the whole window.
bool CpuObserveMemoryTypesBatch(const uint64_t* pages,unsigned count,brook::CpuMemoryTypeObservation* out);
