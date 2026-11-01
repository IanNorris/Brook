#pragma once
#include <stdint.h>

// Externally serialized by the controller event-ring lock. Each active TD
// reserves one queue slot until its completion is consumed.
struct UsbEcmRxCredits {
    static constexpr unsigned Slots = 8;
    static constexpr unsigned Capacity = 32;
    struct Slot { uint64_t token; bool active; } slots[Slots];
    unsigned outstanding;

    bool CanArm(unsigned slot, unsigned depth, uint32_t queued) const {
        return depth <= Slots && slot < depth && !slots[slot].active &&
               queued <= Capacity && outstanding <= Capacity - queued &&
               outstanding < Capacity - queued;
    }
    // Caller must first establish CanArm() under the same controller lock.
    void Arm(unsigned slot, uint64_t token) {
        slots[slot].token = token; slots[slot].active = true; ++outstanding;
    }
    int Find(uint64_t token) const {
        for (unsigned i = 0; i < Slots; ++i)
            if (slots[i].active && slots[i].token == token) return static_cast<int>(i);
        return -1;
    }
    // Keep all TDs pinned on an invalid token, error or impossible residue;
    // endpoint recovery must first establish that DMA has stopped.
    int CompletedSlot(uint64_t token, unsigned code, uint32_t residue) const {
        if ((code != 1 && code != 13) || residue > 2048) return -1;
        return Find(token);
    }
    // Caller must use an active slot returned by Find(), under that lock.
    void Complete(unsigned slot) { slots[slot].active = false; --outstanding; }
};
