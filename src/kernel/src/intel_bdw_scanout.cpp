#include "scheduler.h"
#include "intel_bdw_render_refusal.h"
#include "intel_bdw_worker_overhead.h"
#include "intel_bdw_scanout.h"
#include "intel_bdw_node_cache.h"
#include "intel_bdw_prompt_wake.h"
#include "intel_bdw_submission_timing.h"
#include "intel_bdw_worker_stats.h"
#include "intel_bdw_poll_pacer.h"
#include "hda_dell_inventory.h"
#include "boot_protocol/boot_protocol.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_display_probe.h"
#include "intel_bdw_scanout_target.h"
#include "intel_bdw_forcewake_ack.h"
#include "intel_bdw_forcewake_adapter.h"
#include "intel_bdw_oneshot.h"
#include "intel_bdw_pat.h"
#include "intel_bdw_render_gt_init.h"
#include "native_tsc_observe.h"
#include "memory/virtual_memory.h"
#include "intel_bdw_critical.h"
#include "memory/heap.h"
#include "acpi.h"
#include "smp.h"
#include "serial.h"
#include "kprintf.h"
#include "vfs.h"
#include "panic_test_reset.h"
#include "intel_bdw_submission_worker.h"
#include "profiler.h"
#if defined(BROOK_BDW_RPS_TRIAL)
#include "shutdown.h"
#include "intel_bdw_render_clock.h"
#include "intel_bdw_render_clock_runtime.h"
#endif
#include "process.h"
#include "intel_bdw_file.h"
#include "compositor.h"
#include "panic_qr.h"
#include "intel_bdw_video.h"
#include "intel_bdw_vcs_user.h"
#include "tsc_clock.h"
#include "kprintf.h"
extern "C" [[noreturn]] void KernelPanic(const char*,...);
namespace brook {
namespace {
BdwGopObservation observation{};
bool probeAttempted=false;
// Covers Brook diagnostic operations through CPU-side retirement. Contenders
// are refused without consuming their one-shot attempt. Firmware is separate.
uint32_t diagnosticOwner=0;
struct DiagnosticLease {
    bool held=false;
    DiagnosticLease(){uint32_t expected=0;held=__atomic_compare_exchange_n(&diagnosticOwner,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);}
    ~DiagnosticLease(){if(held)__atomic_store_n(&diagnosticOwner,0,__ATOMIC_RELEASE);}
};
#if defined(BROOK_SSD_AUTO_RECOVERY)
uint32_t scratchMappingReady=0;
uint32_t firstReleased=0;
// Read from the boot filesystem before the scheduler starts. Device identity,
// D0 and recovery admission are still checked at the existing call sites.
struct RuntimeBootOptions {
    bool execStore=false, contextWorker=false, renderWorker=false;
    bool intelNode=false, renderExperiment=false, autoBootstrap=false;
    // Kill switch for the RCS completion spin slices (BdwPollPacer).
    bool pollSpinDisabled=false;
};
RuntimeBootOptions runtimeBootOptions{};
uint32_t* retainedRenderContext=nullptr;
uint32_t* rebasedRenderContext=nullptr;
#endif
bool OptedIn(const char* path){auto* f=VfsOpen(path,VFS_O_STREAM);if(!f)return false;
    VnodeStat s{};uint64_t offset=0;char t[2]{};bool ok=!VfsStat(f,&s) && !s.isDir && !s.isSymlink && s.size==2 &&
        VfsRead(f,t,2,&offset)==2 && t[0]=='1' && t[1]=='\n';VfsRelease(f);return ok;}
bool PowerD0(const PciDevice& d){
    const uint16_t status=PciConfigRead16(d.bus,d.dev,d.fn,6);
    if(status==0xffff || !(status&0x10))return false;
    uint8_t cap=PciConfigRead8(d.bus,d.dev,d.fn,0x34);uint64_t seen=0;
    for(unsigned n=0;n<48 && cap;++n){
        if(cap<0x40 || (cap&3) || cap>0xf8)return false;
        const uint64_t bit=uint64_t(1)<<(cap/4);if(seen&bit)return false;seen|=bit;
        if(PciConfigRead8(d.bus,d.dev,d.fn,cap)==1){
            const uint16_t pmc=PciConfigRead16(d.bus,d.dev,d.fn,cap+2),pmcsr=PciConfigRead16(d.bus,d.dev,d.fn,cap+4);
            return pmc!=0xffff && (pmc&7)>=1 && (pmc&7)<=3 && pmcsr!=0xffff && !(pmcsr&3);
        }
        cap=PciConfigRead8(d.bus,d.dev,d.fn,cap+1);
    }return false;
}
bool PageAdmitted(uint64_t page){BootResourceClassification c{};
    constexpr uint32_t allowed=(1u<<uint32_t(MemoryType::Reserved))|(1u<<uint32_t(MemoryType::Mmio));
    if(!AcpiClassifyBootRange(page,4096,c))return false;
    return (!c.types && c.unreported) || (!c.unreported && c.types && !(c.types&~allowed));
}
bool LiveGpuMatches(const PciBootEntry& e){const auto& d=e.device;
    const uint16_t command=PciConfigRead16(d.bus,d.dev,d.fn,4);
    if(PciConfigRead32(d.bus,d.dev,d.fn,0)!=(uint32_t(d.deviceId)<<16|d.vendorId) ||
       PciConfigRead32(d.bus,d.dev,d.fn,0x2c)!=(uint32_t(e.subsystemDevice)<<16|e.subsystemVendor) ||
       (PciConfigRead32(d.bus,d.dev,d.fn,8)&255)!=e.revision || command==0xffff || !(command&2) ||
       PciConfigRead16(d.bus,d.dev,d.fn,0x50)!=e.bdwGraphics.graphicsControl ||
       PciConfigRead8(d.bus,d.dev,d.fn,0x62)!=e.bdwGraphics.apertureControl)return false;
    for(unsigned i=0;i<4;++i)if(PciConfigRead32(d.bus,d.dev,d.fn,0x10+i*4)!=d.bar[i])return false;
    return true;
}
}
bool BdwObserveDisplayLive(BdwDisplaySnapshot& out) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    if(!PanicRecoveryResetArmed() || !observation.inventory || !observation.planned)return false;
    for(unsigned i=0;i<3;++i)if(!PageAdmitted(0xf6070000ull+i*4096))return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        if(!LiveGpuMatches(entry) || !PowerD0(d))return false;
        BdwDisplaySnapshot snapshot{};
        if(!VmmDellDisplayCriticalRead(snapshot) || !LiveGpuMatches(entry) || !PowerD0(d))return false;
        out=snapshot;return true;
    }
#else
    (void)out;
#endif
    return false;
}
// Display state admissible for GPU work: the GOP mode with the GOP surface
// or a mapped kernel flip buffer on plane A. A flip can latch between the
// two stable reads of a snapshot, so flip builds retry a few times.
bool BdwObserveDisplayAccepted(BdwDisplaySnapshot& out,unsigned registers) {
#if defined(BROOK_BDW_SCANOUT_FLIP)
    constexpr unsigned attempts=4;
#else
    constexpr unsigned attempts=1;
#endif
    for(unsigned i=0;i<attempts;++i){
        BdwDisplaySnapshot display{};
        if(!BdwObserveDisplayLive(display))continue;
        if(!BdwDisplayCompatible(display,[](uint32_t surface){return BdwScanoutSurfaceAccepted(surface);},registers))return false;
        out=display;return true;
    }
    return false;
}
bool BdwObserveForcewakeLive(BdwForcewakeAdapterResult& out,bool otherEngines,bool scopeCheck) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    DiagnosticLease lease;if(!lease.held)return false;
    if((scopeCheck && !otherEngines) || !PanicRecoveryResetArmed() || SmpCurrentCpuIndex()!=0 || !observation.inventory || !observation.planned ||
       !observation.patRequested || !PageAdmitted(BdwPrivatePatPage))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    if(otherEngines)for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        if(!LiveGpuMatches(entry) || !PowerD0(d))return false;
        BdwForcewakeAdapterResult sample{};VmmDellForcewakeCriticalRun(tsc.khz,sample,otherEngines,true,false,false,scopeCheck);
        out=sample;
        return sample.aliases && sample.clock && sample.sequence.acquired && sample.sequence.threadReady &&
            sample.sequence.sampled && sample.sequence.patSampled && (!otherEngines || sample.sequence.otherEnginesSampled) &&
            (!scopeCheck || (sample.sequence.workAttempted && sample.sequence.workComplete)) && sample.clearDelivered && sample.sequence.released &&
            LiveGpuMatches(entry) && PowerD0(d);
    }
#else
    (void)out;(void)otherEngines;(void)scopeCheck;
