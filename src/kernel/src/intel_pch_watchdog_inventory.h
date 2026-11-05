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
struct PchWatchdogStatus {uint16_t base=0,control=0,status2=0,timer=0;uint32_t smiEnable=0;};
// Read only fixed v2 TCO words after the exact device and decode have been
// checked, and independently cross-check the FADT PM timer address. The
// caller supplies serialized PCI reads and narrow 16-bit port reads. These
// observations cannot establish exclusive ownership or stop firmware/SMM
// from changing the configuration during or after the probe.
template<class Read32,class Read16,class ReadIo32>
bool PchWatchdogReadStatus(const PchWatchdogInventory& boot,bool timerAvailable,uint16_t timerPort,
                           Read32 pci,Read16 io,ReadIo32 io32,PchWatchdogStatus& out) {
    out={};PchWatchdogInventory before{},after{};
    if(!timerAvailable || !PchWatchdogReadStable(boot,pci,before) ||
       (before.pci40&0xffff007fu)!=1u || !(before.pci44&0x80u))return false;
    const uint32_t base=before.pci40&0xff80u;
    if(base<0x80 || base>0xff00 || timerPort!=base+8)return false;
    // SMI_EN is ACPIBASE+0x30 on this PCH. Sample twice; a stable TCO_EN
    // observation is still not proof that firmware has relinquished SMI.
    const uint32_t smi=io32(uint16_t(base+0x30));
    const uint16_t control=io(uint16_t(base+0x68)),status2=io(uint16_t(base+0x66)),timer=io(uint16_t(base+0x72));
    const uint32_t smiAgain=io32(uint16_t(base+0x30));
    if(control==0xffff || status2==0xffff || timer==0xffff ||
       smi==0xffffffffu || smi!=smiAgain ||
       !PchWatchdogReadStable(boot,pci,after) || before.pci40!=after.pci40 || before.pci44!=after.pci44)return false;
    out={uint16_t(base),control,status2,timer,smi};return true;
}
}
