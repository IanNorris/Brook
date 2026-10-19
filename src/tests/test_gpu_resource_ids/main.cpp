#include "../../drivers/virtio_gpu/resource_ids.h"
#include <cassert>
#include <cstdint>
#include <set>

int main() {
    using namespace brook;
    GpuAppResourceIds<> ids;
    std::set<uint32_t> all;
    for (uint32_t slot = 0; slot < GpuResourceNamespace::ContextSlots; ++slot) {
        ids.BeginContext(slot);
        assert(!ids.ContainsIssued(slot, GpuResourceNamespace::AppFirst + slot));
        uint32_t old = 0;
        // Model callers of either creation/upload and burn every seventh
        // result as a failed host attempt. The helper must never reissue it.
        for (uint32_t i = 0; i < 10000; ++i) {
            const uint32_t id = ids.Allocate(slot);
            assert(id && id <= INT32_MAX && all.insert(id).second);
            assert(ids.ContainsIssued(slot, id));
            for (uint32_t other = 0; other < GpuResourceNamespace::ContextSlots; ++other)
                if (other != slot) assert(!ids.ContainsIssued(other, id));
            if (i % 7 == 0) continue;
            if (!old) old = id;
        }
        assert(ids.Issued(slot) == 10000);
        ids.BeginContext(slot);
        assert(!ids.ContainsIssued(slot, old));
        const uint32_t fresh = ids.Allocate(slot);
        assert(fresh && all.insert(fresh).second && ids.ContainsIssued(slot, fresh));
        assert(ids.Issued(slot) == 10001);
    }
    assert(!ids.Allocate(16) && !ids.Allocate(UINT32_MAX));
    assert(!ids.ContainsIssued(16, GpuResourceNamespace::AppFirst));
    assert(!ids.ContainsIssued(0, 0) && !ids.ContainsIssued(0, UINT32_MAX));

    // Exercise the same saturation arithmetic with a small representable
    // range, including a nonaligned final ID and repeated failed attempts.
    constexpr uint32_t last = GpuResourceNamespace::AppFirst + 18;
    GpuAppResourceIds<last> bounded;
    for (uint32_t slot = 0; slot < 16; ++slot) {
        assert(bounded.Allocate(slot) == GpuResourceNamespace::AppFirst + slot);
        if (slot <= 2) assert(bounded.Allocate(slot) == GpuResourceNamespace::AppFirst + 16 + slot);
        const uint32_t count = bounded.Issued(slot);
        for (unsigned retry = 0; retry < 5; ++retry) assert(!bounded.Allocate(slot));
        assert(bounded.Issued(slot) == count);
        bounded.BeginContext(slot);
        assert(!bounded.Allocate(slot));
        assert(!bounded.ContainsIssued(slot, GpuResourceNamespace::AppFirst + slot));
    }
    GpuKernelTextureIds kernel;
    const std::set<uint32_t> fixed{1,2,3,4,5,6,7,8,13,14,15,16,
                                   64,65,200,201,202,203,204,210,211,212,213};
    for (uint32_t expected = GpuResourceNamespace::KernelTextureFirst;
         expected < GpuResourceNamespace::AppFirst; ++expected) {
        assert(kernel.Allocate() == expected);
        assert(!fixed.count(expected));
        assert(!ids.ContainsIssued(0, expected));
    }
    for (unsigned retry = 0; retry < 5; ++retry) assert(!kernel.Allocate());
}
