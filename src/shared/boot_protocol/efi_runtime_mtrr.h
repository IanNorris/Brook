#pragma once
#include <stdint.h>
namespace brook {
inline uint64_t RuntimeMtrrFingerprint(uint32_t physicalBits,uint64_t capability,uint64_t defaults,
                                       const uint64_t* base,const uint64_t* mask,const uint64_t* fixed,uint32_t count) {
    if(!base || !mask || !fixed || count>32 || count!=(capability&255) || physicalBits<36 || physicalBits>52)return 0;
    uint64_t hash=14695981039346656037ull;
    auto add=[&](uint64_t value){for(unsigned i=0;i<8;++i)hash=(hash^uint8_t(value>>(8*i)))*1099511628211ull;};
    add(physicalBits);add(capability);add(defaults);add(count);
    for(uint32_t i=0;i<count;++i){add(base[i]);add(mask[i]);}
    for(unsigned i=0;i<11;++i)add(fixed[i]);
    return hash;
}
}
