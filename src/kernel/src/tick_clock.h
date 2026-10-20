#pragma once
#include <stdint.h>

namespace brook {

// Preserve the existing millisecond origin when selecting an elapsed source.
// Invalid/regressing samples never manufacture time or wrap a deadline clock.
inline bool TickClockElapsedMs(uint64_t originNs, uint64_t originMs,
                               uint64_t nowNs, uint64_t* output) {
    if (!output || nowNs < originNs) return false;
    const uint64_t elapsed = (nowNs - originNs) / 1000000ULL;
    if (originMs > ~uint64_t(0) - elapsed) return false;
    *output = originMs + elapsed;
    return true;
}

// All CPUs may refresh the cache. A failed clock read can safely return this
// floor even after another CPU has already published a newer sample.
inline uint64_t TickClockPublishFloor(volatile uint64_t* counter, uint64_t sample) {
    uint64_t current = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
    while (current < sample &&
           !__atomic_compare_exchange_n(counter, &current, sample, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    return current < sample ? sample : current;
}

} // namespace brook
