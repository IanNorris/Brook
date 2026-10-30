#include "display_power.h"
#include "pci.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "boot_protocol/boot_protocol.h"
#include "apic.h"
#include "input.h"
#include "spinlock.h"
#include "serial.h"

namespace brook {
namespace {
DisplayBacklightController controller;
auto& backlight=controller.pwm;
auto& policy=controller.idle;
SpinLock lock;
volatile uint32_t* cpuPage=nullptr;
volatile uint32_t* pchPage=nullptr;
uint32_t Read(void*,uint32_t offset) {
    auto* page=offset<0xc0000?cpuPage:pchPage;
    return page[(offset&4095)/4];
}
void Write(void*,uint32_t offset,uint32_t value) {
    auto* page=offset<0xc0000?cpuPage:pchPage;
    page[(offset&4095)/4]=value;
}
BacklightIo io{nullptr,Read,Write};

bool DeviceRange(const BootProtocol* boot,uint64_t base) {
    if(!base || (base&4095) || base>~uint64_t{0}-4096)return false;
    for(uint32_t i=0;i<boot->memoryMapCount;++i){
        const auto& d=boot->memoryMap[i];
        if(d.pageCount>(~uint64_t{0}-d.physicalStart)/4096)return false;
        const uint64_t end=d.physicalStart+d.pageCount*4096;
        if(d.physicalStart>=base+4096 || end<=base)continue;
        if(d.type!=MemoryType::Mmio && d.type!=MemoryType::Reserved)return false;
    }
    return true;
}
volatile uint32_t* Map(uint64_t physical) {
    const auto address=VmmAllocPages(1,VMM_WRITABLE|VMM_NO_EXEC,MemTag::Device,KernelPid);
    if(!address)return nullptr;
    const auto leaf=VmmDetachPage(KernelPageTable,address);
    if(leaf)PmmFreePage(VmmLeafPhysical(leaf));
    if(!VmmMapPage(KernelPageTable,address,PhysicalAddress(physical),
        VMM_WRITABLE|VMM_NO_EXEC|VMM_CACHE_DISABLE|VMM_WRITE_THRU,MemTag::Device,KernelPid)){
        VmmFreePages(address,1);return nullptr;
    }
    // Boot-lifetime borrowed MMIO mapping, deliberately never VmmFreePages'd.
    return reinterpret_cast<volatile uint32_t*>(address.raw());
}
}
void DisplayPowerConfigure(const BootProtocol* boot) {
    if(!boot || cpuPage || pchPage)return;
    bool lpt=false;PciBootEntry gpu{};bool found=false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i){
        PciBootEntry e{};PciBootInventoryRead(i,&e);const auto& d=e.device;
        if(d.vendorId==0x8086 && d.deviceId==0x9cc3 && d.classCode==6 && d.subclass==1)lpt=true;
        if(d.vendorId==0x8086 && d.deviceId==0x1616 && d.classCode==3 &&
           e.subsystemVendor==0x1028 && e.subsystemDevice==0x0665 && !(e.headerType&0x7f)){
            gpu=e;found=true;
        }
    }
    if(!found || !lpt)return;
    const auto& d=gpu.device;
    if(!(gpu.command&2) || PciBarIsIo(d.bar[0]) || !PciBarIs64(d.bar[0])){
        backlight.reason="GPU MMIO decode/layout unavailable";return;
    }
    const uint64_t base=PciBarMemBase32(d.bar[0])|(uint64_t(d.bar[1])<<32);
    if(!base || base>~uint64_t{0}-0xc9000 || !DeviceRange(boot,base+0x48000) || !DeviceRange(boot,base+0xc8000)){
        backlight.reason="GPU register range overlaps RAM or is invalid";return;
    }
    cpuPage=Map(base+0x48000);pchPage=Map(base+0xc8000);
    if(!cpuPage || !pchPage){backlight.reason="Backlight register mapping failed";return;}
    backlight.Probe(io);
    policy.Reset(ApicTickCount(),InputLatestSequence());
    // Passive until explicit manual/idle commands; do not assume a live PWM
    // register is physically wired to the internal panel without observation.
    SerialPrintf("backlight: %s available=%u (idle disabled)\n",backlight.reason,backlight.ready?1u:0u);
}
DisplayPowerStatus DisplayPowerRead() {
    SpinLockAcquire(&lock);
    DisplayPowerStatus out{bool(cpuPage&&pchPage),backlight.ready,backlight.on,backlight.uncertain,policy.timeoutSeconds,
        policy.lastActivityMs,policy.restoreAtMs,backlight.reason,backlight.original,{}};
    if(out.mapped)out.current=backlight.Read(io);
    SpinLockRelease(&lock);return out;
}
bool DisplayPowerSetOn(bool on,uint32_t restoreSeconds) {
    SpinLockAcquire(&lock);
    const bool result=controller.SetOn(io,on,restoreSeconds,ApicTickCount(),InputLatestSequence());
    SpinLockRelease(&lock);return result;
}
bool DisplayPowerSetIdle(uint32_t seconds) {
    SpinLockAcquire(&lock);
    const bool result=controller.SetIdle(io,seconds,ApicTickCount(),InputLatestSequence());
    SpinLockRelease(&lock);return result;
}
void DisplayPowerTick(uint64_t now,uint64_t inputSequence) {
    if(!backlight.ready)return;
    SpinLockAcquire(&lock);
    controller.Tick(io,now,inputSequence);
    SpinLockRelease(&lock);
}
void DisplayPowerPanicRestore(){backlight.PanicRestore(io);}
}
