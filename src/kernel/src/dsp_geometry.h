#pragma once
#include <stdint.h>
#include "audio_refill_budget.h"

namespace brook {
struct DspGeometry {
    uint32_t fragmentBytes=0,fragments=0,totalBytes=0;
    uint32_t capacityFrames=0,writableFrames=0;
    bool horizonQualified=false,changed=false;
};
// The high word limits fragment COUNT. An experimental device constraint may
// enlarge the power-of-two fragment size, but may not exceed that count or the
// physical endpoint bound. Existing invalid requests retain their error.
inline bool DspPlanGeometry(uint32_t request,uint32_t frameBytes,uint32_t maxFrames,
                            bool horizon,DspGeometry* result) {
    if(!result || (frameBytes!=1 && frameBytes!=2 && frameBytes!=4) || !maxFrames || maxFrames>16384)return false;
    const uint32_t exponent=request&0xffff,limit=request>>16;
    if(exponent<8 || exponent>16)return false;
    const uint32_t maximum=maxFrames*frameBytes,originalSize=1u<<exponent;
    const uint32_t originalCount=limit?limit:maximum/originalSize;
    if(!originalCount || uint64_t(originalCount)*originalSize>maximum)return false;
    auto geometry=[&](uint32_t size,uint32_t count){
        DspGeometry g{size,count,size*count,size*count/frameBytes,size/frameBytes};
        g.horizonQualified=AudioRefillQualified(g.capacityFrames,g.writableFrames);
        g.changed=size!=originalSize || count!=originalCount;
        return g;
    };
    const auto original=geometry(originalSize,originalCount);
    if(!horizon || original.horizonQualified){*result=original;return true;}
    for(uint32_t size=originalSize;size<=maximum;) {
        const uint64_t required=AudioRefillRequiredFrames(size/frameBytes)*frameBytes;
        const uint64_t count=(required+size-1)/size;
        if(count && count<=maximum/size && (!limit || count<=limit)) {
            const auto candidate=geometry(size,uint32_t(count));
            if(candidate.horizonQualified){*result=candidate;return true;}
        }
        if(size>maximum/2)break;
        size*=2;
    }
    // A valid but unsupported tuple keeps its original, honestly reported
    // geometry. No queued frames are resized or fictitiously completed.
    *result=original;return true;
}
} // namespace brook
