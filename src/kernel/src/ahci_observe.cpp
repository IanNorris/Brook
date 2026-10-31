#include "ahci_observe.h"
#include "pci.h"
#include "vfs.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "boot_protocol/boot_protocol.h"

namespace brook {
namespace {
AhciObservation observation{};
volatile uint32_t* page{};

bool SafeRange(const BootProtocol* boot, uint64_t base) {
    if (!boot || !base || (base & 4095u) || base > ~uint64_t{0} - 4096) return false;
    for (uint32_t i = 0; i < boot->memoryMapCount; ++i) {
        const auto& d = boot->memoryMap[i];
        if (d.pageCount > (~uint64_t{0} - d.physicalStart) / 4096) return false;
        const uint64_t end = d.physicalStart + d.pageCount * 4096;
        if (d.physicalStart >= base + 4096 || end <= base) continue;
        if (d.type != MemoryType::Mmio && d.type != MemoryType::Reserved) return false;
    }
    return true;
}

volatile uint32_t* MapPage(uint64_t physical) {
    const auto address = VmmAllocPages(1, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
    if (!address) return nullptr;
    const auto leaf = VmmDetachPage(KernelPageTable, address);
    if (leaf) PmmFreePage(VmmLeafPhysical(leaf));
    if (!VmmMapPage(KernelPageTable, address, PhysicalAddress(physical),
                    VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE | VMM_WRITE_THRU,
                    MemTag::Device, KernelPid)) {
        VmmFreePages(address, 1); return nullptr;
    }
    // Boot-lifetime borrowed MMIO mapping; it must not free the device page.
    return reinterpret_cast<volatile uint32_t*>(address.raw());
}
uint32_t Read(uint32_t offset) { return page[offset / 4]; }
}

void AhciObserveConfigure(const BootProtocol* boot) {
    auto* marker = VfsOpen("/etc/ahci-observe");
    if (!marker) return;
    VfsClose(marker); observation.requested = true; observation.reason = "AHCI controller not found";
    PciBootEntry chosen{};
    for (uint32_t i = 0; i < PciBootInventoryCount(); ++i) {
        PciBootEntry e{}; if (!PciBootInventoryRead(i, &e)) continue;
        const auto& d = e.device;
        if (d.classCode != 1 || d.subclass != 6 || d.progIf != 1 || (e.headerType & 0x7f)) continue;
        if (!observation.controllerCount) chosen = e;
        ++observation.controllerCount;
    }
    if (!observation.controllerCount) return;
    const auto& d = chosen.device;
    observation.vendor=d.vendorId; observation.device=d.deviceId;
    observation.bus=d.bus; observation.slot=d.dev; observation.function=d.fn;
    if (!AhciObserveBar(d.bar[5], observation.bar) || !SafeRange(boot, observation.bar)) {
        observation.reason="invalid or unsafe AHCI BAR"; return;
    }
    page=MapPage(observation.bar);
    if (!page) { observation.reason="AHCI mapping failed"; return; }
    struct Io {
        const PciDevice& device;
        uint16_t command() {return PciConfigRead16(device.bus,device.dev,device.fn,4);}
        // The adjacent PCI status word contains write-one-to-clear bits.
        // PciConfigWrite16 currently performs a 32-bit read/modify/write and
        // would echo those status bits; write zero to the status half instead.
        void command(uint16_t value) {PciConfigWrite32(device.bus,device.dev,device.fn,4,value);}
        uint32_t read(uint32_t offset) {return Read(offset);}
    } io{d};
    AhciObserveRegisters(io,observation);
}
const AhciObservation& AhciObserveRead() { return observation; }
} // namespace brook
