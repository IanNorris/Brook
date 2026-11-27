#pragma once
#include <stdint.h>

namespace brook {
// One predictor per open file. Repeated disjoint reads don't trigger I/O.
struct Ext2SequentialRead {
    static constexpr uint32_t Initial = 128 * 1024;
    static constexpr uint32_t Maximum = 1024 * 1024;
    uint64_t end = 0;
    uint64_t sequential = 0;
    uint32_t window = Initial;

    uint32_t Observe(uint64_t offset, uint32_t requested) {
        if (offset != end || requested > ~uint64_t{0} - offset) {
            sequential = 0;
            window = Initial;
        }
        return sequential >= 16 * 1024 ? window : 0;
    }

    void Complete(uint64_t offset, uint32_t bytes) {
        if (!bytes || bytes > ~uint64_t{0} - offset) {
            sequential = end = 0;
            window = Initial;
            return;
        }
        if (offset != end) sequential = 0;
        sequential += bytes;
        end = offset + bytes;
    }

    void Filled() {
        if (window < Maximum) window *= 2;
    }
};
}
