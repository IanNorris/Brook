#include "intel_bdw_scanout.h"
#include "hda_dell_inventory.h"
#include "boot_protocol/boot_protocol.h"
namespace brook {
namespace { BdwGopObservation observation{}; }
void BdwGopObserveConfigure(const BootProtocol* boot) {
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
        return;
    }
}
const BdwGopObservation& BdwGopObserveRead(){return observation;}
}
