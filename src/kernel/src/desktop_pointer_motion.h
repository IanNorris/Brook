#pragma once
#include <stdint.h>
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
}
