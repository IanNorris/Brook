#pragma once
#include "intel_bdw_renderstate_data.h"

namespace brook {
// Populate one pinned private page with Intel's Gen8 null-render-state batch.
// Its four 64-bit relocations reference the batch's own GGTT allocation; this
// function builds RAM only and does not reserve, map, publish or submit it.
inline bool BdwBuildRenderNullState(uint64_t batchGgtt,uint32_t (&page)[1024]) {
    constexpr unsigned count=sizeof(gen8_null_state_batch)/sizeof(gen8_null_state_batch[0]);
    constexpr unsigned relocCount=sizeof(gen8_null_state_relocs)/sizeof(gen8_null_state_relocs[0]);
    static_assert(count<=1024 && relocCount==5,"unexpected upstream render state");
    if(!batchGgtt || (batchGgtt&4095) || batchGgtt>0xfffff000ull ||
       gen8_null_state_relocs[relocCount-1]!=0xffffffffu)return false;
    for(unsigned r=0;r+1<relocCount;++r){
        const uint32_t off=gen8_null_state_relocs[r];
        if((off&3) || off/4+1>=count || gen8_null_state_batch[off/4+1]!=0 ||
           (r && off<=gen8_null_state_relocs[r-1]) ||
           batchGgtt+gen8_null_state_batch[off/4]>0xffffffffull)return false;
    }
    for(unsigned i=0;i<1024;++i)page[i]=i<count?gen8_null_state_batch[i]:0;
    for(unsigned r=0;r+1<relocCount;++r){
        const unsigned i=gen8_null_state_relocs[r]/4;
        const uint64_t value=batchGgtt+page[i];page[i]=uint32_t(value);page[i+1]=uint32_t(value>>32);
    }
    return true;
}
}
