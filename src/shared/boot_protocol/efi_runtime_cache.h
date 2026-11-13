#pragma once
#include "boot_protocol.h"
namespace brook {
struct RuntimeCacheFingerprint { uint64_t hash=14695981039346656037ull;uint64_t pages=0;uint32_t slots=0;bool valid=false; };
// Fingerprints address/PAT-index pairs rather than memory-map capability bits.
// Probe must read page tables without touching the mapped runtime pages.
template<class Probe> RuntimeCacheFingerprint FingerprintRuntimeCache(
    const MemoryDescriptor* map,uint32_t count,Probe& probe) {
    RuntimeCacheFingerprint out{};
    if(!map || !count || count>4096)return out;
    auto add=[&](uint8_t byte){out.hash=(out.hash^byte)*1099511628211ull;};
    for(uint32_t i=0;i<count;++i){const auto& d=map[i];if(!(d._reserved&BootMemoryEfiRuntime))continue;
        if(!d.pageCount || (d.physicalStart&4095) || d.pageCount>(~uint64_t{0}-d.physicalStart)/4096 ||
           d.pageCount>65536-out.pages)return {};
        for(uint64_t j=0;j<d.pageCount;++j){const uint64_t address=d.physicalStart+j*4096;uint64_t physical=0;uint8_t pat=0;
            if(!probe(address,physical,pat) || physical!=address || pat>7)return {};
            for(unsigned b=0;b<8;++b)add(uint8_t(address>>(b*8)));
            add(pat);
            out.slots|=1u<<pat;++out.pages;
        }
    }
    out.valid=out.pages && out.slots;return out;
}
inline bool RuntimePatSlotsMatch(uint64_t original,uint64_t current,uint32_t slots) {
    if(!slots || (slots&~255u))return false;
    for(unsigned i=0;i<8;++i)if((slots&(1u<<i)) && uint8_t(original>>(i*8))!=uint8_t(current>>(i*8)))return false;
    return true;
}
}
