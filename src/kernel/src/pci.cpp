#include "pci.h"
#include "serial.h"

namespace brook {

// ---- Port I/O ----

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

// ---- PCI config space via CF8/CFC ----

static constexpr uint16_t PCI_CONFIG_ADDR = 0xCF8;
static constexpr uint16_t PCI_CONFIG_DATA = 0xCFC;

static uint32_t MakeAddress(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    return (1u << 31)
        | (static_cast<uint32_t>(bus) << 16)
        | (static_cast<uint32_t>(dev & 0x1F) << 11)
        | (static_cast<uint32_t>(fn  & 0x07) <<  8)
        | (offset & 0xFC);
}

uint32_t PciConfigRead32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    outl(PCI_CONFIG_ADDR, MakeAddress(bus, dev, fn, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t PciConfigRead16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    uint32_t val = PciConfigRead32(bus, dev, fn, offset & ~3u);
    return static_cast<uint16_t>((val >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t PciConfigRead8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    uint32_t val = PciConfigRead32(bus, dev, fn, offset & ~3u);
    return static_cast<uint8_t>((val >> ((offset & 3) * 8)) & 0xFF);
}

void PciConfigWrite32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint32_t val)
{
    outl(PCI_CONFIG_ADDR, MakeAddress(bus, dev, fn, offset));
    outl(PCI_CONFIG_DATA, val);
}

void PciConfigWrite16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint16_t val)
{
    uint32_t cur = PciConfigRead32(bus, dev, fn, offset & ~3u);
    uint32_t shift = (offset & 2) * 8;
    cur = (cur & ~(0xFFFFu << shift)) | (static_cast<uint32_t>(val) << shift);
    PciConfigWrite32(bus, dev, fn, offset & ~3u, cur);
}

// ---- Device scan helpers ----

static void ReadDevice(uint8_t bus, uint8_t dev, uint8_t fn, PciDevice& out)
{
    out.bus      = bus;
    out.dev      = dev;
    out.fn       = fn;
    uint32_t id  = PciConfigRead32(bus, dev, fn, 0x00);
    out.vendorId = static_cast<uint16_t>(id & 0xFFFF);
    out.deviceId = static_cast<uint16_t>(id >> 16);
    uint32_t cls = PciConfigRead32(bus, dev, fn, 0x08);
    out.classCode = static_cast<uint8_t>(cls >> 24);
    out.subclass  = static_cast<uint8_t>(cls >> 16);
    out.progIf    = static_cast<uint8_t>(cls >>  8);
    for (int i = 0; i < 6; ++i)
        out.bar[i] = PciConfigRead32(bus, dev, fn, 0x10 + i * 4);
}

// ---- Public API ----

bool PciFindDevice(uint16_t vendorId, uint16_t deviceId, PciDevice& out)
{
    for (uint32_t bus = 0; bus < 256; ++bus)
    {
        for (uint32_t dev = 0; dev < 32; ++dev)
        {
            // Check if function 0 exists.
            uint32_t id0 = PciConfigRead32(bus, dev, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue; // no device

            // Check all functions (support multi-function devices).
            uint8_t hdrType = PciConfigRead8(bus, dev, 0, 0x0E);
            uint32_t fnCount = (hdrType & 0x80) ? 8u : 1u;

            for (uint32_t fn = 0; fn < fnCount; ++fn)
            {
                uint32_t id = PciConfigRead32(bus, dev, fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                if ((id & 0xFFFF) == vendorId && (id >> 16) == deviceId)
                {
                    ReadDevice(bus, dev, fn, out);
                    return true;
                }
            }
        }
    }
    return false;
}

void PciEnableBusMaster(const PciDevice& dev)
{
    uint16_t cmd = PciConfigRead16(dev.bus, dev.dev, dev.fn, 0x04);
    cmd |= (1u << 2) | (1u << 0); // Bus Master + I/O Space
    PciConfigWrite16(dev.bus, dev.dev, dev.fn, 0x04, cmd);
}

void PciEnableMemSpace(const PciDevice& dev)
{
    uint16_t cmd = PciConfigRead16(dev.bus, dev.dev, dev.fn, 0x04);
    cmd |= (1u << 2) | (1u << 1); // Bus Master + Memory Space
    PciConfigWrite16(dev.bus, dev.dev, dev.fn, 0x04, cmd);
}

void PciScanPrint()
{
    SerialPuts("PCI scan:\n");
    for (uint32_t bus = 0; bus < 256; ++bus)
    {
        for (uint32_t dev = 0; dev < 32; ++dev)
        {
            uint32_t id0 = PciConfigRead32(bus, dev, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;

            uint8_t hdrType = PciConfigRead8(bus, dev, 0, 0x0E);
            uint32_t fnCount = (hdrType & 0x80) ? 8u : 1u;

            for (uint32_t fn = 0; fn < fnCount; ++fn)
            {
                uint32_t id = PciConfigRead32(bus, dev, fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                uint32_t cls = PciConfigRead32(bus, dev, fn, 0x08);
                SerialPrintf("  %02x:%02x.%x  %04x:%04x  class %02x:%02x\n",
                             bus, dev, fn,
                             static_cast<unsigned>(id & 0xFFFF),
                             static_cast<unsigned>(id >> 16),
                             static_cast<unsigned>(cls >> 24),
                             static_cast<unsigned>((cls >> 16) & 0xFF));
            }
        }
    }
}

} // namespace brook
