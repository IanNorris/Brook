#pragma once
#include <stdint.h>

namespace brook {
// Firmware configuration only. This deliberately does not map or touch GPU MMIO.
struct BdwFirmwareGraphics {
    bool captured;
    uint16_t graphicsControl;
    uint32_t stolenBase;
};

template<class Read16, class Read32>
BdwFirmwareGraphics BdwCaptureFirmwareGraphics(
    uint8_t bus, uint8_t dev, uint8_t fn, uint16_t vendor, uint16_t device,
    uint8_t revision, uint16_t subsystemVendor, uint16_t subsystemDevice,
    Read16 read16, Read32 read32) {
    if (bus != 0 || dev != 2 || fn != 0 || vendor != 0x8086 || device != 0x1616 ||
        revision != 9 || subsystemVendor != 0x1028 || subsystemDevice != 0x0665)
        return {};
    return {true, read16(0x50), read32(0x5c)};
}
}
