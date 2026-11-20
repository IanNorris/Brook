#include <cassert>
#include <cstdio>
#include "ac_shutdown_record.h"
#include "../../kernel/src/shutdown.cpp"
namespace {
void TestAcShutdownRecord() {
    using namespace brook;
    struct Disk {
        uint8_t sectors[2][AcShutdownSectorBytes]{};
        unsigned reads=0,failRead=0;
        bool torn=false,failFlush=false;
        bool read(unsigned slot,uint8_t* out) {
            if(++reads==failRead)return false;
            memcpy(out,sectors[slot],AcShutdownSectorBytes);return true;
        }
        bool write(unsigned slot,const uint8_t* in) {
            memcpy(sectors[slot],in,torn?24:AcShutdownSectorBytes);return !torn;
        }
        bool flush(){return !failFlush;}
    } disk;
    constexpr char oldBoot[]="11111111111111111111111111111111";
    constexpr char newBoot[]="22222222222222222222222222222222";
    AcShutdownSelection selection{};
    assert(!AcShutdownWrite(disk,"00000000000000000000000000000000"));
    assert(AcShutdownRead(disk,selection) && selection.newest<0);
    assert(AcShutdownWrite(disk,oldBoot));
    disk.torn=true;
    assert(!AcShutdownWrite(disk,newBoot));
    assert(AcShutdownRead(disk,selection) && selection.newest==0 && selection.latest.sequence==1);
    assert(selection.kind[1]==AcShutdownSlotKind::Torn && !strcmp(selection.latest.bootId,oldBoot));
    disk.torn=false;
    assert(AcShutdownWrite(disk,newBoot));
    assert(AcShutdownRead(disk,selection) && selection.newest==1 && selection.latest.sequence==2);
    assert(!strcmp(selection.latest.bootId,newBoot));
    disk.sectors[0][0]=0xff;
    assert(!AcShutdownWrite(disk,oldBoot));
    memcpy(disk.sectors[0],disk.sectors[1],AcShutdownSectorBytes);
    assert(!AcShutdownRead(disk,selection) && !AcShutdownWrite(disk,oldBoot));
    Disk missingRead{};missingRead.failRead=3;
    assert(!AcShutdownWrite(missingRead,oldBoot));
    Disk failedFlush{};failedFlush.failFlush=true;
    assert(!AcShutdownWrite(failedFlush,oldBoot) && failedFlush.reads==2);
}
struct Yield {};
bool launched=true,jobsAllowed=true,jobsBusy=false,dispatching=false,gate=false,canFreeze=true;
uint32_t births=0;unsigned closes=0,signals=0,syncs=0,freezes=0;
unsigned writebackStops=0,powerWrites=0;
uint64_t tick=0;
bool terminalSleeps=false,lastRestart=false;
brook::PlatformPowerPlan powerPlan;
bool writebackStopped=false;
int syncError=0;
int finalError=0,directFinalError=0;unsigned finalCalls=0,directFinalCalls=0;
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
unsigned largeFinalCalls=0;
#endif
brook::ProcessCleanupStatus cleanup{};
brook::Process serviceTasks[2]{};unsigned signalPid=0;
brook::Process appTasks[3]{};int signalLog[32]{};unsigned signalPids[32]{};unsigned signalCount=0;
brook::DebugTaskSnapshot census[4]{};unsigned censusCount=0;
brook::WmDebugWindow window{};bool visible=false;
void step(){try{brook::Worker(nullptr);}catch(Yield&){} }
void command(const char* id,const char* verb,const char* expected,int expectedError=0){char state[24];int error=123;brook::ShutdownCommand(id,verb,state,&error);if(strcmp(state,expected)||error!=expectedError)std::fprintf(stderr,"power test %s: expected %s/%d got %s/%d\n",verb,expected,expectedError,state,error);assert(!strcmp(state,expected));assert(error==expectedError);}
const char* id="11111111111111111111111111111111";
const char* second="22222222222222222222222222222222";
}
namespace brook {
Process* KernelThreadCreate(const char*,void(*)(void*),void*,uint8_t){return nullptr;}
void SchedulerAddProcess(Process*){}
extern "C" void SchedulerSleepMs(uint32_t ms){if(terminalSleeps && ms!=25)return;throw Yield{};}
uint32_t SchedulerSnapshotDebugTasks(DebugTaskSnapshot* out,uint32_t cap,uint32_t* total,uint64_t* begin,uint64_t* end){*total=censusCount;*begin=*end=0;for(unsigned i=0;i<censusCount&&i<cap;++i)out[i]=census[i];return censusCount;}
uint32_t SchedulerSetUserBirthClosed(bool closed){gate=closed;return births;}
void SchedulerUserBirthStatus(bool* closed,uint32_t* pending){*closed=gate;*pending=births;}
Process* SchedulerFindProcessReference(uint16_t pid){for(auto& p:serviceTasks)if(p.pid==pid){ProcessRef(&p);return &p;}for(auto& p:appTasks)if(p.pid==pid){ProcessRef(&p);return &p;}return nullptr;}
int ProcessSendSignal(Process* p,int signal,const SignalInfo*){++signals;signalPid=p->pid;if(signalCount<32){signalLog[signalCount]=signal;signalPids[signalCount++]=p->pid;}return 0;}
void WmSetLaunchInhibited(bool inhibited){launched=!inhibited;}
bool WmLauncherDispatching(){return dispatching;}
void DebugJobsSetLaunchInhibited(bool inhibited){jobsAllowed=!inhibited;}
bool DebugJobsActive(){return jobsBusy;}
uint32_t WmSnapshotDebugWindows(WmDebugWindow* out,uint32_t,uint64_t* tick){*tick=0;if(visible){*out=window;return 1;}return 0;}
int WmApplyDebugAction(uint32_t target,uint64_t incarnation,WmDebugAction,uint32_t,WmDebugWindow*,uint32_t*){assert(target==window.id&&incarnation==window.incarnation);++closes;return 0;}
ProcessCleanupStatus ProcessGetCleanupStatus(){return cleanup;}
void FileObjectStopWriteback(){++writebackStops;writebackStopped=true;assert(!cleanup.liveUserAddressSpaces&&!cleanup.activeFinalizers);}
void Ext2StopWriteback(){assert(writebackStopped);}
void PlatformPowerConfigure(){}
const PlatformPowerPlan& PlatformPowerGetPlan(){return powerPlan;}
bool PlatformPowerIssue(bool restart){assert(writebackStopped && gate && syncError==0);++powerWrites;lastRestart=restart;return true;}
bool DeviceCloseWrites(){assert(writebackStopped);++freezes;return canFreeze;}
void AudioStop(){}
void AudioPrepareShutdown(){}
void HdaRuntimeShutdown(){}
int VfsSync(Vnode*){++syncs;return syncError;}
int AhciSystemDirectFinalSync(){assert(writebackStopped && canFreeze && !syncError);++directFinalCalls;return directFinalError;}
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
bool SsdQemuInstallCloseReady(){return true;}
int AhciSystemQemuLargeDataFinalSync(){assert(writebackStopped && canFreeze && !syncError);++largeFinalCalls;return 0;}
#endif
int WritableDesktopFinalSync(){assert(writebackStopped && canFreeze && !syncError);++finalCalls;return finalError;}
void CompositorMarkDirty(){}
void CompositorWake(){}
uint32_t FwCfgReadFile(const char*,void*,uint32_t){return 0;}
bool UsbBootReadReset(UsbBootResetReport&){return false;}
void SerialPrintf(const char*,...){}
void SerialPuts(const char*){}
void KLog(const char*,...){}
uint64_t ApicTickCount(){return tick;}
}
namespace {
using namespace brook;
void App(unsigned slot,uint16_t pid,const char* name,ProcessState state=ProcessState::Ready) {
    auto& c=census[slot];c={};c.pid=c.tgid=pid;c.incarnation=c.groupIncarnation=1000+pid;c.state=state;
    {unsigned i=0;for(;name[i]&&i+1<sizeof(c.name);++i)c.name[i]=name[i];c.name[i]=0;}
    auto& p=appTasks[slot];p.pid=pid;p.tgid=pid;p.incarnation=1000+pid;p.lifecycleRole=ProcessLifecycleRole::Application;p.state=state;p.sigMask=0;p.refCount=1;
}
bool Contains(const char* haystack,const char* needle) {
    for(;*haystack;++haystack){unsigned i=0;while(needle[i]&&haystack[i]==needle[i])++i;if(!needle[i])return true;}
    return !*needle;
}
void Show(const char* request,const char* stage,const char* text) {
    ShutdownBlockerReport r{};ShutdownRequestBlockers(request,&r);
    if(strcmp(r.stage,stage)||!Contains(r.status,text))std::fprintf(stderr,"escalation: expected %s/'%s' got %s/'%s'\n",stage,text,r.stage,r.status);
    assert(!strcmp(r.stage,stage));assert(Contains(r.status,text));assert(!strcmp(ShutdownRequestStatus(request),r.status));
}
// Windowless user processes (a bridge-started daemon) are escalated
// SIGTERM -> SIGKILL only while no client window is open; an open window
// (a save prompt) is never escalated and a reappearing one restarts grace.
void TestWindowlessEscalation() {
    current=-1;irreversible=false;acRecovery={};powerRestart=true;powerOff=true;tick=1000000;
    cleanup={};births=0;gate=false;jobsBusy=true;dispatching=false;visible=false;signalCount=0;signals=0;
    const char* req="99999999999999999999999999999999";
    command(req,"restart","closing");
    // Editor with a save prompt (pid 40) plus a windowless daemon (pid 50).
    censusCount=2;App(0,40,"mousepad");App(1,50,"pulseaudio");
    visible=true;window={};window.id=9;window.incarnation=90;window.focusable=true;window.clientPid=40;window.clientIncarnation=1040;
    const unsigned closesBefore=closes;ShutdownCompositorPoll();assert(closes==closesBefore+1);
    for(unsigned i=0;i<20;++i){tick+=5000;ShutdownCompositorPoll();step();}
    assert(!signalCount);Show(req,"windows","force close loses unsaved work; 1 window open");
    assert(ShutdownCanForceApps(req));
    {ShutdownBlockerReport r{};ShutdownRequestBlockers(req,&r);assert(r.count==2&&r.listed==2&&r.entries[0].pid==40&&r.entries[0].window&&r.entries[1].pid==50&&!r.entries[1].window);}
    // Prompt answered: the editor exits; only the daemon remains.
    visible=false;censusCount=1;App(0,50,"pulseaudio",ProcessState::Stopped);ShutdownCompositorPoll();
    step();Show(req,"grace","no window: pulseaudio pid 50; SIGTERM in 10 s");
    tick+=WindowlessStopPolicy::GraceMs-1;step();assert(!signalCount);
    // A window opened since the compositor's last poll vetoes the step.
    visible=true;window.clientPid=61;window.clientIncarnation=1061;window.incarnation=91;
    tick+=1;step();assert(!signalCount);Show(req,"waiting","no window: pulseaudio pid 50");
    ShutdownCompositorPoll();step();Show(req,"windows","1 window open");
    tick+=60000;step();assert(!signalCount);
    visible=false;ShutdownCompositorPoll();step();Show(req,"grace","SIGTERM in 10 s");
    tick+=WindowlessStopPolicy::GraceMs;step();
    // SIGTERM, then SIGCONT because the daemon was stopped.
    assert(signalCount==2&&signalLog[0]==15&&signalPids[0]==50&&signalLog[1]==18);
    Show(req,"sigterm","SIGKILL in 5 s");
    tick+=WindowlessStopPolicy::KillMs-1;step();assert(signalCount==2);
    tick+=1;step();assert(signalCount==3&&signalLog[2]==9&&signalPids[2]==50);Show(req,"sigkill","killed, waiting for exit");
    tick+=WindowlessStopPolicy::RekillMs;step();assert(signalCount==4&&signalLog[3]==9);
    assert(!gate&&operations[current].phase==Phase::Closing);
    // Exit observed; the bridge job drains, then the normal path continues.
    censusCount=0;step();assert(operations[current].phase==Phase::Closing);Show(req,"draining","bridge job output");
    jobsBusy=false;for(unsigned i=0;i<10&&operations[current].phase!=Phase::Ready;++i)step();
    command(req,"power_status","ready");assert(signalCount==4);
    // Cancel is honoured promptly during escalation and stops it.
    current=-1;irreversible=false;signalCount=0;
    const char* cancelled="aaaaaaaa999999999999999999999999";
    command(cancelled,"shutdown","closing");censusCount=1;App(0,70,"sleep");ShutdownCompositorPoll();
    step();tick+=WindowlessStopPolicy::GraceMs;step();assert(signalCount==1&&signalLog[0]==15);
    command(cancelled,"cancel","closing");Show(cancelled,"cancelling","Cancelling");
    step();command(cancelled,"power_status","cancelled");assert(launched&&jobsAllowed&&!gate);
    tick+=60000;step();assert(signalCount==1);
    censusCount=0;current=-1;irreversible=false;
}
void TestConfirmedForceApps() {
    const char* request="abababababababababababababababab";
    current=-1;irreversible=false;visible=true;tick=2000000;signalCount=signals=0;
    censusCount=2;App(0,40,"editor");App(1,50,"daemon");
    window={};window.id=9;window.incarnation=90;window.focusable=true;
    window.clientPid=40;window.clientIncarnation=1040;
    command(request,"restart","closing");ShutdownCompositorPoll();
    command(request,"force_apps","not_ready",-16);step();
    assert(!ShutdownCanForceApps(request) && !signals);
    tick+=ForceAppsGraceMs;
    ShutdownCompositorPoll();step();
    assert(ShutdownCanForceApps(request) && !signals);
    command(request,"force_apps","closing");
    step();assert(signalCount==1 && signalLog[0]==9 && signalPids[0]==40);
    command(request,"force_apps","closing");step();
    assert(signalCount==1 && !ShutdownCanForceApps(request));
    assert(!gate && operations[current].phase==Phase::Closing);
    command(request,"cancel","closing");step();

    const char* later="cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";
    command(later,"restart","closing");ShutdownCompositorPoll();
    tick+=ForceAppsGraceMs;ShutdownCompositorPoll();
    command(later,"force_apps","closing");
    ++appTasks[0].incarnation; // same pid was reused before execution
    step();assert(signalCount==1);
    command(later,"cancel","closing");step();
    censusCount=0;current=-1;irreversible=false;visible=false;
}
}
int main(){
    TestAcShutdownRecord();
    using namespace brook;workerStarted=false;
    command(id,"shutdown","unsupported",-95);assert(launched&&jobsAllowed);
    workerStarted=true;powerOff=true;powerRestart=true;
    command(id,"shutdown","closing");assert(!launched&&!jobsAllowed);
    command(id,"shutdown","closing");command(id,"restart","conflict",-22);command(second,"restart","busy",-16);
    // A normal close is sent once. Keeping that window (save dialog or hung
    // client) must neither close it repeatedly nor stop its service provider.
    visible=true;window.id=1;window.incarnation=12;window.focusable=true;window.clientPid=7;window.clientIncarnation=12;
    censusCount=1;census[0].pid=7;census[0].state=ProcessState::Ready;
    ShutdownCompositorPoll();ShutdownCompositorPoll();assert(closes==1);
    window.incarnation=13;ShutdownCompositorPoll();assert(closes==1); // same-client save dialog
    window.clientPid=8;window.clientIncarnation=14;window.incarnation=14;ShutdownCompositorPoll();assert(closes==2); // late new app
step();assert(!signals&&!syncs&&!gate);
    command(id,"cancel","closing");step();command(id,"power_status","cancelled");assert(launched&&jobsAllowed&&!gate);
    // Same old identity stays cancelled; a new explicit request is required.
    command(id,"shutdown","cancelled");command(second,"shutdown","closing");
    visible=false;censusCount=0;ShutdownCompositorPoll();jobsBusy=true;step();assert(!gate);
    jobsBusy=false;dispatching=true;step();assert(!gate);dispatching=false;
    births=1;step();assert(gate);command(second,"power_status","closing");assert(!signals&&!syncs);
    births=0;censusCount=3;
    for(unsigned i=0;i<3;++i){census[i]={};census[i].pid=20+i;census[i].tgid=20;census[i].groupIncarnation=30;census[i].incarnation=30+i;census[i].systemService=true;census[i].state=i?ProcessState::Ready:ProcessState::Terminated;}
    for(unsigned i=0;i<2;++i){auto& p=serviceTasks[i];p.pid=21+i;p.incarnation=31+i;p.lifecycleRole=ProcessLifecycleRole::Service;p.state=ProcessState::Ready;p.sigMask=i?0:1ULL<<14;p.refCount=1;}
    step();assert(signals==1&&signalPid==22);assert(serviceTasks[0].refCount==1&&serviceTasks[1].refCount==1);command(second,"power_status","services");command(second,"cancel","too_late",-16);
    censusCount=0;step();command(second,"power_status","cleanup");
    cleanup.liveUserAddressSpaces=1;step();assert(!freezes&&!syncs&&!writebackStops);
    cleanup.liveUserAddressSpaces=0;cleanup.activeFinalizers=1;step();assert(!freezes&&!syncs&&!writebackStops);
    cleanup.activeFinalizers=0;canFreeze=false;step();assert(freezes==1&&!syncs);
    canFreeze=true;syncError=-5;step();command(second,"power_status","failed",-5);assert(syncs==1 && !finalCalls && !directFinalCalls);
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    assert(!largeFinalCalls); // failed global sync must not mark the large store clean
#endif
    step();assert(syncs==1);command(second,"shutdown","failed",-5);
    // A fresh explicit attempt may retry a failed flush; no implicit retry.
    const char* third="33333333333333333333333333333333";
    command(third,"shutdown","busy",-16);assert(!launched&&!jobsAllowed&&gate);
    // Simulate a fresh boot for the successful transition case. Production
    // must not undo the irreversible service/write-barrier stage after error.
    current=-1;irreversible=false;
    command(third,"shutdown","closing");ShutdownCompositorPoll();step();step();syncError=0;step();
    command(third,"power_status","ready");assert(syncs==2&&gate && finalCalls==1);
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    assert(largeFinalCalls==1);
#endif
    command(third,"cancel","too_late",-16);
    command(third,"commit","ready");assert(operations[current].commit);
    // Mocked hardware writes are reached only after successful sync, once.
    assert(!powerWrites);terminalSleeps=true;step();terminalSleeps=false;
    assert(powerWrites==1 && !lastRestart && operations[current].phase==Phase::Failed);
    current=-1;irreversible=false;powerOff=false;powerRestart=true;
    const char* fourth="44444444444444444444444444444444";
    command(fourth,"shutdown","closing");operations[current].automatic=true;ShutdownCompositorPoll();
    for(unsigned i=0;i<10 && operations[current].phase!=Phase::Ready;++i)step();
    command(fourth,"power_status","ready");assert(!strcmp(ShutdownRequestStatus(id),"Shutdown cancelled"));assert(!strcmp(ShutdownDesktopStatus(),"Safe to power off manually"));
    command(fourth,"commit","ready",-95);step();assert(powerWrites==1 && operations[current].phase==Phase::Ready);
    // Firmware reset availability is independent of automatic soft-off.
    current=-1;irreversible=false;
    const char* fifth="55555555555555555555555555555555";
    command(fifth,"restart","closing");ShutdownCompositorPoll();
    for(unsigned i=0;i<10 && operations[current].phase!=Phase::Ready;++i)step();
    command(fifth,"commit","ready");terminalSleeps=true;step();terminalSleeps=false;
    assert(powerWrites==2 && lastRestart);
    current=-1;irreversible=false;powerRestart=false;
    command("66666666666666666666666666666666","restart","unsupported",-95);assert(powerWrites==2);
    current=-1;irreversible=false;powerRestart=true;finalError=-5;
    const char* failedClean="77777777777777777777777777777777";
    command(failedClean,"restart","closing");ShutdownCompositorPoll();
    for(unsigned i=0;i<10 && operations[current].phase!=Phase::Failed;++i)step();
    command(failedClean,"power_status","failed",-5);const auto calls=finalCalls;step();
    assert(finalCalls==calls && powerWrites==2); // no implicit clean-write retry or reset
    current=-1;irreversible=false;finalError=0;directFinalError=-5;
    const char* failedDirect="88888888888888888888888888888888";
    const auto legacyBefore=finalCalls;
    command(failedDirect,"restart","closing");ShutdownCompositorPoll();
    for(unsigned i=0;i<10 && operations[current].phase!=Phase::Failed;++i)step();
    command(failedDirect,"power_status","failed",-5);const auto directBefore=directFinalCalls;step();
    assert(directFinalCalls==directBefore && finalCalls==legacyBefore && powerWrites==2);
    current=-1;irreversible=false;powerOff=true;directFinalError=0;
    auto sample=[](AcPowerSource source){tick+=1000;ShutdownReportAcSource(source);};
    assert(!ShutdownGetAcStatus().enabled && !ShutdownGetAcStatus().ready);
    sample(AcPowerSource::Online);
    for(unsigned i=0;i<10;++i)sample(AcPowerSource::Offline);
    step();assert(current<0 && !ShutdownGetAcStatus().requested);
    acRecovery={};acRecovery.enabled=true;
    for(unsigned i=0;i<10;++i)sample(AcPowerSource::Offline);
    step();assert(current<0 && !ShutdownGetAcStatus().requested); // boot on battery
    sample(AcPowerSource::Online);
    assert(ShutdownGetAcStatus().ready);
    // The real Dell kept reporting Offline for nearly three seconds after
    // restoring AC. A brief relay interruption must not close the desktop.
    for(unsigned i=0;i<4;++i)sample(AcPowerSource::Offline);
    sample(AcPowerSource::Online);
    step();assert(current<0 && ShutdownGetAcStatus().ready);
    sample(AcPowerSource::Offline);
    sample(AcPowerSource::Unknown);
    sample(AcPowerSource::Offline);
    ShutdownReportAcSource(AcPowerSource::Offline); // same-tick duplicates do not count
    sample(AcPowerSource::Offline);
    tick+=3000;assert(ShutdownGetAcStatus().source==AcPowerSource::Unknown);
    for(unsigned i=0;i<8;++i)sample(AcPowerSource::Offline);
    step();assert(current<0);
    sample(AcPowerSource::Offline);
    step();assert(ShutdownGetAcStatus().requested && operations[current].automatic && operations[current].acLoss);
    assert(!ShutdownGetAcStatus().ready && operations[current].phase==Phase::Closing);
    ShutdownCompositorPoll();
    for(unsigned i=0;i<10 && operations[current].phase!=Phase::Ready;++i)step();
    assert(operations[current].phase==Phase::Ready && powerWrites==2);
    terminalSleeps=true;step();terminalSleeps=false;
    assert(powerWrites==3 && !lastRestart);
    current=-1;irreversible=false;
    sample(AcPowerSource::Online);
    for(unsigned i=0;i<10;++i)sample(AcPowerSource::Offline);
    step();assert(current<0 && powerWrites==3); // one attempt per boot
    TestWindowlessEscalation();
    TestConfirmedForceApps();
    // Never execute the host's actual power I/O from this native fixture.
    std::puts("SHUTDOWN_COORDINATOR PASS");
}
