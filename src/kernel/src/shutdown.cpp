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
#include "serial.h"
#include "string.h"
#if defined(BROOK_SSD_GUARDED_DESKTOP)
#include "debug_epoch.h"
#include "guard_checker_runner.h"
#endif
#if defined(BROOK_SSD_GUARDED_DESKTOP_RESET_TEST) || defined(BROOK_SSD_GUARDED_DESKTOP_RESET)
#include "panic_test_reset.h"
#endif

namespace brook {
namespace {
enum class Phase { Empty,Closing,Services,Cleanup,Syncing,Ready,Committing,Failed,Cancelled };
struct Operation {char id[33];Phase phase;bool restart,automatic,cancel,commit,acLoss;int error;};
Operation operations[64]{};int current=-1;SpinLock lock{};
bool workerStarted=false,powerOff=false,powerRestart=false,irreversible=false;
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
struct ClientIdentity {uint32_t pid;uint64_t incarnation;};
ClientIdentity closeClients[MAX_PROCESSES]{};uint32_t closeClientCount=0;
CloseTarget closeTargets[WM_MAX_WINDOWS]{};uint32_t closeCount=0,guiRemaining=0;
DebugTaskSnapshot tasks[MAX_PROCESSES];uint32_t taskCount=0;
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
bool Active(Phase phase) {return phase!=Phase::Empty&&phase!=Phase::Failed&&phase!=Phase::Cancelled;}
void SetPhase(int index,Phase phase,int error=0) {
    SpinLockAcquire(&lock);operations[index].phase=phase;operations[index].error=error;SpinLockRelease(&lock);
    SerialPrintf("shutdown: request=%s phase=%s result=%d\n",operations[index].id,Name(phase),error);
    CompositorMarkDirty();CompositorWake();
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
void StopServices() {
    uint32_t unused=0;Census(&unused);
    for(uint32_t i=0;i<taskCount;++i) {
        const auto& t=tasks[i];if(!t.pid||t.kernel||!t.systemService||t.state==ProcessState::Terminated)continue;
        bool prior=false;
        for(uint32_t j=0;j<i;++j)if(!tasks[j].kernel&&tasks[j].systemService&&tasks[j].state!=ProcessState::Terminated&&tasks[j].tgid==t.tgid&&tasks[j].groupIncarnation==t.groupIncarnation)prior=true;
        if(prior)continue;
        Process* recipient=nullptr;
        for(uint32_t j=i;j<taskCount;++j) {
            const auto& member=tasks[j];
            if(member.kernel||!member.systemService||member.state==ProcessState::Terminated||member.tgid!=t.tgid||member.groupIncarnation!=t.groupIncarnation)continue;
            auto* p=SchedulerFindProcessReference(member.pid);
            if(!p)continue;
            if(p->incarnation!=member.incarnation||p->isKernelThread||p->lifecycleRole!=ProcessLifecycleRole::Service||p->state==ProcessState::Terminated){ProcessUnref(p);continue;}
            const bool unmasked=!(__atomic_load_n(&p->sigMask,__ATOMIC_ACQUIRE)&(1ULL<<14));
            if(!recipient||unmasked){if(recipient)ProcessUnref(recipient);recipient=p;}else ProcessUnref(p);
            if(unmasked)break;
        }
        if(recipient){ProcessSendSignal(recipient,15);ProcessUnref(recipient);}
    }
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
                SchedulerSetUserBirthClosed(false);WmSetLaunchInhibited(false);DebugJobsSetLaunchInhibited(false);
                SetPhase(selected,Phase::Cancelled);continue;
            }
            uint32_t apps=0;Census(&apps);
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
                    if(proceed){StopServices();CompositorMarkDirty();}
                }
            }
        }else if(op.phase==Phase::Services) {
            uint32_t apps=0;if(!Census(&apps))SetPhase(selected,Phase::Cleanup);
        }else if(op.phase==Phase::Cleanup) {
            bool closed=false;uint32_t births=0;SchedulerUserBirthStatus(&closed,&births);
            const auto cleanup=ProcessGetCleanupStatus();uint32_t apps=0;
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
                if(!DeviceCloseWrites()){SchedulerSleepMs(25);continue;}
                SetPhase(selected,Phase::Syncing);AudioPrepareShutdown();AudioStop();HdaRuntimeShutdown();
                int error=VfsSync(nullptr);
#if defined(BROOK_SSD_INSTALLER)
                if(!error)error=AhciPhysicalBlockToolsFinalSync();
#endif
                if(!error)error=AhciSystemDirectFinalSync();
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
                if(!error)error=AhciSystemQemuLargeDataFinalSync();
#endif
                if(!error)error=WritableDesktopFinalSync();
                SetPhase(selected,error?Phase::Failed:Phase::Ready,error);
            }
        }else if(op.phase==Phase::Ready&&(op.automatic||op.commit)&&(op.restart?powerRestart:powerOff)) {
            SetPhase(selected,Phase::Committing);
            // Allow the UART reply and a final progress frame to be delivered.
            SchedulerSleepMs(500);
#if defined(BROOK_SSD_GUARDED_DESKTOP)
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
    auto* worker=KernelThreadCreate("shutdown",Worker,nullptr,3);
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
            current=found;closeCaptured=false;guiRemaining=0;admissionReady=false;admitted=true;
        }
    }else if(!start&&strcmp(verb,"power_status")&&strcmp(verb,"cancel")&&strcmp(verb,"commit")){result="unsupported";*error=-95;}
    if(!result&&found<0){result="missing";*error=-2;}
    if(!result) {
        auto& op=operations[found];
        if(!strcmp(verb,"cancel")) {if(op.phase==Phase::Closing)op.cancel=true;else if(op.phase!=Phase::Cancelled){result="too_late";*error=-16;}}
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
static const char* DescribePhase(Phase phase,bool restart) {
    switch(phase) {
    case Phase::Closing:return "Closing apps - resolve save dialogs, or Cancel";
    case Phase::Services:return "Stopping desktop services";
    case Phase::Cleanup:return "Waiting for process cleanup";
    case Phase::Syncing:return "Saving filesystem changes";
    case Phase::Ready:return restart?"Ready to restart":powerOff?"Ready to power off":"Safe to power off manually";
    case Phase::Committing:return "Power transition";
    case Phase::Failed:return "Shutdown failed - inspect lifecycle status";
    case Phase::Cancelled:return "Shutdown cancelled";
    default:return "Unknown shutdown request";
    }
}
const char* ShutdownRequestStatus(const char* id) {
    Phase phase=Phase::Empty;bool restart=false;
    SpinLockAcquire(&lock);
    for(const auto& op:operations)if(op.id[0]&&!strcmp(op.id,id)){phase=op.phase;restart=op.restart;break;}
    SpinLockRelease(&lock);return DescribePhase(phase,restart);
}
const char* ShutdownDesktopStatus() {
    SpinLockAcquire(&lock);const auto phase=current<0?Phase::Empty:operations[current].phase;
    const bool restart=current>=0&&operations[current].restart;SpinLockRelease(&lock);
    if(phase!=Phase::Empty&&phase!=Phase::Cancelled)return DescribePhase(phase,restart);
    const auto* notice=__atomic_load_n(&desktopNotice,__ATOMIC_ACQUIRE);return notice?notice:"Applications";
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
        bool known=false;for(uint32_t j=0;j<seenBefore;++j)if(closeClients[j].pid==w.clientPid&&closeClients[j].incarnation==w.clientIncarnation)known=true;
        if(known||(!capture&&(!w.clientPid||!w.clientIncarnation)))continue;
        if(closeCount==WM_MAX_WINDOWS||closeClientCount==MAX_PROCESSES)continue; // fail closed on finite tracking capacity
        closeTargets[closeCount++]={w.id,w.incarnation,false};
        bool added=false;for(uint32_t j=seenBefore;j<closeClientCount;++j)if(closeClients[j].pid==w.clientPid&&closeClients[j].incarnation==w.clientIncarnation)added=true;
        if(!added)closeClients[closeClientCount++]={w.clientPid,w.clientIncarnation};
    }
    for(uint32_t i=0;i<closeCount;++i)if(!closeTargets[i].sent) {
        WmDebugWindow result{};uint32_t method=0;auto& target=closeTargets[i];
        const int error=WmApplyDebugAction(target.id,target.incarnation,WmDebugAction::Close,0,&result,&method);
        if(error!=-11)target.sent=true;
    }
    uint32_t remaining=0;for(uint32_t i=0;i<count;++i)if(!windows[i].providerKernel&&windows[i].focusable)++remaining;
    SpinLockAcquire(&lock);closePolling=false;if(current==selected&&operations[selected].phase==Phase::Closing){closeCaptured=true;guiRemaining=remaining;}SpinLockRelease(&lock);
}
}