#endif
    return false;
}
bool BdwTestTlbInvalidate(BdwForcewakeAdapterResult& out) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    DiagnosticLease lease;if(!lease.held)return false;
    static uint32_t attempted=0;
    if(__atomic_load_n(&attempted,__ATOMIC_ACQUIRE) || !PanicRecoveryResetArmed() || SmpCurrentCpuIndex()!=0 || !observation.inventory || !observation.planned ||
       !observation.patRequested || !OptedIn("/etc/dell-gpu-tlb-test") || !PageAdmitted(BdwPrivatePatPage))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        if(!LiveGpuMatches(entry) || !PowerD0(d))return false;
        // One explicit attempt per boot, including denied or uncertain writes.
        uint32_t expected=0;
        if(!__atomic_compare_exchange_n(&attempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        BdwForcewakeAdapterResult sample{};VmmDellForcewakeCriticalRun(tsc.khz,sample,true,true,true);
        out=sample;
        return sample.sequence.tlbAttempted && sample.sequence.tlbComplete && sample.clearDelivered &&
            sample.sequence.released && LiveGpuMatches(entry) && PowerD0(d);
    }
#else
    (void)out;
#endif
    return false;
}
bool BdwTestBcsReset(BdwForcewakeAdapterResult& out) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    DiagnosticLease lease;if(!lease.held)return false;
    static uint32_t attempted=0;
    if(__atomic_load_n(&attempted,__ATOMIC_ACQUIRE) || !PanicRecoveryResetArmed() || SmpCurrentCpuIndex()!=0 ||
       !observation.inventory || !observation.planned || !observation.patRequested ||
       !OptedIn("/etc/dell-gpu-bcs-reset-test") || !PageAdmitted(BdwPrivatePatPage) || !PageAdmitted(BdwResetPage))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        if(!LiveGpuMatches(entry) || !PowerD0(d))return false;
        uint32_t expected=0;if(!__atomic_compare_exchange_n(&attempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        BdwForcewakeAdapterResult sample{};VmmDellForcewakeCriticalRun(tsc.khz,sample,true,true,false,true);
        out=sample;
        return sample.sequence.resetAttempted && sample.sequence.resetComplete && sample.clearDelivered &&
            sample.sequence.released && LiveGpuMatches(entry) && PowerD0(d);
    }
#else
    (void)out;
#endif
    return false;
}
bool BdwTestScratchTakeover(BdwForcewakeAdapterResult& out,BdwScratchTakeoverResult& scratch) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    DiagnosticLease lease;if(!lease.held)return false;
    static uint32_t attempted=0;
    scratch.stage=1;
    if(__atomic_load_n(&attempted,__ATOMIC_ACQUIRE) || !PanicRecoveryResetArmed())return false;
    scratch.stage=2;if(SmpCurrentCpuIndex()!=0)return false;
    scratch.stage=3;
    // The VMM callback resets BCS under the same forcewake lease before it
    // replaces an inherited entry. Other engines must be quiescent and the
    // live display reservation must be outside this explicitly selected slot.
    if(!observation.inventory || !observation.planned || !observation.patRequested ||
       !OptedIn("/etc/dell-gpu-scratch-takeover"))return false;
    scratch.stage=4;if(observation.plan.lastIndex>=65536 || BdwInspectPrivateBcsDma()!=BdwDmaSnapshotOk)return false;
    scratch.stage=5;
    if(!PageAdmitted(BdwPrivatePatPage) || !PageAdmitted(BdwResetPage) || !PageAdmitted(0xf6880000ull) || !PageAdmitted(0xf6101000ull))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    scratch.stage=6;
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    scratch.stage=7;
    BdwDisplaySnapshot display{};
    if(!BdwObserveDisplayLive(display) || display.regs[0][0]!=0x98000000u || display.regs[0][1]!=12800)return false;
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned reg=0;reg<10;++reg)
        if((pipe || reg>1) && display.regs[pipe][reg])return false;
    scratch.stage=8;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        if(!LiveGpuMatches(entry) || !PowerD0(d) || !(PciConfigRead16(d.bus,d.dev,d.fn,4)&4))return false;
        BdwGgttReservation reserved{};BdwOwnedPage candidate{};
        if(!BdwRetainedBcsReservation(reserved) || reserved.first!=65536 || reserved.count!=8 ||
           !BdwRetainedBcsScratch(candidate))return false;
        scratch.stage=9;
        uint32_t expected=0;if(!__atomic_compare_exchange_n(&attempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        BdwOwnedPage pinned{};BdwGgttReservation pinnedRange{};
        if(!BdwPinRetainedScratchForTakeover(pinned,pinnedRange) || pinned.dma!=candidate.dma ||
           pinned.cpu!=candidate.cpu || pinnedRange.token!=reserved.token || pinnedRange.first!=reserved.first)return false;
        scratch.stage=10;
        BdwForcewakeAdapterResult result{};BdwScratchTakeoverResult takeover{};
        VmmDellForcewakeScratchRun(tsc.khz,pinnedRange.first,pinned.dma,result,takeover);
        out=result;scratch=takeover;scratch.stage=11;
        const bool ok=takeover.resetComplete && takeover.complete && result.sequence.workComplete && result.clearDelivered && result.sequence.released &&
            LiveGpuMatches(entry) && PowerD0(d);
        if(ok)__atomic_store_n(&scratchMappingReady,1,__ATOMIC_RELEASE);
        return ok;
    }
#else
    (void)out;(void)scratch;
#endif
    return false;
}
bool BdwTestBcsInitialize(BdwForcewakeAdapterResult& out,BdwBcsInitResult& init) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    DiagnosticLease lease;if(!lease.held)return false;
    static uint32_t attempted=0;init.stage=1;
    if(__atomic_load_n(&attempted,__ATOMIC_ACQUIRE) || !PanicRecoveryResetArmed() ||
       !__atomic_load_n(&scratchMappingReady,__ATOMIC_ACQUIRE))return false;
    init.stage=2;if(SmpCurrentCpuIndex()!=0)return false;
    init.stage=3;
    if(!observation.inventory || !observation.planned || !observation.patRequested ||
       !OptedIn("/etc/dell-gpu-bcs-init-test") || BdwInspectPrivateBcsDma()!=BdwDmaSnapshotOk ||
       !PageAdmitted(BdwResetPage) || !PageAdmitted(BdwPrivatePatPage) || !PageAdmitted(0xf6880000ull))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    init.stage=4;const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    BdwDisplaySnapshot display{};
    if(!BdwObserveDisplayLive(display) || display.regs[0][0]!=0x98000000u || display.regs[0][1]!=12800)return false;
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned reg=0;reg<10;++reg)
        if((pipe || reg>1) && display.regs[pipe][reg])return false;
    init.stage=5;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        BdwOwnedPage scratch{};BdwGgttReservation reservation{};
        if(!LiveGpuMatches(entry) || !PowerD0(d) || !(PciConfigRead16(d.bus,d.dev,d.fn,4)&4) ||
           !BdwRetainedBcsReservation(reservation) || reservation.first!=65536 ||
           !BdwRetainedBcsScratch(scratch))return false;
        uint32_t expected=0;if(!__atomic_compare_exchange_n(&attempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        BdwForcewakeAdapterResult result{};BdwBcsInitResult state{};
        VmmDellForcewakeBcsInit(tsc.khz,reservation.first,scratch.dma,scratch.cpu,result,state);
        out=result;init=state;init.stage=6;
        return state.initComplete && result.sequence.workComplete && result.clearDelivered && result.sequence.released &&
            LiveGpuMatches(entry) && PowerD0(d);
    }
#else
    (void)out;(void)init;
#endif
    return false;
}
bool BdwTestPrivatePat(BdwForcewakeAdapterResult& out,BdwPatTestResult& pat) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    DiagnosticLease lease;if(!lease.held)return false;
    static uint32_t attempted=0;
    if(__atomic_load_n(&attempted,__ATOMIC_ACQUIRE) || !PanicRecoveryResetArmed() || SmpCurrentCpuIndex()!=0 ||
       !observation.inventory || !observation.planned || !observation.patRequested ||
       !OptedIn("/etc/dell-gpu-pat-transition-test") || !PageAdmitted(BdwPrivatePatPage) || !PageAdmitted(BdwResetPage))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    BdwDisplaySnapshot display{};
    if(!BdwObserveDisplayLive(display) || display.regs[0][0]!=0x98000000u || display.regs[0][1]!=12800)return false;
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned reg=0;reg<10;++reg)
        if((pipe || reg>1) && display.regs[pipe][reg])return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        if(!LiveGpuMatches(entry) || !PowerD0(d))return false;
        uint32_t expected=0;if(!__atomic_compare_exchange_n(&attempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        VmmDellForcewakePatTest(tsc.khz,out,pat);
        return pat.resetComplete && pat.established && pat.restored && out.sequence.workComplete && out.clearDelivered && out.sequence.released &&
            LiveGpuMatches(entry) && PowerD0(d);
    }
#else
    (void)out;(void)pat;
#endif
    return false;
}
// ---- VA-API phase 1: VCS0 video engine bring-up (docs/bdw-video-engine.md) ----
namespace {
BdwVideoGate videoGate;
uint32_t videoOwner=0;
// A diagnostic-slot lease that waits briefly (1 ms steps) instead of failing
// immediately when another diagnostic holds the slot.
struct RetryLease {
    uint32_t* owner;bool held=false;uint32_t tries=0;
    RetryLease(uint32_t* o,uint32_t limit):owner(o){
        for(;;){uint32_t expected=0;
            if(__atomic_compare_exchange_n(owner,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)){held=true;return;}
            if(++tries>=limit)return;
            SchedulerSleepMs(1);}
    }
    ~RetryLease(){if(held)__atomic_store_n(owner,0,__ATOMIC_RELEASE);}
    RetryLease(const RetryLease&)=delete;RetryLease& operator=(const RetryLease&)=delete;
};
struct VideoLease {
    bool held=false;
    VideoLease(){uint32_t expected=0;held=__atomic_compare_exchange_n(&videoOwner,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);}
    ~VideoLease(){if(held)__atomic_store_n(&videoOwner,0,__ATOMIC_RELEASE);}
};
// Phase 1c: the CPU0 worker's VCS user-job executor cannot take the video
// lease (a self-test holding it waits on that same worker), so it reports a
// quarantine through this flag. Holders of the video lease fold it into the
// gate and publish BdwVideoUserSubmissionReady (HAS_BSD and BSD admission).
uint32_t vcsUserQuarantined=0;
const char* vcsUserQuarantineDetail="none";
BdwVcsUserCounts vcsUserCounts{};
[[maybe_unused]] void VideoSyncUserReadiness(){
    if(__atomic_load_n(&vcsUserQuarantined,__ATOMIC_ACQUIRE) && videoGate.stage()!=BdwVideoStage::Quarantined){
        videoGate.quarantine(BdwVideoRefusal::SubmitFailed);videoGate.setDetail(vcsUserQuarantineDetail);}
#if defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY)
    const bool ready=videoGate.advertiseBsd() && !__atomic_load_n(&vcsUserQuarantined,__ATOMIC_ACQUIRE);
#else
    const bool ready=false;
#endif
    const uint32_t was=__atomic_exchange_n(&BdwVideoUserSubmissionStorage(),ready?1u:0u,__ATOMIC_ACQ_REL);
    // A worker quarantine that raced this publication wins.
    if(ready && __atomic_load_n(&vcsUserQuarantined,__ATOMIC_ACQUIRE))__atomic_store_n(&BdwVideoUserSubmissionStorage(),0u,__ATOMIC_RELEASE);
    if(was!=uint32_t(ready))KPrintf("gpu-video: I915_EXEC_BSD user submission %s\n",ready?"ready (HAS_BSD=1)":"withdrawn (HAS_BSD=0)");
}
struct VideoReadinessSync {~VideoReadinessSync(){VideoSyncUserReadiness();}};
}
namespace {
// Precise sub-reasons (static strings) for `gpu video status` detail.
[[maybe_unused]] const char* VideoAdapterDetail(const BdwForcewakeAdapterResult& out){
    switch(out.refusal){
    case BdwFwRefusalClaimBusy:return "forcewake-claim-busy";
    case BdwFwRefusalClaimUncertain:return "forcewake-claim-uncertain";
    case BdwFwRefusalArguments:return "forcewake-arguments";
    case BdwFwRefusalForcewakePage:return "forcewake-page-evidence";
    case BdwFwRefusalEnginePage:return "engine-page-evidence";
    case BdwFwRefusalPatPage:return "pat-page-evidence";
    case BdwFwRefusalResetPage:return "reset-page-evidence";
    case BdwFwRefusalClock:return "tsc-read";
    case BdwFwRefusalClockStalled:return "tsc-stalled";
    default:break;
    }
    const auto& q=out.sequence;
    if(!q.requested)return "forcewake-not-requested";
    if(!q.acquired)return "forcewake-not-acknowledged";
    if(!q.threadReady)return "forcewake-thread-not-ready";
    if(!q.sampled)return "bcs-not-sampled";
    if(!q.patSampled)return "pat-not-sampled";
    if(!q.submissionQuiescent)return "bcs-not-quiescent";
    if(!q.otherEnginesSampled)return "engines-not-sampled";
    if(!q.other[0].submissionQuiescent)return "rcs-not-quiescent";
    if(!q.other[1].submissionQuiescent)return "vcs-not-quiescent";
    if(!q.other[2].submissionQuiescent)return "vecs-not-quiescent";
    return "work-not-attempted";
}
[[maybe_unused]] const char* VideoStepDetail(uint8_t step){
    switch(step){
    case 1:return "callback-lease-window";
    case 2:return "callback-gtt-page-evidence";
    case 3:return "callback-no-token";
    case 4:return "callback-cpu-or-clock";
    case 5:return "callback-reset-before-refused";
    case 6:return "callback-vcs-page-evidence";
    default:return "callback-refused";
    }
}
// The diagnostic runs on the debug thread and contends with the GPU worker for
// the shared forcewake claim (and with the worker's idle-engine requirement).
// Those refusals touch nothing, so retry them for a bounded time.
[[maybe_unused]] bool VideoTransient(const BdwForcewakeAdapterResult& out){
    return out.refusal==BdwFwRefusalClaimBusy || (out.sequence.requested && !out.sequence.workAttempted);
}
}
bool BdwVideoBuilt(){
#if defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY)
    return true;
#else
    return false;
#endif
}
bool BdwVideoStatus(BdwVideoGate& out){VideoLease lease;if(!lease.held)return false;VideoSyncUserReadiness();out=videoGate;return true;}
void BdwVideoUserCountsRead(BdwVcsUserCounts& out){
    out.jobs=__atomic_load_n(&vcsUserCounts.jobs,__ATOMIC_RELAXED);out.completed=__atomic_load_n(&vcsUserCounts.completed,__ATOMIC_RELAXED);
    out.refused=__atomic_load_n(&vcsUserCounts.refused,__ATOMIC_RELAXED);out.failed=__atomic_load_n(&vcsUserCounts.failed,__ATOMIC_RELAXED);
    out.retries=__atomic_load_n(&vcsUserCounts.retries,__ATOMIC_RELAXED);out.lastUs=__atomic_load_n(&vcsUserCounts.lastUs,__ATOMIC_RELAXED);
    out.maxUs=__atomic_load_n(&vcsUserCounts.maxUs,__ATOMIC_RELAXED);
    {const auto& c=vcsUserCounts;out.timed=c.timed;out.prepUs=c.prepUs;out.runUs=c.runUs;out.releaseUs=c.releaseUs;
     out.resetBeforeUs=c.resetBeforeUs;out.patUs=c.patUs;out.initUs=c.initUs;out.decodeUs=c.decodeUs;out.csbUs=c.csbUs;
     out.tailUs=c.tailUs;out.maxRunUs=c.maxRunUs;out.skippedResetsBefore=c.skippedResetsBefore;
     out.skippedResetsAfter=c.skippedResetsAfter;out.firstJobNs=c.firstJobNs;out.lastJobNs=c.lastJobNs;}
    out.quarantined=__atomic_load_n(&vcsUserQuarantined,__ATOMIC_ACQUIRE)!=0;out.detail=vcsUserQuarantineDetail;
}
bool BdwVideoSetEnabled(bool on,BdwVideoRefusal& refusal){
    refusal=BdwVideoRefusal::None;
    if(!BdwVideoBuilt()){refusal=BdwVideoRefusal::NotImplemented;return false;}
    VideoLease lease;if(!lease.held){refusal=BdwVideoRefusal::Busy;return false;}
    VideoReadinessSync sync;VideoSyncUserReadiness();
    if(!videoGate.setEnabled(on)){refusal=videoGate.lastRefusal();return false;}
    KPrintf("gpu-video: gate %s\n",on?"on":"off");return true;
}
// (forward) release of the prepared self-test image through the worker
namespace {[[maybe_unused]] bool VideoWorkerRelease();}
// One device step of a VCS diagnostic, executed by the CPU0 application
// worker between RCS jobs and presents (ServiceScanout). The orchestration
// (gate, preconditions, retries, sleeps) stays on the gpu-video thread and
// never holds the diagnostic lease: gen 392 showed that holding it across
// retries and sleeps starved render-job preparation and presents and stranded
// a zero-copy handoff for the rest of the boot.
namespace {
enum class VwKind : uint8_t {Reset,Prepare,Run,Release,Pm};
struct VideoWorkerStep {
    BdwVideoStepSlot slot{};VwKind kind=VwKind::Reset;uint8_t pmOp=0;
    uint32_t nonce=0,data=0;BdwGgttReservation reservation{};BdwOwnedPage scratch{};
    BdwForcewakeAdapterResult fw{};BdwRcsResetTestResult reset{};BdwBcsRunResult run{};bool ok=false;
} videoStep [[maybe_unused]];
[[maybe_unused]] uint64_t videoStepServiced=0,videoStepLeaseBusy=0,videoStepTimeouts=0;
}
bool BdwVideoWorkerPending(){return videoStep.slot.pending();}
void BdwVideoWorkerCounts(uint64_t& serviced,uint64_t& leaseBusy,uint64_t& timeouts){
    serviced=videoStepServiced;leaseBusy=videoStepLeaseBusy;timeouts=videoStepTimeouts;}
bool BdwVideoWorkerService(){
#if defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY)
    if(!videoStep.slot.claim())return false;
    DiagnosticLease lease;
    if(!lease.held){++videoStepLeaseBusy;videoStep.slot.requeue();return false;}
    const auto& tsc=NativeTscObserveRead();
    auto& v=videoStep;v.ok=false;v.fw={};
    if(SmpCurrentCpuIndex()==0 && tsc.capable && tsc.measured && tsc.consistent && tsc.khz){
        switch(v.kind){
        case VwKind::Reset:VmmDellVcsReset(tsc.khz,v.fw,v.reset);v.ok=v.reset.complete;break;
        case VwKind::Prepare:v.ok=BdwPrepareVcsSelftest(v.nonce,v.data,v.reservation,v.scratch);break;
        case VwKind::Run:VmmDellVcsSelftest(tsc.khz,v.reservation.first,v.reservation.token,v.scratch.dma,v.fw,v.run);v.ok=true;break;
        case VwKind::Release:v.ok=BdwReleasePreparedVcsSelftest();break;
        case VwKind::Pm:VmmDellVcsPmOp(tsc.khz,v.pmOp,v.fw,v.reset);v.ok=v.reset.attempted;break;
        }
    }
    ++videoStepServiced;
    videoStep.slot.complete();
    BdwGpuEventSignal(BdwGpuEvent::Retire);
    return true;
#else
    return false;
#endif
}
namespace {
// Queue one step and wait for the worker (bounded). Returns false if the step
// never started (timeout while queued: it is withdrawn, nothing touched).
[[maybe_unused]] bool VideoWorkerCall(VwKind kind,uint8_t pmOp=0){
    if(!videoStep.slot.begin())return false;
    videoStep.kind=kind;videoStep.pmOp=pmOp;videoStep.fw={};videoStep.ok=false;
    if(kind==VwKind::Reset || kind==VwKind::Pm)videoStep.reset={};
    if(kind==VwKind::Run)videoStep.run={};
    videoStep.slot.publish();
    if(!BdwVideoWakeWorker() && videoStep.slot.withdraw())return false;
    const uint64_t start=ApicTickCount();
    for(;;){
        const uint32_t st=videoStep.slot.state();
        if(st==BdwVideoStepSlot::Done)break;
        if(st==BdwVideoStepSlot::Queued && ApicTickCount()-start>=2000 && videoStep.slot.withdraw()){++videoStepTimeouts;return false;}
        SchedulerSleepMs(1);(void)BdwVideoWakeWorker();
    }
    return true; // caller reads videoStep, then calls VideoWorkerFinish()
}
[[maybe_unused]] void VideoWorkerFinish(){videoStep.slot.finish();}
// RC6 entered at all over ~300 ms after a reset or self-test (worker-serviced samples).
[[maybe_unused]] void VideoRc6Check();
}
bool BdwVideoProbe(BdwForcewakeAdapterResult& out,BdwVideoRefusal& refusal){
    refusal=BdwVideoRefusal::None;
    if(!BdwVideoBuilt()){refusal=BdwVideoRefusal::NotImplemented;return false;}
    VideoLease lease;if(!lease.held){refusal=BdwVideoRefusal::Busy;return false;}
    refusal=videoGate.mayProbe();if(refusal!=BdwVideoRefusal::None)return false;
    // Read-only: the shared forcewake observation samples VCS0 (other[1]) twice.
    // The observation contends with the GPU worker for the forcewake claim and
    // the diagnostic slot; both refusals touch nothing, so retry briefly.
    bool ok=false;unsigned attempts=0;
    for(;;){++attempts;out={};ok=BdwObserveForcewakeLive(out,true,false);
        if(ok || attempts>=100)break;
        SchedulerSleepMs(2);}
    videoGate.addRetries(attempts-1);
    videoGate.setDetail(ok?"none":out.refusal || out.sequence.requested?VideoAdapterDetail(out):"diagnostic-lease-or-precondition");
    const auto& e=out.sequence.other[1];
    BdwVcsSample sample{};
    sample.sampled=ok && e.sampled;sample.idle=e.idle;sample.quiescent=e.submissionQuiescent;
    sample.head=e.first.head;sample.tail=e.first.tail;sample.mode=e.first.mode;sample.start=e.first.start;
    sample.control=e.first.control;sample.execlistLo=e.first.execlistLo;sample.execlistHi=e.first.execlistHi;
    sample.submissionMode=e.first.submissionMode;
    videoGate.probed(ok,sample);
    if(!ok || !sample.sampled){refusal=BdwVideoRefusal::ProbeFailed;return false;}
    return true;
}
bool BdwVideoReset(BdwForcewakeAdapterResult& out,BdwRcsResetTestResult& reset,BdwVideoRefusal& refusal){
    refusal=BdwVideoRefusal::None;reset={};
#if defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY)
    VideoLease video;if(!video.held){refusal=BdwVideoRefusal::Busy;return false;}
    VideoReadinessSync sync;VideoSyncUserReadiness();
    refusal=videoGate.mayReset();if(refusal!=BdwVideoRefusal::None)return false;
    if(!PanicRecoveryResetArmed() || SmpCurrentCpuIndex()!=0 || !observation.inventory || !observation.planned ||
       !observation.patRequested || !PageAdmitted(BdwPrivatePatPage) || !PageAdmitted(BdwResetPage)){
        refusal=BdwVideoRefusal::ResetRefused;return false;}
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page)){refusal=BdwVideoRefusal::ResetRefused;return false;}
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page)){refusal=BdwVideoRefusal::ResetRefused;return false;}
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz){refusal=BdwVideoRefusal::ResetRefused;return false;}
    for(uint32_t i=0;i<PciBootInventoryCount();++i){
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))break;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        if(!LiveGpuMatches(entry) || !PowerD0(d))break;
        unsigned attempts=0;
        for(;;){++attempts;bool ran=false;
            if(VideoWorkerCall(VwKind::Reset)){out=videoStep.fw;reset=videoStep.reset;ran=true;VideoWorkerFinish();}
            if(ran && (reset.attempted || !VideoTransient(out)))break;
            if(attempts>=250)break;
            SchedulerSleepMs(2);}
        videoGate.addRetries(attempts-1);
        videoGate.setDetail(reset.complete?"none":!out.sequence.workAttempted?VideoAdapterDetail(out):"reset-not-started");
        videoGate.resetDone(reset.complete && out.sequence.workComplete,reset.uncertain);
        if(reset.uncertain){
            // GDRST is shared with the per-job RCS resets; an unverified media
            // reset may leave the GT reset domain in an unknown state. Follow the
            // RCS-reset policy: stop here. The runtime gate defaults off, so the
            // recovery reboot does not repeat this.
            KernelPanic("Broadwell video (VCS) reset uncertain: attempted=%u wake=%u released=%u",
                unsigned(reset.attempted),unsigned(out.sequence.acquired),unsigned(out.sequence.released));
        }
        KPrintf("gpu-video: VCS reset %s\n",reset.complete?"acknowledged":"refused before any write");
        if(!reset.complete){refusal=BdwVideoRefusal::ResetRefused;return false;}
        VideoRc6Check();
        return out.sequence.workComplete && out.clearDelivered && out.sequence.released && LiveGpuMatches(entry) && PowerD0(d);
    }
    refusal=BdwVideoRefusal::ResetRefused;return false;
