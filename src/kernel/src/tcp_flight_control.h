#pragma once
#include <stdint.h>

namespace brook {
// Experimental bounded per-connection congestion state. Call under the
// socket lock with validated newly acknowledged bytes, never duplicate ACKs.
struct TcpFlightControl {
    uint32_t cwnd = 0;
    uint32_t threshold = 0;
    uint64_t additiveCredit = 0;
    bool recovering = false;

    void Init(uint32_t mss) {
        const uint32_t cap = 8 * mss;
        if (!cwnd) { cwnd = 2 * mss; threshold = cap; additiveCredit = 0; recovering = false; }
        if (cwnd > cap) cwnd = cap;
        if (threshold > cap) threshold = cap;
    }
    void Ack(uint32_t bytes, uint32_t mss) {
        Init(mss);
        if (!bytes) return;
        recovering = false;
        const uint32_t cap = 8 * mss;
        if (cwnd < threshold) {
            uint32_t growth = threshold - cwnd;
            if (growth > bytes) growth = bytes;
            cwnd += growth; bytes -= growth;
        }
        if (bytes && cwnd < cap) {
            uint64_t credit = additiveCredit + static_cast<uint64_t>(bytes) * mss;
            // Growing by k bytes costs cwnd + (cwnd+1) + ... + (cwnd+k-1).
            // Binary search yields the same result regardless of ACK grouping
            // without a potentially long per-byte loop in the network path.
            uint32_t low = 0, high = cap - cwnd;
            while (low < high) {
                const uint32_t mid = low + (high - low + 1) / 2;
                const uint64_t cost = static_cast<uint64_t>(mid)*cwnd + static_cast<uint64_t>(mid)*(mid-1)/2;
                if (cost <= credit) low = mid; else high = mid - 1;
            }
            if (low) credit -= static_cast<uint64_t>(low)*cwnd + static_cast<uint64_t>(low)*(low-1)/2;
            cwnd += low;
            additiveCredit = credit;
        }
        if (cwnd >= cap) { cwnd = cap; additiveCredit = 0; }
    }
    void Loss(uint32_t mss) {
        Init(mss);
        if (recovering) return;
        threshold = cwnd / 2;
        if (threshold < 2 * mss) threshold = 2 * mss;
        cwnd = mss;
        additiveCredit = 0;
        recovering = true;
    }
};
}
