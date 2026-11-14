#include "intel_bdw_scanout.h"
#include "intel_bdw_submission_timing.h"
#include "hda_dell_inventory.h"
#include "boot_protocol/boot_protocol.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_display_probe.h"
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
#if defined(BROOK_GPU_SUBMISSION_TIMING)
#include "kprintf.h"
#endif
#include "vfs.h"
#include "panic_test_reset.h"
#include "intel_bdw_submission_worker.h"
#include "profiler.h"
#if defined(BROOK_BDW_RPS_TRIAL)
#include "shutdown.h"
#endif
#include "process.h"
#include "intel_bdw_file.h"
#include "compositor.h"
#include "panic_qr.h"
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
static BdwSubmissionOutcome ExecuteRetainedWorker(BdwExecSnapshot* snapshot,bool contextTest){
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    BdwTraceSnapshot(snapshot,FramePrepBegin);
#endif
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
    BdwDisplaySnapshot display{};
    if(!BdwObserveDisplayLive(display) || display.regs[0][0]!=0x98000000u || display.regs[0][1]!=12800)return {false,true};
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned reg=0;reg<10;++reg)
        if((pipe || reg>1) && display.regs[pipe][reg])return {false,true};
    PciBootEntry entry{};bool found=false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i){PciBootEntry e{};if(!PciBootInventoryRead(i,&e))return {false,true};
        const auto& d=e.device;if(d.bus==0 && d.dev==2 && d.fn==0){entry=e;found=true;break;}}
    if(!found || !LiveGpuMatches(entry) || !PowerD0(entry.device) ||
       !(PciConfigRead16(entry.device.bus,entry.device.dev,entry.device.fn,4)&4))return {false,true};
    static uint32_t nonce=0x42525000;
    if(nonce==~uint32_t{0})return {false,true};
    auto* memory=static_cast<uint32_t*>(kmalloc(20*4096));if(!memory)return {false,true};
    auto& context=*reinterpret_cast<uint32_t (*)[20*1024]>(memory);
    struct ContextGuard {BdwGpuContext* owner;bool active=false;
        ~ContextGuard(){if(active)BdwGpuContextFinish(owner,nullptr,false);}} guard{owner};
    bool present=false;
    if(owner && !BdwGpuContextBegin(owner,context,present)){kfree(memory);return {false,true};}
    guard.active=owner!=nullptr;
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
    if(!BdwReprepareRetainedSubmission(saved,++nonce)){kfree(memory);PanicWorkerStage("reprepare",snapshot);}
    const uint32_t dmaStatus=BdwInspectPrivateRenderDma();
    if(dmaStatus!=BdwDmaSnapshotOk){kfree(memory);PanicWorkerStage("private-dma",snapshot,dmaStatus);}
    BdwGgttReservation reservation{};
    if(!BdwRetainedRenderReservation(reservation) || reservation.first!=65544 || reservation.count!=29){kfree(memory);PanicWorkerStage("reservation",snapshot);}
#ifdef BROOK_BDW_COHERENT_RENDER
    // This worker has verified the exact Dell GT2 and owns the RCS path
    // that installs and verifies the WB LLC/eLLC PPGTT policy.
    if(!BdwExecSnapshotUseCoherentRender(snapshot)){kfree(memory);PanicWorkerStage("cache policy",snapshot);}
#endif
    if(!BdwPrepareRetainedSubmission(snapshot)){kfree(memory);PanicWorkerStage("prepare",snapshot);}
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t preparedTicks=tick();
#endif
    BdwOwnedPage scratch{};BdwGgttReservation pinned{};
    if(!BdwReuseRetainedRenderScratch(scratch,pinned) || pinned.first!=reservation.first ||
       pinned.count!=reservation.count || pinned.token!=reservation.token){kfree(memory);PanicWorkerStage("scratch",snapshot);}
    BdwForcewakeAdapterResult out{};BdwRenderRunResult run{};
    run.ggttClearRequested=true;run.pollYieldRequested=true;
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    run.sampleFrequency=ProfilerFrameTraceActive();
#if defined(BROOK_BDW_RPS_TRIAL)
    run.tryFrequency=ShutdownGetAcStatus().source==AcPowerSource::Online &&
        PageAdmitted(BdwRenderFrequencyCapsPage);
#endif
    BdwTraceSnapshot(snapshot,FrameDeviceBegin);
