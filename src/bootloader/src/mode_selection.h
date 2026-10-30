#pragma once
#include <stdint.h>
namespace brook { namespace bootloader {
// Order independent ranking. A bounded request must not fall back to a huge mode.
inline bool PreferMode(uint32_t w,uint32_t h,uint32_t bw,uint32_t bh,
                       uint32_t requestW,uint32_t requestH) {
    if(!w || !h) return false;
    if(!bw || !bh) return true;
    const uint64_t area=uint64_t(w)*h, best=uint64_t(bw)*bh;
    if(requestW && requestH) {
        const bool fits=w<=requestW && h<=requestH;
        const bool bestFits=bw<=requestW && bh<=requestH;
        if(fits!=bestFits) return fits;
        if(area!=best) return fits ? area>best : area<best;
    } else {
        const int rank=(w==1920 && h==1080)?2:(w==1280 && h==720)?1:0;
        const int bestRank=(bw==1920 && bh==1080)?2:(bw==1280 && bh==720)?1:0;
        if(rank!=bestRank) return rank>bestRank;
        if(area!=best) return area>best;
    }
    return w>bw; // deterministic tie break
}
} }
