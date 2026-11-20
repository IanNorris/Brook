#pragma once

#include <stdint.h>

namespace brook::e1000 {

constexpr uint16_t RingSize = 128;
constexpr uint16_t BufferSize = 2048;
constexpr uint16_t MinFrameSize = 14;
constexpr uint16_t MaxFrameSize = 1514;

struct RxDescriptor {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
};

struct TxDescriptor {
    uint64_t address;
    uint16_t length;
    uint8_t checksumOffset;
    uint8_t command;
    uint8_t status;
    uint8_t checksumStart;
    uint16_t special;
};

static_assert(sizeof(RxDescriptor) == 16 && sizeof(TxDescriptor) == 16);
static_assert(RingSize * sizeof(RxDescriptor) % 128 == 0);
static_assert((RingSize & (RingSize - 1)) == 0);

constexpr uint8_t DescriptorDone = 1u << 0;
constexpr uint8_t RxEndOfPacket = 1u << 1;
constexpr uint8_t TxCommand = (1u << 0) | (1u << 1) | (1u << 3); // EOP, IFCS, RS

inline uint16_t Next(uint16_t index)
{
    return (index + 1) & (RingSize - 1);
}

struct TxRing {
    uint16_t head = 0;
    uint16_t tail = 0;

    bool Full() const { return Next(tail) == head; }

    void Reap(const TxDescriptor* descriptors)
    {
        while (head != tail &&
               (__atomic_load_n(&descriptors[head].status, __ATOMIC_ACQUIRE) & DescriptorDone))
            head = Next(head);
    }
};

// Consume malformed multi-descriptor packets through EOP. A valid-looking
// final fragment must never be delivered as a new Ethernet frame.
struct RxRing {
    uint16_t next = 0;
    bool discarding = false;

    bool Accept(uint8_t status, uint8_t errors, uint16_t length)
    {
        const bool end = status & RxEndOfPacket;
        const bool valid = end && !discarding && !errors &&
                           length >= MinFrameSize && length <= MaxFrameSize;
        discarding = !end;
        return valid;
    }
};

inline const char* DeviceName(uint16_t device)
{
    switch (device) {
        case 0x100e: return "82540EM";
        case 0x100f: return "82545EM";
        default: return nullptr;
    }
}

}
