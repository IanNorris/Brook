#include "intel_bdw_scanout.h"
#include "hda_dell_inventory.h"
#include "boot_protocol/boot_protocol.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_forcewake_adapter.h"
#include "native_tsc_observe.h"
#include "acpi.h"
#include "vfs.h"
#include <cassert>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
using namespace brook;
static unsigned scenario=0,reads=0,ackReads=0,pageChecks=0,wakeRuns=0,prepares=0;static Vnode marker;
namespace brook {
bool BdwPreparePrivateImages(){++prepares;return scenario!=28;}
extern "C" void SerialPuts(const char*){}
extern "C" void SerialPrintf(const char*,...){}
extern "C" Vnode* VfsOpen(const char* p,int){return ((scenario>=4 && scenario<=13 && !std::strcmp(p,"/etc/dell-gpu-gtt-probe")) || (scenario>=14 && scenario<=18 && !std::strcmp(p,"/etc/dell-gpu-forcewake-ack")) || (scenario>=19 && scenario<=22 && !std::strcmp(p,"/etc/dell-gpu-forcewake-run")) || (scenario>=23 && scenario<=26 && !std::strcmp(p,"/etc/dell-gpu-engines-run")) || (scenario>=27 && !std::strcmp(p,"/etc/dell-gpu-prepare-only")))?&marker:nullptr;}
void VfsRelease(Vnode*){}
int VfsStat(Vnode*,VnodeStat* s){*s={};s->size=2;return 0;}
extern "C" int VfsRead(Vnode*,void* out,uint64_t n,uint64_t*){if(n!=2)return -1;std::memcpy(out,"1\n",2);return 2;}
uint32_t SmpCurrentCpuIndex(){return scenario==7 || scenario==16?1:0;}
extern "C" uint8_t PciConfigRead8(uint8_t,uint8_t,uint8_t,uint8_t o){return o==0x34?0x40:o==0x40 || o==0x62?1:0;}
extern "C" uint16_t PciConfigRead16(uint8_t,uint8_t,uint8_t,uint8_t o){return o==6?(scenario==13?0xffff:0x10):o==4?(scenario==12 || scenario==18?0xffff:2):o==0x50?0x01c1:o==0x42?(scenario==9?0:1):o==0x44 && (scenario==5 || scenario==22 || scenario==26)?3:0;}
extern "C" uint32_t PciConfigRead32(uint8_t,uint8_t,uint8_t,uint8_t o){
    if(o==0)return 0x16168086;if(o==0x2c)return 0x06651028;if(o==8)return 9;
    if(o>=0x10 && o<0x20)return HdaDellPciReferenceEntries[1].bars[(o-0x10)/4]^(scenario==10 && o==0x10?0x1000000:0);
    return 0;
}
bool AcpiClassifyBootRange(uint64_t page,uint32_t bytes,BootResourceClassification& c){++pageChecks;
    assert(bytes==4096 && page==(scenario>=19?(pageChecks<=4?BdwForcewakePages[pageChecks-1]:BdwOtherEnginePages[pageChecks-5]):scenario>=14?0xf6130000ull:pageChecks==1?0xf6800000ull:0xf680a000ull));
    c={scenario==11 && pageChecks==2?3u:2u,false};return scenario!=6 && scenario!=15 && !(scenario==20 && pageChecks==4) && !(scenario==24 && pageChecks==7);}
bool VmmDellGopPtesCriticalRead(const BdwGopPtePlan& plan,BdwGopPtes& p){assert(plan.firstIndex==0 && plan.lastIndex==5624 && plan.firstAddress==0xf6800000ull && plan.lastAddress==0xf680afc0ull);++reads;if(scenario==8)return false;p={5,6};return true;}
const NativeTscObservation& NativeTscObserveRead(){static NativeTscObservation o{};o.capable=o.measured=o.consistent=scenario!=21 && scenario!=25;o.khz=1000;return o;}
void VmmDellForcewakeCriticalRun(uint32_t khz,BdwForcewakeAdapterResult& r,bool other) {assert(khz==1000 && other==(scenario>=23));++wakeRuns;r.aliases=r.clock=true;r.sequence.acquired=r.sequence.released=true;}
bool VmmDellForcewakeAckCriticalRead(uint32_t& value){++ackReads;if(scenario==17)return false;value=0;return true;}
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
    BootProtocol boot{};boot.framebuffer.physicalBase=0xe0000000;boot.framebuffer.width=3200;
    boot.framebuffer.height=1800;boot.framebuffer.stride=12800;
    BdwGopObserveConfigure(&boot);assert(BdwGopObserveRead().planned && BdwGopObserveRead().plan.firstIndex==0);
    for(scenario=1;scenario<=3;++scenario){BdwGopObserveConfigure(&boot);assert(!BdwGopObserveRead().planned);}
    BdwGopObserveConfigure(nullptr);assert(!BdwGopObserveRead().planned);
    for(unsigned n=4;n<=13;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;reads=0;pageChecks=0;BdwGopObserveConfigure(&boot);
        const auto& r=BdwGopObserveRead();assert(r.probeRequested && r.planned);
        if(n==4)assert(r.sampled && reads==1 && r.firstPte==5 && r.lastPte==6);
        else assert(!r.sampled && reads==(n==8?1u:0u));
        BdwGopObserveConfigure(&boot);assert(reads==(n==4 || n==8?1u:0u));_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=14;n<=18;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;ackReads=0;pageChecks=0;BdwGopObserveConfigure(&boot);
        const auto& r=BdwGopObserveRead();assert(r.ackRequested && !r.probeRequested);
        assert(r.ackSampled==(n==14) && ackReads==(n==14 || n==17?1u:0u));
        BdwGopObserveConfigure(&boot);assert(ackReads==(n==14 || n==17?1u:0u));_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=19;n<=22;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;wakeRuns=0;pageChecks=0;BdwGopObserveConfigure(&boot);
        const auto& r=BdwGopObserveRead();assert(r.wakeRequested && !r.ackRequested && !r.probeRequested && !r.otherEnginePagesAdmitted);
        assert(wakeRuns==(n==19?1u:0u));if(n==19)assert(r.wake.acquired && r.wake.released);
        BdwGopObserveConfigure(&boot);assert(wakeRuns==(n==19?1u:0u));_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=23;n<=26;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;wakeRuns=0;pageChecks=0;BdwGopObserveConfigure(&boot);
        const auto& r=BdwGopObserveRead();assert(r.wakeRequested && r.otherEnginesRequested && pageChecks==7);
        assert(wakeRuns==(n==23?1u:0u));BdwGopObserveConfigure(&boot);assert(wakeRuns==(n==23?1u:0u));_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=27;n<=28;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;prepares=0;pageChecks=0;BdwGopObserveConfigure(&boot);
        const auto& r=BdwGopObserveRead();assert(r.prepareRequested && r.prepareBuilt==(n==27) && prepares==1 && pageChecks==0);
        BdwGopObserveConfigure(&boot);assert(prepares==1);_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
}
