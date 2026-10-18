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
}
