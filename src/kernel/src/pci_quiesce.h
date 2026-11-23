#pragma once
#include <stdint.h>

namespace brook {
// Pre-reset PCI function quiesce, as Linux's pci_disable_device plus
// free_irq leave a function: bus mastering off, INTx disabled, MSI and MSI-X
// disabled. Memory/I/O decode stays on so firmware (and a driver's own last
// register reads) still reach the BARs. The capability walk is bounded.
// Config is a type with read8/read16/write16(offset[,value]).
template <typename Config>
uint32_t PciQuiesceForReset(Config& config)
{
    uint32_t changes = 0;
    if (config.read16(0) == 0xffff) return 0;
    const uint16_t command = config.read16(4);
    const uint16_t quiet = uint16_t((command & ~uint16_t(1u << 2)) | uint16_t(1u << 10));
    if (quiet != command) { config.write16(4, quiet); ++changes; }
    if (!(config.read16(6) & (1u << 4))) return changes;
    uint8_t offset = uint8_t(config.read8(0x34) & 0xfc);
    for (unsigned guard = 0; offset >= 0x40 && guard < 48; ++guard) {
        const uint8_t id = config.read8(offset);
        if (id == 0x05) {
            const uint16_t control = config.read16(uint8_t(offset + 2));
            if (control & 1u) { config.write16(uint8_t(offset + 2), uint16_t(control & ~1u)); ++changes; }
        } else if (id == 0x11) {
            const uint16_t control = config.read16(uint8_t(offset + 2));
            if (control & 0x8000u) { config.write16(uint8_t(offset + 2), uint16_t(control & ~0x8000u)); ++changes; }
        }
        offset = uint8_t(config.read8(uint8_t(offset + 1)) & 0xfc);
    }
    return changes;
}
// Kernel wrapper over CF8 configuration access (exported to modules).
extern "C" uint32_t PciQuiesceFunctionForReset(uint8_t bus, uint8_t dev, uint8_t fn);
}
