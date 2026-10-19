#pragma once
#include <stdint.h>

namespace brook {

// Host resource IDs must remain positive through the signed module ABI. Fixed
// boot-test resources occupy the low region; compositor textures and application
// resources have disjoint dynamic ranges. IDs are burned even when creation
// fails: an ambiguous host completion must never alias a later allocation.
struct GpuResourceNamespace {
    static constexpr uint32_t KernelTextureFirst = 0x1000;
    static constexpr uint32_t AppFirst = 0x10000;
    static constexpr uint32_t Last = 0x7fffffff;
    static constexpr uint32_t ContextSlots = 16;
};

class GpuKernelTextureIds {
    uint32_t next = GpuResourceNamespace::KernelTextureFirst;
public:
    uint32_t Allocate() {
        return next < GpuResourceNamespace::AppFirst ? next++ : 0;
    }
};

// All callers serialize allocation/context activation with GpuCmdGuard. The
// low bits identify the context slot; the remaining bits are its monotonic
// allocation sequence. Both texture creation and upload use this allocator.
// BeginContext rejects a prior incarnation's IDs without recycling them.
// ContainsIssued is a namespace check, not proof of a live successful resource;
// canonical DRM resource references still enforce that ownership/lifetime.
template<uint32_t LastId = GpuResourceNamespace::Last>
class GpuAppResourceIds {
    static_assert(LastId >= GpuResourceNamespace::AppFirst &&
                  LastId <= GpuResourceNamespace::Last);
    uint32_t next[GpuResourceNamespace::ContextSlots]{};
    uint32_t first[GpuResourceNamespace::ContextSlots]{};
public:
    void BeginContext(uint32_t slot) {
        if (slot < GpuResourceNamespace::ContextSlots) first[slot] = next[slot];
    }
    uint32_t Allocate(uint32_t slot) {
        if (slot >= GpuResourceNamespace::ContextSlots) return 0;
        const uint64_t id = GpuResourceNamespace::AppFirst +
            uint64_t(next[slot]) * GpuResourceNamespace::ContextSlots + slot;
        if (id > LastId) return 0;
        ++next[slot];
        return static_cast<uint32_t>(id);
    }
    bool ContainsIssued(uint32_t slot, uint32_t id) const {
        if (slot >= GpuResourceNamespace::ContextSlots ||
            id < GpuResourceNamespace::AppFirst || id > LastId) return false;
        const uint32_t relative = id - GpuResourceNamespace::AppFirst;
        const uint32_t sequence = relative / GpuResourceNamespace::ContextSlots;
        return relative % GpuResourceNamespace::ContextSlots == slot &&
               sequence >= first[slot] && sequence < next[slot];
    }
    uint32_t Issued(uint32_t slot) const {
        return slot < GpuResourceNamespace::ContextSlots ? next[slot] : 0;
    }
};

} // namespace brook
