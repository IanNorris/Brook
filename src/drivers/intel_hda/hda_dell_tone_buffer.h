#pragma once
#include <stdint.h>

namespace brook {
// DMA payload construction only; caller owns and pins all four pages and
// publishes their cache-coherent contents before enabling bus mastering/RUN.
// The allocator/adapter must also validate the platform's physical address width.
struct HdaToneBdlEntry {uint64_t address;uint32_t length;uint32_t flags;};
static_assert(sizeof(HdaToneBdlEntry)==16,"HDA BDL entry size");
constexpr unsigned HdaTonePages=3,HdaTonePageBytes=4096,HdaToneFramesPerPage=1024;
inline bool HdaDellBuildToneBuffer(HdaToneBdlEntry* bdl,uint64_t bdlPhys,
                                  int16_t* const* pages,const uint64_t* physical,
                                  bool address64,uint16_t peak) {
    if(!bdl || !pages || !physical || !bdlPhys || (bdlPhys&4095) ||
       bdlPhys>~uint64_t(0)-4095 || peak>2048)return false;
    // Inputs can themselves reside in output pages: snapshot before writes.
    int16_t* data[HdaTonePages];uint64_t addresses[HdaTonePages];
    for(unsigned i=0;i<HdaTonePages;++i){data[i]=pages[i];addresses[i]=physical[i];}
    if(!address64 && bdlPhys>0xfffff000ULL)return false;
    const auto bdlVa=reinterpret_cast<uintptr_t>(bdl);
    if((bdlVa&4095) || bdlVa>~uintptr_t(0)-HdaTonePageBytes)return false;
    for(unsigned i=0;i<HdaTonePages;++i) {
        const auto va=reinterpret_cast<uintptr_t>(data[i]);
        if(!va || (va&4095) || va>~uintptr_t(0)-HdaTonePageBytes || !addresses[i] ||
           (addresses[i]&4095) || addresses[i]>~uint64_t(0)-4095 ||
           (!address64 && addresses[i]>0xfffff000ULL) ||
           addresses[i]==bdlPhys || (va<bdlVa+HdaTonePageBytes && bdlVa<va+HdaTonePageBytes))return false;
        for(unsigned j=0;j<i;++j) {
            const auto other=reinterpret_cast<uintptr_t>(data[j]);
            if(addresses[i]==addresses[j] || (va<other+HdaTonePageBytes && other<va+HdaTonePageBytes))return false;
        }
    }
    for(unsigned i=0;i<HdaTonePages;++i) {
        bdl[i]={addresses[i],HdaTonePageBytes,0}; // no completion interrupts
        for(unsigned f=0;f<HdaToneFramesPerPage;++f) {
            const unsigned phase=(i*HdaToneFramesPerPage+f)%48; // 1 kHz at 48 kHz
            int32_t value=phase<12?int32_t(phase)*peak/12:
                phase<24?int32_t(24-phase)*peak/12:
                phase<36?-int32_t(phase-24)*peak/12:-int32_t(48-phase)*peak/12;
            data[i][2*f]=static_cast<int16_t>(value);data[i][2*f+1]=static_cast<int16_t>(value);
        }
    }
    return true;
}
}