#endif
    VmmDellRenderRun(tsc.khz,pinned.first,pinned.token,scratch.dma,context,out,run);
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
    uint32_t originPid=0,originContext=0;BdwExecSnapshotOrigin(snapshot,originPid,originContext);
    const auto sample=BdwExecSnapshotBatchSample(snapshot);
    if(!ok){CaptureWorkerObjects(snapshot);const auto current=BdwExecSnapshotCurrentBatchSample(snapshot);KernelPanic("Broadwell worker uncertain: pid=%u context=%u batch=%lx length=%lu objects=%u pages=%u gt=%u pat=%u reset=%u restore=%u ggtt=%u port=%u yield=%u/%u sampled=%u head=%x tail=%x mode=%x ctl=%x acthd=%x ipeir=%x ipehr=%x elsp=%x current_bytes=%u current_words=%x,%x,%x,%x,%x,%x,%x,%x admission_bytes=%u admission_words=%x,%x,%x,%x,%x,%x,%x,%x",
        originPid,originContext,BdwExecSnapshotBatchAddress(snapshot),BdwExecSnapshotBatchLength(snapshot),BdwExecSnapshotObjectCount(snapshot),BdwExecSnapshotPageCount(snapshot),
        unsigned(t.gtReady),unsigned(t.patReady),unsigned(t.resetComplete),unsigned(t.patRestored),run.ggttWriteAttempts,
        run.portWriteAttempts,unsigned(run.pollYieldFailed),run.pollYields,unsigned(run.engineSampled),run.head,run.tail,run.mode,run.control,run.acthd,run.ipeir,run.ipehr,run.execlist,
        current.bytes,current.words[0],current.words[1],current.words[2],current.words[3],current.words[4],current.words[5],current.words[6],current.words[7],sample.bytes,sample.words[0],sample.words[1],sample.words[2],sample.words[3],sample.words[4],sample.words[5],sample.words[6],sample.words[7]);}
    if(!BdwRetireRetainedSubmission(snapshot)){kfree(memory);PanicWorkerStage("retire",snapshot);}
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
#endif
    if(owner){
        if(!BdwGpuContextFinish(owner,&context,true)){kfree(memory);PanicWorkerStage("context-finish",snapshot);}
        guard.active=false;kfree(memory);return {true,true};
    }
    kfree(retainedRenderContext);retainedRenderContext=memory;return {true,true};
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
bool BdwIntelUserNodeEnabled(){
#if defined(BROOK_SSD_AUTO_RECOVERY)
    if(!PanicRecoveryResetArmed() || !observation.inventory || !observation.planned || !runtimeBootOptions.intelNode)return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i){PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry))return false;
        const auto& d=entry.device;if(d.bus==0 && d.dev==2 && d.fn==0)return LiveGpuMatches(entry) && PowerD0(d);}
    return false;
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
        if(!stable() || !BdwObserveDisplayLive(display) || !BdwDisplayUsesGop(display))return false;
        BdwGopBacking sample{};
        const auto readPage=[](uint32_t first,uint32_t count,uint64_t& physical) {
            const auto address=BdwDellGgttBase+uint64_t(first)*8;
            return PageAdmitted(address) && VmmDellGopBackingPage(first,count,physical);
        };
        if(!BdwGopBackingSnapshot(observation.plan,BdwDecodeStolenRange(firmware),readPage,sample) ||
           !BdwObserveDisplayLive(display) || !BdwDisplayUsesGop(display) || !stable())return false;
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
bool BdwExecuteBcsScanout(BdwGpuBuffer* source,uint64_t token,const CompositorPresentRegion* regions,uint32_t count){
    __atomic_add_fetch(&copyStats.requested,1,__ATOMIC_RELAXED);
    auto stage=[](uint32_t value){__atomic_store_n(&copyStats.lastStage,value,__ATOMIC_RELAXED);};
    stage(1);
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    const auto* current=ProcessCurrent();
    const uint16_t pid=current?current->pid:0;
    const auto sourceId=BdwGpuBufferIdentity(source);
    ProfilerTraceGpu(pid,FrameBlitBegin,token,sourceId);
#endif
    DiagnosticLease lease;
    if(!lease.held || !BdwBcsScanoutEnabled() || !BdwRenderExperimentEnabled() || SmpCurrentCpuIndex()!=0 ||
       !PanicRecoveryResetArmed() || !observation.patRequested || !PageAdmitted(BdwPrivatePatPage) ||
       !PageAdmitted(BdwResetPage) || !PageAdmitted(0xf6880000ull) || !PageAdmitted(0xf6101000ull))return false;
    for(auto page:BdwForcewakePages)if(!PageAdmitted(page))return false;
    for(auto page:BdwOtherEnginePages)if(!PageAdmitted(page))return false;
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz || !(PciConfigRead16(0,2,0,4)&4))return false;
    stage(2);
    BdwGopBacking backing{};
    if(!BdwObserveGopBacking(backing))return false;
    stage(3);
    BdwGgttReservation reservation{};
    BdwOwnedPage scratch{};
    if(!BdwPrepareBcsScanout(source,token,backing,regions,count,reservation,scratch))return false;
    stage(4);
    BdwForcewakeAdapterResult wake{};
    BdwBcsRunResult copy{};
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    ProfilerTraceGpu(pid,FrameBlitIssue,token,sourceId);
#endif
    VmmDellBcsScanout(tsc.khz,reservation.first,reservation.token,scratch.dma,wake,copy);
    if(copy.portWriteAttempts)__atomic_add_fetch(&copyStats.submitted,1,__ATOMIC_RELAXED);
    if((copy.resetBeforeAttempted && !copy.resetBefore) || copy.patUncertain ||
       (copy.attempted && (!wake.clearDelivered || !wake.sequence.released)) ||
       !BdwReleasePreparedBcsScanout())
        KernelPanic("Broadwell scanout retirement uncertain: reset=%u/%u pat=%u ggtt=%u elsp=%u head=%x tail=%x acthd=%x ipeir=%x ipehr=%x",
            unsigned(copy.resetBefore),unsigned(copy.resetAfter),unsigned(copy.patRestored),
            copy.ggttWriteAttempts,copy.portWriteAttempts,copy.head,copy.tail,copy.acthd,copy.ipeir,copy.ipehr);
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    ProfilerTraceGpu(pid,FrameBlitEnd,token,sourceId);
#endif
    if(!copy.copyComplete || !copy.contextComplete || !copy.scanoutRetired || !wake.sequence.workComplete)return false;
    const bool full=count==1 && !regions[0].x && !regions[0].y && regions[0].width==3200 && regions[0].height==1800;
    __atomic_add_fetch(full?&copyStats.full:&copyStats.partial,1,__ATOMIC_RELAXED);
    stage(5);
    return true;
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
        };
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
