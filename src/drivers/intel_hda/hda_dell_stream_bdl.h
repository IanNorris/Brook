#pragma once
#include "hda_dell_tone_buffer.h"

namespace brook {
constexpr unsigned HdaDellStreamPages=16,HdaDellStreamPageBytes=4096;
// Build the immutable 64 KiB mixer BDL from individually owned pages.
// Validation is completed before modifying the BDL. The owner must separately
// verify RAM/cache aliases, pin the pages and publish all descriptors before
// enabling bus mastering; this function does not establish any of those.
inline bool HdaDellBuildStreamBdl(HdaToneBdlEntry* bdl,uint64_t bdlPhys,
                                 void* const* pages,const uint64_t* physical,bool address64) {
    if(!bdl || !pages || !physical || !bdlPhys || (bdlPhys&4095) ||
       bdlPhys>~uint64_t(0)-4095 || (!address64 && bdlPhys>0xfffff000ULL))return false;
    const uintptr_t bdlVa=reinterpret_cast<uintptr_t>(bdl);
    if((bdlVa&4095) || bdlVa>~uintptr_t(0)-4096)return false;
    uintptr_t addresses[HdaDellStreamPages];uint64_t phys[HdaDellStreamPages];
    for(unsigned i=0;i<HdaDellStreamPages;++i){addresses[i]=reinterpret_cast<uintptr_t>(pages[i]);phys[i]=physical[i];}
    for(unsigned i=0;i<HdaDellStreamPages;++i){
        const auto va=addresses[i];const auto pa=phys[i];
        if(!va || (va&4095) || va>~uintptr_t(0)-4096 || !pa || (pa&4095) ||
           pa>~uint64_t(0)-4095 || (!address64 && pa>0xfffff000ULL) || pa==bdlPhys ||
           (va<bdlVa+4096 && bdlVa<va+4096))return false;
        for(unsigned j=0;j<i;++j)if(pa==phys[j] || (va<addresses[j]+4096 && addresses[j]<va+4096))return false;
    }
    for(unsigned i=0;i<HdaDellStreamPages;++i)bdl[i]={phys[i],HdaDellStreamPageBytes,0};
    return true;
}
}
