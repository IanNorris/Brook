#pragma once

#include "pci.h"
#include <stdint.h>

namespace brook::e1000 {

// Outside the shared INTx pool and distinct from AHCI MSI (64).
constexpr uint8_t I219MsiVector = 65, E1000eMsiVector = 66;

struct PciMsiConfig {
    const PciDevice& device;
    uint8_t read8(uint8_t off) { return PciConfigRead8(device.bus, device.dev, device.fn, off); }
    uint16_t read16(uint8_t off) { return PciConfigRead16(device.bus, device.dev, device.fn, off); }
    uint32_t read32(uint8_t off) { return PciConfigRead32(device.bus, device.dev, device.fn, off); }
    void write16(uint8_t off, uint16_t v) { PciConfigWrite16(device.bus, device.dev, device.fn, off, v); }
    void write32(uint8_t off, uint32_t v) { PciConfigWrite32(device.bus, device.dev, device.fn, off, v); }
};

constexpr uint16_t MsiEnable = 1u << 0;
constexpr uint16_t MsiMultipleEnabled = 7u << 4;
constexpr uint16_t MsiAddress64 = 1u << 7;
constexpr uint16_t MsiVectorMask = 1u << 8;
constexpr uint32_t InterruptTx = 1u << 0; // TXDW
constexpr uint32_t InterruptLsc = 1u << 2;
constexpr uint32_t InterruptReceive = (1u << 4) | (1u << 6) | (1u << 7);
constexpr uint32_t MsiCauses = InterruptTx | InterruptLsc | InterruptReceive;
constexpr uint32_t ModerationInterval = 195; // 256 ns units, about 50 us
constexpr uint32_t InterruptAutoMask = (1u << 27) | (1u << 24);

// Both devices use a single conventional MSI message. Walk conventional PCI
// capabilities defensively, including validating the extent of the MSI block.
template<typename Config, typename Install>
uint8_t EnableMsi(Config& config, uint8_t apic, uint8_t vector, Install install)
{
    if (!(config.read16(6) & (1u << 4)) || apic == 0xff) return 0;
    uint8_t offset = config.read8(0x34) & 0xfc;
    uint8_t cap = 0;
    uint64_t visited = 0;
    while (offset >= 0x40) {
        const uint64_t bit = uint64_t(1) << (offset / 4);
        if (visited & bit) return 0;
        visited |= bit;
        const uint8_t id = config.read8(offset);
        if (id == 0x11 && (config.read16(offset + 2) & 0x8000)) return 0;
        if (id == 0x05) {
            if (cap) return 0;
            cap = offset;
        }
        offset = config.read8(offset + 1) & 0xfc;
    }
    if (!cap) return 0;

    const uint16_t control = config.read16(cap + 2);
    const bool wide = control & MsiAddress64;
    const bool mask = control & MsiVectorMask;
    const unsigned data = cap + (wide ? 12 : 8);
    if (data + (mask ? 12 : 2) > 256) return 0;

    const uint16_t single = control & ~(MsiMultipleEnabled | MsiEnable);
    config.write16(cap + 2, single);
    install(vector);
    config.write32(cap + 4, 0xfee00000u | (uint32_t(apic) << 12));
    if (wide) config.write32(cap + 8, 0);
    config.write16(data, vector);
    if (mask) config.write32(data + 4, config.read32(data + 4) & ~1u);
    config.write16(cap + 2, single | MsiEnable);
    return (config.read16(cap + 2) & MsiEnable) ? cap : 0;
}

template<typename Config>
void DisableMsi(Config& config, uint8_t cap)
{
    if (cap) config.write16(cap + 2, config.read16(cap + 2) & ~MsiEnable);
}

}
