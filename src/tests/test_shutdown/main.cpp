#include <cassert>
#include <cstdio>
#include "../../kernel/src/shutdown.cpp"
namespace {
struct Yield {};
bool launched=true,jobsAllowed=true,jobsBusy=false,dispatching=false,gate=false,canFreeze=true;
uint32_t births=0;unsigned closes=0,signals=0,syncs=0,freezes=0;
unsigned writebackStops=0,powerWrites=0;
bool terminalSleeps=false,lastRestart=false;
brook::PlatformPowerPlan powerPlan;
bool writebackStopped=false;
int syncError=0;
int finalError=0;unsigned finalCalls=0;
brook::ProcessCleanupStatus cleanup{};
brook::Process serviceTasks[2]{};unsigned signalPid=0;
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
Process* SchedulerFindProcessReference(uint16_t pid){for(auto& p:serviceTasks)if(p.pid==pid){ProcessRef(&p);return &p;}return nullptr;}
int ProcessSendSignal(Process* p,int,const SignalInfo*){++signals;signalPid=p->pid;return 0;}
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
int VfsSync(Vnode*){++syncs;return syncError;}
int WritableDesktopFinalSync(){assert(writebackStopped && canFreeze && !syncError);++finalCalls;return finalError;}
void CompositorMarkDirty(){}
void CompositorWake(){}
uint32_t FwCfgReadFile(const char*,void*,uint32_t){return 0;}
void SerialPrintf(const char*,...){}
}
int main(){
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
    canFreeze=true;syncError=-5;step();command(second,"power_status","failed",-5);assert(syncs==1 && !finalCalls);
    step();assert(syncs==1);command(second,"shutdown","failed",-5);
    // A fresh explicit attempt may retry a failed flush; no implicit retry.
    const char* third="33333333333333333333333333333333";
    command(third,"shutdown","busy",-16);assert(!launched&&!jobsAllowed&&gate);
    // Simulate a fresh boot for the successful transition case. Production
    // must not undo the irreversible service/write-barrier stage after error.
    current=-1;irreversible=false;
    command(third,"shutdown","closing");ShutdownCompositorPoll();step();step();syncError=0;step();
    command(third,"power_status","ready");assert(syncs==2&&gate && finalCalls==1);
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
    // Never execute the host's actual power I/O from this native fixture.
    std::puts("SHUTDOWN_COORDINATOR PASS");
}
