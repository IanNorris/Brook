#pragma once
#include <stdint.h>

// Software-scanout row filter. The CPU compositor marks rows dirty generously
// (the taskbar every frame, the whole screen while the launcher is open), but
// writes to the firmware framebuffer are uncached/write-combined and dominate
// the idle cost. A cached shadow of what the framebuffer holds lets a dirty row
// whose pixels did not change be skipped without reading the framebuffer.
//
// The shadow is only trusted after the compositor itself wrote a complete
// frame; whenever another writer (a leased renderer present, a usermode flip,
// a display change) may have touched the framebuffer, the caller invalidates
// it and the next software flip copies every row again.
namespace brook {

struct GopRowFilterResult {
    uint32_t copied=0, skipped=0;
    uint32_t minY=0xFFFFFFFFu, maxY=0; // copied row span, [minY,maxY)
};

inline bool GopRowEqual(const uint32_t* a, const uint32_t* b, uint32_t width)
{
    // Word-wise compare; rows are 4-byte aligned. Stops at the first change.
    uint32_t x=0;
    for(;x+2<=width;x+=2){
        uint64_t p,q;__builtin_memcpy(&p,a+x,8);__builtin_memcpy(&q,b+x,8);
        if(p!=q)return false;
    }
    return x==width || a[x]==b[x];
}

// Copies rows [minY,maxY) of source that differ from the shadow into the
// framebuffer (via copyRow) and the shadow. With trustShadow false every row is
// copied and the shadow refreshed. copyRow(y) writes source row y to the
// framebuffer; the shadow update is done here with ordinary cached stores.
template<class CopyRow>
GopRowFilterResult GopRowFilterCopy(const uint32_t* source, uint32_t sourceStride,
                                    uint32_t* shadow, uint32_t shadowStride, uint32_t width,
                                    uint32_t minY, uint32_t maxY, bool trustShadow, CopyRow copyRow)
{
    GopRowFilterResult r;
    for(uint32_t y=minY;y<maxY;++y){
        const uint32_t* src=source+uint64_t(y)*sourceStride;
        uint32_t* sh=shadow+uint64_t(y)*shadowStride;
        if(trustShadow && GopRowEqual(src,sh,width)){++r.skipped;continue;}
        copyRow(y);
        __builtin_memcpy(sh,src,uint64_t(width)*4);
        ++r.copied;
        if(y<r.minY)r.minY=y;
        if(y+1>r.maxY)r.maxY=y+1;
    }
    return r;
}

} // namespace brook
