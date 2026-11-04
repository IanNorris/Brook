#include "intel_bdw_scanout.h"
#include "hda_dell_inventory.h"
#include "boot_protocol/boot_protocol.h"
#include "intel_bdw_gtt_probe.h"
#include "memory/virtual_memory.h"
#include "acpi.h"
#include "smp.h"
#include "serial.h"
#include "vfs.h"
namespace brook {
namespace {
BdwGopObservation observation{};
bool probeAttempted=false;
bool OptedIn(){auto* f=VfsOpen("/etc/dell-gpu-gtt-probe",VFS_O_STREAM);if(!f)return false;
    VnodeStat s{};uint64_t offset=0;char t[2]{};bool ok=!VfsStat(f,&s) && !s.isDir && !s.isSymlink && s.size==2 &&
        VfsRead(f,t,2,&offset)==2 && t[0]=='1' && t[1]=='\n';VfsRelease(f);return ok;}
bool PowerD0(const PciDevice& d){
    const uint16_t status=PciConfigRead16(d.bus,d.dev,d.fn,6);
    if(status==0xffff || !(status&0x10))return false;
    uint8_t cap=PciConfigRead8(d.bus,d.dev,d.fn,0x34);uint64_t seen=0;
    for(unsigned n=0;n<48 && cap;++n){
        if(cap<0x40 || (cap&3) || cap>0xf8)return false;
        const uint64_t bit=uint64_t(1)<<(cap/4);if(seen&bit)return false;seen|=bit;
        if(PciConfigRead8(d.bus,d.dev,d.fn,cap)==1){
            const uint16_t pmc=PciConfigRead16(d.bus,d.dev,d.fn,cap+2),pmcsr=PciConfigRead16(d.bus,d.dev,d.fn,cap+4);
            return pmc!=0xffff && (pmc&7)>=1 && (pmc&7)<=3 && pmcsr!=0xffff && !(pmcsr&3);
        }
        cap=PciConfigRead8(d.bus,d.dev,d.fn,cap+1);
    }return false;
}
bool PageAdmitted(uint64_t page){BootResourceClassification c{};
    constexpr uint32_t allowed=(1u<<uint32_t(MemoryType::Reserved))|(1u<<uint32_t(MemoryType::Mmio));
    if(!AcpiClassifyBootRange(page,4096,c))return false;
    return (!c.types && c.unreported) || (!c.unreported && c.types && !(c.types&~allowed));
}
bool LiveGpuMatches(const PciBootEntry& e){const auto& d=e.device;
    const uint16_t command=PciConfigRead16(d.bus,d.dev,d.fn,4);
    if(PciConfigRead32(d.bus,d.dev,d.fn,0)!=(uint32_t(d.deviceId)<<16|d.vendorId) ||
       PciConfigRead32(d.bus,d.dev,d.fn,0x2c)!=(uint32_t(e.subsystemDevice)<<16|e.subsystemVendor) ||
       (PciConfigRead32(d.bus,d.dev,d.fn,8)&255)!=e.revision || command==0xffff || !(command&2) ||
       PciConfigRead16(d.bus,d.dev,d.fn,0x50)!=e.bdwGraphics.graphicsControl ||
       PciConfigRead8(d.bus,d.dev,d.fn,0x62)!=e.bdwGraphics.apertureControl)return false;
    for(unsigned i=0;i<4;++i)if(PciConfigRead32(d.bus,d.dev,d.fn,0x10+i*4)!=d.bar[i])return false;
    return true;
}
}
void BdwGopObserveConfigure(const BootProtocol* boot) {
    if(probeAttempted)return;
    observation={};
    if(!boot)return;
    observation.inventory=HdaDellToneInventoryMatches();
    if(!observation.inventory)return;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry e{};if(!PciBootInventoryRead(i,&e))return;
        const auto& d=e.device;
        if(d.bus!=0 || d.dev!=2 || d.fn!=0 || !e.bdwGraphics.captured || !(e.command&2))continue;
        if(e.revision!=9 || e.subsystemVendor!=0x1028 || e.subsystemDevice!=0x0665 ||
           PciBarIsIo(d.bar[0]) || !PciBarIs64(d.bar[0]) || PciBarIsIo(d.bar[2]) || !PciBarIs64(d.bar[2]))return;
        const uint64_t bar0=PciBarMemBase32(d.bar[0])|(uint64_t(d.bar[1])<<32);
        const uint64_t bar2=PciBarMemBase32(d.bar[2])|(uint64_t(d.bar[3])<<32);
        const auto& f=boot->framebuffer;
        observation.planned=BdwPlanGopPtes(bar0,16ull<<20,bar2,256ull<<20,e.bdwGraphics.apertureControl,
            e.bdwGraphics.graphicsControl,f.physicalBase,f.width,f.height,f.stride,observation.plan);
        observation.probeRequested=OptedIn();
        if(!observation.probeRequested)return;
        probeAttempted=true;
        if(!observation.planned || SmpCurrentCpuIndex()!=0)return;
        observation.powerD0=LiveGpuMatches(e) && PowerD0(d);
        observation.pagesAdmitted=PageAdmitted(observation.plan.firstAddress&~4095ull) &&
            PageAdmitted(observation.plan.lastAddress&~4095ull);
        if(observation.powerD0 && observation.pagesAdmitted){
            SerialPuts("gpu-gtt: admitted; sampling GOP endpoint entries\n");BdwGopPtes p{};
            observation.sampled=VmmDellGopPtesCriticalRead(observation.plan,p);
            if(observation.sampled){observation.firstPte=p.first;observation.lastPte=p.last;}
        }
        SerialPrintf("gpu-gtt: power=%u pages=%u sampled=%u\n",observation.powerD0,observation.pagesAdmitted,observation.sampled);
        return;
    }
}
const BdwGopObservation& BdwGopObserveRead(){return observation;}
}
