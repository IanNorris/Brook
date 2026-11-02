#pragma once
#include "mtrr_page_probe.h"

namespace brook {
// A single CPU's boot-time cache configuration; does not validate mappings or
// establish that these registers stay unchanged after the snapshot.
struct CpuCacheSnapshot {
    FramebufferMtrrs mtrrs{};
    uint64_t pat=0;
    uint32_t clflushLine=0;
    bool valid=false,cacheDisabled=false,notWriteThrough=false,clflush=false;
};

inline bool CpuCacheSnapshotsMatch(const CpuCacheSnapshot& a,const CpuCacheSnapshot& b) {
    return a.valid && b.valid && !a.cacheDisabled && !b.cacheDisabled &&
        !a.notWriteThrough && !b.notWriteThrough && a.clflush && b.clflush &&
        a.clflushLine>=16 && a.clflushLine<=4096 && !(a.clflushLine&(a.clflushLine-1)) &&
        !(4096%a.clflushLine) && a.clflushLine==b.clflushLine && a.pat==b.pat &&
        FramebufferMtrrsEqual(a.mtrrs,b.mtrrs);
}

// HDA requires the borrowed UC MMIO mapping (PAT3) and WB direct-map RAM
// (PAT0), with the legacy WB aperture aliases resolved to UC by the MTRR.
inline bool CpuCacheHdaApertureType(const CpuCacheSnapshot& snapshot,uint64_t aperture) {
    if(!snapshot.valid || snapshot.cacheDisabled || snapshot.notWriteThrough ||
       (snapshot.pat&255)!=6 || ((snapshot.pat>>24)&255)!=0 ||
       (aperture&4095) || aperture>~uint64_t(0)-12288)return false;
    for(unsigned i=0;i<4;++i){MtrrPageObservation page{};
        if(!MtrrObservePage(snapshot.mtrrs,aperture+uint64_t(i)*4096,page) || page.type!=0)return false;}
    return true;
}
inline bool CpuCacheHdaDmaPageType(const CpuCacheSnapshot& snapshot,uint64_t physical) {
    MtrrPageObservation page{};return snapshot.valid && !snapshot.cacheDisabled && !snapshot.notWriteThrough &&
        (snapshot.pat&255)==6 && MtrrObservePage(snapshot.mtrrs,physical,page) && page.type==6;
}
inline bool CpuCacheHdaPageTypes(const CpuCacheSnapshot& snapshot,uint64_t aperture,
                                const uint64_t* dmaPages,unsigned count) {
    if(!dmaPages || count!=17 || !CpuCacheHdaApertureType(snapshot,aperture))return false;
    for(unsigned i=0;i<count;++i)if(!CpuCacheHdaDmaPageType(snapshot,dmaPages[i]))return false;
    return true;
}
}