#else
    (void)out;refusal=BdwVideoRefusal::NotImplemented;return false;
#endif
}
bool BdwVideoSelftest(BdwForcewakeAdapterResult& out,BdwBcsRunResult& run,uint32_t& readback,uint32_t& expected,BdwVideoRefusal& refusal){
    refusal=BdwVideoRefusal::None;run={};readback=expected=0;
#if defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY)
    RetryLease video(&videoOwner,200);
    if(!video.held){refusal=BdwVideoRefusal::Busy;return false;}
    VideoReadinessSync sync;VideoSyncUserReadiness();
    refusal=videoGate.maySelftest();if(refusal!=BdwVideoRefusal::None){videoGate.setDetail("gate");return false;}
    auto refuseEarly=[&](BdwVideoRefusal r,const char* detail){refusal=r;videoGate.setDetail(detail);videoGate.selftestDone(false,false,r);return false;};
    if(!PanicRecoveryResetArmed())return refuseEarly(BdwVideoRefusal::ResetRefused,"precondition-recovery-unarmed");
    if(SmpCurrentCpuIndex()!=0)return refuseEarly(BdwVideoRefusal::ResetRefused,"precondition-not-cpu0");
    if(!observation.inventory || !observation.planned || !observation.patRequested)
        return refuseEarly(BdwVideoRefusal::ResetRefused,"precondition-observation");
    if(!PageAdmitted(BdwPrivatePatPage) || !PageAdmitted(BdwResetPage))return refuseEarly(BdwVideoRefusal::ResetRefused,"precondition-pat-or-reset-page");
    if(!PageAdmitted(0xf6880000ull) || !PageAdmitted(0xf6101000ull))return refuseEarly(BdwVideoRefusal::ResetRefused,"precondition-ggtt-pages");
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return refuseEarly(BdwVideoRefusal::ResetRefused,"precondition-forcewake-pages");
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return refuseEarly(BdwVideoRefusal::ResetRefused,"precondition-engine-pages");
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return refuseEarly(BdwVideoRefusal::ResetRefused,"precondition-tsc");
    if(!(PciConfigRead16(0,2,0,4)&4))return refuseEarly(BdwVideoRefusal::ResetRefused,"precondition-pci-bus-master");
    bool gpu=false;
    for(uint32_t i=0;i<PciBootInventoryCount() && !gpu;++i){
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))break;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        gpu=LiveGpuMatches(entry) && PowerD0(d);
        if(!gpu)break;
    }
    if(!gpu)return refuseEarly(BdwVideoRefusal::ResetRefused,"precondition-gpu-identity-or-d0");
    static uint32_t sequence=0x56435300; // "VCS\0"
    const uint32_t nonce=++sequence,data=nonce^0xa5a5a5a5u;
    expected=data;
    BdwGgttReservation reservation{};BdwOwnedPage scratch{};
    // Deterministic preparation failures (allocation, reservation window, DMA
    // snapshot) touch no device state: refuse without quarantine.
    {
        bool prepared=false;
        for(unsigned i=0;i<50 && !prepared;++i){
            videoStep.nonce=nonce;videoStep.data=data;
            if(!VideoWorkerCall(VwKind::Prepare)){SchedulerSleepMs(2);continue;}
            prepared=true;const bool ok=videoStep.ok;reservation=videoStep.reservation;scratch=videoStep.scratch;VideoWorkerFinish();
            if(!ok)return refuseEarly(BdwVideoRefusal::PrepareFailed,"prepare");
        }
        if(!prepared)return refuseEarly(BdwVideoRefusal::Busy,"worker-not-servicing");
    }
    // Retry refusals that touched nothing (claim busy, engines not idle, or a
    // callback precondition), up to ~0.5 s. The image stays prepared.
    unsigned attempts=0;
    for(;;){
        ++attempts;
        videoStep.reservation=reservation;videoStep.scratch=scratch;
        if(!VideoWorkerCall(VwKind::Run)){out={};run={};
            if(attempts>=250)break;SchedulerSleepMs(2);continue;}
        out=videoStep.fw;run=videoStep.run;VideoWorkerFinish();
        const bool untouched=!run.resetBeforeAttempted || (run.refusalStep==5 && !run.resetUncertain);
        const bool transient=VideoTransient(out) ||
            (out.sequence.workAttempted && untouched && run.refusalStep && run.refusalStep!=5);
        if(!transient || attempts>=250)break;
        SchedulerSleepMs(2);
    }
    videoGate.addRetries(attempts-1);
    readback=BdwVcsSelftestReadback();
    // Genuinely uncertain device state: the GT reset after submission did not
    // complete, the shared private PAT was not restored, or forcewake was not
    // released. Every RCS/BCS job depends on these, so stop (as the scanout
    // path does). The gate is off at boot: recovery does not repeat this.
    const bool forcewakeUncertain=out.sequence.requested && (!out.clearDelivered || !out.sequence.released);
    // A reset that reported Uncertain (GDRST state unknown), or a submission
    // whose engine could not be stopped afterwards (the private PAT then stays
    // leased and cannot be restored for RCS/BCS).
    const bool resetUncertain=run.resetUncertain || (run.attempted && !run.resetAfter);
    const bool patUncertain=run.patEstablished && !run.patRestored;
    if(forcewakeUncertain || resetUncertain || patUncertain){
        videoGate.selftestDone(false,true,BdwVideoRefusal::ResetUncertain);
        KernelPanic("Broadwell video (VCS) self-test uncertain: reset=%u/%u pat=%u/%u ggtt=%u elsp=%u wake=%u head=%x tail=%x acthd=%x ipehr=%x",
            unsigned(run.resetBefore),unsigned(run.resetAfter),unsigned(run.patEstablished),unsigned(run.patRestored),
            run.ggttWriteAttempts,run.portWriteAttempts,unsigned(out.sequence.released),run.head,run.tail,run.acthd,run.ipehr);
    }
    const bool touched=(run.resetBeforeAttempted && !(run.refusalStep==5 && !run.resetUncertain)) ||
        run.attempted || run.ggttWriteAttempts || run.portWriteAttempts;
    if(!touched){
        // Nothing reached VCS: give the prepared image back and report why.
        const bool released=VideoWorkerRelease();
        const char* detail=!out.sequence.workAttempted?VideoAdapterDetail(out):VideoStepDetail(run.refusalStep);
        refusal=run.refusalStep==5?BdwVideoRefusal::ResetRefused:BdwVideoRefusal::Busy;
        videoGate.setDetail(detail);
        if(!released){videoGate.selftestDone(false,true,BdwVideoRefusal::RetireFailed);refusal=BdwVideoRefusal::RetireFailed;
            videoGate.setDetail("prepared-image-not-released");}
        else videoGate.selftestDone(false,false,refusal);
        KPrintf("gpu-video: VCS self-test refused before any engine write after %u attempts: %s\n",attempts,detail);
        return false;
    }
    // The engine was reset and the PAT restored. If the lease could not be
    // unbound, or the pages not released, they stay retained for the boot
    // (never reused) and VCS is quarantined; RCS/BCS are unaffected.
    const bool released=run.scanoutRetired && VideoWorkerRelease();
    const bool ok=released && run.copyComplete && run.contextComplete && out.sequence.workComplete && readback==data;
    BdwVideoRefusal reason=BdwVideoRefusal::None;const char* detail="none";
    if(!ok){
        if(!released){reason=BdwVideoRefusal::RetireFailed;detail=run.scanoutRetired?"release":"retire";}
        else if(run.copyComplete && readback!=data){reason=BdwVideoRefusal::Mismatch;detail="readback";}
        else if(!run.copyComplete){reason=BdwVideoRefusal::SubmitFailed;detail=run.portWriteAttempts<4?"execlist-init-or-elsp":"breadcrumb-timeout";}
        else {reason=BdwVideoRefusal::SubmitFailed;detail="csb-context-complete";}
        if(!released)KPrintf("gpu-video: self-test pages retained for the boot (retire=%u)\n",unsigned(run.scanoutRetired));
    }
    videoGate.setDetail(detail);
    videoGate.selftestDone(ok,true,reason);
    if(ok)VideoRc6Check();
    refusal=reason;
    KPrintf("gpu-video: VCS self-test %s (%s) nonce=%x data=%x readback=%x ctx=%u ggtt=%u elsp=%u attempts=%u\n",ok?"ok":BdwVideoRefusalName(reason),
        detail,nonce,data,readback,unsigned(run.contextComplete),run.ggttWriteAttempts,run.portWriteAttempts,attempts);
    return ok;
#else
    (void)out;refusal=BdwVideoRefusal::NotImplemented;return false;
