#pragma once
#include <stdint.h>
namespace brook {
constexpr uint32_t XhciLinkControl(bool cycle,bool chain) {
    return (6u<<10)|2u|(chain?16u:0u)|(cycle?1u:0u);
}
template<class Trb> void XhciInitializeLink(Trb& link,uint64_t target) {
    link.param=target;link.status=0;
    link.control=XhciLinkControl(false,false); // producer/DCS initially one
}
// Hold the first ring element (which may be a Link) unowned until every
// element in the submission is initialized. Its cycle predates any wrap.
struct XhciTdPublication {
    uint32_t* first=nullptr;uint32_t control=0;
    void Stage(uint32_t& field,uint32_t value) {
        if(!first){first=&field;control=value;value^=1u;}
        __atomic_store_n(&field,value,__ATOMIC_RELEASE);
    }
    void Publish(){if(first)__atomic_store_n(first,control,__ATOMIC_RELEASE);}
};
// Caller serializes pump/lookup with an IRQ-safe lock. Preserve unrelated
// endpoint completions instead of overwriting one controller-wide flag.
template<class Event,unsigned Capacity=64> struct XhciCompletionQueue {
    Event entries[Capacity]{};bool used[Capacity]{};
    bool Push(const Event& event) {
        for(unsigned i=0;i<Capacity;++i)if(!used[i]){entries[i]=event;used[i]=true;return true;}
        return false;
    }
    bool Take(uint32_t type,uint64_t token,Event& result) {
        for(unsigned i=0;i<Capacity;++i)if(used[i] &&
            (!type || (entries[i].control&(63u<<10))==type) && (!token || entries[i].param==token)) {
            result=entries[i];used[i]=false;return true;
        }
        return false;
    }
    bool TakeAny(uint32_t type,const uint64_t* tokens,unsigned count,Event& result) {
        if(!tokens)return false;
        for(unsigned i=0;i<count;++i)if(Take(type,tokens[i],result))return true;
        return false;
    }
};
inline uint32_t XhciDmaChunk(uint64_t physical,uint32_t remaining) {
    const uint32_t boundary=65536-static_cast<uint32_t>(physical&65535);
    return remaining<boundary?remaining:boundary;
}
// xHCI >= 1.0, section 4.11.2.4: packets remaining after all complete
// packets described so far, including a packet split across TRBs.
constexpr uint32_t XhciTdPackets(uint32_t total,uint32_t after,uint32_t maxPacket) {
    if(!maxPacket || after>=total)return 0;
    const uint32_t left=(total+maxPacket-1)/maxPacket-after/maxPacket;
    return left>31?31:left;
}
}
