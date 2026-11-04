#include "intel_vtd_observe.h"
#include "intel_dmar_gpu.h"
#include "hda_dell_inventory.h"
#include "acpi.h"
#include "boot_protocol/boot_protocol.h"
#include "memory/virtual_memory.h"
#include "smp.h"
#include "vfs.h"
#include "serial.h"

namespace brook {
namespace {
DellGpuVtdObservation observation{};
bool configured=false;
bool OptedIn() {
    auto* file=VfsOpen("/etc/dell-gpu-vtd-probe",VFS_O_STREAM);if(!file)return false;
    VnodeStat st{};uint64_t offset=0;char text[2]{};
    const bool ok=!VfsStat(file,&st) && !st.isDir && !st.isSymlink && st.size==2 &&
        VfsRead(file,text,2,&offset)==2 && text[0]=='1' && text[1]=='\n';
    VfsRelease(file);return ok;
}
bool ExactIdentity() {
    bool gpu=false,bridge=false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i){PciBootEntry e{};if(!PciBootInventoryRead(i,&e))return false;
        const auto& d=e.device;
        if(d.bus==0 && d.dev==2 && d.fn==0 && d.vendorId==0x8086 && d.deviceId==0x1616 &&
           e.revision==9 && e.subsystemVendor==0x1028 && e.subsystemDevice==0x0665)gpu=true;
        if(d.bus==0 && d.dev==0 && d.fn==0 && d.vendorId==0x8086 && d.deviceId==0x1604 &&
           e.revision==9 && e.subsystemVendor==0x1028 && e.subsystemDevice==0x0665)bridge=true;
    }
    return gpu && bridge;
}
}
void DellGpuVtdObserveConfigure() {
    if(configured)return;configured=true;
    if(!OptedIn())return;
    observation.requested=true;
    if(SmpCurrentCpuIndex()!=0)return;
    uint8_t dmar[212]{};uint32_t size=0;
    observation.topology=AcpiUniqueDmarCopy(dmar,sizeof(dmar),size) && DmarAuditedDellGpuUnit(dmar,size).found;
    observation.inventory=HdaDellToneInventoryMatches() && ExactIdentity();
    BootResourceClassification map{};
    constexpr uint32_t allowed=(1u<<uint32_t(MemoryType::Reserved))|(1u<<uint32_t(MemoryType::Mmio));
    observation.bootMapClassified=AcpiClassifyBootRange(DellGpuVtdPage,4096,map);
    observation.bootMapTypes=map.types;observation.bootMapUnreported=map.unreported;
    observation.bootMap=observation.bootMapClassified && !map.unreported && map.types && !(map.types&~allowed);
    if(observation.topology && observation.inventory && observation.bootMap) {
        SerialPuts("gpu-vtd: admitted; sampling version and status\n");
        observation.sampled=VmmDellGpuVtdCriticalRead(observation.status);
    }
    SerialPrintf("gpu-vtd: topology=%u inventory=%u bootmap=%u sampled=%u\n",observation.topology,observation.inventory,observation.bootMap,observation.sampled);
}
const DellGpuVtdObservation& DellGpuVtdObserveRead(){return observation;}
}