#endif
}
// ---- VA-API phase 1c: VCS0 user jobs (I915_EXEC_BSD) on the CPU0 worker ----
// The application worker routes snapshots tagged BdwExecEngineVideo here, one
// job at a time between RCS jobs. Each job runs the phase 1b sequence on its
// own image, tables and GGTT reservation: VCS reset (firmware engine values
// restored), PAT lease, takeover bind, execlist init, ELSP, breadcrumb and CSB
// within the 2 s budget, VCS reset, unbind. Fences fail or complete through
// the queue's snapshot retirement.
//
// Failure policy (docs/bdw-video-engine.md, phase 1c):
//  - a refusal before any VCS write (gate, preconditions, sizes, allocation,
//    claim busy) fails the job's fence and keeps the device: {false,true,true};
//  - a failure after VCS was touched (hang: no breadcrumb or CSB completion
//    in the budget, CSB fault) with an acknowledged reset fails the fence and
//    quarantines VCS for the boot (HAS_BSD withdrawn, BSD admission refused);
//  - only an uncertain GDRST, PAT or forcewake state panics, as in 1b.
#if defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY)
namespace {
void VcsUserQuarantine(const char* detail){
    vcsUserQuarantineDetail=detail?detail:"vcs-user-job";
    __atomic_store_n(&vcsUserQuarantined,1u,__ATOMIC_RELEASE);
    __atomic_store_n(&BdwVideoUserSubmissionStorage(),0u,__ATOMIC_RELEASE);
    KPrintf("gpu-video: VCS quarantined for this boot (%s); I915_EXEC_BSD withdrawn\n",vcsUserQuarantineDetail);
}
BdwVcsUserRunView VcsUserView(const BdwForcewakeAdapterResult& out,const BdwBcsRunResult& run){
    BdwVcsUserRunView v{};
    v.forcewakeRequested=out.sequence.requested;v.forcewakeReleased=out.sequence.released;v.clearDelivered=out.clearDelivered;
    v.workAttempted=out.sequence.workAttempted;v.workComplete=out.sequence.workComplete;
    v.resetBeforeAttempted=run.resetBeforeAttempted;v.resetBefore=run.resetBefore;v.resetUncertain=run.resetUncertain;
    v.attempted=run.attempted;v.copyComplete=run.copyComplete;v.contextComplete=run.contextComplete;
    v.resetAfter=run.resetAfter;v.retired=run.scanoutRetired;v.patEstablished=run.patEstablished;v.patRestored=run.patRestored;
    v.refusalStep=run.refusalStep;v.ggttWrites=run.ggttWriteAttempts;v.portWrites=run.portWriteAttempts;
    return v;
}
}
static BdwSubmissionOutcome ExecuteVcsUserJob(BdwExecSnapshot* snapshot){
    __atomic_add_fetch(&vcsUserCounts.jobs,1,__ATOMIC_RELAXED);
    uint32_t pid=0,ctx=0;BdwExecSnapshotOrigin(snapshot,pid,ctx);
    auto refuse=[&](const char* reason)->BdwSubmissionOutcome{
        const uint64_t n=__atomic_add_fetch(&vcsUserCounts.refused,1,__ATOMIC_RELAXED);
        if(n<=16 || !(n&(n-1)))KPrintf("gpu-exec: vcs job refused pid=%u context=%u objects=%u pages=%u reason=%s count=%lu\n",
            pid,ctx,BdwExecSnapshotObjectCount(snapshot),BdwExecSnapshotPageCount(snapshot),reason?reason:"?",n);
        return {false,true,true};
    };
    if(!snapshot)return {false,true,true};
    if(!BdwVideoUserSubmissionReady() || __atomic_load_n(&vcsUserQuarantined,__ATOMIC_ACQUIRE))return refuse("video engine not ready");
    // The diagnostic slot covers every Brook GPU operation; wait briefly for
    // a shell diagnostic instead of failing the job at once.
    RetryLease lease(&diagnosticOwner,20);if(!lease.held)return refuse("diagnostic lease busy");
    if(!PanicRecoveryResetArmed() || SmpCurrentCpuIndex()!=0 || !__atomic_load_n(&firstReleased,__ATOMIC_ACQUIRE) ||
       !observation.inventory || !observation.planned || !observation.patRequested)return refuse("precondition");
    constexpr uint64_t pages[]={BdwDellBar0,BdwPrivatePatPage,BdwResetPage,0xf6880000ull,0xf6101000ull};
    for(auto page:pages)if(!PageAdmitted(page))return refuse("precondition-pages");
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return refuse("precondition-forcewake-pages");
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return refuse("precondition-engine-pages");
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return refuse("precondition-tsc");
    PciBootEntry entry{};bool found=false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i){PciBootEntry e{};if(!PciBootInventoryRead(i,&e))break;
        const auto& d=e.device;if(d.bus==0 && d.dev==2 && d.fn==0){entry=e;found=true;break;}}
    if(!found || !LiveGpuMatches(entry) || !PowerD0(entry.device) ||
       !(PciConfigRead16(entry.device.bus,entry.device.dev,entry.device.fn,4)&4))return refuse("precondition-gpu-identity-or-d0");
    static uint32_t nonce=0x56435500; // "VCU\0"
    if(nonce==~uint32_t{0})return refuse("nonce exhausted");
    const uint64_t startNs=TscClockReadNs();
    BdwGgttReservation reservation{};BdwOwnedPage scratch{};const char* reason=nullptr;
    const auto prepared=BdwPrepareVcsUserJob(snapshot,++nonce,reservation,scratch,reason);
    if(prepared==BdwPrepareOutcome::Refused)return refuse(reason);
    if(prepared!=BdwPrepareOutcome::Ready){
        // Nothing reached VCS, but its private objects are in an unknown
        // state: refuse this job and every later one.
        VcsUserQuarantine("prepare-uncertain");return refuse(reason?reason:"prepare");
    }
    const uint64_t preparedNs=TscClockReadNs();
    BdwForcewakeAdapterResult out{};BdwBcsRunResult run{};
    BdwVcsUserClass result=BdwVcsUserClass::RefusedUntouched;
    unsigned attempt=0;
    for(;;){
        ++attempt;
        VmmDellVcsUserRun(tsc.khz,reservation.first,reservation.token,scratch.dma,out,run);
        result=BdwClassifyVcsUserRun(VcsUserView(out,run));
        if(result!=BdwVcsUserClass::RefusedUntouched || attempt>=20)break;
        SchedulerSleepMs(1);
    }
    if(attempt>1)__atomic_add_fetch(&vcsUserCounts.retries,attempt-1,__ATOMIC_RELAXED);
    const uint64_t ranNs=TscClockReadNs();
    const uint64_t us=(ranNs-startNs)/1000;
    __atomic_store_n(&vcsUserCounts.lastUs,us,__ATOMIC_RELAXED);
    if(us>__atomic_load_n(&vcsUserCounts.maxUs,__ATOMIC_RELAXED))__atomic_store_n(&vcsUserCounts.maxUs,us,__ATOMIC_RELAXED);
    switch(result){
    case BdwVcsUserClass::Uncertain:
        KernelPanic("Broadwell video (VCS) user job uncertain: pid=%u context=%u batch=%lx reset=%u/%u/%u pat=%u/%u retired=%u ggtt=%u elsp=%u wake=%u/%u head=%x tail=%x acthd=%x ipehr=%x",
            pid,ctx,BdwExecSnapshotBatchAddress(snapshot),unsigned(run.resetBefore),unsigned(run.resetAfter),unsigned(run.resetUncertain),
            unsigned(run.patEstablished),unsigned(run.patRestored),unsigned(run.scanoutRetired),run.ggttWriteAttempts,run.portWriteAttempts,
            unsigned(out.clearDelivered),unsigned(out.sequence.released),run.head,run.tail,run.acthd,run.ipehr);
    case BdwVcsUserClass::RefusedUntouched:
        // Never submitted: the armed snapshot retires through the queue as
        // skipped (fence error, buffers not poisoned).
        if(!BdwReleaseVcsUserJob(snapshot,true)){VcsUserQuarantine("release-after-refusal");return {false,true};}
        return refuse(!out.sequence.workAttempted?VideoAdapterDetail(out):VideoStepDetail(run.refusalStep));
    case BdwVcsUserClass::StoppedRetained:
        // VCS was reset, but the GGTT lease could not be unbound: keep the
        // image and tables for the boot (unreachable by the reset engine).
        __atomic_add_fetch(&vcsUserCounts.failed,1,__ATOMIC_RELAXED);
        KPrintf("gpu-exec: vcs job failed pid=%u context=%u reason=lease-not-retired; pages retained\n",pid,ctx);
        VcsUserQuarantine("lease-not-retired");
        return {false,true};
    case BdwVcsUserClass::FailedStopped:{
        __atomic_add_fetch(&vcsUserCounts.failed,1,__ATOMIC_RELAXED);
        const char* detail=!run.attempted?"table-admission":!run.copyComplete?(run.portWriteAttempts<4?"execlist-init-or-elsp":"breadcrumb-timeout"):
            !run.contextComplete?"csb-context-complete":"work-incomplete";
        const auto current=BdwExecSnapshotCurrentBatchSample(snapshot);
        KPrintf("gpu-exec: vcs job failed pid=%u context=%u reason=%s batch=%lx objects=%u pages=%u ggtt=%u elsp=%u head=%x tail=%x acthd=%x ipehr=%x execlist=%x words=%x,%x,%x,%x us=%lu\n",
            pid,ctx,detail,BdwExecSnapshotBatchAddress(snapshot),BdwExecSnapshotObjectCount(snapshot),BdwExecSnapshotPageCount(snapshot),
            run.ggttWriteAttempts,run.portWriteAttempts,run.head,run.tail,run.acthd,run.ipehr,run.execlist,
            current.words[0],current.words[1],current.words[2],current.words[3],us);
        const bool released=BdwReleaseVcsUserJob(snapshot,true);
        VcsUserQuarantine(released?detail:"release-after-failure");
        return {false,true};
    }
    case BdwVcsUserClass::Completed:
        break;
    }
    if(!BdwReleaseVcsUserJob(snapshot,true)){
        // The job completed and VCS was reset; only our bookkeeping failed.
        VcsUserQuarantine("release-after-completion");
    }
    __atomic_add_fetch(&vcsUserCounts.completed,1,__ATOMIC_RELAXED);
    {   // Phase 4 timing of this completed job (single CPU0 writer).
        const uint64_t doneNs=TscClockReadNs();
        auto d=[&](unsigned a,unsigned b)->uint64_t{const uint64_t x=run.stamps[a],y=run.stamps[b];
            return x && y>x?(y-x)*1000/tsc.khz:0;};
        auto& c=vcsUserCounts;
        const uint64_t runUs=(ranNs-preparedNs)/1000;
        ++c.timed;c.prepUs+=(preparedNs-startNs)/1000;c.runUs+=runUs;c.releaseUs+=(doneNs-ranNs)/1000;
        if(runUs>c.maxRunUs)c.maxRunUs=runUs;
        c.resetBeforeUs+=d(BdwVcsStampWorkStart,BdwVcsStampResetBefore);c.patUs+=d(BdwVcsStampResetBefore,BdwVcsStampPat);
        c.initUs+=d(BdwVcsStampInitStart,BdwVcsStampInitEnd);c.decodeUs+=d(BdwVcsStampWaitStart,BdwVcsStampBreadcrumb);
        c.csbUs+=d(BdwVcsStampBreadcrumb,BdwVcsStampCsb);c.tailUs+=d(BdwVcsStampCsb,BdwVcsStampWorkEnd);
        if(!run.resetBeforeAttempted)++c.skippedResetsBefore;
        if(run.resetAfterSkipped)++c.skippedResetsAfter;
        if(!c.firstJobNs)c.firstJobNs=startNs;
        c.lastJobNs=doneNs;
    }
    return {true,true};
}
#endif
// VCS power-management experiments and residency sampling (gen 391 clock sag).
namespace {
BdwGtPmSnapshot videoSampleA{},videoSampleB{};uint64_t videoSampleUs=0;
}
bool BdwVideoPmOp(uint8_t op,BdwForcewakeAdapterResult& out,BdwRcsResetTestResult& r,BdwVideoRefusal& refusal){
    refusal=BdwVideoRefusal::None;r={};
#if defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY)
    const auto pmOp=BdwVcsPmOp(op);
    RetryLease video(&videoOwner,200);
    if(!video.held){refusal=BdwVideoRefusal::Busy;return false;}
    // Writes need the same proof as a reset (gate on, probed idle, not quarantined);
    // sampling only needs the gate.
    refusal=pmOp==BdwVcsPmOp::Sample?videoGate.mayProbe():videoGate.mayReset();
    if(refusal!=BdwVideoRefusal::None){videoGate.setDetail("gate");return false;}
    if(!PanicRecoveryResetArmed() || SmpCurrentCpuIndex()!=0 || !observation.inventory || !observation.planned ||
       !observation.patRequested || !PageAdmitted(BdwPrivatePatPage) || !PageAdmitted(BdwResetPage)){
        refusal=BdwVideoRefusal::ResetRefused;videoGate.setDetail("precondition");return false;}
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page)){refusal=BdwVideoRefusal::ResetRefused;videoGate.setDetail("precondition-forcewake-pages");return false;}
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page)){refusal=BdwVideoRefusal::ResetRefused;videoGate.setDetail("precondition-engine-pages");return false;}
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz){refusal=BdwVideoRefusal::ResetRefused;videoGate.setDetail("precondition-tsc");return false;}
    auto once=[&](uint8_t which){
        unsigned attempts=0;
        for(;;){++attempts;bool ran=false;
            if(VideoWorkerCall(VwKind::Pm,which)){out=videoStep.fw;r=videoStep.reset;ran=true;VideoWorkerFinish();}
            if(ran && (r.attempted || !VideoTransient(out)))break;
            if(attempts>=250)break;
            SchedulerSleepMs(2);}
        videoGate.addRetries(attempts-1);
        return r.attempted;
    };
    if(pmOp==BdwVcsPmOp::Sample){
        uint32_t w=0,f=0;
        if(!once(op)){refusal=BdwVideoRefusal::Busy;videoGate.setDetail(VideoAdapterDetail(out));return false;}
        VmmDellVcsPmSample(videoSampleA,w,f);
        uint64_t t0=0,t1=0;uint32_t lo,hi;
        __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));t0=(uint64_t(hi)<<32)|lo;
        SchedulerSleepMs(1000);
        if(!once(op)){refusal=BdwVideoRefusal::Busy;videoGate.setDetail(VideoAdapterDetail(out));return false;}
        __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));t1=(uint64_t(hi)<<32)|lo;
        VmmDellVcsPmSample(videoSampleB,w,f);
        videoSampleUs=t1>t0?(t1-t0)*1000/tsc.khz:0;
        videoGate.setDetail("none");
        return videoSampleA.valid && videoSampleB.valid;
    }
    if(!once(op)){refusal=BdwVideoRefusal::Busy;videoGate.setDetail(VideoAdapterDetail(out));return false;}
    uint32_t writes=0,failures=0;BdwGtPmSnapshot after{};VmmDellVcsPmSample(after,writes,failures);
    KPrintf("gpu-video: pm %s %s writes=%u failures=%u\n",BdwVcsPmOpName(pmOp),r.complete?"ok":"failed",writes,failures);
    videoGate.setDetail(r.complete?"none":"pm-op-write-or-wait-failed");
    if(!r.complete)refusal=BdwVideoRefusal::SubmitFailed;
    return r.complete;
#else
    (void)op;(void)out;refusal=BdwVideoRefusal::NotImplemented;return false;
#endif
}
void BdwVideoPmSamples(BdwGtPmSnapshot& a,BdwGtPmSnapshot& b,uint64_t& us){a=videoSampleA;b=videoSampleB;us=videoSampleUs;}
namespace {
bool VideoWorkerRelease(){
    for(unsigned i=0;i<250;++i){
        if(VideoWorkerCall(VwKind::Release)){const bool ok=videoStep.ok;VideoWorkerFinish();return ok;}
        SchedulerSleepMs(2);
    }
    return false;
}
void VideoRc6Check(){
#if defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY)
    const auto& tsc=NativeTscObserveRead();
    auto sample=[&](BdwGtPmSnapshot& out){
        for(unsigned i=0;i<100;++i){
            if(VideoWorkerCall(VwKind::Pm,uint8_t(BdwVcsPmOp::Sample))){const bool ok=videoStep.reset.attempted;VideoWorkerFinish();
                if(ok){uint32_t w=0,f=0;VmmDellVcsPmSample(out,w,f);return out.valid;}}
            SchedulerSleepMs(2);
        }
        return false;
    };
    BdwGtPmSnapshot a{},b{};uint32_t lo,hi;
    if(!tsc.khz || !sample(a)){videoGate.setRc6(false,0);return;}
    __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));const uint64_t t0=(uint64_t(hi)<<32)|lo;
    SchedulerSleepMs(300);
    if(!sample(b)){videoGate.setRc6(false,0);return;}
    __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));const uint64_t t1=(uint64_t(hi)<<32)|lo;
    const uint64_t us=t1>t0?(t1-t0)*1000/tsc.khz:0;
    const int i=BdwGtPmIndex(0x138108);
    const uint32_t delta=i<0?0:b.value[i]-a.value[i];
    // Residency counts 1.28 us units: percent = delta*128/us.
    const uint32_t pct=us?uint32_t(uint64_t(delta)*128/us):0;
    videoGate.setRc6(true,pct>100?100:pct);
    KPrintf("gpu-video: RC6 after VCS op %u%% (%s)\n",pct>100?100u:pct,delta?"resumed":"NOT resumed");
