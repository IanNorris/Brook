#pragma once
#include <stdint.h>

namespace brook {
enum class UsbPointerStage : uint8_t { Queued, Complete, Rearm, Report, Ignored };
enum class UsbPointerIgnore : uint8_t { None, Inactive, TransferError, Length, Idle, SharedButton, RingFull };

struct UsbPointerEndpoint {
    char port[24];
    uint16_t vendor, product;
    uint8_t controller, slot, endpoint, speed, ttSlot, ttPort;
    uint8_t interval, esitExponent, requestedBytes;
    bool bootProtocol;
};

// Copies the endpoint and stamps the event without printing or allocating.
// Safe from the xHCI interrupt handler with its event-ring lock held.
extern "C" void BootTimingUsbPointerTransfer(const UsbPointerEndpoint& endpoint,
    UsbPointerStage stage, uint8_t cc, uint32_t residue, UsbPointerIgnore reason);
}
