#pragma once
#include <stdint.h>

namespace brook {
// Linux x86-64 timespec timeout ABI: invalid fields fail, valid large values
// saturate to signed ktime's horizon. Keep that finite horizon distinct from
// null-timeout/infinite sentinels used by individual wait implementations.
inline constexpr uint64_t TimeoutMaxNs = ~uint64_t(0) >> 1;
inline bool TimeoutNanoseconds(int64_t sec, int64_t nsec, uint64_t* output) {
    constexpr uint64_t Billion = 1000000000;
    if (!output || sec < 0 || nsec < 0 || nsec >= int64_t(Billion)) return false;
    // Match ktime_set's seconds threshold, including the last partial second.
    *output = uint64_t(sec) >= TimeoutMaxNs / Billion ? TimeoutMaxNs
        : uint64_t(sec) * Billion + uint64_t(nsec);
    return true;
}
inline uint64_t TimeoutAddSafe(uint64_t now, uint64_t duration) {
    return now >= TimeoutMaxNs || duration >= TimeoutMaxNs - now
        ? TimeoutMaxNs : now + duration;
}
} // namespace brook