#endif
}
}
// Run a VCS operation on a CPU0-pinned kernel thread: the critical section
// and the PAT/forcewake transaction are CPU0-only, and the debug channel's
// thread may be scheduled anywhere (gen 390: precondition-not-cpu0). Same
// pattern as the RCS debug jobs in debug_hardware.cpp.
namespace {
struct VideoJob {
    uint32_t state=0; // 0 idle, 1 running, 2 done
    uint8_t op=0;bool ok=false;
    BdwForcewakeAdapterResult fw{};BdwRcsResetTestResult reset{};BdwBcsRunResult run{};
    uint32_t readback=0,expected=0;BdwVideoRefusal refusal=BdwVideoRefusal::None;
};
[[maybe_unused]] VideoJob videoJob;
[[maybe_unused]] void VideoJobWorker(void* opaque){
    auto* job=static_cast<VideoJob*>(opaque);
    job->refusal=BdwVideoRefusal::None;
    if(job->op==1)job->ok=BdwVideoProbe(job->fw,job->refusal);
    else if(job->op==2)job->ok=BdwVideoReset(job->fw,job->reset,job->refusal);
    else if(job->op==3)job->ok=BdwVideoSelftest(job->fw,job->run,job->readback,job->expected,job->refusal);
    else if(job->op>=10)job->ok=BdwVideoPmOp(uint8_t(job->op-10),job->fw,job->reset,job->refusal);
    __atomic_store_n(&job->state,2,__ATOMIC_RELEASE);
}
}
bool BdwVideoRunOnCpu0(uint8_t op,BdwForcewakeAdapterResult& fw,BdwRcsResetTestResult& reset,BdwBcsRunResult& run,
        uint32_t& readback,uint32_t& expected,BdwVideoRefusal& refusal,bool& pending){
    pending=false;refusal=BdwVideoRefusal::None;
#if !(defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY))
    (void)op;(void)fw;(void)reset;(void)run;(void)readback;(void)expected;
    refusal=BdwVideoRefusal::NotImplemented;return false;
#else
    if((op<1 || op>3) && (op<11 || op>15)){refusal=BdwVideoRefusal::NotImplemented;return false;}
    uint32_t idle=0,done=2;
    // A previous job that outlived its wait finishes in the background; collect it first.
    __atomic_compare_exchange_n(&videoJob.state,&done,0,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    if(!__atomic_compare_exchange_n(&videoJob.state,&idle,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
        refusal=BdwVideoRefusal::Busy;pending=true;return false;}
    videoJob.op=op;videoJob.ok=false;videoJob.fw={};videoJob.reset={};videoJob.run={};videoJob.readback=videoJob.expected=0;
    auto* worker=KernelThreadCreate("gpu-video",VideoJobWorker,&videoJob);
    if(!worker){__atomic_store_n(&videoJob.state,0,__ATOMIC_RELEASE);refusal=BdwVideoRefusal::Busy;return false;}
    worker->cpuAffinity=0;SchedulerAddProcess(worker);
    const uint64_t deadline=ApicTickCount()+10000;
    while(__atomic_load_n(&videoJob.state,__ATOMIC_ACQUIRE)!=2 && ApicTickCount()<deadline)SchedulerSleepMs(5);
    if(__atomic_load_n(&videoJob.state,__ATOMIC_ACQUIRE)!=2){pending=true;refusal=BdwVideoRefusal::Busy;return false;}
    fw=videoJob.fw;reset=videoJob.reset;run=videoJob.run;readback=videoJob.readback;expected=videoJob.expected;
    refusal=videoJob.refusal;const bool ok=videoJob.ok;
    __atomic_store_n(&videoJob.state,0,__ATOMIC_RELEASE);
    return ok;
#endif
}
bool BdwTestRcsReset(BdwForcewakeAdapterResult& out,BdwRcsResetTestResult& reset) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    DiagnosticLease lease;if(!lease.held)return false;
    static uint32_t attempted=0;
    if(__atomic_load_n(&attempted,__ATOMIC_ACQUIRE) || !PanicRecoveryResetArmed() || SmpCurrentCpuIndex()!=0 ||
       !observation.inventory || !observation.planned || !observation.patRequested ||
       !OptedIn("/etc/dell-gpu-rcs-reset-test") || BdwInspectPrivateRenderDma()!=BdwDmaSnapshotOk ||
       !PageAdmitted(BdwPrivatePatPage) || !PageAdmitted(BdwResetPage))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    BdwDisplaySnapshot display{};
    if(!BdwObserveDisplayLive(display) || display.regs[0][0]!=0x98000000u || display.regs[0][1]!=12800)return false;
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned reg=0;reg<10;++reg)
        if((pipe || reg>1) && display.regs[pipe][reg])return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i){
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        BdwGgttReservation reservation{};
        if(!LiveGpuMatches(entry) || !PowerD0(d) || !BdwRetainedRenderReservation(reservation) || reservation.count!=29)return false;
        uint32_t expected=0;if(!__atomic_compare_exchange_n(&attempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        VmmDellRcsReset(tsc.khz,out,reset);
        if(reset.uncertain)KernelPanic("Broadwell render reset uncertain: attempted=%u wake=%u released=%u",
            unsigned(reset.attempted),unsigned(out.sequence.acquired),unsigned(out.sequence.released));
        return reset.complete && out.sequence.workComplete && out.clearDelivered && out.sequence.released &&
            LiveGpuMatches(entry) && PowerD0(d);
    }
#else
    (void)out;(void)reset;
#endif
    return false;
}
bool BdwTestRenderGtInit(BdwForcewakeAdapterResult& out,BdwRenderGtInitResult& init) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    DiagnosticLease lease;if(!lease.held)return false;
    static uint32_t attempted=0;
    if(__atomic_load_n(&attempted,__ATOMIC_ACQUIRE) || !PanicRecoveryResetArmed() || SmpCurrentCpuIndex()!=0 ||
       !observation.inventory || !observation.planned || !observation.patRequested ||
       !OptedIn("/etc/dell-gpu-rcs-gt-init") || BdwInspectPrivateRenderDma()!=BdwDmaSnapshotOk ||
       !PageAdmitted(BdwDellBar0) || !PageAdmitted(BdwDellBar0+0xb000) ||
       !PageAdmitted(BdwPrivatePatPage) || !PageAdmitted(BdwResetPage))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    BdwDisplaySnapshot display{};
    if(!BdwObserveDisplayLive(display) || display.regs[0][0]!=0x98000000u || display.regs[0][1]!=12800)return false;
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned reg=0;reg<10;++reg)
        if((pipe || reg>1) && display.regs[pipe][reg])return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i){
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        BdwGgttReservation reservation{};
        if(!LiveGpuMatches(entry) || !PowerD0(d) || !BdwRetainedRenderReservation(reservation) || reservation.count!=29)return false;
        uint32_t expected=0;if(!__atomic_compare_exchange_n(&attempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        VmmDellRenderGtInit(tsc.khz,out,init);
        const bool finished=init.status==BdwRenderGtInitStatus::Applied && out.sequence.workComplete &&
            out.clearDelivered && out.sequence.released && LiveGpuMatches(entry) && PowerD0(d);
        if(init.status==BdwRenderGtInitStatus::Uncertain || (init.writes && !finished))
            KernelPanic("Broadwell GT init uncertain: stage=%u writes=%u wake=%u released=%u l3_before=%x l3_after=%x dop_restore=%u",
                unsigned(init.stage),init.writes,unsigned(out.sequence.acquired),unsigned(out.sequence.released),
                init.l3Before,init.l3After,unsigned(init.dopRestored));
        return finished;
    }
#else
    (void)out;(void)init;
#endif
    return false;
}
static bool BdwTestRenderRunImpl(BdwForcewakeAdapterResult& out,BdwRenderRunResult& render,bool rebased,bool repeat=false) {
    out={};render={};
#if defined(BROOK_SSD_AUTO_RECOVERY)
    DiagnosticLease lease;if(!lease.held)return false;
    static uint32_t attempted[3]{};auto* gate=&attempted[repeat?2:rebased?1:0];
    render.preflightStage=1;if(__atomic_load_n(gate,__ATOMIC_ACQUIRE))return false;
    render.preflightStage=2;if(!PanicRecoveryResetArmed())return false;
    render.preflightStage=3;if(SmpCurrentCpuIndex()!=0)return false;
    render.preflightStage=4;if(!observation.inventory || !observation.planned || !observation.patRequested)return false;
    render.preflightStage=5;if(!OptedIn(rebased?"/etc/dell-gpu-rcs-rebase-test":"/etc/dell-gpu-rcs-run-test"))return false;
#ifndef BROOK_HOST_TEST
    if(repeat){
        if(rebased || !__atomic_load_n(&firstReleased,__ATOMIC_ACQUIRE) || !OptedIn("/etc/dell-gpu-rcs-repeat-test"))return false;
        uint32_t expected=0;if(!__atomic_compare_exchange_n(gate,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        if(!BdwReprepareRetainedGemProbe())return false;
    }
#else
    if(repeat)return false;
#endif
    render.preflightStage=6;
    if(rebased){if(!retainedRenderContext)return false;}
    else {render.dmaStatus=BdwInspectPrivateRenderDma();if(render.dmaStatus!=BdwDmaSnapshotOk)return false;}
    render.preflightStage=7;
    if(!PageAdmitted(BdwDellBar0) || !PageAdmitted(BdwDellBar0+0xb000) || !PageAdmitted(BdwPrivatePatPage) ||
       !PageAdmitted(BdwResetPage) || !PageAdmitted(0xf6880000ull) || !PageAdmitted(0xf6101000ull))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    render.preflightStage=8;const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    render.preflightStage=9;BdwDisplaySnapshot display{};
    if(!BdwObserveDisplayLive(display) || display.regs[0][0]!=0x98000000u || display.regs[0][1]!=12800)return false;
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned reg=0;reg<10;++reg)
        if((pipe || reg>1) && display.regs[pipe][reg])return false;
    render.preflightStage=10;for(uint32_t i=0;i<PciBootInventoryCount();++i){
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        BdwGgttReservation reservation{};
        if(!LiveGpuMatches(entry) || !PowerD0(d) || !(PciConfigRead16(d.bus,d.dev,d.fn,4)&4) ||
           (!rebased && (!BdwRetainedRenderReservation(reservation) || reservation.first!=65544 || reservation.count!=29)))return false;
        if(!repeat){uint32_t expected=0;if(!__atomic_compare_exchange_n(gate,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;}
        render.admitted=true;
        render.preflightStage=11;
        auto* memory=static_cast<uint32_t*>(kmalloc(20*4096));if(!memory)return false;
#ifndef BROOK_HOST_TEST
        const bool gemProbe=!rebased && OptedIn("/etc/dell-gpu-rcs-gem-probe");
        render.externalTablesRequested=gemProbe && OptedIn("/etc/dell-gpu-rcs-external-tables");
        if(gemProbe && !BdwSetupRetainedGemProbe(render.externalTablesRequested)){kfree(memory);return false;}
        render.gemQueued=gemProbe;render.gemIoctlDecoded=gemProbe;render.gemIoctlSubmitted=gemProbe;
#endif
        if(rebased){
            const auto& saved=*reinterpret_cast<const uint32_t (*)[20*1024]>(retainedRenderContext);
            if(!BdwRetainRebasedRenderImages(saved) || BdwInspectRebasedRenderDma()!=BdwDmaSnapshotOk ||
               !BdwRebasedRenderReservation(reservation) || reservation.first!=65573 || reservation.count!=29){kfree(memory);return false;}
        }
        BdwOwnedPage scratch{};BdwGgttReservation pinned{};
        bool scratchReady=false;
#ifndef BROOK_HOST_TEST
        if(repeat)scratchReady=BdwReuseRetainedRenderScratch(scratch,pinned);
        else
#endif
        scratchReady=rebased?BdwPinRebasedRenderScratch(scratch,pinned):BdwPinRetainedRenderScratch(scratch,pinned);
        if(!scratchReady || pinned.first!=reservation.first ||
           pinned.count!=reservation.count || pinned.token!=reservation.token){kfree(memory);return false;}
        auto& snapshot=*reinterpret_cast<uint32_t (*)[20*1024]>(memory);
        render.ggttClearRequested=!rebased && OptedIn("/etc/dell-gpu-rcs-clear-after-reset");
        render.pollYieldRequested=OptedIn("/etc/dell-gpu-rcs-yield-poll");
        VmmDellRenderRun(tsc.khz,pinned.first,pinned.token,scratch.dma,snapshot,out,render);
        const auto& t=render.transaction;
        const bool deviceOk=out.clearDelivered && out.sequence.released && LiveGpuMatches(entry) && PowerD0(d);
        if(t.uncertain || (t.gtReady && !deviceOk))
            KernelPanic("Broadwell render uncertain: gt=%u pat=%u reset=%u restore=%u ggtt=%u port=%u yield=%u/%u/%u sampled=%u head=%x tail=%x mode=%x ctl=%x acthd=%x ipeir=%x ipehr=%x elsp=%x",
                unsigned(t.gtReady),unsigned(t.patReady),unsigned(t.resetComplete),unsigned(t.patRestored),
                render.ggttWriteAttempts,render.portWriteAttempts,unsigned(render.pollYieldRequested),unsigned(render.pollYieldFailed),render.pollYields,
                unsigned(render.engineSampled),render.head,render.tail,
                render.mode,render.control,render.acthd,render.ipeir,render.ipehr,render.execlist);
        bool finished=t.contextSaved && t.resetComplete && t.patRestored && !t.uncertain &&
            out.sequence.workComplete && deviceOk;
#ifndef BROOK_HOST_TEST
        if(gemProbe && finished)finished=BdwFinishRetainedGemProbe(true);
        render.gemReleaseRequested=gemProbe && OptedIn("/etc/dell-gpu-rcs-release-after-reset");
        if(render.gemReleaseRequested && finished) {
            if(render.ggttCleared)render.gemReleased=BdwReleaseRetainedGemProbe();
            if(!render.gemReleased)finished=false;
        }
        if(gemProbe && !finished)BdwFailRetainedGemProbe();
#endif
        if(finished){uint32_t hash=2166136261u;for(unsigned j=0;j<20*1024;++j){hash^=memory[j];hash*=16777619u;}
            render.contextHash=hash;if(rebased)rebasedRenderContext=memory;else {
                if(repeat && retainedRenderContext)kfree(retainedRenderContext);
                retainedRenderContext=memory;
            }
#ifndef BROOK_HOST_TEST
            if(!repeat && !rebased && render.gemReleased && render.externalTablesRequested)
                __atomic_store_n(&firstReleased,1,__ATOMIC_RELEASE);
#endif
        } else kfree(memory);
        return finished;
    }
#else
    (void)out;(void)render;(void)rebased;
#endif
    return false;
}
bool BdwTestRenderRun(BdwForcewakeAdapterResult& out,BdwRenderRunResult& render){return BdwTestRenderRunImpl(out,render,false);}
bool BdwTestRenderRepeat(BdwForcewakeAdapterResult& out,BdwRenderRunResult& render){return BdwTestRenderRunImpl(out,render,false,true);}
bool BdwTestRebasedRenderRun(BdwForcewakeAdapterResult& out,BdwRenderRunResult& render){return BdwTestRenderRunImpl(out,render,true);}
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
static void CaptureWorkerObjects(const BdwExecSnapshot* snapshot){
    static BdwGpuCapture capture{};BdwExecSnapshotCapture(snapshot,capture);
    if(capture.requested)PanicSetGpuCapture(capture);
}
[[noreturn]] static void PanicWorkerStage(const char* stage,const BdwExecSnapshot* snapshot,uint32_t detail=0){
    CaptureWorkerObjects(snapshot);
    uint32_t pid=0,context=0;BdwExecSnapshotOrigin(snapshot,pid,context);
    const auto sample=BdwExecSnapshotBatchSample(snapshot);
    const auto current=BdwExecSnapshotCurrentBatchSample(snapshot);
    KernelPanic("Broadwell worker recovery stage=%s detail=%u pid=%u context=%u batch=%lx length=%lu objects=%u pages=%u current_bytes=%u current_words=%x,%x,%x,%x,%x,%x,%x,%x admission_bytes=%u admission_words=%x,%x,%x,%x,%x,%x,%x,%x",
        stage,detail,pid,context,BdwExecSnapshotBatchAddress(snapshot),BdwExecSnapshotBatchLength(snapshot),
        BdwExecSnapshotObjectCount(snapshot),BdwExecSnapshotPageCount(snapshot),current.bytes,current.words[0],current.words[1],current.words[2],current.words[3],current.words[4],current.words[5],current.words[6],current.words[7],sample.bytes,sample.words[0],sample.words[1],sample.words[2],sample.words[3],sample.words[4],sample.words[5],sample.words[6],sample.words[7]);
}
static BdwRenderAttemptView BdwRenderAttemptViewOf(const BdwForcewakeAdapterResult& out,const BdwRenderRunResult& run){
    BdwRenderAttemptView v{};
    v.adapterRefusal=out.refusal;v.requested=out.sequence.requested;v.released=out.sequence.released;
    v.clearDelivered=out.clearDelivered;v.workAttempted=out.sequence.workAttempted;
    v.entryRefusal=run.entryRefusal;v.gtWrites=run.gtWrites;
    const auto& t=run.transaction;
    v.gtReady=t.gtReady;v.patReady=t.patReady;v.runComplete=t.runComplete;v.contextSaved=t.contextSaved;
    v.resetAttempted=t.resetAttempted;v.patRestored=t.patRestored;v.uncertain=t.uncertain;
    v.executeAttempted=run.attempted;v.pollYieldFailed=run.pollYieldFailed;
    return v;
}
static BdwSubmissionOutcome ExecuteRetainedWorker(BdwExecSnapshot* snapshot,bool contextTest){
#if defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY)
    // I915_EXEC_BSD jobs never reach the render path. (Without the build
    // option readiness is never published, so no BSD snapshot is admitted.)
    if(snapshot && BdwExecSnapshotEngine(snapshot)==BdwExecEngineVideo)return ExecuteVcsUserJob(snapshot);
#endif
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    BdwTraceSnapshot(snapshot,FramePrepBegin);
#endif
    uint64_t ohMark=BdwOverheadTicks();
    auto ohStep=[&ohMark](BdwOverheadStep step){const uint64_t now=BdwOverheadTicks();BdwOverheadAdd(step,now>ohMark?now-ohMark:0);ohMark=now;};
    DiagnosticLease lease;if(!lease.held)return {false,true};
    auto* owner=snapshot?BdwExecSnapshotContext(snapshot):nullptr;
    if(!snapshot || (contextTest?!owner:owner!=nullptr) || !PanicRecoveryResetArmed() || SmpCurrentCpuIndex()!=0 ||
       !__atomic_load_n(&firstReleased,__ATOMIC_ACQUIRE) || !retainedRenderContext ||
       !observation.inventory || !observation.planned || !observation.patRequested ||
       !(contextTest?runtimeBootOptions.contextWorker:runtimeBootOptions.renderWorker))return {false,true};
    constexpr uint64_t pages[]={BdwDellBar0,BdwDellBar0+0xb000,BdwPrivatePatPage,BdwResetPage,0xf6880000ull,0xf6101000ull};
    for(auto page:pages)if(!PageAdmitted(page))return {false,true};
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return {false,true};
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return {false,true};
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return {false,true};
    // Accepts the GOP surface or a mapped kernel flip buffer on plane A.
    BdwDisplaySnapshot display{};
    if(!BdwObserveDisplayAccepted(display,10))return {false,true};
    PciBootEntry entry{};bool found=false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i){PciBootEntry e{};if(!PciBootInventoryRead(i,&e))return {false,true};
        const auto& d=e.device;if(d.bus==0 && d.dev==2 && d.fn==0){entry=e;found=true;break;}}
    if(!found || !LiveGpuMatches(entry) || !PowerD0(entry.device) ||
       !(PciConfigRead16(entry.device.bus,entry.device.dev,entry.device.fn,4)&4))return {false,true};
    static uint32_t nonce=0x42525000;
    if(nonce==~uint32_t{0})return {false,true};
    ohStep(BdwOhEntry);
    auto* memory=static_cast<uint32_t*>(kmalloc(20*4096));if(!memory)return {false,true};
    auto& context=*reinterpret_cast<uint32_t (*)[20*1024]>(memory);
    struct ContextGuard {BdwGpuContext* owner;bool active=false;
        ~ContextGuard(){if(active)BdwGpuContextFinish(owner,nullptr,false);}} guard{owner};
    bool present=false;
    if(owner && !BdwGpuContextBegin(owner,context,present)){kfree(memory);return {false,true};}
    guard.active=owner!=nullptr;
    ohStep(BdwOhContextBegin);
#if defined(BROOK_GPU_CAPTURE_FAULT_TEST)
    // Test kernels only: the dedicated probe uses this exact six-word batch.
    // Panic before device submission, so this tests the capture/reboot path
    // without intentionally hanging the GPU or changing its DMA state.
    const auto testSample=BdwExecSnapshotBatchSample(snapshot);
    if(testSample.bytes>=24 && testSample.words[0]==0x10000002 && testSample.words[1]==0x40011000 &&
       testSample.words[2]==0 && testSample.words[3]==0x54494d45 && testSample.words[4]==0x05000000 &&
       testSample.words[5]==0){kfree(memory);PanicWorkerStage("capture-probe",snapshot);}
#endif
    const auto* saved=present?&context:reinterpret_cast<const uint32_t (*)[20*1024]>(retainedRenderContext);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    auto tick=[](){uint32_t lo,hi;__asm__ volatile("lfence; rdtsc":"=a"(lo),"=d"(hi)::"memory");return (uint64_t(hi)<<32)|lo;};
    const uint64_t startTicks=tick();
#endif
    // A job the kernel cannot serve for deterministic resource reasons is
    // refused: nothing was published to the GPU, the render image and tables
    // are unwound to Released, the context is returned unchanged (not lost)
    // and the client sees a fence error. Uncertain state still panics.
    auto refuseJob=[&](const char* reason)->BdwSubmissionOutcome{
        static uint32_t refusals=0;const uint32_t n=++refusals;
        if(n<=16 || !(n&(n-1))){uint32_t pid=0,ctx=0;BdwExecSnapshotOrigin(snapshot,pid,ctx);
            KPrintf("gpu-submit: job refused pid=%u context=%u objects=%u pages=%u reason=%s count=%u\n",pid,ctx,
                BdwExecSnapshotObjectCount(snapshot),BdwExecSnapshotPageCount(snapshot),reason?reason:"?",n);}
        kfree(memory);
        if(guard.active){(void)BdwGpuContextFinish(owner,nullptr,true);guard.active=false;}
        return {false,true,true};
    };
    if(!BdwReprepareRetainedSubmission(saved,++nonce)){
        // Only the clean case (no pages for the private render image, e.g.
        // under memory pressure) is undone; anything else is uncertain.
        if(!BdwUnwindFailedReprepare()){kfree(memory);PanicWorkerStage("reprepare",snapshot);}
        return refuseJob("private render image allocation failed");
    }
    ohStep(BdwOhReprepare);
    const uint32_t dmaStatus=BdwInspectPrivateRenderDma();
    if(dmaStatus!=BdwDmaSnapshotOk){kfree(memory);PanicWorkerStage("private-dma",snapshot,dmaStatus);}
    ohStep(BdwOhPrivateDma);
    BdwGgttReservation reservation{};
    if(!BdwRetainedRenderReservation(reservation) || reservation.first!=65544 || reservation.count!=29){kfree(memory);PanicWorkerStage("reservation",snapshot);}
#ifdef BROOK_BDW_COHERENT_RENDER
    // This worker has verified the exact Dell GT2 and owns the RCS path
    // that installs and verifies the WB LLC/eLLC PPGTT policy.
    if(!BdwExecSnapshotUseCoherentRender(snapshot)){kfree(memory);PanicWorkerStage("cache policy",snapshot);}
#endif
    ohStep(BdwOhReservation);
    {
        // Too many pages or page tables, or an allocation failure: refused.
        const char* reason=nullptr;
        const auto prepared=BdwPrepareRetainedSubmissionChecked(snapshot,reason);
        if(prepared==BdwPrepareOutcome::Refused)return refuseJob(reason);
        if(prepared!=BdwPrepareOutcome::Ready){kfree(memory);PanicWorkerStage(reason?reason:"prepare",snapshot);}
    }
    ohStep(BdwOhPrepare);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t preparedTicks=tick();
#endif
    BdwOwnedPage scratch{};BdwGgttReservation pinned{};
    if(!BdwReuseRetainedRenderScratch(scratch,pinned) || pinned.first!=reservation.first ||
       pinned.count!=reservation.count || pinned.token!=reservation.token){kfree(memory);PanicWorkerStage("scratch",snapshot);}
    ohStep(BdwOhScratch);
    BdwForcewakeAdapterResult out{};BdwRenderRunResult run{};
    run.ggttClearRequested=true;run.pollYieldRequested=true;
    uint32_t originPid=0,originContext=0;BdwExecSnapshotOrigin(snapshot,originPid,originContext);
    // Worker-thread-only completion estimate per submitter context; it only
    // moves where completion polling switches from ticks to spin slices.
    static BdwCompletionPredictor<8> completionPredictor;
    const uint64_t predictionKey=originPid?(uint64_t(originPid)<<32)|originContext:0;
    run.pollSpinRequested=!runtimeBootOptions.pollSpinDisabled && BdwPromptWakeEnabled();
    run.expectedCompletionTicks=completionPredictor.Predict(predictionKey);
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    run.sampleFrequency=ProfilerFrameTraceActive();
    run.statsFrequency=true;
#if defined(BROOK_BDW_RPS_TRIAL)
    run.tryFrequency=ShutdownGetAcStatus().source==AcPowerSource::Online &&
        PageAdmitted(BdwRenderFrequencyCapsPage);
    BdwClockSampleThermal(tsc.khz);
    run.trialBin=BdwClockEffectiveBin(BdwClock());
    BdwClock().lastRequestedBin=run.trialBin;
#endif
    BdwTraceSnapshot(snapshot,FrameDeviceBegin);
#endif
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    // Zero-copy images are mapped with PPAT slot 3; lease it UC for this job
    // (inherited: WB, invisible to the non-snooping display engine).
    const bool scanoutImages=BdwExecSnapshotHasUncached(snapshot);
    run.uncachedScanout=scanoutImages && BdwZeroCopyUncachedRender();
    run.scanoutPat=BdwZeroCopyScanoutPat();
#endif
    ohMark=BdwOverheadTicks();
    // gen 387: a run can end before any device write (forcewake claim busy,
    // entry evidence, engines not yet idle, GT init NotStarted). The job is
    // still Prepared and armed, so rerun it a bounded number of times after a
    // tick; only genuine device uncertainty reaches the panic below.
    const BdwRenderRunResult request=run;
    BdwRenderAttemptView attemptView{};
    BdwRenderAttemptClass attemptClass=BdwRenderAttemptClass::Ran;
    for(unsigned attempt=1;;++attempt){
        VmmDellRenderRun(tsc.khz,pinned.first,pinned.token,scratch.dma,context,out,run);
        attemptView=BdwRenderAttemptViewOf(out,run);
        attemptClass=BdwClassifyRenderAttempt(attemptView);
        if(attemptClass!=BdwRenderAttemptClass::RefusedUntouched)break;
        static uint32_t refusals=0;const uint32_t n=++refusals;
        if(n<=16 || !(n&(n-1)))
            KPrintf("gpu-submit: render run refused before device access pid=%u context=%u reason=%s detail=%u gt_refusal=%u attempt=%u count=%u\n",
                originPid,originContext,BdwRenderAttemptReason(attemptView),
                unsigned(attemptView.adapterRefusal?out.refusalDetail:run.entryDetail),unsigned(run.entryDetail),attempt,n);
        if(attempt>=BdwRenderRefusalRetries)break;
        SchedulerSleepMs(1);
        run=request;
    }
    ohStep(BdwOhRenderRun);
#if defined(BROOK_GPU_SUBMISSION_TIMING) && defined(BROOK_BDW_RPS_TRIAL) && !defined(BROOK_HOST_TEST)
    if(run.trialFrequencyCaps && run.trialFrequencyCaps!=0xffffffffu)BdwClock().lastCaps=run.trialFrequencyCaps;
#endif
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    if(scanoutImages)BdwZeroCopyNoteRender(run.uncachedScanout);
#endif
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t deviceTicks=tick();
#endif
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    BdwTraceSnapshot(snapshot,FrameDeviceEnd);
    if(run.sampleFrequency && run.issueTicks){
        BdwTraceSnapshot(snapshot,FrameRenderIssue,0,run.issueTicks);
        BdwTraceSnapshot(snapshot,FrameRenderBreadcrumb,0,run.breadcrumbTicks);
        BdwTraceSnapshot(snapshot,FrameRenderContextRetired,0,run.contextRetiredTicks);
        if(run.frequencySamples){
            const auto first=run.frequency[0].ticks;
            BdwTraceSnapshot(snapshot,FrameRenderFrequencyRequest,run.frequencyRequest,first);
            BdwTraceSnapshot(snapshot,FrameRenderPowerControl,(uint64_t{run.rc6Control}<<32)|run.rpsControl,first);
#if defined(BROOK_BDW_RPS_TRIAL)
            BdwTraceSnapshot(snapshot,FrameRenderFrequencyCaps,run.trialFrequencyCaps,first);
            if(run.trialFrequencyOriginal)
                BdwTraceSnapshot(snapshot,FrameRenderFrequencyTrial,
                    (uint64_t{run.trialFrequencyOriginal}<<32)|run.trialFrequencyPosted,first);
#endif
            for(unsigned i=0;i<run.frequencySamples;++i){
                const auto& sample=run.frequency[i];
                BdwTraceSnapshot(snapshot,FrameRenderFrequency,sample.status,sample.ticks);
            }
        }
    }
#endif
    const auto& t=run.transaction;
    const bool ok=t.contextSaved && t.resetComplete && t.patRestored && !t.uncertain && run.ggttCleared &&
        !run.pollYieldFailed && out.sequence.workComplete && out.clearDelivered && out.sequence.released &&
        LiveGpuMatches(entry) && PowerD0(entry.device);
    if(ok && run.completionEndTicks>run.completionStartTicks)
        completionPredictor.Record(predictionKey,run.completionEndTicks-run.completionStartTicks);
    // gen 400: the job ran but its completion was not observed cleanly (for
    // example an unexpected context-status event). If the engine reset was
    // verified, this run's GGTT binding retired and PAT restored, nothing on
    // the device is uncertain: retire the job as failed (fence error, its
    // buffers marked failed, its context lost) instead of panicking.
    BdwRenderFailureView failure{};
    failure.ok=ok;failure.retiredAfterFailedRun=run.retiredAfterFailedRun;failure.resetComplete=t.resetComplete;
    failure.patRestored=t.patRestored;failure.uncertain=t.uncertain;failure.ggttCleared=run.ggttCleared;
    failure.pollYieldFailed=run.pollYieldFailed;failure.clearDelivered=out.clearDelivered;failure.released=out.sequence.released;
    failure.liveGpu=!ok && LiveGpuMatches(entry) && PowerD0(entry.device);
    const bool recoveredFailure=BdwRenderFailureRecoverable(failure);
    if(recoveredFailure){
        static uint32_t recovered=0;const uint32_t n=++recovered;
        if(n<=16 || !(n&(n-1)))
            KPrintf("gpu-submit: job failed after verified reset pid=%u context=%u objects=%u pages=%u site=%u csb_site=%u csb_events=%u csb_tail=%u csb_head=%u csb_event=%lx breadcrumb=%u ctx_complete=%u head=%x tail=%x acthd=%x elsp=%x eir=%x esr=%x ipehr=%x count=%u\n",
                originPid,originContext,BdwExecSnapshotObjectCount(snapshot),BdwExecSnapshotPageCount(snapshot),
                unsigned(run.runFailSite),unsigned(run.csbSite),unsigned(run.csbEvents),unsigned(run.csbTail),unsigned(run.csbHead),run.csbEvent,
                unsigned(run.breadcrumb),unsigned(run.contextComplete),run.head,run.tail,run.acthd,run.execlist,run.eir,run.esr,run.ipehr,n);
        if(!BdwRetireRetainedSubmission(snapshot)){kfree(memory);PanicWorkerStage("retire after failed run",snapshot);}
        kfree(memory);
        if(owner){BdwGpuContextFinish(owner,nullptr,false);guard.active=false;} // marks the context lost
        return {false,true,false};
    }
    const auto sample=BdwExecSnapshotBatchSample(snapshot);
    if(!ok){CaptureWorkerObjects(snapshot);const auto current=BdwExecSnapshotCurrentBatchSample(snapshot);
        KPrintf("gpu-submit: render run failed site=%u csb=%u/%u/%u/%u event=%lx breadcrumb=%u ctx_complete=%u saved=%u run=%u ggtt_cleared=%u retired_after_failure=%u uncertain=%u eir=%x esr=%x\n",
            unsigned(run.runFailSite),unsigned(run.csbSite),unsigned(run.csbEvents),unsigned(run.csbTail),unsigned(run.csbHead),run.csbEvent,
            unsigned(run.breadcrumb),unsigned(run.contextComplete),unsigned(t.contextSaved),unsigned(t.runComplete),unsigned(run.ggttCleared),
            unsigned(run.retiredAfterFailedRun),unsigned(t.uncertain),run.eir,run.esr);
        KPrintf("gpu-submit: render run failed class=%u reason=%s fw_refusal=%u/%u requested=%u acquired=%u idle=%u quiescent=%u/%u released=%u clear=%u work=%u/%u entry=%u/%u gt_writes=%u\n",
            unsigned(attemptClass),BdwRenderAttemptReason(attemptView),unsigned(out.refusal),unsigned(out.refusalDetail),
            unsigned(out.sequence.requested),unsigned(out.sequence.acquired),unsigned(out.sequence.idle),
            unsigned(out.sequence.submissionQuiescent),unsigned(out.sequence.otherEnginesQuiescent),unsigned(out.sequence.released),
            unsigned(out.clearDelivered),unsigned(out.sequence.workAttempted),unsigned(out.sequence.workComplete),
            unsigned(run.entryRefusal),unsigned(run.entryDetail),run.gtWrites);
        KernelPanic("Broadwell worker uncertain: fw=%u/%u/%u/%u entry=%u/%u reason=%s pid=%u context=%u batch=%lx length=%lu objects=%u pages=%u gt=%u pat=%u reset=%u restore=%u ggtt=%u port=%u yield=%u/%u sampled=%u head=%x tail=%x mode=%x ctl=%x acthd=%x ipeir=%x ipehr=%x elsp=%x current_bytes=%u current_words=%x,%x,%x,%x,%x,%x,%x,%x admission_bytes=%u admission_words=%x,%x,%x,%x,%x,%x,%x,%x",
        unsigned(out.refusal),unsigned(out.sequence.requested),unsigned(out.sequence.workAttempted),unsigned(out.clearDelivered && out.sequence.released),
        unsigned(run.entryRefusal),unsigned(run.entryDetail),BdwRenderAttemptReason(attemptView),
        originPid,originContext,BdwExecSnapshotBatchAddress(snapshot),BdwExecSnapshotBatchLength(snapshot),BdwExecSnapshotObjectCount(snapshot),BdwExecSnapshotPageCount(snapshot),
        unsigned(t.gtReady),unsigned(t.patReady),unsigned(t.resetComplete),unsigned(t.patRestored),run.ggttWriteAttempts,
        run.portWriteAttempts,unsigned(run.pollYieldFailed),run.pollYields,unsigned(run.engineSampled),run.head,run.tail,run.mode,run.control,run.acthd,run.ipeir,run.ipehr,run.execlist,
        current.bytes,current.words[0],current.words[1],current.words[2],current.words[3],current.words[4],current.words[5],current.words[6],current.words[7],sample.bytes,sample.words[0],sample.words[1],sample.words[2],sample.words[3],sample.words[4],sample.words[5],sample.words[6],sample.words[7]);}
    if(!BdwRetireRetainedSubmission(snapshot)){kfree(memory);PanicWorkerStage("retire",snapshot);}
    ohStep(BdwOhRetire);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t retiredTicks=tick();
    if(tsc.khz && preparedTicks>=startTicks && deviceTicks>=preparedTicks && retiredTicks>=deviceTicks){
        const auto pageCount=BdwExecSnapshotPageCount(snapshot);
        BdwTimingRecord(snapshot,BdwTimingPrepare,(preparedTicks-startTicks)*1000/tsc.khz,pageCount);
        BdwTimingRecord(snapshot,BdwTimingDevice,(deviceTicks-preparedTicks)*1000/tsc.khz,pageCount);
        BdwTimingRecord(snapshot,BdwTimingRetire,(retiredTicks-deviceTicks)*1000/tsc.khz,pageCount);
        for(unsigned i=0;i<6;++i)if(run.transactionTicks[i] && run.transactionTicks[i]<=~uint64_t{0}/1000)
            BdwTimingRecord(snapshot,static_cast<BdwTimingStage>(BdwTimingGtInit+i),run.transactionTicks[i]*1000/tsc.khz,pageCount);
        if(run.issueTicks){
            const auto issueToBreadcrumb=run.breadcrumbTicks-run.issueTicks;
            const auto breadcrumbToRetirement=run.contextRetiredTicks-run.breadcrumbTicks;
            if(issueToBreadcrumb<=~uint64_t{0}/1000)
                BdwTimingRecord(snapshot,BdwTimingIssueToBreadcrumb,issueToBreadcrumb*1000/tsc.khz,pageCount);
            if(breadcrumbToRetirement<=~uint64_t{0}/1000)
                BdwTimingRecord(snapshot,BdwTimingBreadcrumbToRetirement,breadcrumbToRetirement*1000/tsc.khz,pageCount);
        }
    }
#endif
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    BdwTraceSnapshot(snapshot,FrameRetired);
    BdwWorkerStatsRender(run.pollYields,run.completionPollYields,run.pollYieldTicks,
        run.pollSpins,run.completionPollSpins,run.pollSpinTicks,
        run.statsFrequencySamples!=0,run.statsIssueStatus,run.statsEndStatus);
#endif
    if(owner){
        if(!BdwGpuContextFinish(owner,&context,true)){kfree(memory);PanicWorkerStage("context-finish",snapshot);}
        guard.active=false;kfree(memory);ohStep(BdwOhFinish);return {true,true};
    }
    kfree(retainedRenderContext);retainedRenderContext=memory;ohStep(BdwOhFinish);return {true,true};
}
BdwSubmissionOutcome BdwExecuteRetainedWorkerSnapshot(void*,BdwExecSnapshot* snapshot){return ExecuteRetainedWorker(snapshot,false);}
BdwSubmissionOutcome BdwExecuteRetainedContextWorkerSnapshot(void*,BdwExecSnapshot* snapshot){return ExecuteRetainedWorker(snapshot,true);}
bool BdwStoreProbeEnabled(){
    return __atomic_load_n(&firstReleased,__ATOMIC_ACQUIRE) && PanicRecoveryResetArmed() &&
        runtimeBootOptions.execStore && runtimeBootOptions.contextWorker &&
        BdwIntelUserNodeEnabled();
}
bool BdwRenderExperimentEnabled(){
    return BdwStoreProbeEnabled() && runtimeBootOptions.renderExperiment;
}
#endif
static BdwNodeCache g_intelNodeCache{};
BdwNodeCache& BdwIntelNodeCache(){return g_intelNodeCache;}
bool BdwIntelUserNodeEnabled(){
#if defined(BROOK_SSD_AUTO_RECOVERY)
    if(!PanicRecoveryResetArmed() || !observation.inventory || !observation.planned || !runtimeBootOptions.intelNode)return false;
    return BdwNodeCachedCheck(g_intelNodeCache,ApicTickCount(),[]{
        for(uint32_t i=0;i<PciBootInventoryCount();++i){PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
            const auto& d=entry.device;if(d.bus==0 && d.dev==2 && d.fn==0)return LiveGpuMatches(entry) && PowerD0(d);}
        return false;});
#else
    return false;
#endif
}
bool BdwAutomaticRenderRequested(){
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    return BdwIntelUserNodeEnabled() && runtimeBootOptions.autoBootstrap &&
        runtimeBootOptions.renderExperiment;
#else
    return false;
#endif
}
bool BdwTestBcsCopy(BdwForcewakeAdapterResult& out,BdwBcsRunResult& copy) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    DiagnosticLease lease;if(!lease.held)return false;
    static uint32_t attempted=0;
    if(__atomic_load_n(&attempted,__ATOMIC_ACQUIRE) || !PanicRecoveryResetArmed() || SmpCurrentCpuIndex()!=0 ||
       !__atomic_load_n(&scratchMappingReady,__ATOMIC_ACQUIRE) || !observation.inventory || !observation.planned ||
       !observation.patRequested || !OptedIn("/etc/dell-gpu-bcs-copy-test") || BdwInspectPrivateBcsDma()!=BdwDmaSnapshotOk ||
       !PageAdmitted(BdwPrivatePatPage) || !PageAdmitted(BdwResetPage) || !PageAdmitted(0xf6880000ull) ||
       !PageAdmitted(0xf6101000ull))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    BdwDisplaySnapshot display{};
    if(!BdwObserveDisplayLive(display) || display.regs[0][0]!=0x98000000u || display.regs[0][1]!=12800)return false;
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned reg=0;reg<10;++reg)
        if((pipe || reg>1) && display.regs[pipe][reg])return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        BdwOwnedPage scratch{};BdwGgttReservation reservation{};
        if(!LiveGpuMatches(entry) || !PowerD0(d) || !(PciConfigRead16(d.bus,d.dev,d.fn,4)&4) ||
           !BdwRetainedBcsReservation(reservation) || reservation.first!=65536 || reservation.count!=8 ||
           !BdwRetainedBcsScratch(scratch))return false;
        uint32_t expected=0;if(!__atomic_compare_exchange_n(&attempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        VmmDellBcsCopy(tsc.khz,reservation.first,reservation.token,scratch.dma,out,copy);
        if((copy.resetBeforeAttempted && !copy.resetBefore) || copy.patUncertain)
            KernelPanic("Broadwell copy recovery uncertain: reset=%u pat=%u ggtt=%u port=%u sampled=%u head=%x tail=%x mode=%x ctl=%x acthd=%x ipeir=%x ipehr=%x elsp=%x",
                unsigned(copy.resetBefore),unsigned(copy.patUncertain),copy.ggttWriteAttempts,copy.portWriteAttempts,
                unsigned(copy.engineSampled),copy.head,copy.tail,copy.mode,copy.control,copy.acthd,copy.ipeir,copy.ipehr,copy.execlist);
        return copy.copyComplete && copy.contextComplete && copy.resetAfter && copy.patRestored &&
            out.sequence.workComplete && out.clearDelivered && out.sequence.released && LiveGpuMatches(entry) && PowerD0(d);
    }
#else
    (void)out;(void)copy;
#endif
    return false;
}
// One-time recovery for the exact mapping written on generation 47. A warm
// firmware reset left it valid after the page lost its boot-lifetime pin.
// This is deliberately not a generic command for arbitrary valid PTEs.
bool BdwClearPriorScratch(BdwForcewakeAdapterResult& out,BdwScratchTakeoverResult& clear) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    DiagnosticLease lease;if(!lease.held)return false;
    static uint32_t attempted=0;clear.stage=1;
    if(__atomic_load_n(&attempted,__ATOMIC_ACQUIRE) || !PanicRecoveryResetArmed())return false;
    clear.stage=2;if(SmpCurrentCpuIndex()!=0)return false;
    clear.stage=3;
    if(!observation.inventory || !observation.planned || !observation.patRequested ||
       !OptedIn("/etc/dell-gpu-clear-stale") || BdwInspectPrivateBcsDma()!=BdwDmaSnapshotOk ||
       !PageAdmitted(BdwPrivatePatPage) || !PageAdmitted(0xf6880000ull) || !PageAdmitted(0xf6101000ull))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    clear.stage=4;const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    BdwDisplaySnapshot display{};
    if(!BdwObserveDisplayLive(display) || display.regs[0][0]!=0x98000000u || display.regs[0][1]!=12800)return false;
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned reg=0;reg<10;++reg)
        if((pipe || reg>1) && display.regs[pipe][reg])return false;
    clear.stage=5;
    for(uint32_t i=0;i<PciBootInventoryCount();++i){
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        BdwGgttReservation reservation{};
        if(!LiveGpuMatches(entry) || !PowerD0(d) || !BdwRetainedBcsReservation(reservation) || reservation.first!=65536)return false;
        uint32_t expected=0;if(!__atomic_compare_exchange_n(&attempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        BdwForcewakeAdapterResult result{};BdwScratchTakeoverResult state{};
        VmmDellForcewakeClearStale(tsc.khz,119971841ull,result,state);
        out=result;clear=state;clear.stage=6;
        return state.complete && result.sequence.workComplete && result.clearDelivered && result.sequence.released &&
            LiveGpuMatches(entry) && PowerD0(d);
    }
#else
    (void)out;(void)clear;
#endif
    return false;
}
bool BdwObserveGgttSurvey(uint32_t first,BdwGgttSurvey& out) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    if(first<65536 || first>1048576-64 || (first&63) || !PanicRecoveryResetArmed() || !observation.inventory || !observation.planned)return false;
    const uint64_t address=0xf6800000ull+uint64_t(first)*8;
    if(!PageAdmitted(address&~4095ull))return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i){PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        const uint32_t entries=BdwGgttEntries(entry.bdwGraphics);
        if(entries<64 || first>entries-64 || !LiveGpuMatches(entry) || !PowerD0(d))return false;
        BdwGgttSurvey sample{};
        if(!VmmDellGgttSurveyCriticalRead(first,sample) || !LiveGpuMatches(entry) || !PowerD0(d))return false;
        out=sample;return true;
    }
#else
    (void)first;(void)out;
#endif
    return false;
}
bool BdwObserveReservedGgtt(BdwGgttReservedPtes& out) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    BdwGgttReservation reservation{};
    if(!PanicRecoveryResetArmed() || !observation.inventory || !observation.planned ||
       !BdwRetainedBcsReservation(reservation) || reservation.count!=8)return false;
    const uint64_t address=0xf6800000ull+uint64_t(reservation.first)*8;
    if(!PageAdmitted(address&~4095ull))return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;
        if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        if(!LiveGpuMatches(entry) || !PowerD0(d))return false;
        BdwGgttReservedPtes sample{};
        if(!VmmDellReservedGgttCriticalRead(reservation.first,sample) ||
           !LiveGpuMatches(entry) || !PowerD0(d))return false;
        out=sample;return true;
    }
