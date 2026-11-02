#pragma once
#include <stdint.h>

namespace brook {
// Apply a signed Linux seek offset without signed overflow or wraparound.
// File positions are limited to the representable positive off_t range.
inline bool SeekPosition(uint64_t base,int64_t delta,uint64_t& result) {
    constexpr uint64_t max=0x7fffffffffffffffULL;
    if(base>max)return false;
    if(delta<0) {
        const uint64_t magnitude=uint64_t(-(delta+1))+1;
        if(magnitude>base)return false;
        result=base-magnitude;
    } else {
        const uint64_t amount=uint64_t(delta);
        if(amount>max-base)return false;
        result=base+amount;
    }
    return true;
}
}
