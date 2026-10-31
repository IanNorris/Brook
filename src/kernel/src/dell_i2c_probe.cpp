#include "dell_i2c_probe.h"
#include "dell_audio_probe.h"
#include "acpi.h"
#include "pci.h"
#include "vfs.h"
#include "memory/virtual_memory.h"
namespace brook {
namespace {
DellI2cIdOneShot once;
bool OptedIn() {
    auto* node=VfsOpen("/etc/dell-i2c-id-probe",VFS_O_STREAM);if(!node)return false;
    VnodeStat st{};uint64_t offset=0;char marker[2]{};
    const bool ok=!VfsStat(node,&st) && !st.isDir && !st.isSymlink && st.size==2 &&
        VfsRead(node,marker,2,&offset)==2 && marker[0]=='1' && marker[1]=='\n';
    VfsRelease(node);return ok;
}
bool PciEvidence(bool& complete,bool& noBase) {
    const uint32_t count=PciBootInventoryCount();complete=count && count==PciBootInventorySeen();
    noBase=complete;bool gpu=false,lpc=false;
    for(uint32_t i=0;i<count;++i) {
        PciBootEntry e{};if(!PciBootInventoryRead(i,&e)){complete=false;noBase=false;return false;}
        const auto& d=e.device;
        if(d.vendorId==0x8086 && d.deviceId==0x1616 && e.subsystemVendor==0x1028 && e.subsystemDevice==0x0665 &&
           d.bus==0 && d.dev==2 && d.fn==0)gpu=true;
        if(d.vendorId==0x8086 && d.deviceId==0x9cc3 && d.bus==0 && d.dev==31 && d.fn==0)lpc=true;
        const uint8_t header=e.headerType&0x7f;const unsigned limit=header==0?6:header==1?2:0;
        if(!limit){complete=false;noBase=false;return false;}
        for(unsigned bar=0;bar<limit;++bar) {
            const uint32_t raw=d.bar[bar];if(!raw || (raw&1))continue;
            const uint32_t kind=raw&6;uint64_t base=raw&~15u;
            if(kind==4){if(bar+1>=limit){complete=false;noBase=false;return false;}base|=uint64_t(d.bar[++bar])<<32;}
            else if(kind!=0){complete=false;noBase=false;return false;}
            if(base>=DellI2c0Page && base<DellI2c0Page+4096)noBase=false;
        }
    }
    return gpu && lpc;
}
struct Io {bool criticalRead(uint32_t& value){return VmmDellI2cIdCriticalRead(value);}};
}
DellI2cIdResult DellI2cIdProbe(uint32_t& value) {
    DellI2cIdPrerequisites p{};p.optedIn=OptedIn();
    if(p.optedIn) {
        DellAudioFirmwareSnapshot snapshot{};const char* reason=nullptr;
        const bool firmware=DellAudioProbeRead(snapshot,reason);bool complete=false,noBase=false;
        const bool model=PciEvidence(complete,noBase);BootResourceClassification c{};
        p.reviewedFirmware=firmware && model && snapshot.pchGeneration==2 && snapshot.codecSelection==1;
        p.stableNvs=firmware;p.controller=snapshot.i2cBase0;p.privatePage=snapshot.i2cBase1;p.mode=snapshot.i2cMode;
        p.bootMapHole=AcpiClassifyBootRange(DellI2c0Page,4096,c) && !c.types && c.unreported;
        p.pciInventoryComplete=complete;p.noPciBaseInPage=noBase;
    }
    Io io;return once.Run(p,io,value);
}
DellI2cIdStatus DellI2cIdProbeStatus(){return once.Status();}
}
