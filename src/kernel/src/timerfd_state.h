#pragma once
#include <stdint.h>
#include "timeout_value.h"

namespace brook {
// Protected by the owning timerfd mutex. Units are selected-clock nanoseconds.
struct TimerFdState {
    uint64_t pending = 0;
    uint64_t interval = 0;
    uint64_t deadline = 0;
    bool armed = false;
    void refresh(uint64_t now) {
        if (!armed || now < deadline) return;
        uint64_t count = 1;
        if (!interval) {
            armed = false;
        } else {
            const uint64_t overdue = now - deadline;
            const uint64_t missed = overdue / interval;
            count = missed == ~uint64_t(0) ? missed : missed + 1;
            const uint64_t untilNext = interval - overdue % interval;
            // One terminal expiration is allowed at the signed horizon.
            // There is no later representable deadline to rearm repeatedly.
            if (now >= TimeoutMaxNs) armed = false;
            else deadline = TimeoutAddSafe(now, untilNext);
        }
        pending = count > ~uint64_t(0) - pending ? ~uint64_t(0) : pending + count;
    }
};
inline bool TimerFdNanoseconds(int64_t sec, int64_t ns, uint64_t* result) {
    return TimeoutNanoseconds(sec, ns, result);
}
}
