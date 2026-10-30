#pragma once
#include <stdint.h>
namespace brook {
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
};
inline uint32_t XhciDmaChunk(uint64_t physical,uint32_t remaining) {
    const uint32_t boundary=65536-static_cast<uint32_t>(physical&65535);
    return remaining<boundary?remaining:boundary;
}
}
