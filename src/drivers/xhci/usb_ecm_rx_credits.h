#pragma once
#include <stdint.h>

// Externally serialized by the controller event-ring lock. Each active TD
// reserves one queue slot until its completion is consumed.
struct UsbEcmRxCredits {
    static constexpr unsigned Slots = 128;
    // A second flight can queue while the first remains posted.
    static constexpr unsigned Capacity = 256;
    static constexpr unsigned RingEntries = 256;
    static constexpr unsigned BufferBytes = 2048;
    uint64_t ringBase;
    uint16_t owner[RingEntries]; // zero or one-based DMA slot
    int16_t slotTrb[Slots];     // one-based ring index, zero when idle
    unsigned outstanding;

    bool CanArm(unsigned slot, unsigned depth, uint32_t queued) const {
        return depth <= Slots && slot < depth && !slotTrb[slot] &&
               queued <= Capacity && outstanding <= Capacity - queued &&
               outstanding < Capacity - queued;
    }
    // Caller serializes the ring producer and establishes CanArm first.
    void Arm(unsigned slot, uint64_t token) {
        const unsigned index = static_cast<unsigned>((token-ringBase)/16);
        owner[index] = slot+1;
        slotTrb[slot] = index+1;
        ++outstanding;
    }
    int Find(uint64_t token) const {
        const uint64_t offset=token-ringBase;
        if ((offset&15) || offset>=16*(RingEntries-1))return -1;
        return static_cast<int>(owner[offset/16])-1;
    }
    // Keep all TDs pinned on an invalid token, error or impossible residue;
    // endpoint recovery must first establish that DMA has stopped.
    int CompletedSlot(uint64_t token, unsigned code, uint32_t residue) const {
        if ((code != 1 && code != 13) || residue > BufferBytes) return -1;
        return Find(token);
    }
    // Caller must use an active slot returned by Find(), under that lock.
    void Complete(unsigned slot) {
        owner[slotTrb[slot]-1] = 0;
        slotTrb[slot] = 0;
        --outstanding;
    }
};

// Externally serialized observations after rearming. These count entries into
// queue-credit pressure and into its zero-outstanding subset, not packets lost.
struct UsbEcmRxPressure {
    uint64_t entries,zeroEntries;
    bool blocked,zero;
    void Observe(unsigned depth,unsigned outstanding,uint32_t queued) {
        bool next=outstanding<depth && queued+outstanding>=UsbEcmRxCredits::Capacity;
        bool nextZero=next && !outstanding;
        if(next&&!blocked)++entries;
        if(nextZero&&!zero)++zeroEntries;
        blocked=next;zero=nextZero;
    }
};
