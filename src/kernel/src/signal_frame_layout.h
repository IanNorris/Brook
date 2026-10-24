#pragma once
#include <stdint.h>
namespace brook {
// Compute a function-entry-aligned frame without touching user memory. A
// successful layout is not proof of mapped/writable pages; CopyToUser owns that.
inline bool PlaceSignalFrame(uint64_t rsp,uint64_t altBase,uint64_t altSize,
                             bool useAlt,uint64_t frameBytes,uint64_t* result,
                             bool alreadyOnAlt=false) {
    constexpr uint64_t limit=0x0000800000000000ULL,minimum=0x10000;
    uint64_t top;
    if(useAlt || alreadyOnAlt) {
        if(altBase<minimum || altBase>=limit || altSize>limit-altBase)return false;
    }
    if(useAlt)top=altBase+altSize;
    else {
        if(rsp>=limit || rsp<128)return false;
        top=rsp-128;
    }
    if(!frameBytes || top<frameBytes)return false;
    const uint64_t aligned=(top-frameBytes)&~uint64_t{15};
    if(aligned<8)return false;
    const uint64_t address=aligned-8;
    if(address<minimum || address>=limit || frameBytes>limit-address ||
       ((useAlt || alreadyOnAlt) && (address<altBase ||
          address>altBase+altSize || frameBytes>altBase+altSize-address)))return false;
    *result=address;return true;
}
}
