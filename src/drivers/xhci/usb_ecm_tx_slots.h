#pragma once
#include <stdint.h>

// Caller serializes slot allocation, publication and completion using the
// controller event-ring lock. A failed completion retains all DMA ownership.
struct UsbEcmTxSlots {
    static constexpr unsigned Capacity=16;
    struct Slot {uint64_t first,final;bool active,dataDone;} slots[Capacity];
    enum class Result { Unknown, Error, Intermediate, Finished };
    int Free(unsigned depth) const {
        if(depth>Capacity)return -1;
        for(unsigned i=0;i<depth;++i)if(!slots[i].active)return static_cast<int>(i);
        return -1;
    }
    // Caller supplies a free index and tokens unique among live transfers.
    void Reserve(unsigned index,uint64_t first,uint64_t final) {
        slots[index]={first,final,true,false};
    }
    Result Complete(uint64_t token,unsigned code,uint32_t residue) {
        for(auto& slot:slots)if(slot.active && (token==slot.first || token==slot.final)) {
            if(code!=1 || residue)return Result::Error;
            if(slot.first!=slot.final && token==slot.first) {
                if(slot.dataDone)return Result::Error;
                slot.dataDone=true;return Result::Intermediate;
            }
            if(slot.first!=slot.final && !slot.dataDone)return Result::Error;
            slot.active=false;return Result::Finished;
        }
        return Result::Unknown;
    }
};
