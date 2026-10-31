#pragma once
#include "framebuffer_memory_type.h"
namespace brook {
struct MtrrPageObservation {uint8_t type=0;uint32_t matchedTypes=0;bool usedDefault=false;};
// Decode one above-1MiB page from a caller-supplied Intel MTRR snapshot. This
// neither reads registers nor combines the result with PAT or CR0.
inline bool MtrrObservePage(const FramebufferMtrrs& m,uint64_t page,MtrrPageObservation& out) {
    out={};
    if((page&4095) || page<(1ull<<20) || m.physicalBits<36 || m.physicalBits>52 ||
       m.count>32 || m.count!=(m.capability&255) || !(m.defaults&(1ull<<11)) ||
       (m.defaults&~(255ull|(1ull<<10)|(1ull<<11))) || !FramebufferMtrrTypeValid(m.defaults&255) ||
       ((m.defaults&255)==1 && !(m.capability&(1ull<<10))) || page>=(1ull<<m.physicalBits))return false;
    const uint64_t physicalMask=(1ull<<m.physicalBits)-1,addressMask=physicalMask&~4095ull;
    uint32_t types=0;
    for(unsigned i=0;i<m.count;++i) {
        if(!(m.mask[i]&(1ull<<11)))continue;
        const auto base=m.base[i],rawMask=m.mask[i];
        if((base&~(addressMask|255ull)) || (rawMask&~(addressMask|(1ull<<11))) ||
           !FramebufferMtrrTypeValid(base&255) || ((base&255)==1 && !(m.capability&(1ull<<10))))return false;
        const uint64_t mask=rawMask&addressMask,size=(~mask&physicalMask)+1;
        if(!size || (size&(size-1)) || mask!=(addressMask&~(size-1)) || ((base&addressMask)&(size-1)))return false;
        if((page&mask)==(base&mask))types|=1u<<(base&255);
    }
    if(!types){out.type=uint8_t(m.defaults&255);out.usedDefault=true;return true;}
    out.matchedTypes=types;
    if(types&1){out.type=0;return true;} // UC overrides overlapping ranges.
    if(types==((1u<<4)|(1u<<6))){out.type=4;return true;} // WT + WB.
    if(types&(types-1))return false;
    for(unsigned i=0;i<7;++i)if(types==(1u<<i)){out.type=uint8_t(i);return true;}
    return false;
}
}
