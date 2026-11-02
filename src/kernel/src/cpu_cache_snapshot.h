#pragma once
#include "mtrr_page_probe.h"

namespace brook {
// A single CPU's boot-time cache configuration; does not validate mappings or
// establish that these registers stay unchanged after the snapshot.
struct CpuCacheSnapshot {
    FramebufferMtrrs mtrrs{};
    uint64_t pat=0;
    bool valid=false,cacheDisabled=false,notWriteThrough=false;
};

inline bool CpuCacheSnapshotsMatch(const CpuCacheSnapshot& a,const CpuCacheSnapshot& b) {
    return a.valid && b.valid && !a.cacheDisabled && !b.cacheDisabled &&
        !a.notWriteThrough && !b.notWriteThrough && a.pat==b.pat &&
        FramebufferMtrrsEqual(a.mtrrs,b.mtrrs);
}

// HDA requires the borrowed UC MMIO mapping (PAT3) and WB direct-map RAM
// (PAT0), with the legacy WB aperture aliases resolved to UC by the MTRR.
inline bool CpuCacheHdaPageTypes(const CpuCacheSnapshot& snapshot,uint64_t aperture,
                                const uint64_t* dmaPages,unsigned count) {
    if(!snapshot.valid || snapshot.cacheDisabled || snapshot.notWriteThrough ||
       (snapshot.pat&255)!=6 || ((snapshot.pat>>24)&255)!=0 ||
       !dmaPages || count!=17 || (aperture&4095) || aperture>~uint64_t(0)-12288)return false;
    for(unsigned i=0;i<4;++i){MtrrPageObservation page{};
        if(!MtrrObservePage(snapshot.mtrrs,aperture+uint64_t(i)*4096,page) || page.type!=0)return false;}
    for(unsigned i=0;i<count;++i){MtrrPageObservation page{};
        if(!MtrrObservePage(snapshot.mtrrs,dmaPages[i],page) || page.type!=6)return false;}
    return true;
}
}
