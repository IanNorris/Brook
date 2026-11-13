#pragma once
#include "intel_bdw_context.h"
#include "intel_bdw_ggtt_bind.h"
#include "intel_bdw_render_wa.h"

namespace brook {
// Planning only. A backend must pin these pages, reserve the entire GGTT
// range, verify DMA/IOMMU access and PAT, initialize render workarounds and
// default state, publish caches and prove retirement before releasing them.
// This function does not establish any of those ownership properties.
struct BdwRenderPlan {
    static constexpr uint32_t ContextPages=20,RingPages=4,PageCount=28;
    BdwGgttLease lease{};
    uint64_t descriptor=0,hwsGgtt=0,completionGgtt=0,waGgtt=0,waScratchGgtt=0;
};

inline bool BdwBuildRenderPlan(uint32_t firstIndex,uint64_t token,uint32_t contextId,
                               uint32_t tailBytes,uint64_t pml4Dma,
                               const uint64_t (&dma)[BdwRenderPlan::PageCount],
                               const uint64_t (&expected)[BdwRenderPlan::PageCount],
                               uint32_t (&context)[20*1024],uint32_t (&wa)[1024],BdwRenderPlan& out,
                               const uint32_t (*saved)[20*1024]=nullptr) {
    constexpr uint64_t dmaLimit=uint64_t{1}<<39,rmrrStart=0xdd000000ull,rmrrEnd=0xdf800000ull;
    constexpr uint32_t count=BdwRenderPlan::PageCount;
    const uintptr_t contextStart=reinterpret_cast<uintptr_t>(context),waStart=reinterpret_cast<uintptr_t>(wa),outStart=reinterpret_cast<uintptr_t>(&out);
    if(contextStart>~uintptr_t{0}-sizeof(context) || waStart>~uintptr_t{0}-sizeof(wa) || outStart>~uintptr_t{0}-sizeof(out) ||
       (contextStart<outStart+sizeof(out) && outStart<contextStart+sizeof(context)) ||
       (contextStart<waStart+sizeof(wa) && waStart<contextStart+sizeof(context)) ||
       (waStart<outStart+sizeof(out) && outStart<waStart+sizeof(wa)))return false;
    if(!token || firstIndex<65536 || firstIndex>1048576-count || !pml4Dma ||
       (pml4Dma&4095) || pml4Dma>=dmaLimit || (pml4Dma<rmrrEnd && pml4Dma+4096>rmrrStart))return false;
    BdwRenderPlan next{};next.lease.token=token;next.lease.count=count;
    for(uint32_t i=0;i<count;++i) {
        if(!dma[i] || (dma[i]&4095) || dma[i]>=dmaLimit || dma[i]==pml4Dma ||
           (dma[i]<rmrrEnd && dma[i]+4096>rmrrStart))return false;
        for(uint32_t j=0;j<i;++j)if(dma[i]==dma[j])return false;
        next.lease.index[i]=firstIndex+i;next.lease.dma[i]=dma[i];next.lease.expected[i]=expected[i];
    }
    const uint64_t base=uint64_t(firstIndex)*4096;
    BdwContext64 c{base,base+BdwRenderPlan::ContextPages*4096ull,pml4Dma,
                   BdwRenderPlan::RingPages*4096,tailBytes,contextId};
    if(!BdwContext64Valid(c,BdwRenderPlan::ContextPages*4096ull))return false;
    uint64_t descriptor=0;
    if(saved ? !BdwBuildRcsContextFromDefault(c,*saved,context,descriptor) : !BdwBuildRcsContext64(c,context,descriptor))return false;
    const uint64_t waAddress=base+26*4096ull,scratchAddress=base+27*4096ull;
    if(!BdwBuildRenderIndirectWa(c,waAddress,scratchAddress,context,wa))return false;
    next.descriptor=descriptor;next.hwsGgtt=base+24*4096ull;next.completionGgtt=base+25*4096ull;
    next.waGgtt=waAddress;next.waScratchGgtt=scratchAddress;
    out=next;return true;
}
}
