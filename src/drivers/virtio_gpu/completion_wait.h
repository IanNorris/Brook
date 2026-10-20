#pragma once
#include <stdint.h>

namespace brook {

// Runtime uses a stable elapsed clock, independent of IRQ delivery. Early boot
// or failed clock reads retain the previous bounded polling fallback. The
// caller retains its command-buffer ownership; this helper never releases it.
struct GpuCompletionWaitLimits {
    uint64_t timeoutNs = 5000000000ull;
    uint64_t maxPolls = 200000000ull;
    uint32_t checkEvery = 2048;
};

template<class Ready, class Clock, class Pause>
bool GpuWaitForCompletion(bool runtime, Ready ready, Clock clock, Pause pause,
                          GpuCompletionWaitLimits limits = {}) {
    uint64_t start = 0;
    bool timed = runtime && clock(&start);
    uint32_t untilCheck = limits.checkEvery ? limits.checkEvery : 1;
    for (uint64_t poll = 0; poll < limits.maxPolls; ++poll) {
        if (ready()) return true;
        pause();
        if (--untilCheck) continue;
        untilCheck = limits.checkEvery ? limits.checkEvery : 1;
        if (!timed) continue;
        uint64_t now = 0;
        if (!clock(&now) || now < start) {
            timed = false;
            continue;
        }
        const uint64_t elapsed = now - start;
        if (elapsed >= limits.timeoutNs) return false;
    }
    return false;
}

} // namespace brook
