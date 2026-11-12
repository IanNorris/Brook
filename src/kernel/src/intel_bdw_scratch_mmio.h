#pragma once
#include "intel_bdw_scratch_takeover.h"
namespace brook {
// One PTE in an audited UC-mapped GGTT page and the audited flush-control
// register. Owner holds forcewake and the mapping lock, validates the pinned
// slot/page and provides a bounded, completed engine-TLB invalidation.
template<class Owner> class BdwScratchMmio {
    Owner& owner_;volatile uint64_t* pte_;volatile uint32_t* flush_;
    uint32_t index_;uint64_t dma_;
public:
    BdwScratchMmio(Owner& owner,volatile uint64_t* pte,volatile uint32_t* flush,uint32_t index,uint64_t dma)
        :owner_(owner),pte_(pte),flush_(flush),index_(index),dma_(dma){}
    BdwScratchMmio(const BdwScratchMmio&)=delete;
    BdwScratchMmio& operator=(const BdwScratchMmio&)=delete;
    bool admit(uint32_t index,uint64_t dma) {
        return pte_ && flush_ && !(reinterpret_cast<uintptr_t>(pte_)&7) &&
            !(reinterpret_cast<uintptr_t>(flush_)&3) && index==index_ && dma==dma_ && owner_.owned();
    }
    bool read(uint32_t index,uint64_t& value) {
        if(!admit(index,dma_))return false;
        __asm__ volatile("lfence":::"memory");value=*pte_;
        __asm__ volatile("lfence":::"memory");return owner_.owned();
    }
    bool write(uint32_t index,uint64_t value) {
        if(!admit(index,dma_) || value!=(dma_?(dma_|1):0))return false;
        *pte_=value;__asm__ volatile("mfence":::"memory");return owner_.owned();
    }
    bool invalidate() {
        if(!owner_.owned() || !flush_)return false;
        __asm__ volatile("mfence":::"memory");*flush_=1;
        __asm__ volatile("mfence":::"memory");const uint32_t posted=*flush_;
        __asm__ volatile("lfence":::"memory");
        return posted!=0xffffffffu && owner_.owned() && owner_.invalidateGpuTranslations();
    }
};
}
