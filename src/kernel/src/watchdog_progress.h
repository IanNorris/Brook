#pragma once
#include <stdint.h>

namespace brook {
// Diagnostic only: never arm an unseen CPU and report a stalled peer once
// until its timer accounting advances again.
struct WatchdogProgress {
    uint64_t last = 0;
    uint32_t stagnant = 0;
    bool seenProgress = false;
    void Reset(uint64_t value) { last=value; stagnant=0; seenProgress=false; }
    bool Observe(uint64_t value, uint32_t threshold) {
        if (value != last) { last=value; stagnant=0; seenProgress=true; return false; }
        if (!seenProgress || !threshold || stagnant>threshold) return false;
        if (++stagnant == threshold) { ++stagnant; return true; }
        return false;
    }
};
}
