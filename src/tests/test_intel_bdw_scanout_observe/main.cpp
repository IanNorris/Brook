#include "intel_bdw_scanout.h"
#include "hda_dell_inventory.h"
#include "boot_protocol/boot_protocol.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_forcewake_adapter.h"
#include "intel_bdw_oneshot.h"
#include "intel_bdw_display_probe.h"
#include "intel_bdw_exec_snapshot.h"
#include "intel_bdw_render_gt_init.h"
#include "native_tsc_observe.h"
#include "acpi.h"
#include "vfs.h"
#include <cassert>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "intel_bdw_submission_worker.h"
#include <sys/wait.h>
#include <unistd.h>
using namespace brook;
static unsigned scenario=0,reads=0,ackReads=0,pageChecks=0,wakeRuns=0,prepares=0,renderPrepares=0,retains=0,renderRetains=0,ggttReads=0,heapFrees=0;static Vnode marker;
static unsigned gemReleases=0,gemFailures=0,contendersRefused=0,workerPrepared=0,workerRetired=0;
static uint32_t contextValues[2]{},reprepareValues[3]{};static bool contextActive[2]{},contextPoisoned[2]{};static unsigned reprepareCount=0;
namespace brook {
extern "C" [[noreturn]] void KernelPanic(const char* format,...){
    if(scenario==85 && !std::strncmp(format,"Broadwell worker recovery stage=%s",34)){
        va_list args;va_start(args,format);
        const char* stage=va_arg(args,const char*);
        va_end(args);
        if(!std::strcmp(stage,"prepare") && workerPrepared==1 && !workerRetired)_exit(91);
    }
    std::abort();
}
bool BdwPreparePrivateImages(){++prepares;return scenario!=28;}
bool BdwPreparePrivateRenderImages(uint32_t (&)[20*1024]){++renderPrepares;return scenario!=31;}
bool BdwRetainPrivateBcsImages(uint32_t){++retains;return scenario!=33;}
bool BdwRetainPrivateRenderImages(uint32_t entries,bool,bool){++renderRetains;return entries==1048576 && scenario!=59;}
bool BdwSetupRetainedGemProbe(bool){return scenario>=76;}
bool BdwFinishRetainedGemProbe(bool stopped){return scenario>=76 && scenario!=78 && stopped;}
bool BdwReleaseRetainedGemProbe(){++gemReleases;
    if(scenario==76){BdwForcewakeAdapterResult out{};BdwRcsResetTestResult reset{};
        if(!BdwTestRcsReset(out,reset) && !reset.attempted)++contendersRefused;}
    return scenario!=79;}
void BdwFailRetainedGemProbe(){++gemFailures;}
bool BdwReprepareRetainedGemProbe(){return scenario==81;}
void BdwExecSnapshotCapture(const BdwExecSnapshot*,BdwGpuCapture& capture){capture={};}
void PanicSetGpuCapture(const BdwGpuCapture&){}
void BdwExecSnapshotOrigin(const BdwExecSnapshot*,uint32_t& pid,uint32_t& context){pid=context=0;}
BdwBatchSample BdwExecSnapshotBatchSample(const BdwExecSnapshot*){return {};}
BdwBatchSample BdwExecSnapshotCurrentBatchSample(const BdwExecSnapshot*){return {};}
uint32_t BdwExecSnapshotPageCount(const BdwExecSnapshot*){return 0;}
uint32_t BdwExecSnapshotObjectCount(const BdwExecSnapshot*){return 0;}
uint64_t BdwExecSnapshotBatchLength(const BdwExecSnapshot*){return 0;}
uint64_t BdwExecSnapshotBatchAddress(const BdwExecSnapshot*){return 0;}
BdwGpuContext* BdwExecSnapshotContext(const BdwExecSnapshot* s){
    auto n=reinterpret_cast<uintptr_t>(s);return n==2 || n==3?reinterpret_cast<BdwGpuContext*>(n-1):nullptr;
}
bool BdwGpuContextBegin(BdwGpuContext* context,uint32_t (&saved)[20*1024],bool& present){
    auto n=reinterpret_cast<uintptr_t>(context);if(n<1 || n>2 || contextActive[n-1] || contextPoisoned[n-1])return false;
    contextActive[n-1]=true;present=contextValues[n-1]!=0;if(present)saved[0]=contextValues[n-1];return true;
}
bool BdwGpuContextFinish(BdwGpuContext* context,const uint32_t (*saved)[20*1024],bool stopped){
    auto n=reinterpret_cast<uintptr_t>(context);if(n<1 || n>2 || !contextActive[n-1])return false;
    contextActive[n-1]=false;if(!stopped)contextPoisoned[n-1]=true;
    if(stopped && saved)contextValues[n-1]=(*saved)[0];return stopped;
}
bool BdwReprepareRetainedSubmission(const uint32_t (*saved)[20*1024],uint32_t nonce){
    if(scenario>=84 && saved && reprepareCount<3)reprepareValues[reprepareCount++]=(*saved)[0];
    return scenario>=82 && saved && (scenario>=84 || (*saved)[0]==0x1234) && nonce>0x42525000;
}
bool BdwPrepareRetainedSubmission(BdwExecSnapshot* s){++workerPrepared;return scenario>=82 && scenario!=85 && s;}
bool BdwRetireRetainedSubmission(BdwExecSnapshot* s){++workerRetired;return scenario>=82 && s;}
bool BdwReuseRetainedRenderScratch(BdwOwnedPage& p,BdwGgttReservation& r){p={0x600000,reinterpret_cast<uint8_t*>(0x600000)};r={65544,29,2};return scenario>=81;}
bool BdwRetainedBcsScratch(BdwOwnedPage& p){if(scenario<48)return false;p={0x400000,reinterpret_cast<uint8_t*>(0x400000)};return true;}
bool BdwPinRetainedScratchForTakeover(BdwOwnedPage& p,BdwGgttReservation& r){if(scenario==51)return false;p={0x400000,reinterpret_cast<uint8_t*>(0x400000)};r={65536,8,1};return scenario>=48;}
uint32_t BdwInspectPrivateBcsDma(){return scenario>=48 && scenario!=49?BdwDmaSnapshotOk:BdwDmaNotPrepared;}
uint32_t BdwInspectPrivateRenderDma(){return scenario==62?BdwDmaNotPrepared:BdwDmaSnapshotOk;}
bool BdwRetainedRenderReservation(BdwGgttReservation& out){out={65544,29,2};return true;}
bool BdwPinRetainedRenderScratch(BdwOwnedPage& p,BdwGgttReservation& r){p={0x600000,reinterpret_cast<uint8_t*>(0x600000)};r={65544,29,2};return true;}
bool BdwRetainRebasedRenderImages(const uint32_t (&saved)[20*1024]){return saved[0]==0x1234;}
bool BdwRebasedRenderReservation(BdwGgttReservation& r){r={65573,29,3};return true;}
uint32_t BdwInspectRebasedRenderDma(){return BdwDmaSnapshotOk;}
bool BdwPinRebasedRenderScratch(BdwOwnedPage& p,BdwGgttReservation& r){p={0x700000,reinterpret_cast<uint8_t*>(0x700000)};r={65573,29,3};return true;}
void VmmDellRcsReset(uint32_t,BdwForcewakeAdapterResult& r,BdwRcsResetTestResult& s){++wakeRuns;r.clearDelivered=r.sequence.acquired=r.sequence.released=r.sequence.workComplete=true;s.attempted=true;s.complete=scenario!=63;s.uncertain=scenario==63;}
void VmmDellRenderGtInit(uint32_t,BdwForcewakeAdapterResult& r,BdwRenderGtInitResult& s){++wakeRuns;r.clearDelivered=r.sequence.acquired=r.sequence.released=r.sequence.workComplete=true;s.status=scenario==66?BdwRenderGtInitStatus::Uncertain:BdwRenderGtInitStatus::Applied;s.writes=1;}
void VmmDellRenderRun(uint32_t,uint32_t,uint64_t,uint64_t,uint32_t (&snapshot)[20*1024],BdwForcewakeAdapterResult& r,BdwRenderRunResult& s){
    ++wakeRuns;r.clearDelivered=r.sequence.acquired=r.sequence.workComplete=true;r.sequence.released=scenario!=70;
    if(scenario==76){BdwForcewakeAdapterResult out{};BdwRcsResetTestResult reset{};
        if(!BdwTestRcsReset(out,reset) && !reset.attempted)++contendersRefused;}
    s.attempted=s.breadcrumb=s.contextComplete=true;s.transaction.gtReady=s.transaction.patReady=s.transaction.contextSaved=true;
    s.transaction.resetComplete=s.transaction.patRestored=true;s.transaction.uncertain=scenario==69;
    s.ggttCleared=scenario>=76 && scenario!=77;
    if(scenario==82 && s.pollYieldRequested)s.pollYields=2;
    for(auto& v:snapshot)v=0x1234;
    if(scenario>=84)snapshot[0]=0x1234+wakeRuns;
}
void VmmDellForcewakeScratchRun(uint32_t,uint32_t,uint64_t,BdwForcewakeAdapterResult& r,BdwScratchTakeoverResult& s){++ggttReads;r.clearDelivered=r.sequence.released=r.sequence.workComplete=true;s.resetAttempted=s.resetComplete=s.attempted=s.complete=true;}
void VmmDellForcewakeBcsInit(uint32_t,uint32_t,uint64_t,uint8_t*,BdwForcewakeAdapterResult& r,BdwBcsInitResult& s){r.clearDelivered=r.sequence.released=r.sequence.workComplete=true;s.attempted=s.resetComplete=s.initComplete=true;}
void VmmDellForcewakePatTest(uint32_t,BdwForcewakeAdapterResult& r,BdwPatTestResult& p){++wakeRuns;r.clearDelivered=r.sequence.released=r.sequence.workComplete=true;p.attempted=p.resetComplete=p.established=p.restored=true;}
void VmmDellBcsCopy(uint32_t,uint32_t,uint64_t,uint64_t,BdwForcewakeAdapterResult& r,BdwBcsRunResult& s){r.clearDelivered=r.sequence.released=r.sequence.workComplete=true;s.attempted=s.copyComplete=s.contextComplete=s.resetAfter=s.patRestored=true;if(scenario==57)s.patUncertain=true;}
void VmmDellForcewakeClearStale(uint32_t,uint64_t,BdwForcewakeAdapterResult&,BdwScratchTakeoverResult&){}
bool BdwRetainedBcsReservation(BdwGgttReservation& out){out={65536,8,1};return true;}
bool BdwPrepareBcsScanout(BdwGpuBuffer*,uint64_t,const BdwGopBacking&,const CompositorPresentRegion*,uint32_t,
        BdwGgttReservation&,BdwOwnedPage&){return false;}
bool BdwReleasePreparedBcsScanout(){return false;}
void VmmDellBcsScanout(uint32_t,uint32_t,uint64_t,uint64_t,BdwForcewakeAdapterResult&,BdwBcsRunResult&){std::abort();}
bool PanicRecoveryResetArmed(){return scenario!=36 && scenario!=39;}
bool VmmDellReservedGgttCriticalRead(uint32_t first,BdwGgttReservedPtes& out){
    if(scenario>=74){assert(first>=65544 && first<=65568 && !((first-65544)%8));++ggttReads;
        if(scenario==75 && first==65560)return false;
        for(unsigned i=0;i<8;++i)out.values[i]=first+i;return true;}
    assert(first==65536);++ggttReads;out.values[0]=42;return scenario!=35;}
bool VmmDellGgttSurveyCriticalRead(uint32_t,BdwGgttSurvey&){return false;}
bool VmmDellDisplayCriticalRead(BdwDisplaySnapshot& d){if(scenario<48)return false;d={};d.regs[0][0]=0x98000000;d.regs[0][1]=12800;if(scenario==50)d.regs[1][0]=1;
    if(scenario==88)d.regs[0][BdwDisplaySnapshot::PrimaryOffset]=1;
    return true;}
extern "C" void* kmalloc(uint64_t size){return scenario==30?nullptr:std::malloc(size);}
extern "C" void kfree(void* p){++heapFrees;std::free(p);}
extern "C" void SerialPuts(const char*){}
extern "C" void SerialPrintf(const char*,...){}
extern "C" Vnode* VfsOpen(const char* p,int){if(scenario>=86 && (!std::strcmp(p,"/etc/dell-gpu-render-experiment") || !std::strcmp(p,"/etc/dell-gpu-exec-store-test") || !std::strcmp(p,"/etc/dell-gpu-intel-user-test")))return &marker;if(scenario>=84 && !std::strcmp(p,"/etc/dell-gpu-rcs-context-test"))return &marker;if(scenario>=82 && (!std::strcmp(p,"/etc/dell-gpu-rcs-worker-test") || !std::strcmp(p,"/etc/dell-gpu-rcs-external-tables")))return &marker;if(scenario==81 && (!std::strcmp(p,"/etc/dell-gpu-rcs-repeat-test") || !std::strcmp(p,"/etc/dell-gpu-rcs-external-tables")))return &marker;if(scenario==76 && !std::strcmp(p,"/etc/dell-gpu-rcs-reset-test"))return &marker;if(scenario>=76 && (!std::strcmp(p,"/etc/dell-gpu-rcs-run-test") || !std::strcmp(p,"/etc/dell-gpu-rcs-gem-probe") || !std::strcmp(p,"/etc/dell-gpu-pat-probe") || (scenario!=80 && !std::strcmp(p,"/etc/dell-gpu-rcs-release-after-reset"))))return &marker;if(scenario>=74 && !std::strcmp(p,"/etc/dell-gpu-rcs-clear-after-reset"))return &marker;return ((scenario>=4 && scenario<=13 && !std::strcmp(p,"/etc/dell-gpu-gtt-probe")) || (scenario>=14 && scenario<=18 && !std::strcmp(p,"/etc/dell-gpu-forcewake-ack")) || (scenario>=19 && scenario<=22 && !std::strcmp(p,"/etc/dell-gpu-forcewake-run")) || (scenario>=23 && scenario<=26 && !std::strcmp(p,"/etc/dell-gpu-engines-run")) || (scenario>=27 && scenario<=28 && !std::strcmp(p,"/etc/dell-gpu-prepare-only")) || (scenario>=29 && scenario<=31 && !std::strcmp(p,"/etc/dell-gpu-render-prepare-only")) || (scenario>=32 && scenario<=36 && !std::strcmp(p,"/etc/dell-gpu-retain-bcs")) || (((scenario>=40 && scenario<=57) || (scenario>=60 && scenario<=71)) && !std::strcmp(p,"/etc/dell-gpu-pat-probe")) || ((scenario==58 || scenario==59) && !std::strcmp(p,"/etc/dell-gpu-retain-rcs")) || ((scenario==60 || scenario==62 || scenario==63) && !std::strcmp(p,"/etc/dell-gpu-rcs-reset-test")) || ((scenario==64 || scenario==66) && !std::strcmp(p,"/etc/dell-gpu-rcs-gt-init")) || ((scenario==67 || scenario==69 || scenario==70 || scenario==71) && !std::strcmp(p,"/etc/dell-gpu-rcs-run-test")) || (scenario==71 && !std::strcmp(p,"/etc/dell-gpu-rcs-rebase-test")) || (scenario==72 && !std::strcmp(p,"/etc/dell-gpu-intel-user-test")) || (scenario==44 && !std::strcmp(p,"/etc/dell-gpu-tlb-test")) || (scenario==46 && !std::strcmp(p,"/etc/dell-gpu-bcs-reset-test")) || (scenario>=48 && scenario!=52 && !std::strcmp(p,"/etc/dell-gpu-scratch-takeover")) || (scenario>=48 && !std::strcmp(p,"/etc/dell-gpu-crossboot-safe")) || (scenario==48 && !std::strcmp(p,"/etc/dell-gpu-bcs-init-test")) || (scenario==53 && !std::strcmp(p,"/etc/dell-gpu-pat-transition-test")) || ((scenario==55 || scenario==57) && !std::strcmp(p,"/etc/dell-gpu-bcs-copy-test")))?&marker:nullptr;}
void VfsRelease(Vnode*){}
int VfsStat(Vnode*,VnodeStat* s){*s={};s->size=2;return 0;}
extern "C" int VfsRead(Vnode*,void* out,uint64_t n,uint64_t*){if(n!=2)return -1;std::memcpy(out,"1\n",2);return 2;}
uint32_t SmpCurrentCpuIndex(){return scenario==7 || scenario==16 || scenario==43?1:0;}
extern "C" uint8_t PciConfigRead8(uint8_t,uint8_t,uint8_t,uint8_t o){return o==0x34?0x40:o==0x40 || o==0x62?1:0;}
extern "C" uint16_t PciConfigRead16(uint8_t,uint8_t,uint8_t,uint8_t o){return o==6?(scenario==13?0xffff:0x10):o==4?(scenario==12 || scenario==18?0xffff:scenario>=48?7:2):o==0x50?0x01c1:o==0x42?(scenario==9?0:1):o==0x44 && (scenario==5 || scenario==22 || scenario==26)?3:0;}
extern "C" uint32_t PciConfigRead32(uint8_t,uint8_t,uint8_t,uint8_t o){
    if(o==0)return 0x16168086;if(o==0x2c)return 0x06651028;if(o==8)return 9;
    if(o==0x5c && scenario>=87)return scenario==91?0xdd000001:0xdd800001;
    if(o>=0x10 && o<0x20)return HdaDellPciReferenceEntries[1].bars[(o-0x10)/4]^(scenario==10 && o==0x10?0x1000000:0);
    return 0;
}
bool AcpiClassifyBootRange(uint64_t page,uint32_t bytes,BootResourceClassification& c){++pageChecks;
    if(scenario>=87 && page==0xdd800000){
        assert(bytes==BdwDellGopPages*4096);
        c={1u<<uint32_t(scenario==90?MemoryType::Free:MemoryType::Reserved),false};
        return true;
    }
    if(scenario>=48){assert(bytes==4096);c={2,false};return true;}
    unsigned step=pageChecks;
    if(scenario==42 && step>10)step-=10;else if(scenario>=42 && step>5)step-=5;else step=0;
    const uint64_t expected=step?(scenario==46?(step==1?BdwPrivatePatPage:step==2?BdwResetPage:step<=6?BdwForcewakePages[step-3]:BdwOtherEnginePages[step-7]):step==1?BdwPrivatePatPage:step<=5?BdwForcewakePages[step-2]:BdwOtherEnginePages[step-6]):scenario>=40?(pageChecks<=4?BdwForcewakePages[pageChecks-1]:BdwPrivatePatPage):scenario>=37?(pageChecks==1?0xf6800000ull:0xf680a000ull):scenario>=34?0xf6880000ull:scenario>=19?(pageChecks<=4?BdwForcewakePages[pageChecks-1]:BdwOtherEnginePages[pageChecks-5]):scenario>=14?0xf6130000ull:pageChecks==1?0xf6800000ull:0xf680a000ull;
    assert(bytes==4096 && page==expected);
    c={scenario==11 && pageChecks==2?3u:2u,false};return scenario!=6 && scenario!=15 && !(scenario==20 && pageChecks==4) && !(scenario==24 && pageChecks==7) && !(scenario==41 && pageChecks==5);}
bool VmmDellGopPtesCriticalRead(const BdwGopPtePlan& plan,BdwGopPtes& p){assert(plan.firstIndex==0 && plan.lastIndex==5624 && plan.firstAddress==0xf6800000ull && plan.lastAddress==0xf680afc0ull);++reads;if(scenario==8)return false;p={scenario==38 && reads==2?7u:5u,6};return true;}
bool VmmDellGopBackingPage(uint32_t first,uint32_t count,uint64_t& physical){
    assert(scenario>=87 && count && first+count<=BdwDellGopPages);
    ++reads;physical=0xdd800000ull+uint64_t(first)*4096;
    return scenario!=89 || reads!=14;
}
const NativeTscObservation& NativeTscObserveRead(){static NativeTscObservation o{};o.capable=o.measured=o.consistent=scenario!=21 && scenario!=25;o.khz=1000;return o;}
void VmmDellForcewakeCriticalRun(uint32_t khz,BdwForcewakeAdapterResult& r,bool other,bool pat,bool tlb,bool reset,bool scope) {assert(!scope && khz==1000 && other==((scenario>=23 && scenario<=26) || (scenario==42 && wakeRuns==2) || ((scenario==44 || scenario==46) && wakeRuns==1)) && pat==(scenario>=40) && tlb==(scenario==44 && wakeRuns==1) && reset==(scenario==46 && wakeRuns==1));++wakeRuns;r.aliases=r.clock=r.clearDelivered=true;r.sequence.acquired=r.sequence.released=r.sequence.threadReady=r.sequence.sampled=r.sequence.patSampled=true;r.sequence.otherEnginesSampled=other;r.sequence.tlbAttempted=r.sequence.tlbComplete=tlb;r.sequence.resetAttempted=r.sequence.resetComplete=reset;r.sequence.patLo=r.sequence.patHi=0x03030303;}
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
    for(unsigned n=29;n<=31;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;renderPrepares=heapFrees=pageChecks=0;BdwGopObserveConfigure(&boot);
        const auto& r=BdwGopObserveRead();assert(r.renderPrepareRequested && r.renderPrepareBuilt==(n==29));
        assert(renderPrepares==(n==30?0u:1u) && heapFrees==(n==30?0u:1u) && pageChecks==0);
        BdwGopObserveConfigure(&boot);assert(renderPrepares==(n==30?0u:1u));_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=32;n<=33;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;retains=pageChecks=0;BdwGopObserveConfigure(&boot);
        const auto& r=BdwGopObserveRead();assert(r.retainRequested && r.retainBuilt==(n==32) && retains==1 && pageChecks==0);
        BdwGopObserveConfigure(&boot);assert(retains==1);_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=34;n<=36;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;retains=pageChecks=ggttReads=0;BdwGopObserveConfigure(&boot);
        BdwGgttReservedPtes p{};p.values[0]=99;assert(BdwObserveReservedGgtt(p)==(n==34));
        assert(p.values[0]==(n==34?42u:99u) && ggttReads==(n==36?0u:1u));_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=37;n<=39;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;reads=pageChecks=0;BdwGopObserveConfigure(&boot);
        BdwGopPtes p{99,99};assert(BdwObserveGopPtesLive(p)==(n==37));
        assert(p.first==(n==37?5u:99u) && reads==(n==39?0u:2u));_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=40;n<=41;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;wakeRuns=pageChecks=0;BdwGopObserveConfigure(&boot);
        const auto& r=BdwGopObserveRead();assert(r.patRequested && r.wakeRequested && pageChecks==5);
        assert(r.patPageAdmitted==(n==40) && wakeRuns==(n==40?1u:0u));_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=42;n<=43;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;wakeRuns=pageChecks=0;BdwGopObserveConfigure(&boot);
        BdwForcewakeAdapterResult result{};assert(BdwObserveForcewakeLive(result)==(n==42));
        assert(wakeRuns==(n==42?2u:0u) && pageChecks==(n==42?10u:0u));
        if(n==42){assert(result.sequence.patSampled && result.sequence.patLo==0x03030303);
            assert(BdwObserveForcewakeLive(result,true) && result.sequence.otherEnginesSampled && wakeRuns==3 && pageChecks==18);}_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=44;n<=45;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;wakeRuns=pageChecks=0;BdwGopObserveConfigure(&boot);
        BdwForcewakeAdapterResult result{};assert(BdwTestTlbInvalidate(result)==(n==44));
        assert(wakeRuns==(n==44?2u:1u));if(n==44){assert(result.sequence.tlbComplete);assert(!BdwTestTlbInvalidate(result) && wakeRuns==2);}_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=46;n<=47;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;wakeRuns=pageChecks=0;BdwGopObserveConfigure(&boot);
        BdwForcewakeAdapterResult result{};assert(BdwTestBcsReset(result)==(n==46));
        assert(wakeRuns==(n==46?2u:1u));if(n==46){assert(result.sequence.resetComplete);assert(!BdwTestBcsReset(result) && wakeRuns==2);}_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=48;n<=52;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;wakeRuns=pageChecks=ggttReads=0;BdwGopObserveConfigure(&boot);
        BdwForcewakeAdapterResult result{};BdwScratchTakeoverResult scratch{};
        assert(BdwTestScratchTakeover(result,scratch)==(n==48));
        assert(ggttReads==(n==48?1u:0u));
        if(n==48){assert(scratch.resetComplete && scratch.attempted && scratch.complete);assert(!BdwTestScratchTakeover(result,scratch) && ggttReads==1);
            BdwBcsInitResult init{};assert(BdwTestBcsInitialize(result,init) && init.initComplete);
            assert(!BdwTestBcsInitialize(result,init));}
        _exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=53;n<=54;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;wakeRuns=pageChecks=0;BdwGopObserveConfigure(&boot);
        BdwForcewakeAdapterResult result{};BdwPatTestResult pat{};
        assert(BdwTestPrivatePat(result,pat)==(n==53));
        if(n==53){assert(pat.attempted && pat.established && pat.restored);assert(!BdwTestPrivatePat(result,pat));}
        else assert(!pat.attempted);
        _exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=55;n<=56;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;wakeRuns=pageChecks=ggttReads=0;BdwGopObserveConfigure(&boot);
        BdwForcewakeAdapterResult result{};BdwScratchTakeoverResult scratch{};BdwBcsRunResult copy{};
        assert(!BdwTestBcsCopy(result,copy));assert(BdwTestScratchTakeover(result,scratch));
        assert(BdwTestBcsCopy(result,copy)==(n==55));
        if(n==55){assert(copy.attempted && copy.copyComplete && copy.contextComplete);assert(!BdwTestBcsCopy(result,copy));}
        else assert(!copy.attempted);
        _exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    {pid_t child=fork();assert(child>=0);if(!child){scenario=57;BdwGopObserveConfigure(&boot);
        BdwForcewakeAdapterResult result{};BdwScratchTakeoverResult scratch{};BdwBcsRunResult copy{};
        assert(BdwTestScratchTakeover(result,scratch));(void)BdwTestBcsCopy(result,copy);_exit(1);}
     int status=0;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);}
    for(unsigned n=58;n<=59;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;renderRetains=pageChecks=0;
        BdwGopObserveConfigure(&boot);const auto& r=BdwGopObserveRead();
        assert(r.renderRetainRequested && r.renderRetainBuilt==(n==58) && renderRetains==1 && pageChecks==0);
        BdwGopObserveConfigure(&boot);assert(renderRetains==1);_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=60;n<=62;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;BdwGopObserveConfigure(&boot);wakeRuns=0;
        BdwForcewakeAdapterResult result{};BdwRcsResetTestResult reset{};
        assert(BdwTestRcsReset(result,reset)==(n==60));
        assert(wakeRuns==(n==60?1u:0u));if(n==60){assert(reset.attempted && reset.complete && !reset.uncertain);
            assert(!BdwTestRcsReset(result,reset) && wakeRuns==1);}_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    {pid_t child=fork();assert(child>=0);if(!child){scenario=63;BdwGopObserveConfigure(&boot);
        BdwForcewakeAdapterResult result{};BdwRcsResetTestResult reset{};(void)BdwTestRcsReset(result,reset);_exit(1);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);}
    for(unsigned n=64;n<=65;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;BdwGopObserveConfigure(&boot);wakeRuns=0;
        BdwForcewakeAdapterResult result{};BdwRenderGtInitResult init{};
        assert(BdwTestRenderGtInit(result,init)==(n==64));assert(wakeRuns==(n==64?1u:0u));
        if(n==64)assert(!BdwTestRenderGtInit(result,init) && wakeRuns==1);_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    {pid_t child=fork();assert(child>=0);if(!child){scenario=66;BdwGopObserveConfigure(&boot);
        BdwForcewakeAdapterResult result{};BdwRenderGtInitResult init{};(void)BdwTestRenderGtInit(result,init);_exit(1);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);}

    for(unsigned n=67;n<=68;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;BdwGopObserveConfigure(&boot);wakeRuns=0;
        BdwForcewakeAdapterResult result{};BdwRenderRunResult run{};
        assert(BdwTestRenderRun(result,run)==(n==67));assert(wakeRuns==(n==67?1u:0u));
        if(n==67){assert(run.contextHash && run.transaction.contextSaved);assert(!BdwTestRenderRun(result,run) && wakeRuns==1);}_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=69;n<=70;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;BdwGopObserveConfigure(&boot);
        BdwForcewakeAdapterResult result{};BdwRenderRunResult run{};(void)BdwTestRenderRun(result,run);_exit(1);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);}

    {pid_t child=fork();assert(child>=0);if(!child){scenario=71;BdwGopObserveConfigure(&boot);wakeRuns=0;
        BdwForcewakeAdapterResult result{};BdwRenderRunResult run{};
        assert(!BdwTestRebasedRenderRun(result,run) && !run.admitted);
        assert(BdwTestRenderRun(result,run) && run.contextHash);
        assert(BdwTestRebasedRenderRun(result,run) && run.contextHash && wakeRuns==2);
        assert(!BdwTestRebasedRenderRun(result,run) && wakeRuns==2);_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}

    for(unsigned n=72;n<=73;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;BdwGopObserveConfigure(&boot);
        assert(BdwIntelUserNodeEnabled()==(n==72));_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}

    for(unsigned n=74;n<=75;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;BdwGopObserveConfigure(&boot);
        ggttReads=0;BdwRcsGgttPtes p{};p.values[0]=99;
        assert(BdwObserveRcsGgtt(p)==(n==74));
        if(n==74)assert(ggttReads==4 && p.values[0]==65544 && p.values[28]==65572);
        else assert(ggttReads==3 && p.values[0]==99);
        _exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}

    for(unsigned n=76;n<=80;++n){pid_t child=fork();assert(child>=0);if(!child){scenario=n;BdwGopObserveConfigure(&boot);
        BdwForcewakeAdapterResult result{};BdwRenderRunResult run{};
        assert(BdwTestRenderRun(result,run)==(n==76 || n==80));
        assert(run.gemReleaseRequested==(n!=80) && run.gemReleased==(n==76));
        assert(gemReleases==unsigned(n==76 || n==79));
        assert(gemFailures==unsigned(n==77 || n==78 || n==79));
        if(n==76){assert(contendersRefused==2);BdwForcewakeAdapterResult out{};BdwRcsResetTestResult reset{};
            assert(BdwTestRcsReset(out,reset) && reset.attempted && reset.complete);}
        _exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    {pid_t child=fork();assert(child>=0);if(!child){scenario=81;BdwGopObserveConfigure(&boot);wakeRuns=0;
        BdwForcewakeAdapterResult out{};BdwRenderRunResult run{};
        assert(!BdwTestRenderRepeat(out,run) && !run.attempted);
        assert(BdwTestRenderRun(out,run) && run.gemReleased && run.externalTablesRequested);
        const bool repeated=BdwTestRenderRepeat(out,run);
        if(!repeated || !run.gemReleased || !run.externalTablesRequested || wakeRuns!=2)dprintf(2,"repeat=%u released=%u external=%u wakes=%u stage=%u\n",repeated,run.gemReleased,run.externalTablesRequested,wakeRuns,run.preflightStage);
        assert(repeated && run.gemReleased && run.externalTablesRequested && wakeRuns==2);
        assert(!BdwTestRenderRepeat(out,run) && wakeRuns==2);_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(int workerScenario=82;workerScenario<=83;++workerScenario){pid_t child=fork();assert(child>=0);if(!child){scenario=workerScenario;BdwGopObserveConfigure(&boot);wakeRuns=0;
        auto* snapshot=reinterpret_cast<BdwExecSnapshot*>(uintptr_t{1});
        auto early=BdwExecuteRetainedWorkerSnapshot(nullptr,snapshot);assert(!early.success && early.safelyStopped);
        BdwForcewakeAdapterResult out{};BdwRenderRunResult run{};
        assert(BdwTestRenderRun(out,run) && run.gemReleased && run.externalTablesRequested);
        auto refused=BdwExecuteRetainedWorkerSnapshot(nullptr,reinterpret_cast<BdwExecSnapshot*>(uintptr_t{2}));
        assert(!refused.success && refused.safelyStopped && !workerPrepared);
        auto a=BdwExecuteRetainedWorkerSnapshot(nullptr,snapshot);
        auto b=BdwExecuteRetainedWorkerSnapshot(nullptr,snapshot);
        assert(a.success && a.safelyStopped && b.success && b.safelyStopped && workerPrepared==2 && workerRetired==2 && wakeRuns==3);
        _exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    {pid_t child=fork();assert(child>=0);if(!child){scenario=84;BdwGopObserveConfigure(&boot);wakeRuns=0;
        BdwForcewakeAdapterResult out{};BdwRenderRunResult run{};
        assert(BdwTestRenderRun(out,run) && run.gemReleased);
        auto* a=reinterpret_cast<BdwExecSnapshot*>(uintptr_t{2});auto* b=reinterpret_cast<BdwExecSnapshot*>(uintptr_t{3});
        assert(BdwExecuteRetainedContextWorkerSnapshot(nullptr,a).success);
        assert(BdwExecuteRetainedContextWorkerSnapshot(nullptr,b).success);
        assert(BdwExecuteRetainedContextWorkerSnapshot(nullptr,a).success);
        assert(reprepareCount==3 && reprepareValues[0]==0x1235 && reprepareValues[1]==0x1235 && reprepareValues[2]==0x1236);
        assert(contextValues[0]==0x1238 && contextValues[1]==0x1237 && !contextActive[0] && !contextActive[1]);
        _exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    {pid_t child=fork();assert(child>=0);if(!child){scenario=85;BdwGopObserveConfigure(&boot);wakeRuns=0;
        BdwForcewakeAdapterResult out{};BdwRenderRunResult run{};assert(BdwTestRenderRun(out,run));
        auto* a=reinterpret_cast<BdwExecSnapshot*>(uintptr_t{2});
        (void)BdwExecuteRetainedContextWorkerSnapshot(nullptr,a);
        _exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==91);}
    {pid_t child=fork();assert(child>=0);if(!child){scenario=86;BdwGopObserveConfigure(&boot);wakeRuns=0;
        assert(!BdwRenderExperimentEnabled());BdwForcewakeAdapterResult out{};BdwRenderRunResult run{};
        assert(BdwTestRenderRun(out,run) && run.gemReleased && BdwRenderExperimentEnabled());_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for(unsigned n=87;n<=91;++n){
        pid_t child=fork();assert(child>=0);
        if(!child){
            scenario=n;reads=0;BdwGopObserveConfigure(&boot);
            BdwGopBacking backing{1,2};
            assert(BdwObserveGopBacking(backing)==(n==87));
            if(n==87)assert(backing.physicalBase==0xdd800000 && backing.bytes==23040000 && reads==22);
            else assert(backing.physicalBase==1 && backing.bytes==2);
            if(n==88 || n==91)assert(!reads);
            if(n==89)assert(reads==14);
            _exit(0);
        }
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
}
