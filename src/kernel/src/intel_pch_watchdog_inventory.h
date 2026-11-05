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
}