#else
    (void)out;
#endif
    return false;
}
bool BdwObserveRcsGgtt(BdwRcsGgttPtes& out) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    // Fixed Dell test range; this is read-only and grants no reservation.
    if(!PanicRecoveryResetArmed() || !observation.inventory || !observation.planned ||
       !OptedIn("/etc/dell-gpu-rcs-clear-after-reset") || !PageAdmitted(0xf6880000ull))return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i){PciBootEntry entry{};
        if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        if(!LiveGpuMatches(entry) || !PowerD0(d))return false;
        BdwRcsGgttPtes result{};
        for(unsigned chunk=0;chunk<4;++chunk){BdwGgttReservedPtes part{};
            if(!VmmDellReservedGgttCriticalRead(65544+chunk*8,part))return false;
            for(unsigned j=0;j<8 && chunk*8+j<29;++j)result.values[chunk*8+j]=part.values[j];
        }
        if(!LiveGpuMatches(entry) || !PowerD0(d))return false;
        out=result;return true;
    }
#else
    (void)out;
#endif
    return false;
}
bool BdwObserveGopPtesLive(BdwGopPtes& out) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    if(!PanicRecoveryResetArmed() || !observation.inventory || !observation.planned ||
       !PageAdmitted(observation.plan.firstAddress&~4095ull) ||
       !PageAdmitted(observation.plan.lastAddress&~4095ull))return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        if(!LiveGpuMatches(entry) || !PowerD0(d))return false;
        BdwGopPtes first{},second{};
        if(!VmmDellGopPtesCriticalRead(observation.plan,first) ||
           !VmmDellGopPtesCriticalRead(observation.plan,second) ||
           first.first!=second.first || first.last!=second.last ||
           !LiveGpuMatches(entry) || !PowerD0(d))return false;
        out=first;return true;
    }
