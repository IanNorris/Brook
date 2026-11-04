#pragma once
#include <stdint.h>

namespace brook {
// Firmware configuration only. This deliberately does not map or touch GPU MMIO.
struct BdwFirmwareGraphics {
    bool captured;
    uint16_t graphicsControl;
    uint32_t stolenBase;
    uint8_t apertureControl=0;
};

// A descriptive firmware range only: this does not reserve memory, validate
// ownership, or authorize allocation. End is exclusive and calculated in 64 bits.
struct BdwStolenRange {
    bool valid;
    uint64_t base, size, end;
};
inline BdwStolenRange BdwDecodeStolenRange(const BdwFirmwareGraphics& raw) {
    if (!raw.captured || raw.graphicsControl == 0xffff || raw.stolenBase == 0xffffffff)
        return {};
    const uint32_t gms = raw.graphicsControl >> 8;
    // Mobile fifth-generation Core GMS encodings above 0x3f are reserved.
    if (gms > 0x3f) return {};
    const uint64_t size = uint64_t(gms) << 25;
    const uint64_t base = uint64_t(raw.stolenBase & 0xfff00000u);
    if (!size || !base || base + size > (uint64_t{1} << 32)) return {};
    return {true, base, size, base + size};
}

template<class Read16, class Read32, class Read8>
BdwFirmwareGraphics BdwCaptureFirmwareGraphics(
    uint8_t bus, uint8_t dev, uint8_t fn, uint16_t vendor, uint16_t device,
    uint8_t revision, uint16_t subsystemVendor, uint16_t subsystemDevice,
    Read16 read16, Read32 read32, Read8 read8) {
    if (bus != 0 || dev != 2 || fn != 0 || vendor != 0x8086 || device != 0x1616 ||
        revision != 9 || subsystemVendor != 0x1028 || subsystemDevice != 0x0665)
        return {};
    return {true, read16(0x50), read32(0x5c), read8(0x62)};
}
}
