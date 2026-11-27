#pragma once
// nv-disp <-> nv-gsp seam for the copy-engine present path (head 0). nv-disp kicks a
// copy from its head-0 shadow (the RAM image of what head 0 shows) in FlipPendingLocked
// and flips once it lands (poll hook, or before the next shadow update), so the copy
// overlaps the compositor's next compose. All calls are made with the nv-aux lock held;
// nv-gsp registers it with `nv-gsp ce-present on` and clears it (under the same
// lock) before any RM teardown. Off unless that verb ran this boot.
#include <stdint.h>

namespace brook {
struct NvCeRect { int32_t x0, y0, x1, y1; };
struct NvCeJob {
    const uint32_t* src;           // the head-0 shadow (or any kernel-mapped source)
    uint32_t src_stride_px, width, height;
    uint64_t dst_vram;             // VRAM offset of the target buffer (== its BAR1 offset)
    uint32_t dst_pitch;            // bytes
    const NvCeRect* rects;         // output-local, already clipped by the caller or clipped here
    uint32_t n;
    // Zero-copy step 2: one more copy after the rectangles, from a physically contiguous
    // sysmem buffer (a client's GART BO) to `layer_dst` (output-local), alpha forced to 255.
    // layer_phys 0 = none. The copy is the last one, so it wins over any rect overlapping it.
    uint64_t layer_phys = 0;       // the pixel at layer_dst's top-left
    uint32_t layer_stride = 0;     // bytes
    NvCeRect layer_dst{};
    // Full-frame Glass output. A nonzero address selects a pitch-linear
    // video-memory source instead of the system-memory shadow and layer.
    uint64_t video_src = 0;        // byte address in the NVK VRAM heap
    uint32_t video_pitch = 0;      // bytes
};
// kick: 1 = kicked (*ticket; the copy runs while the caller continues), 0 = not available
//       (CPU copies), -1 = failed (CPU copies; CE now off).
// wait: 1 = done, 0 = not yet (budget_us spent; 0 = just check), -1 = failed (CE now off).
struct NvCeOps {
    int (*kick)(const NvCeJob& job, uint64_t* bytes, uint32_t* ticket);
    int (*wait)(uint32_t ticket, uint32_t budget_us);
};
// nv-aux lock held by the caller. Clearing (nullptr) first finishes a kicked copy.
void NvDispSetCePresent(const NvCeOps* ops);
}
