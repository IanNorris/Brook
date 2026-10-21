#pragma once
#include <stdint.h>
#include "timeout_value.h"

namespace brook {
struct FutexWaitDeadline {
    uint64_t nanoseconds = 0;
    unsigned clockId = 1;
    bool finite = false;

    bool expired(uint64_t nowNs) const {
        return finite && nowNs >= nanoseconds;
    }

    // Scheduler ticks are only a wake hint, not the timeout's clock domain.
    // Entry phase can make this hint early. The waiter must recheck its ns
    // deadline and rearm while still queued rather than report early timeout.
    uint64_t wakeupTick(uint64_t nowNs, uint64_t nowTick) const {
        constexpr uint64_t Max = ~uint64_t(0);
        if (!finite) return Max;
        if (nowNs >= nanoseconds) return nowTick;
        const uint64_t remaining = nanoseconds - nowNs;
        const uint64_t ticks = remaining / 1000000ULL + (remaining % 1000000ULL != 0);
        // Max is reserved for an infinite scheduler wait. Keep finite hints
        // finite even at the representable tick boundary.
        if (nowTick >= Max - 1 || ticks >= Max - 1 - nowTick) return Max - 1;
        return nowTick + ticks;
    }
};

inline bool FutexTimeoutNanoseconds(int64_t sec, int64_t nsec, uint64_t* output) {
    return TimeoutNanoseconds(sec, nsec, output);
}

inline bool FutexMakeDeadline(bool relative, uint64_t valueNs, uint64_t nowNs,
                              unsigned clockId, FutexWaitDeadline* output) {
    if (!output || clockId > 1) return false;
    *output = {relative ? TimeoutAddSafe(nowNs, valueNs)
        : (valueNs > TimeoutMaxNs ? TimeoutMaxNs : valueNs), clockId, true};
    return true;
}
} // namespace brook