#else
    (void)out;
#endif
    return false;
}
bool BdwObserveGopBacking(BdwGopBacking& out) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    if(!PanicRecoveryResetArmed() || !observation.inventory || !observation.planned)return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus!=0 || d.dev!=2 || d.fn!=0)continue;
        const auto& firmware=entry.bdwGraphics;
        auto stable=[&]() {
            return LiveGpuMatches(entry) && PowerD0(d) && PciConfigRead32(0,2,0,0x5c)==firmware.stolenBase;
        };
        BdwDisplaySnapshot display{};
        if(!stable() || !BdwObserveDisplayAccepted(display))return false;
        BdwGopBacking sample{};
        const auto readPage=[](uint32_t first,uint32_t count,uint64_t& physical) {
            const auto address=BdwDellGgttBase+uint64_t(first)*8;
            return PageAdmitted(address) && VmmDellGopBackingPage(first,count,physical);
        };
        if(!BdwGopBackingSnapshot(observation.plan,BdwDecodeStolenRange(firmware),readPage,sample) ||
           !BdwObserveDisplayAccepted(display) || !stable())return false;
        BootResourceClassification classification{};
        constexpr uint32_t allowed=(1u<<uint32_t(MemoryType::Reserved))|(1u<<uint32_t(MemoryType::Framebuffer));
        if(!AcpiClassifyBootRange(sample.physicalBase,uint32_t(sample.bytes),classification) ||
           (classification.types&~allowed))return false;
        out=sample;return true;
    }
