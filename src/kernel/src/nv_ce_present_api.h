#pragma once
// nv-disp <-> nv-gsp seam for the copy-engine present path (head 0). nv-disp
// calls the registered function from FlipPendingLocked with the nv-aux lock held;
// nv-gsp registers it with `nv-gsp ce-present on` and clears it (under the same
// lock) before any RM teardown. Off unless that verb ran this boot.
#include <stdint.h>

namespace brook {
struct NvCeRect { int32_t x0, y0, x1, y1; };
struct NvCeJob {
    const uint32_t* src;           // the output's top-left pixel in the compositor back buffer
    uint32_t src_stride_px, width, height;
    uint64_t dst_vram;             // VRAM offset of the target buffer (== its BAR1 offset)
    uint32_t dst_pitch;            // bytes
    const NvCeRect* rects;         // output-local, already clipped by the caller or clipped here
    uint32_t n;
};
// 1 = copied (all rects in VRAM), 0 = not available (CPU copies), -1 = failed (CPU copies; CE now off).
using NvCePresentFn = int (*)(const NvCeJob& job, uint64_t* bytes);
void NvDispSetCePresent(NvCePresentFn fn);   // nv-aux lock held by the caller
}
