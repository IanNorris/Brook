#pragma once
#include <stdint.h>

namespace brook {
// Forward intervals shorter than half the counter range, including ordinary
// uint64 wrap. A backwards adjustment must not become a huge elapsed interval.
// Prefer a monotonic clock: this guard does not repair a regressing source.
constexpr bool ClockIntervalElapsed(uint64_t now, uint64_t started, uint64_t delay)
{
    const uint64_t elapsed = now - started;
    return elapsed <= (~uint64_t{0} >> 1) && elapsed >= delay;
}

// Sample once after a boundary even if IRQ delivery skips its exact tick.
// Keep the original phase, but do not invent historical samples after a gap.
inline bool ClockTakePeriodicSample(uint64_t now, uint64_t interval, uint64_t& previous)
{
    if (!interval || !ClockIntervalElapsed(now, previous, interval)) return false;
    previous = now - (now - previous) % interval;
    return true;
}
}