#else
    (void)out;
#endif
    return false;
}
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
namespace {
BdwScanoutCopyStats copyStats;
}
BdwScanoutCopyStats BdwReadScanoutCopyStats(){
    return {__atomic_load_n(&copyStats.requested,__ATOMIC_RELAXED),__atomic_load_n(&copyStats.submitted,__ATOMIC_RELAXED),
        __atomic_load_n(&copyStats.full,__ATOMIC_RELAXED),__atomic_load_n(&copyStats.partial,__ATOMIC_RELAXED),
        __atomic_load_n(&copyStats.lastStage,__ATOMIC_RELAXED)};
}
bool BdwBcsScanoutEnabled(){return observation.bcsPresentRequested && observation.planned && observation.inventory;}
#if defined(BROOK_BDW_PERSISTENT_SCANOUT_MAP) || defined(BROOK_BDW_SCANOUT_FLIP)
#define BROOK_BDW_SCANOUT_TARGETS 1
#endif
BdwScanoutStatus BdwExecuteBcsScanout(BdwGpuBuffer* source,uint64_t token,const CompositorPresentRegion* regions,uint32_t count){
    __atomic_add_fetch(&copyStats.requested,1,__ATOMIC_RELAXED);
    auto stage=[](uint32_t value){__atomic_store_n(&copyStats.lastStage,value,__ATOMIC_RELAXED);};
    stage(1);
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    const auto* current=ProcessCurrent();
    const uint16_t pid=current?current->pid:0;
    const auto sourceId=BdwGpuBufferIdentity(source);
    ProfilerTraceGpu(pid,FrameBlitBegin,token,sourceId);
    const auto statsStart=BdwTimingNow();
#endif
    DiagnosticLease lease;
    if(!lease.held || !BdwBcsScanoutEnabled() || !BdwRenderExperimentEnabled() || SmpCurrentCpuIndex()!=0 ||
       !PanicRecoveryResetArmed() || !observation.patRequested || !PageAdmitted(BdwPrivatePatPage) ||
       !PageAdmitted(BdwResetPage) || !PageAdmitted(0xf6880000ull) || !PageAdmitted(0xf6101000ull))return BdwScanoutStatus::Unavailable;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return BdwScanoutStatus::Unavailable;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return BdwScanoutStatus::Unavailable;
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz || !(PciConfigRead16(0,2,0,4)&4))return BdwScanoutStatus::Unavailable;
    stage(2);
    BdwGgttReservation reservation{};
    BdwOwnedPage scratch{};
#if defined(BROOK_BDW_SCANOUT_TARGETS)
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    // Zero-copy: the source is a registered renderer image; arm DSPASURF to
    // it (or queue it) instead of copying. Runtime-gated off by default.
    switch(BdwZeroCopyPresentSource(source,regions,count)){
    case BdwZeroCopyOutcome::Presented:
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
        ProfilerTraceGpu(pid,FrameBlitEnd,token,sourceId);
#endif
        stage(5);return BdwScanoutStatus::Completed;
    case BdwZeroCopyOutcome::RepairRequired:return BdwScanoutStatus::RepairRequired;
    case BdwZeroCopyOutcome::Copy:break;
    }
#endif
    // Persistent GOP map or page flip, both runtime-gated off by default.
    BdwTargetPlan plan{};
    if(BdwScanoutTargetSelect(regions,count,plan)==BdwTargetSelect::RepairRequired)return BdwScanoutStatus::RepairRequired;
    const bool flip=plan.kind==BdwTargetKind::Flip;
    bool prepared=false;
    if(plan.kind==BdwTargetKind::Legacy){
        BdwGopBacking backing{};
        if(!BdwObserveGopBacking(backing))return BdwScanoutStatus::Unavailable;
        stage(3);
        prepared=BdwPrepareBcsScanout(source,token,backing,regions,count,reservation,scratch);
    }else{
        stage(3);
        prepared=BdwPrepareBcsScanoutTarget(source,token,plan.leaves,plan.leafCount,plan.repairs,plan.repairCount,
                                            plan.destination,regions,count,reservation,scratch);
    }
    if(!prepared){
        if(flip){(void)BdwScanoutTargetFinish(plan,false);return BdwScanoutStatus::RepairRequired;}
        return BdwScanoutStatus::Unavailable;
    }
#else
    BdwGopBacking backing{};
    if(!BdwObserveGopBacking(backing))return BdwScanoutStatus::Unavailable;
    stage(3);
    if(!BdwPrepareBcsScanout(source,token,backing,regions,count,reservation,scratch))return BdwScanoutStatus::Unavailable;
#endif
    stage(4);
    BdwForcewakeAdapterResult wake{};
    BdwBcsRunResult copy{};
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    ProfilerTraceGpu(pid,FrameBlitIssue,token,sourceId);
    const auto statsPrepared=BdwTimingNow();
#endif
    VmmDellBcsScanout(tsc.khz,reservation.first,reservation.token,scratch.dma,wake,copy);
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    const auto statsDevice=BdwTimingNow();
#endif
    if(copy.portWriteAttempts)__atomic_add_fetch(&copyStats.submitted,1,__ATOMIC_RELAXED);
    if((copy.resetBeforeAttempted && !copy.resetBefore) || copy.patUncertain ||
       (copy.attempted && (!wake.clearDelivered || !wake.sequence.released)) ||
       !BdwReleasePreparedBcsScanout())
        KernelPanic("Broadwell scanout retirement uncertain: reset=%u/%u pat=%u ggtt=%u elsp=%u head=%x tail=%x acthd=%x ipeir=%x ipehr=%x",
            unsigned(copy.resetBefore),unsigned(copy.resetAfter),unsigned(copy.patRestored),
            copy.ggttWriteAttempts,copy.portWriteAttempts,copy.head,copy.tail,copy.acthd,copy.ipeir,copy.ipehr);
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    const auto statsRetired=BdwTimingNow();
    ProfilerTraceGpu(pid,FrameBlitEnd,token,sourceId);
#endif
    const bool copied=copy.copyComplete && copy.contextComplete && copy.scanoutRetired && wake.sequence.workComplete;
#if defined(BROOK_BDW_SCANOUT_TARGETS)
    // Commit (and arm) a flip only after the copy retired; anything else
    // freezes the chain and asks the compositor for a full software repair.
    if(flip && !BdwScanoutTargetFinish(plan,copied))return BdwScanoutStatus::RepairRequired;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    BdwWorkerStatsScanout(flip?BdwWorkerScanoutEvent::TargetFlip:plan.kind==BdwTargetKind::PersistentGop?
        BdwWorkerScanoutEvent::TargetPersistent:BdwWorkerScanoutEvent::TargetLegacy);
#endif
#endif
    if(!copied)return BdwScanoutStatus::Unavailable;
    const bool full=count==1 && !regions[0].x && !regions[0].y && regions[0].width==3200 && regions[0].height==1800;
    __atomic_add_fetch(full?&copyStats.full:&copyStats.partial,1,__ATOMIC_RELAXED);
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    {
        uint64_t pixels=0;for(uint32_t i=0;i<count;++i)pixels+=uint64_t(regions[i].width)*regions[i].height;
        auto span=[](uint64_t a,uint64_t b){return b>a?b-a:0;};
        const uint64_t ticks[BdwPresentStageCount]={span(statsStart,statsPrepared),span(statsPrepared,statsDevice),
            copy.runTicks,span(statsDevice,statsRetired),span(statsStart,statsRetired)};
        BdwWorkerStatsPresent(full,pixels,ticks,0); // BCS completion spins; it never yields
    }
#endif
    stage(5);
    return BdwScanoutStatus::Completed;
}
#endif
void BdwGopObserveConfigure(const BootProtocol* boot) {
    if(probeAttempted)return;
    observation={};
#if defined(BROOK_SSD_AUTO_RECOVERY)
    runtimeBootOptions={};
#endif
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
#if defined(BROOK_SSD_AUTO_RECOVERY)
        runtimeBootOptions={
            OptedIn("/etc/dell-gpu-exec-store-test"),
            OptedIn("/etc/dell-gpu-rcs-context-test"),
            OptedIn("/etc/dell-gpu-rcs-worker-test"),
            OptedIn("/etc/dell-gpu-intel-user-test"),
            OptedIn("/etc/dell-gpu-render-experiment"),
            OptedIn("/etc/dell-gpu-auto-bootstrap"),
            OptedIn("/etc/dell-gpu-no-poll-spin"),
        };
#endif
#if defined(BROOK_SSD_AUTO_RECOVERY)
        // M4a prompt wakeups stay off unless explicitly opted in at boot or
        // enabled later with `gpu wakeups on`.
        if(OptedIn("/etc/dell-gpu-prompt-wake"))BdwPromptWakeSet(true);
#endif
        observation.probeRequested=OptedIn("/etc/dell-gpu-gtt-probe");
        observation.ackRequested=OptedIn("/etc/dell-gpu-forcewake-ack");
        observation.otherEnginesRequested=OptedIn("/etc/dell-gpu-engines-run");
        observation.patRequested=OptedIn("/etc/dell-gpu-pat-probe");
        observation.bcsPresentRequested=OptedIn("/etc/dell-gpu-bcs-present");
        observation.wakeRequested=observation.patRequested || observation.otherEnginesRequested || OptedIn("/etc/dell-gpu-forcewake-run");
        observation.prepareRequested=OptedIn("/etc/dell-gpu-prepare-only");
        observation.renderPrepareRequested=OptedIn("/etc/dell-gpu-render-prepare-only");
        observation.retainRequested=OptedIn("/etc/dell-gpu-retain-bcs");
        observation.renderRetainRequested=OptedIn("/etc/dell-gpu-retain-rcs");
        if(!observation.probeRequested && !observation.ackRequested && !observation.wakeRequested && !observation.prepareRequested && !observation.renderPrepareRequested && !observation.retainRequested && !observation.renderRetainRequested)return;
        probeAttempted=true;
        if(!observation.planned || SmpCurrentCpuIndex()!=0)return;
        if(observation.prepareRequested){
            observation.prepareBuilt=BdwPreparePrivateImages();
            SerialPrintf("gpu-bcs: private preparation %s; no hardware submission\n",observation.prepareBuilt?"ok":"failed");
        }
        if(observation.retainRequested){
            observation.retainBuilt=BdwRetainPrivateBcsImages(BdwGgttEntries(e.bdwGraphics));
            SerialPrintf("gpu-bcs: private boot-lifetime allocation %s; no hardware submission\n",observation.retainBuilt?"ok":"failed");
        }
        if(observation.renderRetainRequested){
            observation.renderRetainBuilt=BdwRetainPrivateRenderImages(BdwGgttEntries(e.bdwGraphics),
                OptedIn("/etc/dell-gpu-rcs-ppgtt-probe"),OptedIn("/etc/dell-gpu-rcs-ppgtt-store"));
            SerialPrintf("gpu-rcs: private boot-lifetime allocation %s; no hardware submission\n",observation.renderRetainBuilt?"ok":"failed");
        }
        if(observation.renderPrepareRequested){
            auto* staging=reinterpret_cast<uint32_t (*)[20*1024]>(kmalloc(20*4096));
            if(staging){observation.renderPrepareBuilt=BdwPreparePrivateRenderImages(*staging);kfree(staging);}
            SerialPrintf("gpu-rcs: private preparation %s; no hardware submission\n",observation.renderPrepareBuilt?"ok":"failed");
        }
        if(!observation.probeRequested && !observation.ackRequested && !observation.wakeRequested)return;
        observation.powerD0=LiveGpuMatches(e) && PowerD0(d);
        if(observation.probeRequested)observation.pagesAdmitted=PageAdmitted(observation.plan.firstAddress&~4095ull) &&
            PageAdmitted(observation.plan.lastAddress&~4095ull);
        if(observation.probeRequested && observation.powerD0 && observation.pagesAdmitted){
            SerialPuts("gpu-gtt: admitted; sampling GOP endpoint entries\n");BdwGopPtes p{};
            observation.sampled=VmmDellGopPtesCriticalRead(observation.plan,p);
            if(observation.sampled){observation.firstPte=p.first;observation.lastPte=p.last;}
        }
        if(observation.ackRequested){
            observation.ackPageAdmitted=PageAdmitted(DellForcewakeAckPage);
            if(observation.powerD0 && observation.ackPageAdmitted){
                SerialPuts("gpu-forcewake: admitted; sampling ACK only\n");
                observation.ackSampled=VmmDellForcewakeAckCriticalRead(observation.ackRaw);
            }
        }
        if(observation.wakeRequested){
            observation.wakePagesAdmitted=true;
            for(auto page:BdwForcewakePages)if(!PageAdmitted(page))observation.wakePagesAdmitted=false;
            if(observation.otherEnginesRequested) {
                observation.otherEnginePagesAdmitted=true;
                for(auto page:BdwOtherEnginePages)
                    if(!PageAdmitted(page))observation.otherEnginePagesAdmitted=false;
            }
            if(observation.patRequested)observation.patPageAdmitted=PageAdmitted(BdwPrivatePatPage);
            const auto& tsc=NativeTscObserveRead();
            observation.wakeClockReady=tsc.capable && tsc.measured && tsc.consistent && tsc.khz;
            if(observation.powerD0 && observation.wakePagesAdmitted && (!observation.otherEnginesRequested || observation.otherEnginePagesAdmitted) && (!observation.patRequested || observation.patPageAdmitted) && observation.wakeClockReady){
                SerialPuts("gpu-forcewake: opt-in experiment; firmware/SMM bit ownership assumed\n");
                BdwForcewakeAdapterResult r{};DiagnosticLease lease;
                if(lease.held)VmmDellForcewakeCriticalRun(tsc.khz,r,observation.otherEnginesRequested,observation.patRequested);
                observation.wakeAliases=r.aliases;observation.wakeClock=r.clock;
                observation.wakeClearDelivered=r.clearDelivered;observation.wake=r.sequence;
                SerialPrintf("gpu-forcewake: requested=%u acquired=%u c0=%u sampled=%u idle=%u clear=%u released=%u\n",
                    r.sequence.requested,r.sequence.acquired,r.sequence.threadReady,r.sequence.sampled,r.sequence.idle,r.clearDelivered,r.sequence.released);
            }
        }
        SerialPrintf("gpu-gtt: power=%u pages=%u sampled=%u\n",observation.powerD0,observation.pagesAdmitted,observation.sampled);
        return;
    }
}
const BdwGopObservation& BdwGopObserveRead(){return observation;}
}
