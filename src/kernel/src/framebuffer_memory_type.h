#pragma once
#include <stdint.h>
namespace brook {
struct FramebufferMtrrs {
    uint64_t capability=0, defaults=0;
    uint64_t base[32]{}, mask[32]{}, fixed[11]{};
    unsigned physicalBits=0, count=0;
};
inline bool FramebufferMtrrTypeValid(unsigned type) {
    return type==0 || type==1 || type==4 || type==5 || type==6;
}
inline bool FramebufferMtrrsEqual(const FramebufferMtrrs& a,const FramebufferMtrrs& b) {
    if(a.capability!=b.capability || a.defaults!=b.defaults ||
       a.physicalBits!=b.physicalBits || a.count!=b.count) return false;
    for(unsigned i=0;i<a.count;++i)
        if(a.base[i]!=b.base[i] || a.mask[i]!=b.mask[i]) return false;
    for(unsigned i=0;i<11;++i) if(a.fixed[i]!=b.fixed[i]) return false;
    return true;
}
// First implementation targets Intel's documented PAT-WC + enabled MTRR table:
// all five defined MTRR types combine with PAT-WC to give WC. Refuse undefined
// overlaps; leave firmware MTRRs untouched. Framebuffer is above fixed ranges.
inline bool FramebufferMtrrsAllowWc(const FramebufferMtrrs& m,uint64_t begin,uint64_t end) {
    if(begin<(1ull<<20) || begin>=end || (begin&4095) || (end&4095) ||
       m.physicalBits<36 || m.physicalBits>52 || m.count>32 ||
       !(m.capability&(1ull<<10)) || !(m.defaults&(1ull<<11)) ||
       !FramebufferMtrrTypeValid(m.defaults&255)) return false;
    if(end>(1ull<<m.physicalBits)) return false;
    const uint64_t addressMask=((1ull<<m.physicalBits)-1)&~4095ull;
    for(unsigned i=0;i<m.count;++i) {
        if(!(m.mask[i]&(1ull<<11))) continue;
        const uint64_t mask=m.mask[i]&addressMask;
        const uint64_t size=(~mask&((1ull<<m.physicalBits)-1))+1;
        if(!size || (size&(size-1)) || (m.base[i]&addressMask)&(size-1) ||
           !FramebufferMtrrTypeValid(m.base[i]&255)) return false;
    }
    for(uint64_t page=begin;page<end;page+=4096) {
        unsigned types=0;
        for(unsigned i=0;i<m.count;++i) {
            if(!(m.mask[i]&(1ull<<11))) continue;
            const uint64_t mask=m.mask[i]&addressMask;
            if((page&mask)==(m.base[i]&mask)) types|=1u<<(m.base[i]&255);
        }
        if(types&1) continue; // UC wins any overlap
        if(types && (types&(types-1)) && types!=((1u<<4)|(1u<<6))) return false;
    }
    return true;
}
} // namespace brook
