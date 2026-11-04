#include "intel_bdw_scanout.h"
#include "hda_dell_inventory.h"
#include "boot_protocol/boot_protocol.h"
#include <cassert>
using namespace brook;
static unsigned scenario=0;
namespace brook {
uint32_t PciBootInventoryCount(){return 16;}
uint32_t PciBootInventorySeen(){return scenario==1?17:16;}
bool PciBootInventoryRead(uint32_t index,PciBootEntry* out){
    if(index>=16)return false;*out={};const auto& e=HdaDellPciReferenceEntries[index];auto& d=out->device;
    d.bus=e.bus;d.dev=e.dev;d.fn=e.fn;d.vendorId=e.vendor;d.deviceId=e.device;
    d.classCode=e.cls;d.subclass=e.sub;d.progIf=e.prog;out->headerType=e.header;
    for(unsigned i=0;i<6;++i)d.bar[i]=e.bars[i];
    if(index==1){out->revision=9;out->subsystemVendor=0x1028;out->subsystemDevice=0x0665;
        out->command=scenario==2?0:2;out->bdwGraphics={true,0x01c1,0xdd800001,uint8_t(scenario==3?0:1)};}
    return true;
}
}
int main(){
    BootProtocol boot{};boot.framebuffer.physicalBase=0xe0001000;boot.framebuffer.width=16;
    boot.framebuffer.height=4;boot.framebuffer.stride=128;
    BdwGopObserveConfigure(&boot);assert(BdwGopObserveRead().planned && BdwGopObserveRead().plan.firstIndex==1);
    for(scenario=1;scenario<=3;++scenario){BdwGopObserveConfigure(&boot);assert(!BdwGopObserveRead().planned);}
    BdwGopObserveConfigure(nullptr);assert(!BdwGopObserveRead().planned);
}
