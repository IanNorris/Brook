#include "pci.h"
#include "portio.h"
#include "serial.h"
#include "spinlock.h"
#include "pci_config_access.h"

namespace brook {

static PciBootEntry g_bootInventory[256]{};
static uint32_t g_bootInventoryCount = 0, g_bootInventorySeen = 0;
static bool g_bootInventoryCaptured = false;

uint32_t PciBootInventoryCount() { return g_bootInventoryCount; }
uint32_t PciBootInventorySeen() { return g_bootInventorySeen; }
bool PciBootInventoryRead(uint32_t index, PciBootEntry* out)
{
    if (!out || index >= g_bootInventoryCount) return false;
    *out = g_bootInventory[index];
    return true;
}

// ---- PCI config space via CF8/CFC ----

namespace {
IrqSpinLock pciConfigLock;
struct ConfigLock {
    uint64_t Acquire(){return IrqSpinLockAcquire(&pciConfigLock);}
    void Release(uint64_t flags){IrqSpinLockRelease(&pciConfigLock,flags);}
} configLock;
struct ConfigIo {
    void out32(uint16_t port,uint32_t value){outl(port,value);}
    uint32_t in32(uint16_t port){return inl(port);}
    void out16(uint16_t port,uint16_t value){outw(port,value);}
} configIo;
}

uint32_t PciConfigRead32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    return PciConfigAccess<ConfigLock,ConfigIo>(configLock,configIo).Read32(bus,dev,fn,offset);
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
    PciConfigAccess<ConfigLock,ConfigIo>(configLock,configIo).Write32(bus,dev,fn,offset,val);
}

void PciConfigWrite16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint16_t val)
{
    PciConfigAccess<ConfigLock,ConfigIo>(configLock,configIo).Write16(bus,dev,fn,offset,val);
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

// Internal: scan from (startBus, startDev, startFn), exclusive of the start position.
static bool ScanFrom(uint16_t vendorId, uint16_t deviceId,
                     uint32_t startBus, uint32_t startDev, uint32_t startFn,
                     bool skipStart, PciDevice& out)
{
    for (uint32_t bus = startBus; bus < 256; ++bus)
    {
        uint32_t devBegin = (bus == startBus) ? startDev : 0;
        for (uint32_t dev = devBegin; dev < 32; ++dev)
        {
            uint32_t id0 = PciConfigRead32(bus, dev, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;

            uint8_t hdrType = PciConfigRead8(bus, dev, 0, 0x0E);
            uint32_t fnCount = (hdrType & 0x80) ? 8u : 1u;

            uint32_t fnBegin = (bus == startBus && dev == startDev) ? startFn : 0;
            for (uint32_t fn = fnBegin; fn < fnCount; ++fn)
            {
                // Skip the exact start position when requested (for FindNext).
                if (skipStart && bus == startBus && dev == startDev && fn == startFn)
                    continue;

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

bool PciFindDevice(uint16_t vendorId, uint16_t deviceId, PciDevice& out)
{
    return ScanFrom(vendorId, deviceId, 0, 0, 0, false, out);
}

bool PciFindNextDevice(uint16_t vendorId, uint16_t deviceId,
                       const PciDevice& after, PciDevice& out)
{
    return ScanFrom(vendorId, deviceId,
                    after.bus, after.dev, after.fn, /*skipStart=*/true, out);
}

void PciEnumerate(uint16_t vendorId, uint16_t deviceId,
                  bool (*cb)(const PciDevice& dev, void* ctx), void* ctx)
{
    PciDevice cur;
    if (!PciFindDevice(vendorId, deviceId, cur)) return;
    for (;;)
    {
        if (!cb(cur, ctx)) return;
        PciDevice next;
        if (!PciFindNextDevice(vendorId, deviceId, cur, next)) return;
        cur = next;
    }
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
                if (!g_bootInventoryCaptured) {
                    ++g_bootInventorySeen;
                    if (g_bootInventoryCount < sizeof(g_bootInventory)/sizeof(g_bootInventory[0])) {
                        auto& entry = g_bootInventory[g_bootInventoryCount++];
                        ReadDevice(bus, dev, fn, entry.device);
                        entry.command = PciConfigRead16(bus, dev, fn, 0x04);
                        entry.revision = static_cast<uint8_t>(cls);
                        entry.headerType = PciConfigRead8(bus, dev, fn, 0x0e);
                        if ((entry.headerType & 0x7f) == 0) {
                            entry.subsystemVendor = PciConfigRead16(bus, dev, fn, 0x2c);
                            entry.subsystemDevice = PciConfigRead16(bus, dev, fn, 0x2e);
                            entry.bdwGraphics = BdwCaptureFirmwareGraphics(bus, dev, fn,
                                entry.device.vendorId, entry.device.deviceId, entry.revision,
                                entry.subsystemVendor, entry.subsystemDevice,
                                [=](uint8_t offset) { return PciConfigRead16(bus, dev, fn, offset); },
                                [=](uint8_t offset) { return PciConfigRead32(bus, dev, fn, offset); },
                                [=](uint8_t offset) { return PciConfigRead8(bus, dev, fn, offset); });
                            entry.pchWatchdog = PchWatchdogCapture(bus,dev,fn,entry.device.vendorId,entry.device.deviceId,
                                [=](uint8_t offset) { return PciConfigRead32(bus,dev,fn,offset); });
                        }
                    }
                }
                SerialPrintf("  %02x:%02x.%x  %04x:%04x  class %02x:%02x\n",
                             bus, dev, fn,
                             static_cast<unsigned>(id & 0xFFFF),
                             static_cast<unsigned>(id >> 16),
                             static_cast<unsigned>(cls >> 24),
                             static_cast<unsigned>((cls >> 16) & 0xFF));
            }
        }
    }
    g_bootInventoryCaptured = true;
}

} // namespace brook
