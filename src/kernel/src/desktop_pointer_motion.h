#pragma once
#include <stdint.h>
#include "display_output.h"
namespace brook {
// Accumulate fractional DPI gain so slow 150% motion is not rounded away.
// Deltas passed to applications remain raw; only desktop position uses this.
inline int32_t AdvanceDesktopPointer(int32_t position,int32_t delta,uint32_t bound,
                                    uint32_t percent,int32_t& fraction) {
    const int64_t scaled=static_cast<int64_t>(delta)*percent+fraction;
    fraction=static_cast<int32_t>(scaled%100);
    const int64_t next=static_cast<int64_t>(position)+scaled/100;
    if(next<0){fraction=0;return 0;}
    if(next>=bound){fraction=0;return bound?static_cast<int32_t>(bound-1):0;}
    return static_cast<int32_t>(next);
}
// Move the point to the nearest pixel of any output rectangle, so the
// pointer cannot rest in a gap of the desktop union (outputs of different
// heights, or not touching). A point inside an output is unchanged, so with
// one output this never moves a point already clamped to its bounds. Ties
// go to the lower index (the primary output). Returns which axes moved
// (bit 0 x, bit 1 y); no rects leave the point alone.
inline uint32_t DesktopClampToOutputs(const OutputRect* rects,uint32_t count,int32_t& x,int32_t& y) {
    bool found=false;
    int64_t bestX=x,bestY=y;
    uint64_t best=0;
    for(uint32_t i=0;i<count;++i){
        const OutputRect& r=rects[i];
        if(r.empty())continue;
        if(r.contains(x,y))return 0;
        const int64_t cx=x<r.x?r.x:(x>=r.right()?r.right()-1:x);
        const int64_t cy=y<r.y?r.y:(y>=r.bottom()?r.bottom()-1:y);
        const uint64_t dx=uint64_t(cx>x?cx-x:x-cx),dy=uint64_t(cy>y?cy-y:y-cy);
        const uint64_t distance=dx*dx+dy*dy;
        if(!found || distance<best){found=true;best=distance;bestX=cx;bestY=cy;}
    }
    if(!found)return 0;
    const uint32_t moved=(bestX!=x?1u:0u)|(bestY!=y?2u:0u);
    x=static_cast<int32_t>(bestX);y=static_cast<int32_t>(bestY);
    return moved;
}
}
