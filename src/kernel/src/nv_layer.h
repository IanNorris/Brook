#pragma once
// Zero-copy step 2: a window's NVK client buffer (a physically contiguous renderD130 GART
// BO, imported by waylandd from the client's dmabuf) shown in place of the window's VFB.
// WM_SET_NV_LAYER (waylandd) sets or detaches it; the compositor reads it (CPU blit while
// the window is not shown directly) and nv-disp's head-0 presenter reads it at each flip
// (CE copy BO -> scanout while it is). One small spinlock guards the table; lock order is
// nv-aux -> layer (the presenter looks layers up under the nv-aux lock), and nobody takes
// the nv-aux lock while holding it.
#include <stdint.h>

namespace brook {
struct MappingBacking;
struct NvLayer {
    uint64_t phys = 0;              // the client area's top-left pixel; 0 = no layer
    uint32_t stride = 0, w = 0, h = 0;   // bytes per row, pixels
    uint64_t seq = 0;               // bumps on every set (a new client frame)
    uint64_t identity = 0;          // the window's creation identity
    MappingBacking* life = nullptr; // keeps the BO pages alive
};
// Replace window `slot`'s layer (next.life is adopted; next.phys == 0 detaches), then make
// sure no nv-disp copy still reads the old one and drop the old pages' reference. Call
// without the nv-aux lock. Returns the new seq.
uint64_t NvLayerSet(uint32_t slot, uint64_t identity, NvLayer next);
// Detach (window destroy, or a set that lost a race with it) when the layer belongs to
// `identity` (0 = any). No-op without a layer.
void NvLayerDetach(uint32_t slot, uint64_t identity);
// A copy of slot's layer when it belongs to `identity` (0 = any). ref: also take a
// reference on life (NvLayerPut after use).
bool NvLayerGet(uint32_t slot, uint64_t identity, NvLayer* out, bool ref);
void NvLayerPut(const NvLayer& l);
// nv-disp registers this: called with the nv-aux lock held after a swap, it finishes any
// kicked copy that may read the old layer.
void NvLayerSetRetireHook(void (*retireLocked)());
struct NvLayerStats { uint64_t sets, detaches, retires; };
NvLayerStats NvLayerStatsGet();
}
