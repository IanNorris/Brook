#pragma once

#include <stdint.h>

// Minimal PCI configuration space access via port I/O (x86 CF8/CFC mechanism).
// Supports legacy PCI (type 1) bus/device/function enumeration.

namespace brook {

// ---- Config space read/write (32-bit) ----

uint32_t PciConfigRead32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset);
uint16_t PciConfigRead16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset);
uint8_t  PciConfigRead8 (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset);
void     PciConfigWrite32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint32_t val);
void     PciConfigWrite16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint16_t val);

// ---- PCI device descriptor ----

struct PciDevice {
    uint8_t  bus, dev, fn;
    uint16_t vendorId;
    uint16_t deviceId;
    uint8_t  classCode;
    uint8_t  subclass;
    uint8_t  progIf;
    uint32_t bar[6];   // raw BAR values (unmasked)
};

// ---- BAR helpers ----

// Returns true if this BAR is an I/O space BAR (bit 0 set).
inline bool PciBarIsIo(uint32_t bar)     { return (bar & 1) != 0; }
// Returns true if this BAR is a 64-bit memory BAR (bits 2:1 == 0b10).
inline bool PciBarIs64(uint32_t bar)     { return (bar & 0x6) == 0x4; }
// Base I/O port for an I/O BAR.
inline uint16_t PciBarIoBase(uint32_t bar) { return static_cast<uint16_t>(bar & ~0x3u); }
// Base physical address for a 32-bit memory BAR.
inline uint32_t PciBarMemBase32(uint32_t bar) { return bar & ~0xFu; }

// ---- Enumeration ----

// Scan all PCI buses (0-255) for a device matching vendorId:deviceId.
// Returns true and fills 'out' on first match.
bool PciFindDevice(uint16_t vendorId, uint16_t deviceId, PciDevice& out);

// Enable Bus Master and I/O Space access in the PCI command register.
void PciEnableBusMaster(const PciDevice& dev);

// Enable Memory Space access in the PCI command register.
void PciEnableMemSpace(const PciDevice& dev);

// Print all devices found during a scan to serial (useful for debugging).
void PciScanPrint();

} // namespace brook
