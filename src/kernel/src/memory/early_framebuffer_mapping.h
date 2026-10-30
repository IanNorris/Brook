#pragma once
#include <stdint.h>

namespace brook {

// Deliberately early-boot-only: known boot-created slot-0 huge leaves, BSP only.
// Neighbouring RAM/MMIO keeps its original attributes. No general live retyping.
struct EarlyFramebufferRange {
    uint64_t begin=0, end=0;
};
inline bool EarlyFramebufferBounds(uint64_t physical, uint32_t width,
                                   uint32_t height, uint32_t stride,
                                   EarlyFramebufferRange& range) {
    constexpr uint64_t limit=16ull<<30, maxBytes=256ull<<20;
    const uint64_t bytes=uint64_t(stride)*height;
    if(physical<(1ull<<20) || !width || !height || uint64_t(width)*4>stride ||
       (physical&3) || (stride&3) || !bytes || bytes>maxBytes ||
       physical>=limit || bytes>limit-physical) return false;
    range.begin=physical&~4095ull;
    range.end=(physical+bytes+4095)&~4095ull;
    return range.end<=limit;
}

// A newly-created alias must use the same type as every physical page it spans.
// Uniform WC is allowed; reject a mixed range rather than create WB/WC aliases.
inline bool FramebufferAliasCacheFlags(EarlyFramebufferRange wc, bool enabled,
                                       uint64_t physical,uint64_t bytes,uint64_t& flags) {
    flags=0;
    if(!bytes || bytes>~uint64_t{0}-physical) return false;
    const uint64_t end=physical+bytes;
    if(!enabled || physical>=wc.end || end<=wc.begin) return true;
    if(physical<wc.begin || end>wc.end) return false;
    flags=0x80; // 4KiB PAT slot4
    return true;
}

struct EarlyFramebufferSplit {
    uint64_t* slot=nullptr;
    uint64_t oldLeaf=0, tablePhysical=0;
};
struct EarlyFramebufferPlan {
    // At most 129 intersecting huge leaves per alias for a <=256 MiB span.
    EarlyFramebufferSplit splits[258]{};
    unsigned count=0;
    const char* failure="none";
};

// Io supplies table(physical), allocate(), release(), beginTransition(),
// endTransition(). All fallible work finishes before beginTransition().
template<class Io> bool MapEarlyFramebufferWc(Io& io, uint64_t* root,
                                              EarlyFramebufferRange range,
                                              EarlyFramebufferPlan& plan) {
    constexpr uint64_t physicalMask=0x000ffffffffff000ull, huge=1ull<<21;
    plan.count=0;plan.failure="none";
    auto fail=[&](const char* reason) {
        plan.failure=reason;
        while(plan.count) io.release(plan.splits[--plan.count].tablePhysical);
        return false;
    };
    if(!root || range.begin>=range.end || (range.begin&4095) || (range.end&4095) ||
       range.begin<(1ull<<20) || range.end>(16ull<<30) ||
       range.end-range.begin>(256ull<<20)+4096) return fail("bounds");
    for(unsigned aliasIndex=0;aliasIndex<2;++aliasIndex) {
        const unsigned alias=aliasIndex?256:0;
        const uint64_t pml4=root[alias];
        if((pml4&7)!=3 || (pml4&0x80)) return fail("missing alias");
        auto* pdpt=io.table(pml4&physicalMask);
        for(uint64_t chunk=range.begin&~(huge-1);chunk<range.end;chunk+=huge) {
            const uint64_t pdpte=pdpt[(chunk>>30)&511];
            if((pdpte&7)!=3 || (pdpte&0x80)) return fail("unsupported directory");
            auto* pd=io.table(pdpte&physicalMask);
            auto* slot=&pd[(chunk>>21)&511];
            const uint64_t old=*slot;
            // PAT bit12 belongs to flags in a huge leaf, NOT the address.
            // Reject all non-slot0 cache types rather than mis-converting them.
            if((old&0x87)!=0x83 || (old&0x18) || (old&physicalMask&(huge-1)) ||
               (old&physicalMask&~(huge-1))!=chunk)
                return fail("unsupported huge leaf");
            bool duplicate=false;
            for(unsigned i=0;i<plan.count;++i) if(plan.splits[i].slot==slot) duplicate=true;
            if(duplicate) continue;
            if(plan.count>=258) return fail("split capacity");
            const uint64_t page=io.allocate();
            if(!page) return fail("table allocation");
            plan.splits[plan.count++]={slot,old,page};
            if((page&4095) || (page>=range.begin && page<range.end)) return fail("table backing");
            auto* pt=io.table(page);
            const uint64_t flags=old&~physicalMask&~0x80ull;
            for(unsigned i=0;i<512;++i) {
                const uint64_t physical=chunk+uint64_t(i)*4096;
                pt[i]=physical|flags;
                if(physical>=range.begin && physical<range.end) pt[i]|=0x80; // PAT4=WC
            }
        }
    }
    io.beginTransition();
    for(unsigned i=0;i<plan.count;++i) {
        const auto& split=plan.splits[i];
        *split.slot=split.tablePhysical|3|(split.oldLeaf&4); // leaf retains W/NX/etc
    }
    io.endTransition();
    return true;
}
} // namespace brook
