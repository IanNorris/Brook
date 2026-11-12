#pragma once
#include "intel_bdw_ggtt_bind.h"
namespace brook {
// Accessor for one already-audited, uncached GGTT PTE page and the Broadwell
// graphics flush-control register. The owner must hold the VMM mapping lock,
// exclusive GPU/forcewake ownership, and the slot/page pins for the complete
// binding and retirement transaction. This class does not create those rights.
// A read-only owner may provide readable() while refusing owned(), lease
// validation and all writes; the deployed diagnostic uses only that mode.
// flushControl is BAR0+0x101008; ptes points to the first entry of the
// audited 4 KiB PTE page. The owner validates their physical addresses and
// UC memory types before construction. No instance is currently published.
template<class Owner> class BdwGgttMmio {
    Owner& owner_;
    volatile uint64_t* ptes_;
    volatile uint32_t* flushControl_;
    uint32_t first_,count_;
    bool valid()const{return ptes_ && flushControl_ && !(reinterpret_cast<uintptr_t>(ptes_)&7) &&
        !(reinterpret_cast<uintptr_t>(flushControl_)&3) && count_ && count_<=BdwGgttLease::MaximumPages &&
        first_>=65536 && first_<=1048576-count_ && (first_&511)+count_<=512;}
    bool inRange(uint32_t index)const{return valid() && index>=first_ && index-first_<count_;}
public:
    BdwGgttMmio(Owner& owner,volatile uint64_t* ptes,volatile uint32_t* flushControl,
                uint32_t first,uint32_t count)
        :owner_(owner),ptes_(ptes),flushControl_(flushControl),first_(first),count_(count){}
    BdwGgttMmio(const BdwGgttMmio&)=delete;
    BdwGgttMmio& operator=(const BdwGgttMmio&)=delete;
    bool validateLease(const BdwGgttLease& lease){
        if(!valid() || lease.count!=count_)return false;
        for(unsigned i=0;i<lease.count;++i)if(lease.index[i]!=first_+i)return false;
        return owner_.validateLease(lease);
    }
    bool validateTakeover(const BdwGgttLease& lease,uint64_t scratch){
        return validateLease(lease) && owner_.validateTakeover(lease,scratch);
    }
    bool quiescent(){return owner_.quiescent();}
    bool read(uint32_t index,uint64_t& value){
        if(!inRange(index) || !owner_.readable())return false;
        __asm__ volatile("lfence":::"memory");
        value=ptes_[index&511];
        __asm__ volatile("lfence":::"memory");return value!=~uint64_t{0} && owner_.readable();
    }
    bool write(uint32_t index,uint64_t value){
        if(!inRange(index) || !owner_.owned())return false;
        ptes_[index&511]=value;return owner_.owned();
    }
    bool invalidate(){
        if(!valid() || !owner_.owned())return false;
        // Order and post the PTE stores, then require the owner to invalidate
        // the device translations. The flush-control posting read alone is
        // not proof that engine TLBs or other required caches were cleared.
        __asm__ volatile("mfence":::"memory");
        *flushControl_=1;
        __asm__ volatile("mfence":::"memory");
        const uint32_t posted=*flushControl_;
        __asm__ volatile("lfence":::"memory");
        return posted!=0xffffffffu && owner_.owned() && owner_.invalidateGpuTranslations();
    }
};
}
