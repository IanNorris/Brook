#pragma once
#include <stdint.h>
namespace brook {
// Page allocator over the xHCI low-memory DMA reserve (taken once at module
// init). First-fit runs of 1..N pages; freed runs are reused, so hot-plug
// (KVM switching) is not capped by the reserve size. No locking: callers
// hold the reserve lock.
template<uint32_t MaxPages>
struct XhciDmaReserve {
    static constexpr uint32_t Words = (MaxPages + 63) / 64;
    uint64_t used[Words]{};
    uint32_t pages = 0, inUse = 0, peak = 0;
    void init(uint32_t n) { pages = n < MaxPages ? n : MaxPages; for (auto& w : used) w = 0; inUse = peak = 0; }
    bool bit(uint32_t i) const { return (used[i / 64] >> (i % 64)) & 1; }
    void set(uint32_t i, bool v) { if (v) used[i / 64] |= 1ull << (i % 64); else used[i / 64] &= ~(1ull << (i % 64)); }
    // Returns the first page index of a free run of n pages, or -1.
    int alloc(uint32_t n) {
        if (!n || n > pages) return -1;
        for (uint32_t start = 0; start + n <= pages;) {
            uint32_t run = 0;
            while (run < n && !bit(start + run)) ++run;
            if (run == n) {
                for (uint32_t i = 0; i < n; ++i) set(start + i, true);
                inUse += n; if (inUse > peak) peak = inUse;
                return int(start);
            }
            start += run + 1;
        }
        return -1;
    }
    // Frees a run previously returned by alloc; false if out of range or
    // any page was not allocated (nothing is changed then).
    bool release(uint32_t first, uint32_t n) {
        if (!n || first >= pages || n > pages - first) return false;
        for (uint32_t i = 0; i < n; ++i) if (!bit(first + i)) return false;
        for (uint32_t i = 0; i < n; ++i) set(first + i, false);
        inUse -= n;
        return true;
    }
};
}
