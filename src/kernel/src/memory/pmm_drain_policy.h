#pragma once
#include <stdint.h>

namespace brook {
// Back-to-back drain policy for PmmDrainThread (BRO-179/206 quarantine).
//
// Every drain costs one all-CPU TLB barrier (IPI + CR3 reload + ack on every
// CPU, waking idle CPUs out of deep C-states). The first drain after each idle
// nap always runs. A further drain without napping only runs when the batch is
// big enough to amortise the barrier, when free memory is tight, or when the
// quarantine is near the producer throttle's low watermark, so sustained
// production still self-rate-matches (BRO-206) while trickle frees from a
// browser's process churn wait at most one nap interval for their barrier.
//
// Safety is unchanged by this policy: frames still leave quarantine only after
// a barrier that started after their free; deferring a drain only delays reuse.
constexpr uint32_t PmmDrainMinBatch = 256; // frames (1 MiB)

constexpr bool PmmDrainContinue(uint32_t pending, uint64_t freePages, uint64_t totalPages) {
    if (!pending) return false;
    if (pending >= PmmDrainMinBatch) return true;
    if (!totalPages) return true;                       // unknown size: be eager
    if (freePages < totalPages / 32) return true;       // memory tight
    if (pending >= totalPages / 32) return true;        // near the throttle (low = total/16)
    return false;
}

struct PmmDrainStats {
    uint64_t barriers = 0, framesReleased = 0, virtualBatches = 0;
    uint64_t deferred = 0, maxBatch = 0;
};
}
