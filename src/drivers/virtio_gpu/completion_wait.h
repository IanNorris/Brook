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

// Split submission must retain the publication clock origin across CPU packet
// preparation. An unavailable origin stays in bounded-poll mode; it must not
// acquire a fresh elapsed-time budget when Finish is eventually called.
struct GpuCompletionWaitOrigin {
    uint64_t startNs=0;
    bool timed=false;
};
template<class Clock>
GpuCompletionWaitOrigin GpuCompletionOrigin(bool runtime,Clock clock) {
    GpuCompletionWaitOrigin origin;
    origin.timed=runtime && clock(&origin.startNs);return origin;
}

template<class Ready,class Clock,class Pause>
bool GpuWaitForCompletionSince(GpuCompletionWaitOrigin origin,Ready ready,
                              Clock clock,Pause pause,GpuCompletionWaitLimits limits={}) {
    if(!limits.maxPolls)return false;
    // A validated available completion wins at the boundary, as in the
    // synchronous helper. Otherwise charge preparation against the old origin.
    if(ready())return true;
    bool timed=origin.timed;
    if(timed){
        uint64_t now=0;
        if(!clock(&now) || now<origin.startNs)timed=false;
        else if(now-origin.startNs>=limits.timeoutNs)return false;
    }
    uint32_t untilCheck=limits.checkEvery?limits.checkEvery:1;
    for(uint64_t poll=0;poll<limits.maxPolls;++poll){
        if(ready())return true;
        pause();
        if(--untilCheck)continue;
        untilCheck=limits.checkEvery?limits.checkEvery:1;
        if(!timed)continue;
        uint64_t now=0;
        if(!clock(&now) || now<origin.startNs){timed=false;continue;}
        if(now-origin.startNs>=limits.timeoutNs)return false;
    }
    return false;
}

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
