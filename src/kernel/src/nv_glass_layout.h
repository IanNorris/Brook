#pragma once

#include "../../shared/gpu_output_abi.h"
#include "compositor_present_lease.h"
#include "display_output.h"

namespace brook {
inline constexpr uint32_t NvGlassHeads=2;

struct NvGlassCrop {
    uint64_t offset=0, end=0;
};

// The frame occupies pitch bytes per row; its BO need only contain the
// visible pixels of the last row. The CE reads complete pixels in the crop.
inline bool NvGlassFrameCrop(uint32_t width,uint32_t height,uint32_t pitch,
                             const OutputRect& rect,NvGlassCrop& crop)
{
    if(!BrookGpuNvGlassFrameFits(width,height) || pitch<width*4 ||
       pitch>BROOK_GPU_NV_GLASS_MAX_WIDTH*4 || (pitch&3) ||
       rect.empty() || rect.x<0 || rect.y<0 ||
       rect.right()>width || rect.bottom()>height)return false;
    const uint64_t extent=uint64_t(height-1)*pitch+width*4;
    if(extent>BROOK_GPU_OUTPUT_MAX_BYTES)return false;
    crop.offset=uint64_t(rect.y)*pitch+uint32_t(rect.x)*4;
    crop.end=crop.offset+uint64_t(rect.h-1)*pitch+rect.w*4;
    return crop.end<=extent;
}

// Protected by the scanout mapping lock. A transiently busy output retains
// its own lease until it presents again or expires. Other heads can progress.
struct NvGlassLeases {
    CompositorPresentLease heads[NvGlassHeads]{};
    uint64_t layout=0;

    uint32_t ActiveMask(uint64_t generation,uint64_t now,uint64_t owner=0) const {
        uint32_t mask=0;
        if(layout==generation)
            for(uint32_t i=0;i<NvGlassHeads;++i)
                if(heads[i].Active(now) && (!owner || heads[i].owner==owner))mask|=1u<<i;
        return mask;
    }
    bool Reset(uint64_t generation) {
        if(layout==generation)return false;
        layout=generation;
        bool changed=false;
        for(auto& head:heads)changed|=head.Clear();
        return changed;
    }
    bool Expire(uint64_t now) {
        bool changed=false;
        for(auto& head:heads)changed|=head.Expire(now);
        return changed;
    }
    bool Release(uint64_t owner) {
        bool changed=false;
        for(auto& head:heads)changed|=head.Release(owner);
        return changed;
    }
};
}
