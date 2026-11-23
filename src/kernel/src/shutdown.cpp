#include "shutdown.h"
#include "hda_observe.h"
#include "window.h"
#include "debug_jobs.h"
#include "scheduler.h"
#include "ssd_install_qemu.h"
#include "device.h"
#include "audio.h"
#include "vfs.h"
#include "memory/file_object.h"
#include "ext2_vfs.h"
#include "writable_desktop.h"
#include "ahci_system_volume.h"
#include "ahci_readonly.h"
#include "fw_cfg.h"
#include "platform_power.h"
#include "usb_boot_recovery.h"
#include "apic.h"
#include "compositor.h"
#include "klog.h"
#include "kprintf.h"
#include "serial.h"
#include "string.h"
#include "shutdown_service_escalation.h"
#if defined(BROOK_SSD_GUARDED_DESKTOP)
#include "debug_epoch.h"
#include "guard_checker_runner.h"
#endif
#if defined(BROOK_SSD_GUARDED_DESKTOP_RESET_TEST) || defined(BROOK_SSD_GUARDED_DESKTOP_RESET)
#include "panic_test_reset.h"
#endif

namespace brook {
// Strong definition of vfs.cpp's weak sync failure hook: bridge-visible.
void VfsSyncFailureLog(int stage, uint8_t pdrv, const void* callback, int error) {
    static const char* const names[]={"file writeback","backing sync","retired unmount"};
    KPrintf("vfs: sync: %s failed pdrv=%u callback=%p (%d)\n",names[stage>=0&&stage<3?stage:0],unsigned(pdrv),callback,error);
}
namespace {
enum class Phase { Empty,Closing,Services,Cleanup,Syncing,Ready,Committing,Failed,Cancelled };
struct Operation {
    char id[33];Phase phase;bool restart,automatic,cancel,commit,acLoss;
    bool forceRequested,forceDispatched;
    // syncFailed: Failed came from the final sync (an explicit retry may
    // repeat it). retry/forced: the explicit desktop actions after Failed.
    bool syncFailed,retry,forced;
    int error;
};
Operation operations[64]{};int current=-1;SpinLock lock{};
bool workerStarted=false,powerOff=false,powerRestart=false,irreversible=false;
uint32_t failureGeneration=0;
UsbBootRecoveryPolicy usbBootRecovery{};
constexpr uint64_t AcSampleFreshMs=2000;
constexpr uint64_t AcSampleIntervalMs=1000;
constexpr uint64_t AcOfflineConfirmMs=8000;
struct AcRecovery {
    AcPowerSource source=AcPowerSource::Unknown;
    uint64_t sampledAt=0,offlineSince=0;
    bool enabled=false,sampled=false,seenOnline=false,requested=false,dispatch=false;
} acRecovery;
const char* desktopNotice=nullptr;
uint64_t desktopSequence=0;bool closeCaptured=false,admissionReady=false,closePolling=false;
struct CloseTarget {uint32_t id;uint64_t incarnation;bool sent;};
struct ClientIdentity {uint32_t pid;uint64_t incarnation;uint64_t seenAt;};
constexpr uint64_t ForceAppsGraceMs=10000;
ClientIdentity forceCandidates[WM_MAX_WINDOWS]{},approvedApps[WM_MAX_WINDOWS]{};
uint32_t forceCandidateCount=0,approvedAppCount=0;
ClientIdentity closeClients[MAX_PROCESSES]{};uint32_t closeClientCount=0;
CloseTarget closeTargets[WM_MAX_WINDOWS]{};uint32_t closeCount=0,guiRemaining=0;
DebugTaskSnapshot tasks[MAX_PROCESSES];uint32_t taskCount=0;
ServiceStopPolicy serviceStop{};
WindowlessStopPolicy windowless{};
// Client/provider processes of the focusable windows still open, published by
// the compositor poll with guiRemaining (under lock). Used to name windowed
// blockers and to never signal a process that owns an open window.
uint32_t windowOwners[WM_MAX_WINDOWS*2]{};uint32_t windowOwnerCount=0;
WmDebugWindow workerWindows[WM_MAX_WINDOWS];
// Status and blockers for the current request (under lock). The last status
// byte is never written, so an unlocked reader always finds a terminator.
ShutdownBlockerReport blockerReport{};
void RequestDesktopPower(bool restart,bool acLoss);
const char* Name(Phase phase) {
    switch(phase) {
    case Phase::Empty:return "missing";case Phase::Closing:return "closing";
    case Phase::Services:return "services";case Phase::Cleanup:return "cleanup";
    case Phase::Syncing:return "syncing";case Phase::Ready:return "ready";
    case Phase::Committing:return "committing";case Phase::Failed:return "failed";
    case Phase::Cancelled:return "cancelled";
    }return "failed";
}
ClientIdentity WindowClient(const WmDebugWindow& window) {
    if(window.clientPid && window.clientIncarnation)
        return {window.clientPid,window.clientIncarnation,0};
    // Native VFB apps own their windows. Do not fall back to a Wayland
    // provider for a missing client identity.
    if(window.apiWindow && window.clientOrigin==WmClientOrigin::Unknown)
        return {window.providerPid,window.providerIncarnation,0};
    return {};
}
bool SameClient(const ClientIdentity& a,const ClientIdentity& b) {
    return a.pid==b.pid && a.incarnation==b.incarnation;
}
Process* ReferenceForceApp(const ClientIdentity& identity) {
    auto* process=SchedulerFindProcessReference(uint16_t(identity.pid));
    if(!process)return nullptr;
    if(process->incarnation==identity.incarnation && !process->isKernelThread &&
       process->lifecycleRole!=ProcessLifecycleRole::Service &&
       process->state!=ProcessState::Terminated)return process;
    ProcessUnref(process);
    return nullptr;
}
bool CanForceAppsLocked(const Operation& op) {
    return op.phase==Phase::Closing && !op.cancel && !op.forceRequested && forceCandidateCount;
}
bool Active(Phase phase) {return phase!=Phase::Empty&&phase!=Phase::Failed&&phase!=Phase::Cancelled;}
void SetPhase(int index,Phase phase,int error=0) {
    SpinLockAcquire(&lock);operations[index].phase=phase;operations[index].error=error;
    if(phase==Phase::Failed)++failureGeneration;SpinLockRelease(&lock);
    SerialPrintf("shutdown: request=%s phase=%s result=%d\n",operations[index].id,Name(phase),error);
    CompositorMarkDirty();CompositorWake();
}
// Every fallible final write. Writeback is stopped and device writes are
// closed first; an explicit retry after failure repeats only this step.
// Each step's result goes to the kernel log ring (bridge-visible), so a
// failed checked shutdown names the step that failed.
int FinalSync() {
    int error=VfsSync(nullptr);
    KPrintf("shutdown: final sync vfs=%d\n",error);
#if defined(BROOK_SSD_INSTALLER)
    if(!error){error=AhciPhysicalBlockToolsFinalSync();KPrintf("shutdown: final sync block-tools=%d\n",error);}
#endif
    if(!error){error=AhciSystemDirectFinalSync();KPrintf("shutdown: final sync ahci-direct=%d\n",error);}
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    if(!error){error=AhciSystemQemuLargeDataFinalSync();KPrintf("shutdown: final sync qemu-large=%d\n",error);}
#endif
    if(!error){error=WritableDesktopFinalSync();KPrintf("shutdown: final sync writable-desktop=%d\n",error);}
    return error;
}
void FinishSync(int index,int error) {
    SpinLockAcquire(&lock);operations[index].syncFailed=error!=0;operations[index].retry=false;SpinLockRelease(&lock);
    SetPhase(index,error?Phase::Failed:Phase::Ready,error);
}
uint32_t Census(uint32_t* applications) {
    uint32_t total=0;uint64_t begin=0,end=0;
    const auto count=SchedulerSnapshotDebugTasks(tasks,MAX_PROCESSES,&total,&begin,&end);taskCount=count;
    if(total>count){*applications=MAX_PROCESSES;return MAX_PROCESSES;}
    uint32_t users=0;*applications=0;
    for(uint32_t i=0;i<count;++i)if(!tasks[i].kernel&&tasks[i].state!=ProcessState::Terminated) {
        ++users;if(!tasks[i].systemService)++*applications;
    }
    return users;
}
struct Text {
    char* out;uint32_t cap,n=0;
    void put(const char* x){while(*x&&n+1<cap)out[n++]=*x++;out[n]=0;}
    void num(uint64_t v){char d[21];int k=0;do{d[k++]=char('0'+v%10);v/=10;}while(v&&k<20);while(k&&n+1<cap)out[n++]=d[--k];out[n]=0;}
};
bool Member(const DebugTaskSnapshot& t,bool services) {
    return t.pid&&!t.kernel&&t.systemService==services&&t.state!=ProcessState::Terminated;
}
// First live census entry of its thread group (one entry per process).
bool FirstOfGroup(uint32_t i,bool services) {
    for(uint32_t j=0;j<i;++j)if(Member(tasks[j],services)&&tasks[j].tgid==tasks[i].tgid&&tasks[j].groupIncarnation==tasks[i].groupIncarnation)return false;
    return true;
}
bool OwnsWindow(const DebugTaskSnapshot& t,const uint32_t* owners,uint32_t count) {
    for(uint32_t i=0;i<count;++i)if(owners[i]&&(owners[i]==t.tgid||owners[i]==t.pid))return true;
    return false;
}
// Fills the blocker list from the last Census(): one entry per live process
// group of the selected kind, window owners listed first. Returns the count.
uint32_t CollectBlockers(bool services,const uint32_t* owners,uint32_t ownerCount,ShutdownBlockerReport& r) {
    r.count=r.listed=0;
    for(int pass=0;pass<2;++pass)for(uint32_t i=0;i<taskCount;++i) {
        const auto& t=tasks[i];if(!Member(t,services)||!FirstOfGroup(i,services))continue;
        const bool window=OwnsWindow(t,owners,ownerCount);if(window!=(pass==0))continue;
        ++r.count;if(r.listed>=sizeof(r.entries)/sizeof(r.entries[0]))continue;
        auto& e=r.entries[r.listed++];e.pid=t.tgid?t.tgid:t.pid;e.window=window;e.service=services;
        memcpy(e.name,t.name,sizeof(e.name));e.name[sizeof(e.name)-1]=0;
    }
    return r.count;
}
void PutBlockers(Text& t,const ShutdownBlockerReport& r,uint32_t limit) {
    for(uint32_t i=0;i<r.listed&&i<limit;++i){if(i)t.put(", ");t.put(r.entries[i].name);t.put(" pid ");t.num(r.entries[i].pid);if(r.entries[i].window)t.put(" (window)");}
    const uint32_t shown=r.listed<limit?r.listed:limit;
    if(r.count>shown){t.put(" (+");t.num(r.count-shown);t.put(")");}
}
void Publish(ShutdownBlockerReport& r,const char* stage) {
    unsigned i=0;while(stage[i]&&i+1<sizeof(r.stage)){r.stage[i]=stage[i];++i;}r.stage[i]=0;
    SpinLockAcquire(&lock);
    const bool changed=strcmp(blockerReport.status,r.status)!=0;
    memcpy(&blockerReport,&r,sizeof(blockerReport)-sizeof(blockerReport.status));
    uint32_t n=0;while(r.status[n]&&n+1<sizeof(blockerReport.status)){blockerReport.status[n]=r.status[n];++n;}blockerReport.status[n]=0;
    SpinLockRelease(&lock);
    if(changed){CompositorMarkDirty();CompositorWake();}
}
void FormatServicesStatus(uint64_t now) {
    uint32_t unused=0;Census(&unused);
    ShutdownBlockerReport r{};CollectBlockers(true,nullptr,0,r);
    if(!r.count){
        // Remaining user tasks that are not tagged services still block.
        uint32_t remaining=0;
        for(uint32_t i=0;i<taskCount;++i)if(!tasks[i].kernel&&tasks[i].state!=ProcessState::Terminated)++remaining;
        r.count=remaining;
    }
    Text t{r.status,sizeof(r.status)};
    t.put("Stopping desktop services");
    if(r.listed){t.put(" - waiting for ");PutBlockers(t,r,3);}
    else if(r.count){t.put(" - waiting for ");t.num(r.count);t.put(" tasks");}
    const uint64_t left=serviceStop.MsUntilKill(now);r.nextSignalMs=left;
    if(r.count){if(left){t.put(", forcing in ");t.num((left+999)/1000);t.put(" s");}else t.put(", forced");}
    Publish(r,"services");
}
// Signals each live process group of one kind once, through an incarnation-
// checked member (preferring one that does not mask SIGTERM). Application
// groups that own an open window (owners) are never signalled.
uint32_t SignalGroups(bool services,int signal,const uint32_t* owners,uint32_t ownerCount,bool log) {
    uint32_t unused=0;Census(&unused);uint32_t sent=0;
    for(uint32_t i=0;i<taskCount;++i) {
        const auto& t=tasks[i];if(!Member(t,services)||!FirstOfGroup(i,services))continue;
        if(!services&&OwnsWindow(t,owners,ownerCount))continue;
        Process* recipient=nullptr;
        for(uint32_t j=i;j<taskCount;++j) {
            const auto& member=tasks[j];
            if(!Member(member,services)||member.tgid!=t.tgid||member.groupIncarnation!=t.groupIncarnation)continue;
            auto* p=SchedulerFindProcessReference(member.pid);
            if(!p)continue;
            const bool service=p->lifecycleRole==ProcessLifecycleRole::Service;
            if(p->incarnation!=member.incarnation||p->isKernelThread||service!=services||p->state==ProcessState::Terminated){ProcessUnref(p);continue;}
            const bool unmasked=!(__atomic_load_n(&p->sigMask,__ATOMIC_ACQUIRE)&(1ULL<<14));
            if(!recipient||unmasked){if(recipient)ProcessUnref(recipient);recipient=p;}else ProcessUnref(p);
            if(unmasked)break;
        }
        if(recipient){
            if(log)SerialPrintf("shutdown: %s %s pid=%u signal=%d\n",services?"service":"windowless",t.name,t.tgid,signal);
            ProcessSendSignal(recipient,signal);
            // A stopped process acts on SIGTERM only once continued.
            if(!services&&signal==15&&recipient->state==ProcessState::Stopped)ProcessSendSignal(recipient,18);
            ProcessUnref(recipient);++sent;}
    }
    return sent;
}
void ForceApprovedApps(int selected) {
    SpinLockAcquire(&lock);
    auto& op=operations[selected];
    const bool dispatch=current==selected && op.phase==Phase::Closing &&
        op.forceRequested && !op.forceDispatched && !op.cancel;
    if(dispatch)op.forceDispatched=true;
    SpinLockRelease(&lock);
    if(!dispatch)return;

    uint64_t tick=0;
    const uint32_t count=WmSnapshotDebugWindows(workerWindows,WM_MAX_WINDOWS,&tick);
    for(uint32_t i=0;i<approvedAppCount;++i) {
        const auto& approved=approvedApps[i];
        bool stillOpen=false;
        for(uint32_t j=0;j<count;++j) {
            const auto& window=workerWindows[j];
            if(!window.providerKernel && window.focusable && SameClient(WindowClient(window),approved)) {
                stillOpen=true;break;
            }
        }
        if(!stillOpen)continue;
        if(auto* process=ReferenceForceApp(approved)) {
            KLog("shutdown: confirmed force close of app pid %u",approved.pid);
            ProcessSendSignal(process,9);
            ProcessUnref(process);
        }
    }
}
void StopServices(int signal=15) {SignalGroups(true,signal,nullptr,0,true);}
// Closing phase: bounded escalation for processes that have no window to
// answer the advisory close, and the status naming what still blocks.
void UpdateClosing(int selected,bool captured,uint32_t windows,uint32_t apps,uint64_t now) {
    uint32_t owners[WM_MAX_WINDOWS*2];uint32_t ownerCount=0;
    SpinLockAcquire(&lock);ownerCount=windowOwnerCount;memcpy(owners,windowOwners,ownerCount*sizeof(owners[0]));SpinLockRelease(&lock);
    const bool dispatching=WmLauncherDispatching();
    const bool eligible=captured&&!windows&&apps&&!dispatching;
    const auto action=windowless.Poll(now,eligible);
    if(action!=WindowlessStopPolicy::Action::None) {
        // Recheck from this thread: a window opened since the compositor's
        // last poll cancels this step and restarts the grace period.
        uint64_t tick=0;const uint32_t count=WmSnapshotDebugWindows(workerWindows,WM_MAX_WINDOWS,&tick);
        bool open=false;for(uint32_t i=0;i<count;++i)if(!workerWindows[i].providerKernel&&workerWindows[i].focusable)open=true;
        SpinLockAcquire(&lock);const bool cancelled=operations[selected].cancel;SpinLockRelease(&lock);
        if(open)windowless.Reset();
        else if(!cancelled) {
            const bool term=action==WindowlessStopPolicy::Action::Term;
            const bool first=term||windowless.since==windowless.lastKill;
            ShutdownBlockerReport named{};CollectBlockers(false,owners,ownerCount,named);
            if(first){
                char text[160];Text t{text,sizeof(text)};PutBlockers(t,named,4);
                if(term)KLog("shutdown: no window for %u s; SIGTERM to %s",unsigned(WindowlessStopPolicy::GraceMs/1000),text);
                else KLog("shutdown: still running %u s after SIGTERM; SIGKILL to %s",unsigned(WindowlessStopPolicy::KillMs/1000),text);
            }
            SignalGroups(false,term?15:9,owners,ownerCount,first);
        }
    }
    uint32_t unused=0;Census(&unused);
    ShutdownBlockerReport r{};CollectBlockers(false,owners,ownerCount,r);
    Text t{r.status,sizeof(r.status)};const char* stage="waiting";
    t.put("Closing apps - ");
    if(windows) {
        stage="windows";
        SpinLockAcquire(&lock);
        const auto& op=operations[selected];
        const bool forceAvailable=CanForceAppsLocked(op),forceRequested=op.forceRequested;
        SpinLockRelease(&lock);
        if(forceAvailable)t.put("force close loses unsaved work; ");
        t.num(windows);t.put(windows==1?" window open":" windows open");
        if(r.listed){t.put("; waiting for ");PutBlockers(t,r,3);}
        if(forceRequested)t.put("; waiting for force close");
        else if(!forceAvailable)t.put("; resolve save dialogs, or Cancel");
    }else if(r.count) {
        t.put("no window: ");PutBlockers(t,r,3);t.put("; ");
        r.nextSignalMs=windowless.MsUntilNext(now);stage=windowless.StageName();
        if(windowless.stage==WindowlessStopPolicy::Stage::Grace){t.put("SIGTERM in ");t.num((r.nextSignalMs+999)/1000);t.put(" s");}
        else if(windowless.stage==WindowlessStopPolicy::Stage::Terminated){t.put("SIGKILL in ");t.num((r.nextSignalMs+999)/1000);t.put(" s");}
        else if(windowless.stage==WindowlessStopPolicy::Stage::Killed)t.put("killed, waiting for exit");
        else t.put(dispatching?"waiting for an app launch":"waiting for window close");
        t.put(", or Cancel");
    }else if(dispatching) {stage="draining";t.put("waiting for an app launch, or Cancel");}
    else if(DebugJobsActive()) {stage="draining";t.put("waiting for bridge job output, or Cancel");}
    else t.put("resolve save dialogs, or Cancel");
    Publish(r,stage);
}
void FormatCleanupStatus(const ProcessCleanupStatus& c,bool closed,uint32_t births) {
    ShutdownBlockerReport r{};uint32_t unused=0;Census(&unused);CollectBlockers(false,nullptr,0,r);
    ShutdownBlockerReport services{};CollectBlockers(true,nullptr,0,services);
    for(uint32_t i=0;i<services.listed&&r.listed<4;++i)r.entries[r.listed++]=services.entries[i];
    r.count+=services.count;
    Text t{r.status,sizeof(r.status)};t.put("Waiting for process cleanup");
    if(r.listed){t.put(" - ");PutBlockers(t,r,3);}
    t.put(" (address spaces ");t.num(c.liveUserAddressSpaces);t.put(", queued ");t.num(c.queuedProcesses+c.queuedAddressSpaces);
    t.put(", finalizers ");t.num(c.activeFinalizers);if(!closed||births){t.put(", births ");t.num(births);}t.put(")");
    Publish(r,"cleanup");
}
void Worker(void*) {
    for(;;) {
        SpinLockAcquire(&lock);
        const bool acShutdown=acRecovery.dispatch;
        acRecovery.dispatch=false;
        SpinLockRelease(&lock);
        if(acShutdown) {
            KLog("shutdown: reason=ac-loss");
            RequestDesktopPower(false,true);
        }
        if(usbBootRecovery.enabled && !usbBootRecovery.decided && ApicTickCount()>=60000) {
            UsbBootResetReport report{};const bool available=UsbBootReadReset(report);
            const bool shutdown=usbBootRecovery.ShouldShutdown(ApicTickCount(),powerOff,available,report);
            SerialPrintf("shutdown: usb-boot-recovery uptime_ms=%lu report=%u poweroff=%u requested=%u\n",ApicTickCount(),available?1u:0u,powerOff?1u:0u,shutdown?1u:0u);
            if(shutdown)ShutdownDesktopRequest(false);
        }
        int selected;Operation op{};bool captured,ready,polling;uint32_t windows;
        SpinLockAcquire(&lock);selected=current;if(selected>=0)op=operations[selected];captured=closeCaptured;ready=admissionReady;polling=closePolling;windows=guiRemaining;SpinLockRelease(&lock);
        if(selected<0||!ready||!Active(op.phase)){SchedulerSleepMs(25);continue;}
        if(op.phase==Phase::Closing) {
            if(op.cancel&&!polling) {
                windowless.Reset();
                SchedulerSetUserBirthClosed(false);WmSetLaunchInhibited(false);DebugJobsSetLaunchInhibited(false);
                SetPhase(selected,Phase::Cancelled);continue;
            }
            ForceApprovedApps(selected);
            uint32_t apps=0;Census(&apps);
            if(!op.cancel){UpdateClosing(selected,captured,windows,apps,ApicTickCount());Census(&apps);}
            if(!op.cancel&&captured&&!windows&&!apps&&!WmLauncherDispatching()&&!DebugJobsActive()) {
                SchedulerSetUserBirthClosed(true);
                bool closed=false;uint32_t births=0;SchedulerUserBirthStatus(&closed,&births);
                Census(&apps);
                if(apps)SchedulerSetUserBirthClosed(false);
                if(closed&&!births&&!apps) {
                    // Cancellation and irreversible service stop share this lock.
                    SpinLockAcquire(&lock);
                    if(!operations[selected].cancel&&!closePolling){operations[selected].phase=Phase::Services;irreversible=true;}
                    const bool proceed=operations[selected].phase==Phase::Services;SpinLockRelease(&lock);
                    if(proceed){serviceStop.Begin(ApicTickCount());StopServices();FormatServicesStatus(ApicTickCount());CompositorMarkDirty();}
                }
            }
        }else if(op.phase==Phase::Services) {
            uint32_t apps=0;
            if(!Census(&apps))SetPhase(selected,Phase::Cleanup);
            else {
                // Bounded escalation: a service that never exits must not
                // hold a checked restart in this phase indefinitely.
                const uint64_t now=ApicTickCount();
                const auto action=serviceStop.Poll(now);
                if(action==ServiceStopPolicy::Action::Term)StopServices(15);
                else if(action==ServiceStopPolicy::Action::Kill){KLog("shutdown: services did not stop; sending SIGKILL");StopServices(9);}
                FormatServicesStatus(now);
            }
        }else if(op.phase==Phase::Cleanup) {
            bool closed=false;uint32_t births=0;SchedulerUserBirthStatus(&closed,&births);
            const auto cleanup=ProcessGetCleanupStatus();uint32_t apps=0;
            FormatCleanupStatus(cleanup,closed,births);
            if(closed&&!births&&!Census(&apps)&&!cleanup.liveUserAddressSpaces&&!cleanup.queuedProcesses&&!cleanup.queuedAddressSpaces&&!cleanup.activeFinalizers) {
#if defined(BROOK_SSD_GUARDED_DESKTOP_RESET_TEST) || defined(BROOK_SSD_GUARDED_DESKTOP_RESET)
                // The clean-marker transaction may fail after an uncertain
                // write. Do not force a reset across shutdown's final sync.
                PanicRecoveryResetDisarm();
#endif
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
                if(!SsdQemuInstallCloseReady()){SchedulerSleepMs(25);continue;}
#endif
                FileObjectStopWriteback();
                Ext2StopWriteback();
#if defined(BROOK_EFI_SHUTDOWN_TEST)
                for(uint8_t i=0;i<16;++i){Ext2SyncProbe p{};if(Ext2GetSyncProbe(i,p))
                    SerialPrintf("EFI_SHUTDOWN_PRE_SYNC drive=%u pending=%lu dirty=%u\n",i,p.pendingOps,p.backingDirty?1u:0u);}
#endif
#if defined(BROOK_SSD_GUARDED_DESKTOP)
                GuardPrepareCheckedSlotShutdown();
#endif
                if(!DeviceCloseWrites()){SchedulerSleepMs(25);continue;}
                SetPhase(selected,Phase::Syncing);AudioPrepareShutdown();AudioStop();HdaRuntimeShutdown();
                FinishSync(selected,FinalSync());
            }
        }else if(op.phase==Phase::Syncing&&op.retry) {
            KLog("shutdown: retrying final sync after failure");
            FinishSync(selected,FinalSync());
        }else if(op.phase==Phase::Committing&&op.forced) {
            // Explicit user choice after a failed checked shutdown: data the
            // failed sync could not write is lost. Services are already stopped.
            KLog("shutdown: forced power off after failed shutdown");
            SchedulerSleepMs(500);
            if(PlatformPowerIssue(false))SchedulerSleepMs(1000);
            SetPhase(selected,Phase::Failed,-5);
        }else if(op.phase==Phase::Ready&&(op.automatic||op.commit)&&(op.restart?powerRestart:powerOff)) {
            SetPhase(selected,Phase::Committing);
            // Allow the UART reply and a final progress frame to be delivered.
            SchedulerSleepMs(500);
#if defined(BROOK_SSD_GUARDED_DESKTOP)
            GuardRecordCheckedSlotShutdown();
            if(op.acLoss) {
                char bootId[33];DebugEpochCopy(bootId);
                const bool recorded=GuardRecordCheckedAcShutdown(bootId);
                KLog("shutdown: AC-loss finalization record %s",recorded?"verified":"unavailable");
            }
#endif
            // All fallible sync/cleanup precedes this single selected write.
            // Keep the quiesced kernel available for diagnostics if it returns;
            // the panic-only SMP halt API cannot provide recoverable failure.
            if(PlatformPowerIssue(op.restart))SchedulerSleepMs(1000);
            SetPhase(selected,Phase::Failed,-5);
        }
        SchedulerSleepMs(25);
    }
}
}
static bool ReadShutdownOptIn(const char* path) {
    auto* file=VfsOpen(path,VFS_O_STREAM);
    if(!file)return false;
    VnodeStat st{};char value[2]{};uint64_t offset=0;
    const bool enabled=!VfsStat(file,&st) && !st.isDir && !st.isSymlink && st.size==2 &&
        VfsRead(file,value,2,&offset)==2 && value[0]=='1' && value[1]=='\n';
    VfsRelease(file);
    return enabled;
}
void ShutdownConfigure() {
    PlatformPowerConfigure();
#if defined(BROOK_EFI_SHUTDOWN_TEST)
    char injection[2]{};
    if(FwCfgReadFile("opt/brook/usb-reset-inject",injection,sizeof(injection))==1 && injection[0]=='1') {
        UsbBootReportReset(0x8086,0x9cb1,static_cast<uint32_t>(XhciResetPhase::ExistingReset),
            static_cast<uint32_t>(XhciResetError::Timeout),2,0x11,0x11,true,10002);
        SerialPuts("USB_BOOT_RESET_TEST_INJECTED\n");
    }
#endif
    if(ReadShutdownOptIn("/etc/efi-poweroff"))
        SerialPrintf("efi-runtime: power-off opt-in %s\n",PlatformPowerEnableEfi()?"enabled":"unavailable");
    const auto& plan=PlatformPowerGetPlan();powerOff=plan.CanPowerOff();powerRestart=plan.CanReset();
    usbBootRecovery.enabled=ReadShutdownOptIn("/etc/usb-boot-poweroff");
    acRecovery.enabled=ReadShutdownOptIn("/etc/ac-loss-poweroff");
}
void ShutdownReportAcSource(AcPowerSource source) {
    SpinLockAcquire(&lock);
    const uint64_t now=ApicTickCount();
    const uint64_t elapsed=now-acRecovery.sampledAt;
    if(acRecovery.sampled && elapsed<AcSampleIntervalMs) {
        SpinLockRelease(&lock);
        return;
    }
    if(!acRecovery.sampled || elapsed>AcSampleFreshMs || acRecovery.source!=AcPowerSource::Offline)
        acRecovery.offlineSince=now;
    acRecovery.sampled=true;
    acRecovery.sampledAt=now;
    acRecovery.source=source;
    if(source==AcPowerSource::Online)acRecovery.seenOnline=true;
    if(source==AcPowerSource::Offline && acRecovery.seenOnline &&
       now-acRecovery.offlineSince>=AcOfflineConfirmMs &&
       acRecovery.enabled && workerStarted && powerOff && !acRecovery.requested){
        acRecovery.requested=true;
        acRecovery.dispatch=true;
    }
    SpinLockRelease(&lock);
}
ShutdownAcStatus ShutdownGetAcStatus() {
    SpinLockAcquire(&lock);
    const auto age=acRecovery.sampled?ApicTickCount()-acRecovery.sampledAt:~uint64_t{0};
    const auto source=age<=AcSampleFreshMs?acRecovery.source:AcPowerSource::Unknown;
    const bool shutdownActive=irreversible || (current>=0 && Active(operations[current].phase));
    const bool ready=acRecovery.enabled && workerStarted && powerOff &&
        source==AcPowerSource::Online && !acRecovery.requested && !shutdownActive;
    const ShutdownAcStatus status{source,acRecovery.enabled,ready,acRecovery.requested,age};
    SpinLockRelease(&lock);
    return status;
}
ShutdownPowerCapabilities ShutdownGetPowerCapabilities() {
    const auto& plan=PlatformPowerGetPlan();
    return {workerStarted,powerRestart,powerOff,plan.ResetMethod(),plan.PowerOffMethod(),plan.ResetReason(),plan.PowerOffReason()};
}
void ShutdownStart() {
    // High priority: the worker mostly sleeps, but escalation deadlines and
    // Cancel must not wait behind the CPU-bound processes it is stopping.
    auto* worker=KernelThreadCreate("shutdown",Worker,nullptr,1);
    if(worker){if(PlatformPowerGetPlan().efiEntry)worker->cpuAffinity=0;workerStarted=true;SchedulerAddProcess(worker);}
}
bool ShutdownLaunchAllowed() {
    SpinLockAcquire(&lock);const bool result=!irreversible&&(current<0||!Active(operations[current].phase));SpinLockRelease(&lock);return result;
}
namespace {
void HandleShutdownCommand(const char* id,const char* verb,char state[24],int* error,
                           bool automatic,bool acLoss) {
    *error=0;bool admitted=false;SpinLockAcquire(&lock);int found=-1,empty=-1;
    for(int i=0;i<64;++i){if(operations[i].id[0]&&!strcmp(operations[i].id,id))found=i;else if(!operations[i].id[0]&&empty<0)empty=i;}
    const bool start=!strcmp(verb,"shutdown")||!strcmp(verb,"restart");const bool restart=!strcmp(verb,"restart");
    const bool forceApps=!strcmp(verb,"force_apps");
    const char* result=nullptr;
    if(start&&found>=0&&operations[found].restart!=restart){result="conflict";*error=-22;}
    else if(start&&found<0) {
        if(!workerStarted||(restart&&!powerRestart)){result="unsupported";*error=-95;}
        else if(current>=0&&(Active(operations[current].phase)||irreversible)){result="busy";*error=-16;}
        else if(empty<0){result="full";*error=-28;}
        else {
            found=empty;auto& op=operations[found];memcpy(op.id,id,33);op.phase=Phase::Closing;op.restart=restart;
            op.automatic=automatic;op.acLoss=acLoss;
            // Admission closes under the launch/job locks before the worker can
            // observe this operation. Already dispatched work is awaited.
            current=found;closeCaptured=false;guiRemaining=0;windowOwnerCount=0;admissionReady=false;admitted=true;
            forceCandidateCount=approvedAppCount=0;
            windowless.Reset();memset(&blockerReport,0,sizeof(blockerReport));
        }
    }else if(!start&&!forceApps&&strcmp(verb,"power_status")&&strcmp(verb,"cancel")&&strcmp(verb,"commit")&&
             strcmp(verb,"retry")&&strcmp(verb,"force_power_off")){result="unsupported";*error=-95;}
    if(!result&&found<0){result="missing";*error=-2;}
    if(!result) {
        auto& op=operations[found];
        if(forceApps && !op.forceRequested) {
            if(current!=found || !CanForceAppsLocked(op)){result="not_ready";*error=-16;}
            else {
                approvedAppCount=forceCandidateCount;
                memcpy(approvedApps,forceCandidates,approvedAppCount*sizeof(approvedApps[0]));
                op.forceRequested=true;
            }
        }
        if(!strcmp(verb,"cancel")) {if(op.phase==Phase::Closing)op.cancel=true;else if(op.phase!=Phase::Cancelled){result="too_late";*error=-16;}}
        // After a failed checked shutdown the services are gone and the
        // write barrier stays closed; only these explicit actions remain.
        if(!strcmp(verb,"retry")) {
            if(current==found&&op.phase==Phase::Failed&&op.syncFailed&&irreversible){op.phase=Phase::Syncing;op.retry=true;op.error=0;}
            else {result="not_ready";*error=-16;}
        }
        if(!strcmp(verb,"force_power_off")) {
            if(current==found&&op.phase==Phase::Failed&&irreversible&&powerOff){op.phase=Phase::Committing;op.forced=true;op.error=0;}
            else {result="not_ready";*error=-16;}
        }
        if(!strcmp(verb,"commit")) {if(op.phase==Phase::Ready||op.phase==Phase::Committing) {
                if(op.restart?powerRestart:powerOff)op.commit=true;
                else {result="ready";*error=-95;}
            }else {result="not_ready";*error=-16;}}
        if(!result){result=Name(op.phase);*error=op.error;}
    }
    unsigned i=0;while(result[i]&&i<23){state[i]=result[i];++i;}state[i]=0;SpinLockRelease(&lock);
    if(admitted) {
        WmSetLaunchInhibited(true);DebugJobsSetLaunchInhibited(true);
        SpinLockAcquire(&lock);admissionReady=true;SpinLockRelease(&lock);
    }
    CompositorMarkDirty();CompositorWake();
}
void RequestDesktopPower(bool restart,bool acLoss) {
    char id[33];const auto n=__atomic_add_fetch(&desktopSequence,1ULL,__ATOMIC_RELAXED);
    for(unsigned i=0;i<32;++i)id[i]=i<16?'d':"0123456789abcdef"[(n>>((31-i)*4))&15];id[32]=0;
    char state[24];int error;HandleShutdownCommand(id,restart?"restart":"shutdown",state,&error,true,acLoss);
    if(error)__atomic_store_n(&desktopNotice,error==-95?"Requested power transition is unavailable on this machine":"Another shutdown operation is pending",__ATOMIC_RELEASE);
    else __atomic_store_n(&desktopNotice,static_cast<const char*>(nullptr),__ATOMIC_RELEASE);
}
}
void ShutdownCommand(const char* id,const char* verb,char state[24],int* error) {
    HandleShutdownCommand(id,verb,state,error,false,false);
}
void ShutdownDesktopRequest(bool restart) {RequestDesktopPower(restart,false);}
void ShutdownDesktopCancel() {
    char id[33]{};SpinLockAcquire(&lock);if(current>=0)memcpy(id,operations[current].id,33);SpinLockRelease(&lock);
    if(id[0]){char state[24];int error;ShutdownCommand(id,"cancel",state,&error);if(!error)__atomic_store_n(&desktopNotice,static_cast<const char*>(nullptr),__ATOMIC_RELEASE);}
}
void ShutdownDesktopForceApps() {
    char id[33]{};SpinLockAcquire(&lock);if(current>=0)memcpy(id,operations[current].id,33);SpinLockRelease(&lock);
    if(id[0]){char state[24];int error;ShutdownCommand(id,"force_apps",state,&error);}
}
void ShutdownDesktopRetry() {
    char id[33]{};SpinLockAcquire(&lock);if(current>=0)memcpy(id,operations[current].id,33);SpinLockRelease(&lock);
    if(id[0]){char state[24];int error;ShutdownCommand(id,"retry",state,&error);
        if(!error)SerialPrintf("shutdown: request=%s retry requested\n",id);}
}
void ShutdownDesktopForcePowerOff() {
    char id[33]{};SpinLockAcquire(&lock);if(current>=0)memcpy(id,operations[current].id,33);SpinLockRelease(&lock);
    if(id[0]){char state[24];int error;ShutdownCommand(id,"force_power_off",state,&error);
        if(!error)SerialPrintf("shutdown: request=%s forced power off requested\n",id);}
}
ShutdownFailureActions ShutdownDesktopFailureActions() {
    SpinLockAcquire(&lock);ShutdownFailureActions actions{};
    if(current>=0&&operations[current].phase==Phase::Failed&&irreversible) {
        actions.failed=true;actions.retry=operations[current].syncFailed;actions.forcePowerOff=powerOff;
    }
    SpinLockRelease(&lock);return actions;
}
uint32_t ShutdownFailureGeneration() {
    return __atomic_load_n(&failureGeneration,__ATOMIC_ACQUIRE);
}
bool ShutdownCanForceApps(const char* id) {
    SpinLockAcquire(&lock);
    const bool result=current>=0 && (!id || !strcmp(operations[current].id,id)) &&
        CanForceAppsLocked(operations[current]);
    SpinLockRelease(&lock);
    return result;
}
// Failed: what the user can still do. Strings stay within the launcher
// notice and the shell model's 128-byte status text.
static const char* DescribeFailure(bool restart,bool syncFailed,int error) {
    if(!syncFailed)return powerOff?"Power off did not complete. Force power off, or power off manually.":
        restart?"Restart did not complete. Reset or power off manually.":"Power off did not complete. Power off manually.";
    if(powerOff)return error==-5?(restart?"Restart failed: could not save data (I/O error). Retry or force power off.":
                                      "Shutdown failed: could not save data (I/O error). Retry or force power off."):
        restart?"Restart failed: could not save data. Retry or force power off.":"Shutdown failed: could not save data. Retry or force power off.";
    return error==-5?(restart?"Restart failed: could not save data (I/O error). Retry or power off manually.":
                          "Shutdown failed: could not save data (I/O error). Retry or power off manually."):
        restart?"Restart failed: could not save data. Retry or power off manually.":"Shutdown failed: could not save data. Retry or power off manually.";
}
static const char* DescribePhase(Phase phase,bool restart,bool live=false,bool cancelling=false,
                                 bool syncFailed=false,int error=0) {
    // live: this is the current request, so the worker's detailed status applies.
    if(phase==Phase::Closing&&cancelling)return "Cancelling - reopening the desktop";
    if(live&&(phase==Phase::Closing||phase==Phase::Services||phase==Phase::Cleanup)&&blockerReport.status[0])return blockerReport.status;
    switch(phase) {
    case Phase::Closing:return "Closing apps - resolve save dialogs, or Cancel";
    case Phase::Services:return "Stopping desktop services";
    case Phase::Cleanup:return "Waiting for process cleanup";
    case Phase::Syncing:return "Saving filesystem changes";
    case Phase::Ready:return restart?"Ready to restart":powerOff?"Ready to power off":"Safe to power off manually";
    case Phase::Committing:return "Power transition";
    case Phase::Failed:return DescribeFailure(restart,syncFailed,error);
    case Phase::Cancelled:return "Shutdown cancelled";
    default:return "Unknown shutdown request";
    }
}
const char* ShutdownRequestStatus(const char* id) {
    Phase phase=Phase::Empty;bool restart=false,live=false,cancelling=false,syncFailed=false;int error=0;
    SpinLockAcquire(&lock);
    for(int i=0;i<64;++i){const auto& op=operations[i];if(op.id[0]&&!strcmp(op.id,id)){phase=op.phase;restart=op.restart;live=i==current;cancelling=op.cancel;syncFailed=op.syncFailed;error=op.error;break;}}
    SpinLockRelease(&lock);return DescribePhase(phase,restart,live,cancelling,syncFailed,error);
}
void ShutdownRequestBlockers(const char* id,ShutdownBlockerReport* out) {
    memset(out,0,sizeof(*out));
    Phase phase=Phase::Empty;bool restart=false,live=false,cancelling=false,syncFailed=false;int error=0;
    SpinLockAcquire(&lock);
    for(int i=0;i<64;++i){const auto& op=operations[i];if(op.id[0]&&!strcmp(op.id,id)){phase=op.phase;restart=op.restart;live=i==current;cancelling=op.cancel;syncFailed=op.syncFailed;error=op.error;break;}}
    const bool detailed=live&&!cancelling&&(phase==Phase::Closing||phase==Phase::Services||phase==Phase::Cleanup)&&blockerReport.status[0];
    if(detailed)memcpy(out,&blockerReport,sizeof(*out));
    else {
        const char* stage=phase==Phase::Closing&&cancelling?"cancelling":Name(phase);
        unsigned i=0;while(stage[i]&&i+1<sizeof(out->stage)){out->stage[i]=stage[i];++i;}
        const char* text=DescribePhase(phase,restart,false,cancelling,syncFailed,error);
        i=0;while(text[i]&&i+1<sizeof(out->status)){out->status[i]=text[i];++i;}
    }
    out->status[sizeof(out->status)-1]=0;
    SpinLockRelease(&lock);
}
const char* ShutdownDesktopStatus() {
    SpinLockAcquire(&lock);const auto phase=current<0?Phase::Empty:operations[current].phase;
    const bool restart=current>=0&&operations[current].restart,cancelling=current>=0&&operations[current].cancel;
    const bool syncFailed=current>=0&&operations[current].syncFailed;const int error=current>=0?operations[current].error:0;SpinLockRelease(&lock);
    if(phase!=Phase::Empty&&phase!=Phase::Cancelled)return DescribePhase(phase,restart,true,cancelling,syncFailed,error);
    const auto* notice=__atomic_load_n(&desktopNotice,__ATOMIC_ACQUIRE);return notice?notice:"Applications";
}
bool ShutdownReadyForManualPowerOff() {
    SpinLockAcquire(&lock);
    const bool ready = current >= 0 && operations[current].phase == Phase::Ready &&
                       !operations[current].restart && !powerOff;
    SpinLockRelease(&lock);
    return ready;
}
void ShutdownCompositorPoll() {
    SpinLockAcquire(&lock);const int selected=current;const bool closing=selected>=0&&operations[selected].phase==Phase::Closing&&!operations[selected].cancel&&admissionReady;
    if(closing)closePolling=true;
    const bool capture=closing&&!closeCaptured;SpinLockRelease(&lock);if(!closing)return;
    WmDebugWindow windows[WM_MAX_WINDOWS];uint64_t tick=0;const auto count=WmSnapshotDebugWindows(windows,WM_MAX_WINDOWS,&tick);
    if(capture){closeCount=0;closeClientCount=0;}
    // Retry only previously admitted closes. New dialogs from the same client
    // must remain available for save/cancel decisions.
    uint32_t kept=0;
    for(uint32_t i=0;i<closeCount;++i)if(!closeTargets[i].sent)closeTargets[kept++]=closeTargets[i];
    closeCount=kept;
    const uint32_t seenBefore=closeClientCount;
    for(uint32_t i=0;i<count;++i) {
        const auto& w=windows[i];if(w.providerKernel||!w.focusable)continue;
        auto identity=WindowClient(w);
        bool known=false;for(uint32_t j=0;j<seenBefore;++j)if(SameClient(closeClients[j],identity))known=true;
        if(known||(!capture&&(!identity.pid||!identity.incarnation)))continue;
        if(closeCount==WM_MAX_WINDOWS||closeClientCount==MAX_PROCESSES)continue; // fail closed on finite tracking capacity
        closeTargets[closeCount++]={w.id,w.incarnation,false};
        bool added=false;for(uint32_t j=seenBefore;j<closeClientCount;++j)if(SameClient(closeClients[j],identity))added=true;
        if(!added){identity.seenAt=ApicTickCount();closeClients[closeClientCount++]=identity;}
    }
    for(uint32_t i=0;i<closeCount;++i)if(!closeTargets[i].sent) {
        WmDebugWindow result{};uint32_t method=0;auto& target=closeTargets[i];
        const int error=WmApplyDebugAction(target.id,target.incarnation,WmDebugAction::Close,0,&result,&method);
        if(error!=-11)target.sent=true;
    }
    uint32_t remaining=0,owners[WM_MAX_WINDOWS*2];uint32_t ownerCount=0;
    ClientIdentity candidates[WM_MAX_WINDOWS];uint32_t candidateCount=0;
    const uint64_t now=ApicTickCount();
    for(uint32_t i=0;i<count;++i)if(!windows[i].providerKernel&&windows[i].focusable) {
        ++remaining;
        const auto identity=WindowClient(windows[i]);
        if(identity.pid && identity.pid<MAX_PROCESSES && identity.incarnation) {
            bool mature=false;
            for(uint32_t j=0;j<closeClientCount;++j)
                if(SameClient(closeClients[j],identity) && now-closeClients[j].seenAt>=ForceAppsGraceMs)
                    mature=true;
            bool duplicate=false;
            for(uint32_t j=0;j<candidateCount;++j)if(SameClient(candidates[j],identity))duplicate=true;
            if(mature && !duplicate)if(auto* process=ReferenceForceApp(identity)) {
                candidates[candidateCount++]=identity;
                ProcessUnref(process);
            }
        }
        const uint32_t ids[2]={windows[i].clientPid,windows[i].providerPid};
        for(const uint32_t pid:ids){if(!pid)continue;bool seen=false;for(uint32_t j=0;j<ownerCount;++j)if(owners[j]==pid)seen=true;if(!seen&&ownerCount<WM_MAX_WINDOWS*2)owners[ownerCount++]=pid;}
    }
    SpinLockAcquire(&lock);closePolling=false;if(current==selected&&operations[selected].phase==Phase::Closing){closeCaptured=true;guiRemaining=remaining;
        memcpy(windowOwners,owners,ownerCount*sizeof(owners[0]));windowOwnerCount=ownerCount;
        memcpy(forceCandidates,candidates,candidateCount*sizeof(candidates[0]));forceCandidateCount=candidateCount;}SpinLockRelease(&lock);
}
}
