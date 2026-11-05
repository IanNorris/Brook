#pragma once
#include <stdint.h>

namespace brook {
// Boot-time PCI configuration snapshot only. No TCO port, MMIO register, or
// watchdog write is accessed. Raw fields require chipset-specific review.
struct PchWatchdogInventory {
    bool captured=false;
    uint32_t pci40=0,pci44=0;
};
template<class Read32>
PchWatchdogInventory PchWatchdogCapture(uint8_t bus,uint8_t dev,uint8_t fn,
                                         uint16_t vendor,uint16_t device,Read32 read) {
    PchWatchdogInventory out{};
    if(bus!=0 || dev!=31 || fn!=0 || vendor!=0x8086 || device!=0x9cc3)return out;
    out.pci40=read(0x40);out.pci44=read(0x44);out.captured=true;return out;
}
// Require a boot-pinned identity and two consistent complete observations.
// Only PCI configuration is sampled; the values authorize no I/O access.
template<class Read32>
bool PchWatchdogReadStable(const PchWatchdogInventory& boot,Read32 read,PchWatchdogInventory& out) {
    out={};if(!boot.captured)return false;
    const uint32_t id1=read(0),a=read(0x40),b=read(0x44);
    const uint32_t id2=read(0),c=read(0x40),d=read(0x44);
    if(id1!=0x9cc38086u || id2!=id1 || a==0xffffffffu || b==0xffffffffu || a!=c || b!=d)return false;
    out={true,a,b};return true;
}
}
