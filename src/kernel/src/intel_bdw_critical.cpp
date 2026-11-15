// Dell/Broadwell GPU critical-section work, moved verbatim from
// memory/virtual_memory.cpp. The VMM supplies only the kernel page-table
// critical section and the alias audit; all device logic lives here.
#include "intel_bdw_render_refusal.h"
#include "cpu_memory_type_batch.h"
#include "intel_bdw_worker_overhead.h"
#include "intel_bdw_critical.h"
#include "cpu_idle.h"
#include "memory/virtual_memory.h"
#include "cpu.h"
#include "dell_i2c_id_policy.h"
#include "intel_vtd_probe_policy.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_ggtt_mmio.h"
#include "intel_bdw_ggtt_extent_mmio.h"
#include "intel_bdw_display_flip.h"
#include "intel_bdw_display_probe.h"
#include "intel_bdw_forcewake_ack.h"
#include "intel_bdw_forcewake_adapter.h"
#include "intel_bdw_scratch_mmio.h"
#include "intel_bdw_bcs_mmio.h"
#include "intel_bdw_rcs_reset_mmio.h"
#include "intel_bdw_vcs_reset_mmio.h"
#include "intel_bdw_video.h"
#include "intel_bdw_vcs_user.h"
#include "intel_bdw_render_gt_mmio.h"
#include "intel_bdw_render_backend.h"
#include "intel_bdw_csb.h"
#include "intel_bdw_pat.h"
#include "intel_bdw_live_backend.h"
#include "intel_bdw_publish.h"
#include "intel_bdw_scanout.h"
#if defined(BROOK_GPU_SUBMISSION_TIMING)
#include "intel_bdw_submission_timing.h"
#endif
#include "intel_bdw_wait_yield.h"
#include "intel_bdw_poll_pacer.h"
#include "intel_bdw_prompt_wake.h"
#include "scheduler.h"
#include "smp.h"

inline void* operator new(__SIZE_TYPE__,void* storage) noexcept{return storage;}
namespace brook {
// Last VCS operation's GT PM before/after record (written only under the
// forcewake claim; read by the debug command after the operation returns).
static BdwGtPmRecord vcsPmRecord;
template<class Held> static bool BdwVcsPmSnapshot(Held& held,BdwGtPmSnapshot& out){
    out={};
    for(unsigned i=0;i<BdwGtPmRegCount;++i)if(!held.read32(BdwDellBar0+BdwGtPmRegs[i],out.value[i]))return false;
    out.valid=true;return true;
}
template<class Held> static void BdwVcsPmFinish(Held& held,uint8_t op){
    struct Rw {Held& h;bool read(uint32_t r,uint32_t& v){return h.read32(BdwDellBar0+r,v);}
        bool write(uint32_t r,uint32_t v){return h.write32(BdwDellBar0+r,v);}} rw{held};
    vcsPmRecord.op=op;
    if(!BdwVcsPmSnapshot(held,vcsPmRecord.after))return;
    BdwGtPmRestore(rw,vcsPmRecord);
}
template<class Held> static void BdwVcsPmBegin(Held& held){
    const auto first=vcsPmRecord.first;vcsPmRecord={};vcsPmRecord.first=first;
    (void)BdwVcsPmSnapshot(held,vcsPmRecord.before);
    if(!vcsPmRecord.first.valid)vcsPmRecord.first=vcsPmRecord.before;
}
// A GEN6_GRDOM_MEDIA reset returns VCS engine-block registers to hardware
// defaults. Firmware programmed RING_MAX_IDLE to 10 (hardware default 64);
// at 64 the GT never enters RC6 and the render clock sags for the boot (gen
// 392). Restore every engine register in BdwVcsEngineRestoreRegs to its value
// in the first snapshot of the boot, with readback, right after each reset.
static uint32_t vcsEngineRestores=0,vcsEngineRestoreFailures=0;
// Phase 4: the last VCS user job ended with VCS verifiably stopped and its
// lease retired. Any other VCS operation clears it (the next job resets).
static bool vcsCleanStop=false;
// Phase 4 lane: the PAT values the submit phase left active (held) and the
// inherited values to restore, plus the reservation the job is bound in.
struct VcsLaneHeld {uint32_t inheritedLo,inheritedHi,lo,hi;uint32_t scratchIndex;uint64_t token,scratchDma;};
[[maybe_unused]] static VcsLaneHeld laneHeld{};
template<class Rw> static bool BdwVcsRestoreFirmwareEngine(Rw& io){
    const auto& first=vcsPmRecord.first;
    if(!first.valid)return false;
    bool ok=true;
    for(const auto& r:BdwVcsEngineRestoreRegs){
        const int i=BdwGtPmIndex(r.reg);if(i<0){ok=false;continue;}
        uint32_t now=0;if(!io.read32(BdwDellBar0+r.reg,now) || now==0xffffffffu){ok=false;continue;}
        const uint32_t target=r.masked?(first.value[i]&0xffffu):first.value[i];
        if((r.masked?(now&0xffffu):now)==target)continue;
        const uint32_t value=r.masked?(0xffff0000u|target):target;uint32_t back=0;
        ++vcsEngineRestores;
        if(!io.write32(BdwDellBar0+r.reg,value) || !io.read32(BdwDellBar0+r.reg,back) ||
           (r.masked?(back&0xffffu):back)!=target){++vcsEngineRestoreFailures;ok=false;}
    }
    return ok;
}
void VmmDellVcsEngineRestoreCounts(uint32_t& restores,uint32_t& failures){restores=vcsEngineRestores;failures=vcsEngineRestoreFailures;}
static BdwGtPmSnapshot vcsPmSample{};
static uint32_t vcsPmOpWrites=0,vcsPmOpFailures=0;
void VmmDellVcsPmRecord(BdwGtPmRecord& out){out=vcsPmRecord;}
void VmmDellVcsPmSample(BdwGtPmSnapshot& out,uint32_t& writes,uint32_t& failures){out=vcsPmSample;writes=vcsPmOpWrites;failures=vcsPmOpFailures;}
template<class F> static bool BdwTimedTransaction(BdwRenderRunResult& result,unsigned stage,F f){
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const auto start=BdwTimingNow();
#endif
    const bool ok=f();
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const auto end=BdwTimingNow();if(end>start)result.transactionTicks[stage]=end-start;
#else
    (void)result;(void)stage;
#endif
    return ok;
}

bool VmmDellI2cIdCriticalRead(uint32_t& value) {
    value=0;
    struct Io {
        bool evidence(DellI2cCriticalEvidence& e) {
            KernelAliasMapping identity{},direct{},current{};
            // The debug service is a kernel thread. Refuse other roots rather
            // than assume a process's copied kernel-half entry stays shared.
            e.aliasesValid=VmmKernelPtAuditPage(DellI2c0Page,identity,direct,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==DellI2c0Page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==DellI2c0Page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==DellI2c0Page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(DellI2c0Page,DellI2c0PrivatePage,cpu);
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool readComponent(uint32_t& result) {
            // One fixed aligned volatile load through the existing direct map.
            __asm__ volatile("lfence":::"memory");
            result=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+DellI2c0Page+DellI2cComponentOffset);return true;
        }
    } io;
    return VmmWithKernelPtCritical([&](){return DellI2cIdCritical(io,value);});
}

bool VmmDellGpuVtdCriticalRead(DellGpuVtdStatus& status) {
    struct Io {
        bool evidence(DellI2cCriticalEvidence& e) {
            KernelAliasMapping identity{},direct{},current{};
            e.aliasesValid=VmmKernelPtAuditPage(DellGpuVtdPage,identity,direct,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==DellGpuVtdPage;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==DellGpuVtdPage;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==DellGpuVtdPage;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(DellGpuVtdPage,DellGpuVtdPage,cpu);
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool readVersion(uint32_t& v) {
            __asm__ volatile("lfence":::"memory");
            v=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+DellGpuVtdPage);return true;
        }
        bool readStatus(uint32_t& v) {
            __asm__ volatile("lfence":::"memory");
            v=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+DellGpuVtdPage+0x1c);return true;
        }
        bool readCapability(uint64_t& v) {
            __asm__ volatile("lfence":::"memory");
            v=*reinterpret_cast<const volatile uint64_t*>(DIRECT_MAP_BASE+DellGpuVtdPage+0x08);return true;
        }
        bool readProtectedMemory(uint32_t& v) {
            __asm__ volatile("lfence":::"memory");
            v=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+DellGpuVtdPage+0x64);return true;
        }
    } io;
    return VmmWithKernelPtCritical([&](){return DellGpuVtdCritical(io,status);});
}

bool VmmDellDisplayCriticalRead(BdwDisplaySnapshot& result) {
    struct Io {
        bool evidence(uint64_t page,DellI2cCriticalEvidence& e) {
            KernelAliasMapping identity{},direct{},current{};
            e.aliasesValid=VmmKernelPtAuditPage(page,identity,direct,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu);
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool read32(uint64_t address,uint32_t& value) {
            __asm__ volatile("lfence":::"memory");
            value=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+address);return true;
        }
    } io;
    return VmmWithKernelPtCritical([&](){return BdwDisplayCriticalRead(io,result);});
}

namespace {
struct DellGopGgttIo {
    bool evidence(uint64_t page,DellI2cCriticalEvidence& e) {
        KernelAliasMapping identity{},direct{},current{};
        e.aliasesValid=VmmKernelPtAuditPage(page,identity,direct,current);
        e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
        e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
        e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
        CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu);
        e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
        return true;
    }
    bool read(uint64_t address,uint64_t& value) {
        __asm__ volatile("lfence":::"memory");
        value=*reinterpret_cast<const volatile uint64_t*>(DIRECT_MAP_BASE+address);return true;
    }
};
}
bool VmmDellGopPtesCriticalRead(const BdwGopPtePlan& plan,BdwGopPtes& result) {
    DellGopGgttIo io;
    return VmmWithKernelPtCritical([&](){return BdwGopPtesCritical(plan,io,result);});
}

bool VmmDellGopBackingPage(uint32_t first,uint32_t count,uint64_t& physicalBase) {
    DellGopGgttIo io;
    return VmmWithKernelPtCritical([&](){return BdwGopBackingPageCritical(first,count,io,physicalBase);});
}

bool VmmDellGgttSurveyCriticalRead(uint32_t first,BdwGgttSurvey& result) {
    struct Io {
        uint32_t first;
        bool evidence(uint64_t page,DellI2cCriticalEvidence& e) {
            KernelAliasMapping identity{},direct{},current{};
            e.aliasesValid=VmmKernelPtAuditPage(page,identity,direct,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu);
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool read(uint64_t address,uint64_t& value) {
            const uint64_t begin=0xf6800000ull+uint64_t(first)*8;
            if(address<begin || ((address-begin)&7) || (address-begin)/8>=64)return false;
            __asm__ volatile("lfence":::"memory");
            value=*reinterpret_cast<const volatile uint64_t*>(DIRECT_MAP_BASE+address);
            __asm__ volatile("lfence":::"memory");return true;
        }
    } io{first};
    return VmmWithKernelPtCritical([&](){return BdwGgttSurveyCritical(first,io,result);});
}

bool VmmDellReservedGgttCriticalRead(uint32_t first,BdwGgttReservedPtes& result) {
    struct Io {
        // This diagnostic authorizes reads only. It cannot validate a lease
        // or grant device ownership to a binding transaction.
        struct ReadOnlyOwner {
            bool readable(){return true;}
            bool owned(){return false;}
            bool quiescent(){return false;}
            bool invalidateGpuTranslations(){return false;}
            bool validateLease(const BdwGgttLease&){return false;}
            bool validateTakeover(const BdwGgttLease&,uint64_t){return false;}
        } owner;
        BdwGgttMmio<ReadOnlyOwner> mmio;
        uint32_t first;
        explicit Io(uint32_t firstIndex):mmio(owner,
            reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6800000ull+uint64_t(firstIndex/512)*4096),
            reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6101008ull),firstIndex,8),first(firstIndex){}
        bool evidence(uint64_t page,DellI2cCriticalEvidence& e) {
            KernelAliasMapping identity{},direct{},current{};
            e.aliasesValid=VmmKernelPtAuditPage(page,identity,direct,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu);
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool read(uint64_t address,uint64_t& value) {
            const uint64_t begin=0xf6800000ull+uint64_t(first)*8;
            if(address<begin || ((address-begin)&7) || (address-begin)/8>=8)return false;
            return mmio.read(first+uint32_t((address-begin)/8),value);
        }
    } io(first);
    return VmmWithKernelPtCritical([&](){return BdwReservedGgttPtesCritical(first,io,result);});
}

bool VmmDellForcewakeAckCriticalRead(uint32_t& result) {
    struct Io {
        bool evidence(DellI2cCriticalEvidence& e) {
            constexpr uint64_t page=DellForcewakeAckPage;
            KernelAliasMapping identity{},direct{},current{};
            e.aliasesValid=VmmKernelPtAuditPage(page,identity,direct,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu) && cpu.cpu==0;
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool read(uint32_t& value) {
            __asm__ volatile("lfence":::"memory");
            value=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+DellForcewakeAckPage+0x44);return true;
        }
    } io;
    return VmmWithKernelPtCritical([&](){return BdwForcewakeAckCritical(io,result);});
}

// Serialize all users for this boot. A completed clear/ACK permits another
// bounded observation; an uncertain release permanently blocks further use.
// The page-table lock prevents mapping changes throughout each operation.
static void VmmDellForcewakeCriticalImpl(uint32_t khz,BdwForcewakeAdapterResult& result,bool otherEngines,bool patRequested,bool tlbRequested,bool resetRequested,bool scopeCheck,
                                        uint32_t scratchIndex,uint64_t scratchDma,BdwScratchTakeoverResult* scratchResult,uint8_t* scratchCpu,BdwBcsInitResult* initResult,bool clearScratch,BdwPatTestResult* patResult,uint64_t copyToken,BdwBcsRunResult* copyResult,BdwRcsResetTestResult* rcsReset=nullptr,BdwRenderGtInitResult* gtInit=nullptr,BdwRenderRunResult* renderResult=nullptr,uint32_t (*snapshot)[20*1024]=nullptr,bool scanoutCopy=false,BdwRcsResetTestResult* vcsReset=nullptr,bool vcsSelftest=false,uint8_t vcsPmOp=0,BdwRcsResetTestResult* vcsPmResult=nullptr,bool vcsUser=false,uint8_t vcsUserPhase=0) {
    // VCS0 copy-flow users: the phase 1b self-test and phase 1c user jobs.
    const bool vcsCopy=vcsSelftest || vcsUser;
    static BdwForcewakeClaim claim;result={};
    if(!claim.acquire()){result.refusal=claim.state()==2?BdwFwRefusalClaimUncertain:BdwFwRefusalClaimBusy;return;}
    struct Io {
        bool tlbRequested,resetRequested,scratchRequested,initRequested,patChangeRequested,copyRequested,rcsResetRequested,gtInitRequested,renderRequested,vcsResetRequested,vcsRunRequested;
        uint64_t* irq=nullptr;bool mappingsValid=true;
        bool owned(){return mappingsValid;}
        bool evidence(uint64_t page,DellI2cCriticalEvidence& e){
            KernelAliasMapping identity{},direct{},current{};
            e.aliasesValid=VmmKernelPtAuditPage(page,identity,direct,current);
            if(page==BdwForcewakePages[0] && !current.writable)e.aliasesValid=false;
            if(tlbRequested && page==BdwPrivatePatPage && !current.writable)e.aliasesValid=false;
            if(patChangeRequested && (page==BdwPrivatePatPage || page==BdwResetPage || page==BdwForcewakePages[3]) && !current.writable)e.aliasesValid=false;
            if(copyRequested && (page==BdwPrivatePatPage || page==BdwResetPage || page==BdwForcewakePages[3] ||
               page==0xf6880000ull || page==0xf6101000ull) && !current.writable)e.aliasesValid=false;
            if(resetRequested && (page==BdwResetPage || page==BdwForcewakePages[3]) && !current.writable)e.aliasesValid=false;
            if(scratchRequested && (page==BdwPrivatePatPage || page==0xf6880000ull || page==0xf6101000ull ||
               page==BdwResetPage || page==BdwForcewakePages[3]) && !current.writable)e.aliasesValid=false;
            if(initRequested && (page==BdwResetPage || page==BdwForcewakePages[3]) && !current.writable)e.aliasesValid=false;
            if(rcsResetRequested && (page==BdwResetPage || page==BdwOtherEnginePages[0]) && !current.writable)e.aliasesValid=false;
            if(vcsResetRequested && (page==BdwResetPage || page==BdwOtherEnginePages[1]) && !current.writable)e.aliasesValid=false;
            if(vcsRunRequested && page==BdwOtherEnginePages[1] && !current.writable)e.aliasesValid=false;
            if(gtInitRequested && (page==BdwOtherEnginePages[0] || page==BdwPrivatePatPage || page==BdwResetPage || page==BdwDellBar0+0xb000) && !current.writable)e.aliasesValid=false;
            if(renderRequested && (page==BdwOtherEnginePages[0] || page==BdwPrivatePatPage || page==BdwResetPage || page==BdwDellBar0+0xb000 || page==0xf6880000ull || page==0xf6101000ull) && !current.writable)e.aliasesValid=false;
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};
            const auto* batched=typedFor(page);
            if(batched){cpu=*batched;e.cpuValid=cpu.cpu==0;}
            else e.cpuValid=CpuObserveMemoryTypes(page,page,cpu) && cpu.cpu==0;
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        // One stable MTRR/PAT/CR0 window for a whole audit page set; see
        // cpu_memory_type_batch.h. Valid only until clearTypes(), and only
        // inside the same interrupts-off critical section as the evidence.
        uint64_t typedPages[16]{};CpuMemoryTypeObservation typed[16]{};unsigned typedCount=0;
        const CpuMemoryTypeObservation* typedFor(uint64_t page) const {
            for(unsigned i=0;i<typedCount;++i)if(typedPages[i]==page)return &typed[i];
            return nullptr;
        }
        void observeTypes(const uint64_t* pages,unsigned count){
            typedCount=0;
            if(!BdwAuditBatchEnabled() || count>16)return;
            const uint64_t start=BdwOverheadTicks();
            for(unsigned i=0;i<count;++i)typedPages[i]=pages[i];
            if(CpuObserveMemoryTypesBatch(typedPages,count,typed)){typedCount=count;BdwOverheadCount(BdwOverhead().counters.auditBatches);}
            else BdwOverheadCount(BdwOverhead().counters.auditBatchFallbacks);
            const uint64_t end=BdwOverheadTicks();BdwOverheadAdd(BdwOhAuditTypes,end>start?end-start:0);
        }
        void clearTypes(){typedCount=0;}
        bool read32(uint64_t address,uint32_t& value){
            if(!mappingsValid)return false;
            __asm__ volatile("lfence":::"memory");
            value=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+address);
            __asm__ volatile("lfence":::"memory");return true;
        }
        bool write32(uint64_t address,uint32_t value){
            if(!mappingsValid)return false;
            *reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+address)=value;
            __asm__ volatile("mfence":::"memory");return true;
        }
        bool tsc(uint64_t& value){uint32_t lo,hi,aux;
            __asm__ volatile("rdtscp; lfence":"=a"(lo),"=d"(hi),"=c"(aux)::"memory");
            value=(uint64_t(hi)<<32)|lo;return true;}
        void relax(){__asm__ volatile("pause");}
        bool MayYield(){return mappingsValid && irq && (*irq&0x200) && SmpCurrentCpuIndex()==0;}
        void Unlock(){VmmKernelPtCriticalExit(*irq);}
        // Runs between Unlock and Lock: interrupts are enabled and no device
        // or page-table state is touched. spinTicks selects a short spin
        // slice (yielding first to any ready CPU-0 work) instead of a tick.
        void Wait(){
            BdwOverheadScope waitScope(BdwOhYieldWait);
            if(spinTicks){
                struct Cpu {Io& io;
                    uint64_t flags(){uint64_t f;__asm__ volatile("pushfq; popq %0":"=r"(f)::"memory");return f;}
                    void yield(){SchedulerYield();}
                    void enableInterrupts(){__asm__ volatile("sti":::"memory");}
                    bool tsc(uint64_t& v){return io.tsc(v);}
                    void relax(){io.relax();}
                } cpu{*this};
                BdwSpinSlice(cpu,spinTicks);
            }else{
                SchedulerSleepMs(1);
                // Belt and braces: Wait is only entered with IF=1 (MayYield),
                // and Lock records the current flags as the critical-section
                // restore state. Never let a sleep path hand IF=0 to Lock.
                uint64_t f;__asm__ volatile("pushfq; popq %0":"=r"(f)::"memory");
                if(!(f&0x200)){__asm__ volatile("sti":::"memory");BdwPromptWakeCount(BdwPromptWake().ifRestores);}
            }
            tsc(waitEnd);
        }
        void Lock(){*irq=VmmKernelPtCriticalEnter();}
        bool Audit(){
            if(SmpCurrentCpuIndex()!=0)return false;
            BdwOverheadScope scope(BdwOhAudit);
            constexpr uint64_t extra[]={BdwDellBar0,BdwPrivatePatPage,BdwResetPage,BdwDellBar0+0xb000,0xf6880000ull,0xf6101000ull};
            uint64_t all[sizeof(BdwForcewakePages)/8+sizeof(BdwOtherEnginePages)/8+sizeof(extra)/8];unsigned n=0;
            for(auto page:BdwForcewakePages)all[n++]=page;
            for(auto page:BdwOtherEnginePages)all[n++]=page;
            for(auto page:extra)all[n++]=page;
            observeTypes(all,n);
            bool ok=true;
            for(unsigned i=0;i<n && ok;++i){DellI2cCriticalEvidence e{};ok=evidence(all[i],e) && BdwGttEvidenceValid(e);}
            clearTypes();
            return ok;
        }
        void Poison(){mappingsValid=false;}
        uint64_t spinTicks=0,waitEnd=0;
    } io{tlbRequested,resetRequested,scratchResult!=nullptr,initResult!=nullptr,patResult!=nullptr,copyResult!=nullptr,rcsReset!=nullptr,gtInit!=nullptr,renderResult!=nullptr,vcsReset!=nullptr,vcsCopy || (vcsPmResult && vcsPmOp>uint8_t(BdwVcsPmOp::Sample))};
    uint64_t irq=VmmKernelPtCriticalEnter();io.irq=&irq;
    // Read-only exercise of the callback boundary. The callback remains under
    // the same page-table lock and forcewake claim as the initial samples.
    auto work=[&](Io& held,const BdwForcewakeResult& initial){
        uint32_t ack=0,lo=0,hi=0;
        if(!(held.read32(BdwDellBar0+0x130044,ack) && ack!=0xffffffffu && (ack&1) &&
            held.read32(BdwDellBar0+0x40e0,lo) && held.read32(BdwDellBar0+0x40e4,hi) &&
            lo==initial.patLo && hi==initial.patHi)){
            if(renderResult)renderResult->entryRefusal=BdwRenderRefusalAckOrPat;
            return false;
        }
        struct GtOwner {
                Io& io;uint32_t khz;uint32_t selector=0,fuse=0;bool sampled=false;
                bool owned(){uint32_t a=0;return io.owned() && io.read32(BdwDellBar0+0x130044,a) && a!=0xffffffffu && (a&1);}
                bool allEnginesQuiescent(){return owned();} // sampled immediately before callback
                // Exact display state and PCI identity are checked by the
                // caller; like the existing copy path, firmware/SMM must not
                // concurrently touch the GT while Brook holds the claim.
                bool displayAndFirmwareSafe(){return owned();}
                bool mcrSteeringReady(){return owned();}
                bool readMcrTopology(uint32_t& s,uint32_t& f){
                    if(!owned() || !io.read32(BdwDellBar0+0xfdc,s) || !io.read32(BdwDellBar0+0x9120,f) || !owned())return false;
                    selector=s;fuse=f;sampled=true;return true;
                }
                bool readMcr(uint32_t r,uint32_t& v){return r==0xb100 && owned() && io.read32(BdwDellBar0+r,v) && owned();}
                bool writeMcrBroadcast(uint32_t r,uint32_t v){return r==0xb100 && owned() && io.write32(BdwDellBar0+r,v) && owned();}
                bool delayAtLeastOneMicrosecond(){
                    uint64_t start=0,now=0;if(!owned() || !khz || !io.tsc(start))return false;
                    const uint64_t ticks=(uint64_t(khz)+999)/1000;
                    for(unsigned i=0;i<1000000;++i){if(!owned() || !io.tsc(now) || now<start)return false;
                        if(now-start>=ticks)return true;io.relax();}return false;
                }
        };
        if(renderResult) {
            if(!snapshot || (scratchIndex!=65544 && scratchIndex!=65573) || !scratchDma || !copyToken){
                renderResult->entryRefusal=BdwRenderRefusalArguments;return false;}
            const bool rebased=scratchIndex==65573;
            constexpr uint64_t pages[]={BdwDellBar0,BdwOtherEnginePages[0],BdwPrivatePatPage,BdwResetPage,
                BdwDellBar0+0xb000,0xf6880000ull,0xf6101000ull};
            held.observeTypes(pages,sizeof(pages)/8);
            {uint8_t index=0;
            for(auto page:pages){DellI2cCriticalEvidence e{};if(!held.evidence(page,e) || !BdwGttEvidenceValid(e)){held.clearTypes();
                renderResult->entryRefusal=BdwRenderRefusalPageEvidence;renderResult->entryDetail=index;return false;}++index;}}
            held.clearTypes();
            GtOwner gt{held,khz};
            struct RenderOwner {
                Io& io;GtOwner& gt;BdwRenderRunResult& result;uint32_t khz,line;uint64_t origin;uint32_t budget=4000000;
                bool patActive=false,gtReady=false,fault=false;
                bool firstCompletionPoll=true,firstRetirementPoll=true;
                unsigned gtInitCalls=0;
                bool owned(){return gt.owned();}
                bool quiescent(){
                    struct Reader {RenderOwner& o;bool owned(){return o.owned();}
                        bool read(uint32_t r,uint32_t& v){return o.io.read32(BdwDellBar0+r,v);}} reader{*this};
                    return owned() && BdwObserveEngine(reader,0x2000).submissionQuiescent;
                }
                bool allEnginesQuiescent(){return quiescent();} // others held idle under claim
                bool monotonicMs(uint64_t& ms){uint64_t t=0;if(!io.tsc(t) || t<origin)return false;ms=(t-origin)/khz;return true;}
                bool inTime(){uint64_t ms=0;return budget && --budget && monotonicMs(ms) && ms<2000;}
                BdwPollPacer completionPacer{},retirementPacer{};BdwPollPacer* pacer=nullptr;
                void beginPacer(BdwPollPacer& p,uint64_t expected,uint64_t spinBudget){
                    uint64_t now=0;io.tsc(now);
                    const uint64_t k=khz;
                    p.Begin({k,k/10,k/20,k/4},now,expected,result.pollSpinRequested?spinBudget:0);
                    pacer=&p;
                }
                bool pollContinue(){
                    if(!owned() || !inTime())return false;
                    if(result.pollYieldRequested){
                        uint64_t start=0;io.tsc(start);
                        const uint64_t slice=pacer?pacer->Next(start):0;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
                        const auto yieldStart=BdwTimingNow();
#endif
                        io.spinTicks=slice;io.waitEnd=0;
                        // A job is in flight: keep this CPU out of deep
                        // C-states for its completion polls (governor only).
                        CpuIdleLatencyHint(4000);
                        const bool yielded=BdwYieldGpuWait(io);
                        io.spinTicks=0;
                        if(!yielded){result.pollYieldFailed=true;fault=true;return false;}
#if defined(BROOK_GPU_SUBMISSION_TIMING)
                        const auto yieldEnd=BdwTimingNow();
                        const uint64_t spent=yieldEnd>yieldStart?yieldEnd-yieldStart:0;
                        if(slice)result.pollSpinTicks+=spent;else result.pollYieldTicks+=spent;
#endif
                        if(slice){
                            uint64_t end=0;io.tsc(end);
                            pacer->Spun(end>start?end-start:0,io.waitEnd && end>io.waitEnd?end-io.waitEnd:0);
                            ++result.pollSpins;
                        }else ++result.pollYields;
                    }
                    return owned() && inTime();
                }
                bool immediatePollAllowed(){return owned() && inTime();}
#if defined(BROOK_GPU_SUBMISSION_TIMING)
                void sampleRenderFrequency(){
                    const bool trace=result.sampleFrequency && result.frequencySamples!=result.FrequencySampleLimit;
                    if(!trace && !result.statsFrequency)return;
                    uint64_t now=0;
                    if(!io.tsc(now))return;
                    const auto count=result.frequencySamples;
                    const bool traceDue=trace && !(count && now-result.frequency[count-1].ticks<uint64_t{khz}*5);
                    if(!traceDue && !result.statsFrequency)return;
                    // The forcewake claim already audits the 0xa000 MMIO page.
                    uint32_t status=~uint32_t{0};
                    if(!io.read32(BdwDellBar0+0xa01c,status))return; // GEN6_RPSTAT1
                    if(result.statsFrequency){
                        if(!result.statsFrequencySamples++)result.statsIssueStatus=status;
                        result.statsEndStatus=status;
                    }
                    if(!traceDue)return;
                    result.frequency[count]={now,status};
                    ++result.frequencySamples;
                    if(!count){
                        io.read32(BdwDellBar0+0xa008,result.frequencyRequest); // GEN6_RPNSWREQ
                        io.read32(BdwDellBar0+0xa024,result.rpsControl); // GEN6_RP_CONTROL
                        io.read32(BdwDellBar0+0xa090,result.rc6Control); // GEN6_RC_CONTROL
                    }
                }
#endif
                bool completionContinue(){
                    if(firstCompletionPoll){
                        const uint64_t k=khz;
                        beginPacer(completionPacer,result.expectedCompletionTicks,
                            result.expectedCompletionTicks?2*k:k/4);
                        io.tsc(result.completionStartTicks);
                    }
                    pacer=&completionPacer;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
                    const auto yieldsBefore=result.pollYields;
                    const auto spinsBefore=result.pollSpins;
                    const bool more=BdwFirstGpuPoll(*this,firstCompletionPoll);
                    result.completionPollYields+=result.pollYields-yieldsBefore;
                    result.completionPollSpins+=result.pollSpins-spinsBefore;
                    if(!more)return false;
                    sampleRenderFrequency();
#else
                    if(!BdwFirstGpuPoll(*this,firstCompletionPoll))return false;
#endif
                    // The breadcrumb check follows; on success this is the
                    // observing poll, which feeds the completion predictor.
                    io.tsc(result.completionEndTicks);
                    return true;
                }
                bool retirementContinue(){
                    if(firstRetirementPoll)beginPacer(retirementPacer,0,khz/2);
                    pacer=&retirementPacer;
                    return BdwFirstGpuPoll(*this,firstRetirementPoll);
                }
                void relax(){io.relax();}
                bool renderStateReady(){return owned() && gtReady;}
                bool restoreRenderState(){
                    BdwRenderGtMmio<GtOwner> mmio(gt,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[0]),
                        reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage),reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
                    const auto r=BdwInitializeRenderGt(mmio);gtReady=r.status==BdwRenderGtInitStatus::Applied;
                    if(r.status==BdwRenderGtInitStatus::Uncertain)fault=true;
                    result.gtWrites+=r.writes;
                    // The transaction's first stage refused before writing
                    // anything: nothing else has run yet (see the refusal
                    // classifier). Later re-initialisations never count.
                    if(!gtInitCalls++ && r.status==BdwRenderGtInitStatus::NotStarted && !r.writes && !result.attempted){
                        result.entryRefusal=BdwRenderRefusalGtNotStarted;result.entryDetail=r.refusal;}
                    return gtReady;
                }
                bool gpuPatLease(){return patActive && owned();}
                bool preparePrivatePat(uint64_t descriptor){return gpuPatLease() && (descriptor&0x119)==0x119;}
                bool readPrivatePat(uint32_t& l,uint32_t& h){return owned() && io.read32(BdwDellBar0+0x40e0,l) && io.read32(BdwDellBar0+0x40e4,h);}
                bool safePatTransition(uint32_t l,uint32_t h,uint32_t nl,uint32_t nh){
                    // Inherited firmware policy, or the VCS lane base while a
                    // lane job is in flight (VA-API phase 4).
                    return owned() && ((l==0x03030303u && h==0x03030303u) || BdwVcsLanePatBase(l,h)) &&
                        nl==(result.uncachedScanout?(0x00030307u|(uint32_t(result.scanoutPat)<<24)):0x03030307u) && nh==0x0303030bu;
                }
                bool drainAndInvalidate(){
                    uint32_t l=0,h=0;if(!readPrivatePat(l,h) || l==0xffffffffu || h==0xffffffffu)return false;
                    BdwGen8TlbMmio<RenderOwner> mmio(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage));
                    BdwGen8TlbInvalidate invalidate;const bool ok=invalidate.Run(mmio);
                    if(invalidate.Uncertain())fault=true;return ok;
                }
                bool invalidateGpuTranslations(){return drainAndInvalidate();}
                bool resetRcs(BdwBcsResetIntent intent=BdwBcsResetIntent::IdleTakeover){
                    BdwRcsResetMmio<RenderOwner> reset(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[0]),
                        reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
                    const auto r=reset.reset(intent);gtReady=false;
                    if(r==BdwBcsResetResult::Uncertain)fault=true;
                    return r==BdwBcsResetResult::DomainAcknowledged;
                }
                bool admitPages(BdwOwnedPage* p,unsigned count){
                    if(!owned() || !p || count!=37)return false;
                    for(unsigned i=0;i<count;++i){const auto dma=p[i].dma;
                        if(!dma || (dma&4095) || dma>=(uint64_t{16}<<30) || p[i].cpu!=reinterpret_cast<uint8_t*>(DIRECT_MAP_BASE+dma))return false;
                        for(unsigned j=0;j<i;++j)if(dma==p[j].dma)return false;
                    }return true;
                }
                // PMM ownership and VT-d checks are performed by the caller
                // before taking the page-table lock; recheck direct-map
                // geometry here while the audited device lease is held.
                bool admitExtraTables(BdwOwnedPage* p,unsigned count){
                    if(!owned() || !p || !count || count>BdwPpgttMaxTables+4)return false;
                    for(unsigned i=0;i<count;++i){const auto dma=p[i].dma;
                        if(!dma || (dma&4095) || dma>=(uint64_t{16}<<30) || p[i].cpu!=reinterpret_cast<uint8_t*>(DIRECT_MAP_BASE+dma))return false;
                    }return true;
                }
                bool flushPage(const BdwOwnedPage& p){
                    if(!owned() || !p.cpu || !line || line>4096 || 4096%line)return false;
                    __asm__ volatile("mfence":::"memory");
                    for(uint32_t off=0;off<4096;off+=line)__asm__ volatile("clflush (%0)"::"r"(p.cpu+off):"memory");
                    __asm__ volatile("mfence":::"memory");return owned();
                }
                bool syncForCpu(const BdwOwnedPage& p){return flushPage(p);}
                bool syncForDevice(const BdwOwnedPage& p){return flushPage(p);}
                bool publish(BdwOwnedPage* p,unsigned count){if(!owned() || !p || count!=37)return false;
                    for(unsigned i=0;i<count;++i)if(!flushPage(p[i]))return false;return true;}
                bool publishExtraTables(BdwOwnedPage* p,unsigned count){if(!owned() || !p || !count || count>BdwPpgttMaxTables+4)return false;
                    for(unsigned i=0;i<count;++i)if(!flushPage(p[i]))return false;return true;}
                bool validateLease(const BdwGgttLease&){return owned();}
                bool validateTakeover(const BdwGgttLease&,uint64_t){return owned();}
                bool acknowledgeCsb(unsigned slot){uint32_t posted=0;return slot<6 && owned() &&
                    io.write32(BdwDellBar0+0x23a0,0x07000000u|(slot<<8)) && io.read32(BdwDellBar0+0x23a0,posted) &&
                    posted!=0xffffffffu && ((posted>>8)&7u)==slot && owned();}
            } owner{held,gt,*renderResult,khz,0,0};
            uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
            owner.line=((b>>8)&255)*8;
            if(!(d&(1u<<19)) || owner.line<16 || owner.line>4096 || (owner.line&(owner.line-1)) ||
               4096%owner.line || !held.tsc(owner.origin))return false;
            BdwRenderDeviceBackend<RenderOwner> backend(owner,reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6880000ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6101008ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[0]),scratchIndex,copyToken,scratchDma,rebased?2:1);
            struct Transaction {
                RenderOwner& o;BdwRenderDeviceBackend<RenderOwner>& backend;BdwRenderRunResult& result;
                uint64_t scratch;uint32_t (&snapshot)[20*1024];bool rebased;BdwPatLease lease;
                BdwPatMmio<RenderOwner> pat;
                Transaction(RenderOwner& owner,BdwRenderDeviceBackend<RenderOwner>& b,BdwRenderRunResult& r,uint64_t s,uint32_t (&snap)[20*1024],bool second)
                    :o(owner),backend(b),result(r),scratch(s),snapshot(snap),rebased(second),
                     pat(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage),r.uncachedScanout,r.scanoutPat){}
                bool initializeGt(){return BdwTimedTransaction(result,0,[&](){return o.restoreRenderState();});}
                bool establishPat(){return BdwTimedTransaction(result,1,[&](){const bool ok=lease.Establish(pat,0x119,result.uncachedScanout,result.scanoutPat);o.patActive=ok;
                    if(lease.State()==BdwPatLeaseState::Quarantined)o.fault=true;return ok;});}
#if defined(BROOK_BDW_RPS_TRIAL)
                bool restoreFrequency=false;
                uint32_t savedFrequency=0;
                void requestTrialFrequency(){
                    constexpr uint32_t requestReg=0xa008; // GEN6_RPNSWREQ
                    uint32_t trialBin=result.trialBin; // chosen by the worker (gpu clock), 50 MHz bins
                    constexpr uint32_t ratioMask=0x7f000000u;
                    if(!result.tryFrequency || !o.owned())return;
                    DellI2cCriticalEvidence evidence{};
                    if(!o.io.evidence(BdwRenderFrequencyCapsPage,evidence) || !BdwGttEvidenceValid(evidence))return;
                    if(!o.io.read32(BdwDellBar0+BdwRenderFrequencyCaps,result.trialFrequencyCaps))return;
                    const uint32_t caps=result.trialFrequencyCaps;
                    const uint32_t minBin=(caps>>16)&0xff;
                    const uint32_t maxBin=caps&0xff;
                    // Never request outside the fused [RPn, RP0] range.
                    if(trialBin>maxBin)trialBin=maxBin;
                    if(trialBin<minBin)trialBin=minBin;
                    result.trialBin=trialBin;
                    uint32_t control=0;
                    if(caps==0xffffffffu || !minBin || minBin>trialBin || maxBin<trialBin ||
                       !o.io.read32(BdwDellBar0+0xa024,control) || control==0xffffffffu || !(control&(1u<<7)) ||
                       !o.io.read32(BdwDellBar0+requestReg,savedFrequency) || savedFrequency==0xffffffffu ||
                       ((savedFrequency&ratioMask)>>24)<minBin || ((savedFrequency&ratioMask)>>24)>=trialBin)return;
                    const auto desired=(savedFrequency&~ratioMask)|(trialBin<<24);
                    // Restore the prior request even if the posting read does
                    // not confirm the trial write.
                    restoreFrequency=true;
                    result.trialFrequencyOriginal=savedFrequency;
                    o.io.write32(BdwDellBar0+requestReg,desired);
                    o.io.read32(BdwDellBar0+requestReg,result.trialFrequencyPosted);
                }
                bool restoreTrialFrequency(){
                    if(!restoreFrequency)return true;
                    uint32_t posted=0;
                    return o.owned() && o.io.write32(BdwDellBar0+0xa008,savedFrequency) &&
                        o.io.read32(BdwDellBar0+0xa008,posted) && posted==savedFrequency;
                }
#endif
                bool run(){return BdwTimedTransaction(result,2,[&](){
#if defined(BROOK_BDW_RPS_TRIAL)
                    requestTrialFrequency();
#endif
                    const bool ok=rebased?BdwRunRebasedRender(backend,scratch,result):BdwRunRetainedRender(backend,scratch,result);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
                    const auto retired=BdwTimingNow();uint64_t issued=0,breadcrumb=0;backend.timingTicks(issued,breadcrumb);
                    if(ok && issued && breadcrumb>issued && retired>breadcrumb){
                        result.issueTicks=issued;
                        result.breadcrumbTicks=breadcrumb;
                        result.contextRetiredTicks=retired;
                    }
#endif
                    result.engineSampled=o.owned() && o.io.read32(BdwDellBar0+0x2034,result.head) &&
                        o.io.read32(BdwDellBar0+0x2030,result.tail) && o.io.read32(BdwDellBar0+0x209c,result.mode) &&
                        o.io.read32(BdwDellBar0+0x203c,result.control) && o.io.read32(BdwDellBar0+0x2074,result.acthd) &&
                        o.io.read32(BdwDellBar0+0x2064,result.ipeir) && o.io.read32(BdwDellBar0+0x2068,result.ipehr) &&
                        o.io.read32(BdwDellBar0+0x2234,result.execlist);
                    if(result.engineSampled){(void)o.io.read32(BdwDellBar0+0x20b0,result.eir);(void)o.io.read32(BdwDellBar0+0x20b8,result.esr);}
                    return ok;});}
                bool saveContext(){return BdwTimedTransaction(result,3,[&](){return rebased?BdwCopyRebasedRenderContext(backend,snapshot):BdwCopyRetainedRenderContext(backend,snapshot);});}
                bool resetAfter(){return BdwTimedTransaction(result,4,[&](){
                    if(!o.resetRcs(BdwBcsResetIntent::StopOwnedWork))return false;
                    if(result.ggttClearRequested && result.contextComplete){
                        result.ggttCleared=BdwRetireRetainedRenderAfterReset(backend);
                        if(!result.ggttCleared){o.fault=true;return false;}
                    }else if(result.ggttClearRequested && result.attempted && !o.fault &&
                             BdwRetainedRenderRetirableAfterFailedRun()){
                        // gen 400: the run was submitted but did not complete
                        // cleanly (e.g. an unexpected context-status event).
                        // The verified reset above stopped the engine, so this
                        // run's binding is retired exactly as after success;
                        // the job then fails instead of retaining everything.
                        result.ggttCleared=BdwRetireRetainedRenderAfterReset(backend);
                        if(!result.ggttCleared){o.fault=true;return false;}
                        result.retiredAfterFailedRun=true;
                    }
                    return true;
                });}
                bool restorePat(){return BdwTimedTransaction(result,5,[&](){const bool ok=lease.Release(pat);o.patActive=false;return ok;});}
                bool uncertain(){return o.fault;}
            } transaction(owner,backend,*renderResult,scratchDma,*snapshot,rebased);
            renderResult->transaction=BdwRunOwnedRenderTransaction(transaction);
#if defined(BROOK_BDW_RPS_TRIAL)
            if(!transaction.restoreTrialFrequency())renderResult->transaction.uncertain=true;
#endif
            renderResult->ggttWriteAttempts=backend.ggttWriteAttempts();renderResult->portWriteAttempts=backend.portWriteAttempts();
            const auto& r=renderResult->transaction;
            return r.contextSaved && r.resetComplete && r.patRestored && !r.uncertain;
        }
        if(gtInit) {
            // Include all extra pages in the same locked UC-alias audit as
            // the forcewake/engine samples. No selector write is required.
            constexpr uint64_t pages[]={BdwDellBar0,BdwOtherEnginePages[0],BdwPrivatePatPage,BdwResetPage,BdwDellBar0+0xb000};
            for(auto page:pages){DellI2cCriticalEvidence e{};if(!held.evidence(page,e) || !BdwGttEvidenceValid(e))return false;}
            GtOwner owner{held,khz};
            BdwRenderGtMmio<GtOwner> mmio(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[0]),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage),reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
            *gtInit=BdwInitializeRenderGt(mmio);
            gtInit->selector=owner.selector;gtInit->fuse2=owner.fuse;gtInit->topologySampled=owner.sampled;
            return gtInit->status==BdwRenderGtInitStatus::Applied;
        }
        if(vcsPmResult) {
            // VCS power-management experiments (gen 391 clock-sag bisect). Only
            // engine-idle registers and the RPS block are written, always with
            // readback; no ring is started, no GGTT or PAT state changes.
            vcsPmResult->attempted=true;vcsPmOpWrites=vcsPmOpFailures=0;
            const auto op=BdwVcsPmOp(vcsPmOp);
            if(op!=BdwVcsPmOp::Sample){
                DellI2cCriticalEvidence v{};
                if(!held.evidence(BdwOtherEnginePages[1],v) || !BdwGttEvidenceValid(v)){vcsPmResult->attempted=false;return false;}
            }
            auto rd=[&](uint32_t reg,uint32_t& val){return held.read32(BdwDellBar0+reg,val) && val!=0xffffffffu;};
            auto wr=[&](uint32_t reg,uint32_t val,uint32_t expectMask,uint32_t expect){
                ++vcsPmOpWrites;uint32_t back=0;
                if(!held.write32(BdwDellBar0+reg,val) || !rd(reg,back) || (back&expectMask)!=expect){++vcsPmOpFailures;return false;}
                return true;};
            auto waitIdle=[&](bool wantIdle){
                uint64_t start=0,now=0;if(!held.tsc(start))return false;
                for(unsigned i=0;i<1000000;++i){uint32_t mi=0;if(!rd(0x1209c,mi) || !held.tsc(now))return false;
                    if(bool(mi&0x200)==wantIdle)return true;
                    if((now-start)/khz>=100)return false;held.relax();}
                return false;};
            bool ok=true;
            BdwGtPmSnapshot before{};(void)BdwVcsPmSnapshot(held,before);
            switch(op){
            case BdwVcsPmOp::Sample:break;
            case BdwVcsPmOp::StopStart:
                // STOP_RING (bit 8) set; the ring reports idle (bit 9); then clear.
                ok=wr(0x1209c,0x01000100u,0x100u,0x100u) && waitIdle(true) && wr(0x1209c,0x01000000u,0x100u,0);
                break;
            case BdwVcsPmOp::IdleRing:{
                uint32_t ctl=0;
                ok=rd(0x1203c,ctl) && !(ctl&1u) &&  // never touch an enabled ring
                   wr(0x1203c,0,~0u,0) && wr(0x12034,0,0x001ffffcu,0) && wr(0x12030,0,0x001ffff8u,0);
                break;}
            case BdwVcsPmOp::RpsRewrite:
                for(auto reg:BdwRpsRewriteRegs){const int i=BdwGtPmIndex(reg);
                    if(i<0 || !before.valid){ok=false;break;}
                    if(!wr(reg,before.value[i],~0u,before.value[i]))ok=false;}
                break;
            case BdwVcsPmOp::EngineRestore:{
                const auto& first=vcsPmRecord.first;
                if(!first.valid){ok=false;break;}
                for(const auto& r:BdwVcsEngineRestoreRegs){const int i=BdwGtPmIndex(r.reg);
                    if(i<0 || !before.valid){ok=false;break;}
                    if(before.value[i]==first.value[i])continue;
                    const uint32_t target=r.masked?(first.value[i]&0xffffu):first.value[i];
                    const uint32_t value=r.masked?(0xffff0000u|target):target;
                    if(!wr(r.reg,value,r.masked?0xffffu:~0u,target))ok=false;}
                break;}
            default:ok=false;break;
            }
            (void)BdwVcsPmSnapshot(held,vcsPmSample);
            vcsPmResult->complete=ok;
            // These registers do not hold engine, GGTT or PAT ownership: report the
            // outcome and keep the shared claim clean (an uncertain claim would
            // stop every later GPU job).
            return true;
        }
        if(vcsReset) {
            // VCS0 domain reset (VA-API phase 1). Same narrow owner as the RCS
            // test: forcewake is held and every engine, VCS included, was sampled
            // immediately before this callback by the adapter.
            static_assert(BdwOtherEnginePages[1]==BdwVcsEnginePage,"VCS page");
            DellI2cCriticalEvidence e{},v{};
            if(!held.evidence(BdwResetPage,e) || !BdwGttEvidenceValid(e) ||
               !held.evidence(BdwOtherEnginePages[1],v) || !BdwGttEvidenceValid(v))return false;
            struct VcsOwner {
                Io& io;uint32_t khz;uint64_t origin;
                bool owned(){uint32_t a=0;return io.owned() && io.read32(BdwDellBar0+0x130044,a) && a!=0xffffffffu && (a&1);}
                bool quiescent(){
                    struct Reader {VcsOwner& o;bool owned(){return o.owned();}
                        bool read(uint32_t r,uint32_t& v){return o.io.read32(BdwDellBar0+r,v);}} reader{*this};
                    return owned() && BdwObserveEngine(reader,BdwVcsBase).submissionQuiescent;
                }
                bool monotonicMs(uint64_t& ms){uint64_t ticks=0;if(!io.tsc(ticks) || ticks<origin)return false;ms=(ticks-origin)/khz;return true;}
                void relax(){io.relax();}
            } owner{held,khz,0};
            if(!held.tsc(owner.origin))return false;
            BdwVcsPmBegin(held);
            vcsReset->attempted=true;
            BdwVcsResetMmio<VcsOwner> reset(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[1]),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
            const auto outcome=reset.reset();
            vcsReset->complete=outcome==BdwBcsResetResult::DomainAcknowledged;
            vcsReset->uncertain=outcome==BdwBcsResetResult::Uncertain;
            if(vcsReset->complete)(void)BdwVcsRestoreFirmwareEngine(held);
            BdwVcsPmFinish(held,1);
            return vcsReset->complete;
        }
        if(rcsReset) {
            DellI2cCriticalEvidence e{};
            if(!held.evidence(BdwResetPage,e) || !BdwGttEvidenceValid(e))return false;
            struct RcsOwner {
                Io& io;uint32_t khz;uint64_t origin;
                bool owned(){uint32_t a=0;return io.owned() && io.read32(BdwDellBar0+0x130044,a) && a!=0xffffffffu && (a&1);}
                bool quiescent(){return owned();} // all engines sampled immediately before callback
                bool monotonicMs(uint64_t& ms){uint64_t ticks=0;if(!io.tsc(ticks) || ticks<origin)return false;ms=(ticks-origin)/khz;return true;}
                void relax(){io.relax();}
            } owner{held,khz,0};
            if(!held.tsc(owner.origin))return false;
            rcsReset->attempted=true;
            BdwRcsResetMmio<RcsOwner> reset(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[0]),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
            const auto outcome=reset.reset();
            rcsReset->complete=outcome==BdwBcsResetResult::DomainAcknowledged;
            rcsReset->uncertain=outcome==BdwBcsResetResult::Uncertain;
            return rcsReset->complete;
        }
        if(patResult) {
            struct PatOwner {
                Io& io;uint32_t khz;uint64_t origin;
                bool owned(){uint32_t ack=0;return io.owned() && io.read32(BdwDellBar0+0x130044,ack) && ack!=0xffffffffu && (ack&1);}
                bool allEnginesQuiescent(){return owned();} // sampled immediately before callback
                bool quiescent(){return owned();}
                bool monotonicMs(uint64_t& ms){uint64_t ticks=0;if(!io.tsc(ticks) || ticks<origin)return false;ms=(ticks-origin)/khz;return true;}
                void relax(){io.relax();}
                bool safePatTransition(uint32_t lo,uint32_t hi,uint32_t nextLo,uint32_t nextHi) {
                    return owned() && lo==0x03030303u && hi==0x03030303u && nextLo==0x03030307u && nextHi==0x0303030bu;
                }
                bool drainAndInvalidate() {
                    uint32_t lo=0,hi=0;
                    if(!owned() || !io.read32(BdwDellBar0+0x40e0,lo) || !io.read32(BdwDellBar0+0x40e4,hi) ||
                       lo==0xffffffffu || hi==0xffffffffu)return false;
                    BdwGen8TlbMmio<PatOwner> mmio(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage));
                    BdwGen8TlbInvalidate invalidate;return invalidate.Run(mmio);
                }
            } owner{held,khz,0};
            if(!held.tsc(owner.origin))return false;
            patResult->beforeLo=lo;patResult->beforeHi=hi;patResult->attempted=true;
            DellI2cCriticalEvidence resetEvidence{};
            if(!held.evidence(BdwResetPage,resetEvidence) || !BdwGttEvidenceValid(resetEvidence))return false;
            BdwBcsResetMmio<PatOwner> reset(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
            patResult->resetComplete=reset.reset()==BdwBcsResetResult::DomainAcknowledged;
            if(!patResult->resetComplete)return false;
            BdwPatMmio<PatOwner> mmio(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage));
            BdwPatLease lease;
            patResult->established=lease.Establish(mmio,0x119);
            if(!patResult->established)return false;
            const bool sampled=mmio.readPrivatePat(patResult->activeLo,patResult->activeHi);
            patResult->restored=lease.Release(mmio);
            const bool after=mmio.readPrivatePat(patResult->afterLo,patResult->afterHi);
            return sampled && after && patResult->restored && patResult->afterLo==lo && patResult->afterHi==hi;
        }
        if(!scratchResult && !initResult && !copyResult)return true;
        // The audited MMIO page contains the diagnostic's fixed slots and
        // the separate scanout reservation. Their PTE writes stay disjoint.
        if(!scratchDma || ((scanoutCopy || vcsCopy)?(!copyResult || scratchIndex<65536 || scratchIndex>65536+512-8):scratchIndex!=65536) ||
           (scanoutCopy && vcsCopy) || (vcsSelftest && vcsUser)){
            if(copyResult)copyResult->refusalStep=1;
            return false;}
        const uint64_t scratchPages[]={0xf6880000ull,(scratchResult || copyResult)?0xf6101000ull:BdwResetPage,BdwResetPage};
        for(uint64_t page:scratchPages) {
            DellI2cCriticalEvidence e{};if(!held.evidence(page,e) || !BdwGttEvidenceValid(e)){if(copyResult)copyResult->refusalStep=2;return false;}
        }
        if(vcsCopy){DellI2cCriticalEvidence e{};
            if(!held.evidence(BdwOtherEnginePages[1],e) || !BdwGttEvidenceValid(e)){copyResult->refusalStep=6;return false;}}
        if(copyResult) {
            if(!copyToken){copyResult->refusalStep=3;return false;}
            struct CopyOwner {
                Io& io;uint32_t khz,line;uint64_t origin;uint32_t budget=4000000;
                bool patActive=false,scanout=false;
                uint32_t base=0x22000; // 0x12000 for the VCS0 self-test (VA-API phase 1b)
                bool resetWasUncertain=false;
                bool owned(){uint32_t a=0;return io.owned() && io.read32(BdwDellBar0+0x130044,a) && a!=0xffffffffu && (a&1);}
                bool allEnginesQuiescent(){return owned();} // sampled immediately before callback
                bool monotonicMs(uint64_t& ms){uint64_t ticks=0;if(!io.tsc(ticks) || ticks<origin)return false;ms=(ticks-origin)/khz;return true;}
                bool inTime(){uint64_t ms=0;return budget && --budget && monotonicMs(ms) && ms<2000;}
                bool completionContinue(){return owned() && inTime();}
                bool retirementContinue(){return owned() && inTime();}
                void relax(){io.relax();}
                bool gpuPatLease(){return patActive && owned();}
                bool preparePrivatePat(uint64_t descriptor){return gpuPatLease() && (descriptor&0x119)==0x119;}
                bool readPrivatePat(uint32_t& lo,uint32_t& hi){return owned() && io.read32(BdwDellBar0+0x40e0,lo) && io.read32(BdwDellBar0+0x40e4,hi);}
                bool safePatTransition(uint32_t lo,uint32_t hi,uint32_t nlo,uint32_t nhi){
                    return owned() && ((lo==0x03030303u && hi==0x03030303u) || BdwVcsLanePatBase(lo,hi)) &&
                        nlo==(scanout?0x00030307u:0x03030307u) && nhi==0x0303030bu;
                }
                bool drainAndInvalidate(){
                    uint32_t lo=0,hi=0;
                    if(!readPrivatePat(lo,hi) || lo==0xffffffffu || hi==0xffffffffu)return false;
                    BdwGen8TlbMmio<CopyOwner> mmio(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage));
                    BdwGen8TlbInvalidate invalidate;return invalidate.Run(mmio);
                }
                bool invalidateGpuTranslations(){return drainAndInvalidate();}
                bool resetBcs(BdwBcsResetIntent intent=BdwBcsResetIntent::IdleTakeover){
                    if(base==BdwVcsBase){
                        BdwVcsResetMmio<CopyOwner> reset(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwVcsEnginePage),
                            reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
                        const auto outcome=reset.reset(intent);
                        if(outcome==BdwBcsResetResult::Uncertain)resetWasUncertain=true;
                        if(outcome==BdwBcsResetResult::DomainAcknowledged)(void)BdwVcsRestoreFirmwareEngine(io);
                        return outcome==BdwBcsResetResult::DomainAcknowledged;
                    }
                    BdwBcsResetMmio<CopyOwner> reset(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),
                        reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
                    return reset.reset(intent)==BdwBcsResetResult::DomainAcknowledged;
                }
                bool quiescent(){
                    struct Reader {CopyOwner& o;bool owned(){return o.owned();}
                        bool read(uint32_t r,uint32_t& v){return o.io.read32(BdwDellBar0+r,v);}} reader{*this};
                    return owned() && BdwObserveEngine(reader,base).submissionQuiescent;
                }
                bool admitPages(BdwOwnedPage* pages,unsigned count){
                    if(!owned() || !pages ||
                       (scanout?(count<=BdwBcsLegacyPrivatePages || count>BdwBcsMaxPrivatePages):count!=BdwBcsLegacyPrivatePages))
                        return false;
                    for(unsigned i=0;i<count;++i) {
                        const auto dma=pages[i].dma;
                        if(!dma || (dma&4095) || dma>=(uint64_t{16}<<30) ||
                           pages[i].cpu!=reinterpret_cast<uint8_t*>(DIRECT_MAP_BASE+dma))return false;
                        for(unsigned j=0;j<i;++j)if(dma==pages[j].dma)return false;
                    }
                    return true;
                }
                bool flushPage(const BdwOwnedPage& p){
                    if(!owned() || !p.cpu || !line || line>4096 || (4096%line))return false;
                    __asm__ volatile("mfence":::"memory");
                    for(uint32_t off=0;off<4096;off+=line)__asm__ volatile("clflush (%0)"::"r"(p.cpu+off):"memory");
                    __asm__ volatile("mfence":::"memory");return owned();
                }
                bool syncForCpu(const BdwOwnedPage& p){return flushPage(p);}
                bool syncForDevice(const BdwOwnedPage& p){return flushPage(p);}
                bool publish(BdwOwnedPage* pages,unsigned count){
                    if(!owned() || !pages || !count || count>BdwBcsMaxPrivatePages)return false;
                    for(unsigned i=0;i<count;++i)if(!flushPage(pages[i]))return false;
                    return true;
                }
                bool validateLease(const BdwGgttLease&){return owned();}
                bool validateTakeover(const BdwGgttLease&,uint64_t){return owned();}
                bool acknowledgeCsb(unsigned slot){
                    if(!owned() || slot>=6)return false;
                    uint32_t posted=0;
                    return io.write32(BdwDellBar0+base+0x3a0,0x07000000u|(slot<<8)) &&
                        io.read32(BdwDellBar0+base+0x3a0,posted) && posted!=0xffffffffu && ((posted>>8)&7u)==slot && owned();
                }
            } owner{held,khz,0,0};
            owner.scanout=scanoutCopy;
            if(vcsCopy)owner.base=BdwVcsBase;
            uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
            owner.line=((b>>8)&255)*8;
            if(!(d&(1u<<19)) || owner.line<16 || owner.line>4096 || (owner.line&(owner.line-1)) ||
               4096%owner.line || !held.tsc(owner.origin)){copyResult->refusalStep=4;return false;}
#if defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
            // ---- VA-API phase 4: the VCS lane (async user jobs) ----
            // One persistent backend gives the one-shot and its GGTT bind a
            // stable owner identity across the submit and complete critical
            // sections; LaneOwner forwards to this section's CopyOwner.
            struct LaneOwner {
                CopyOwner* o=nullptr;
                bool owned(){return o && o->owned();}
                bool gpuPatLease(){return o && o->gpuPatLease();}
                bool preparePrivatePat(uint64_t d){return o && o->preparePrivatePat(d);}
                bool readPrivatePat(uint32_t& lo,uint32_t& hi){return o && o->readPrivatePat(lo,hi);}
                bool admitPages(BdwOwnedPage* p,unsigned n){return o && o->admitPages(p,n);}
                bool publish(BdwOwnedPage* p,unsigned n){return o && o->publish(p,n);}
                bool validateLease(const BdwGgttLease& l){return o && o->validateLease(l);}
                bool validateTakeover(const BdwGgttLease& l,uint64_t sc){return o && o->validateTakeover(l,sc);}
                bool quiescent(){return o && o->quiescent();}
                bool resetBcs(BdwBcsResetIntent intent=BdwBcsResetIntent::IdleTakeover){return o && o->resetBcs(intent);}
                bool syncForCpu(const BdwOwnedPage& p){return o && o->syncForCpu(p);}
                bool syncForDevice(const BdwOwnedPage& p){return o && o->syncForDevice(p);}
                bool acknowledgeCsb(unsigned slot){return o && o->acknowledgeCsb(slot);}
                bool completionContinue(){return o && o->completionContinue();}
                bool retirementContinue(){return o && o->retirementContinue();}
                bool invalidateGpuTranslations(){return o && o->invalidateGpuTranslations();}
                void relax(){if(o)o->relax();}
            };
            static LaneOwner lane;
            struct LaneScope {LaneOwner& l;LaneScope(LaneOwner& lane_,CopyOwner& owner_):l(lane_){l.o=&owner_;}~LaneScope(){l.o=nullptr;}};
            // Constructed once on first lane use (the VCS user reservation is
            // fixed for the boot); no static guard is available in the kernel.
            alignas(BdwVcsDeviceBackend<LaneOwner>) static unsigned char laneStorage[sizeof(BdwVcsDeviceBackend<LaneOwner>)];
            static bool laneBuilt=false;
            if(vcsUser && vcsUserPhase && !laneBuilt){
                new(laneStorage) BdwVcsDeviceBackend<LaneOwner>(lane,reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6880000ull),
                    reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6101008ull),
                    reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwVcsEnginePage),scratchIndex,copyToken,scratchDma);
                laneBuilt=true;
            }
            auto& laneVcs=*reinterpret_cast<BdwVcsDeviceBackend<LaneOwner>*>(laneStorage);
            struct LaneSink final:BdwVcsTableSink {
                CopyOwner& o;explicit LaneSink(CopyOwner& owner_):o(owner_){}
                bool admitExtraTables(BdwOwnedPage* p,unsigned count)override{
                    if(!o.owned() || !p || !count || count>BdwPpgttMaxTables+4)return false;
                    for(unsigned i=0;i<count;++i){const auto dma=p[i].dma;
                        if(!dma || (dma&4095) || dma>=(uint64_t{16}<<30) || p[i].cpu!=reinterpret_cast<uint8_t*>(DIRECT_MAP_BASE+dma))return false;
                        for(unsigned j=0;j<i;++j)if(p[j].dma==dma)return false;
                    }
                    return true;
                }
                bool publishExtraTables(BdwOwnedPage* p,unsigned count)override{
                    if(!o.owned() || !p || !count || count>BdwPpgttMaxTables+4)return false;
                    for(unsigned i=0;i<count;++i)if(!o.flushPage(p[i]))return false;
                    return true;
                }
            };
            if(vcsUser && vcsUserPhase==2){
                // Complete: the lane job was submitted earlier and VCS may still
                // be running it. No reset-before and no new PAT lease: adopt
                // the held one, verify the breadcrumb and CSB, stop, unbind,
                // restore the inherited PAT, then clear the lane.
                if(!BdwVcsLaneBusy() || scratchIndex!=laneHeld.scratchIndex || copyToken!=laneHeld.token || scratchDma!=laneHeld.scratchDma){
                    copyResult->refusalStep=7;return false;}
                LaneScope scope(lane,owner);
                copyResult->stamps[BdwVcsStampWorkStart]=__builtin_ia32_rdtsc();
                copyResult->resetBeforeAttempted=false;copyResult->resetBefore=true;copyResult->attempted=true;
                BdwPatMmio<CopyOwner> patMmio(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage),false);
                BdwPatLease pat;copyResult->patEstablished=pat.AdoptHeld(patMmio,laneHeld.inheritedLo,laneHeld.inheritedHi,laneHeld.lo,laneHeld.hi);
                if(!copyResult->patEstablished){
                    // The lane's PAT changed underneath it: state unknown.
                    copyResult->patUncertain=true;copyResult->resetUncertain=true;return false;
                }
                owner.patActive=true;
                const bool ran=BdwCompleteSubmittedVcsUser(laneVcs,*copyResult);
                copyResult->ggttWriteAttempts=laneVcs.ggttWriteAttempts();copyResult->portWriteAttempts=4;
                copyResult->engineSampled=held.read32(BdwDellBar0+BdwVcsBase+0x34,copyResult->head) &&
                    held.read32(BdwDellBar0+BdwVcsBase+0x30,copyResult->tail) && held.read32(BdwDellBar0+BdwVcsBase+0x74,copyResult->acthd) &&
                    held.read32(BdwDellBar0+BdwVcsBase+0x68,copyResult->ipehr) && held.read32(BdwDellBar0+BdwVcsBase+0x234,copyResult->execlist);
                const bool cleanlyIdle=(BdwVideoUserTuning()&BdwVcsTuneMinimalResets) && ran && copyResult->contextComplete && owner.quiescent();
                copyResult->resetAfterSkipped=cleanlyIdle;
                copyResult->resetAfter=cleanlyIdle || owner.resetBcs(BdwBcsResetIntent::StopOwnedWork);
                if(copyResult->resetAfter)__atomic_store_n(&BdwVcsLaneBusyStorage(),0u,__ATOMIC_RELEASE);
                if(copyResult->resetAfter)copyResult->scanoutRetired=BdwRetirePreparedVcsUser(laneVcs);
                if(copyResult->resetAfter)copyResult->patRestored=pat.Release(patMmio);
                copyResult->patUncertain=!copyResult->resetAfter || !copyResult->patRestored || !copyResult->scanoutRetired;
                copyResult->resetUncertain=owner.resetWasUncertain;
                owner.patActive=false;
                copyResult->stamps[BdwVcsStampWorkEnd]=__builtin_ia32_rdtsc();
                vcsCleanStop=ran && copyResult->resetAfter && copyResult->scanoutRetired && copyResult->patRestored && !copyResult->resetUncertain;
                return !copyResult->patUncertain && !copyResult->resetUncertain;
            }
#endif
            if(vcsCopy && !(vcsUser && vcsUserPhase==1))BdwVcsPmBegin(held);
            if(vcsCopy)copyResult->stamps[BdwVcsStampWorkStart]=__builtin_ia32_rdtsc();
            // Phase 4: a user job may skip this GDRST when the previous VCS
            // user job ended in a verified clean stop and VCS is observed
            // quiescent now; the execlist init below still resets it.
            const bool minimalResets=vcsUser && (BdwVideoUserTuning()&BdwVcsTuneMinimalResets);
            if(minimalResets && vcsCleanStop && owner.quiescent()){
                copyResult->resetBeforeAttempted=false;copyResult->resetBefore=true;
            }else{
                copyResult->resetBeforeAttempted=true;copyResult->resetBefore=owner.resetBcs();
            }
            vcsCleanStop=false;
            copyResult->resetUncertain=owner.resetWasUncertain;
            if(vcsCopy)copyResult->stamps[BdwVcsStampResetBefore]=__builtin_ia32_rdtsc();
            if(!copyResult->resetBefore){copyResult->refusalStep=5;if(vcsCopy && !(vcsUser && vcsUserPhase==1))BdwVcsPmFinish(held,2);return false;}
            BdwPatMmio<CopyOwner> patMmio(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage),scanoutCopy);
            BdwPatLease pat;copyResult->patEstablished=pat.Establish(patMmio,0x119,scanoutCopy);
            if(!copyResult->patEstablished){copyResult->patUncertain=pat.State()==BdwPatLeaseState::Quarantined;return false;}
            owner.patActive=true;
            if(vcsCopy)copyResult->stamps[BdwVcsStampPat]=__builtin_ia32_rdtsc();
#if defined(BROOK_BDW_VIDEO_ENGINE) && defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
            if(vcsCopy){
                // Same takeover, PAT lease, execlist and CSB flow as the BCS
                // one-shot, on VCS0. The GGTT PTE window and flush register are
                // the one-shot ones; the engine page is BAR0+0x12000.
                BdwVcsDeviceBackend<CopyOwner> vcs(owner,reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6880000ull),
                    reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6101008ull),
                    reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwVcsEnginePage),scratchIndex,copyToken,scratchDma);
                // User jobs (phase 1c): the external table pages are checked
                // and flushed under this lease before the first engine write.
                struct TableSink final:BdwVcsTableSink {
                    CopyOwner& o;explicit TableSink(CopyOwner& owner):o(owner){}
                    bool admitExtraTables(BdwOwnedPage* p,unsigned count)override{
                        if(!o.owned() || !p || !count || count>BdwPpgttMaxTables+4)return false;
                        for(unsigned i=0;i<count;++i){const auto dma=p[i].dma;
                            if(!dma || (dma&4095) || dma>=(uint64_t{16}<<30) || p[i].cpu!=reinterpret_cast<uint8_t*>(DIRECT_MAP_BASE+dma))return false;
                            for(unsigned j=0;j<i;++j)if(p[j].dma==dma)return false;
                        }
                        return true;
                    }
                    bool publishExtraTables(BdwOwnedPage* p,unsigned count)override{
                        if(!o.owned() || !p || !count || count>BdwPpgttMaxTables+4)return false;
                        for(unsigned i=0;i<count;++i)if(!o.flushPage(p[i]))return false;
                        return true;
                    }
                } sink{owner};
                if(vcsUser && vcsUserPhase==1){
                    // Submit: same preparation, PAT lease and takeover as the
                    // synchronous run, but return with the job running on VCS.
                    // The PAT lease stays active as the lane base; the complete
                    // phase adopts it. Any failure takes the normal stop path.
                    LaneScope scope(lane,owner);
                    laneVcs.ResetForJob();
                    LaneSink laneSink(owner);
                    const bool submitted=BdwSubmitPreparedVcsUser(laneVcs,laneSink,scratchDma,*copyResult);
                    copyResult->ggttWriteAttempts=laneVcs.ggttWriteAttempts();copyResult->portWriteAttempts=laneVcs.portWriteAttempts();
                    copyResult->stamps[BdwVcsStampInitStart]=laneVcs.initStartTicks();copyResult->stamps[BdwVcsStampInitEnd]=laneVcs.initEndTicks();
                    uint32_t heldLo=0,heldHi=0;
                    if(submitted && patMmio.readPrivatePat(heldLo,heldHi) && heldLo==BdwVcsLaneBaseLo && heldHi==BdwVcsLaneBaseHi){
                        laneHeld={pat.InheritedLo(),pat.InheritedHi(),heldLo,heldHi,scratchIndex,copyToken,scratchDma};
                        __atomic_store_n(&BdwVcsLaneBusyStorage(),1u,__ATOMIC_RELEASE);
                        copyResult->laneSubmitted=true;owner.patActive=false;
                        copyResult->stamps[BdwVcsStampWorkEnd]=__builtin_ia32_rdtsc();
                        return true;
                    }
                    // Not submitted (or an unexpected PAT base): stop and unbind.
                    copyResult->resetAfter=owner.resetBcs(BdwBcsResetIntent::StopOwnedWork);
                    if(copyResult->resetAfter)copyResult->scanoutRetired=BdwRetirePreparedVcsUser(laneVcs);
                    if(copyResult->resetAfter)copyResult->patRestored=pat.Release(patMmio);
                    copyResult->patUncertain=!copyResult->resetAfter || !copyResult->patRestored || !copyResult->scanoutRetired;
                    copyResult->resetUncertain=owner.resetWasUncertain;
                    owner.patActive=false;
                    copyResult->stamps[BdwVcsStampWorkEnd]=__builtin_ia32_rdtsc();
                    return !copyResult->patUncertain && !copyResult->resetUncertain;
                }
                const bool ran=vcsUser?BdwRunPreparedVcsUser(vcs,sink,scratchDma,*copyResult):
                    BdwRunPreparedVcsSelftest(vcs,scratchDma,*copyResult);
                copyResult->ggttWriteAttempts=vcs.ggttWriteAttempts();copyResult->portWriteAttempts=vcs.portWriteAttempts();
                copyResult->engineSampled=held.read32(BdwDellBar0+BdwVcsBase+0x34,copyResult->head) &&
                    held.read32(BdwDellBar0+BdwVcsBase+0x30,copyResult->tail) && held.read32(BdwDellBar0+BdwVcsBase+0x9c,copyResult->mode) &&
                    held.read32(BdwDellBar0+BdwVcsBase+0x3c,copyResult->control) && held.read32(BdwDellBar0+BdwVcsBase+0x74,copyResult->acthd) &&
                    held.read32(BdwDellBar0+BdwVcsBase+0x64,copyResult->ipeir) && held.read32(BdwDellBar0+BdwVcsBase+0x68,copyResult->ipehr) &&
                    held.read32(BdwDellBar0+BdwVcsBase+0x234,copyResult->execlist);
                copyResult->stamps[BdwVcsStampInitStart]=vcs.initStartTicks();copyResult->stamps[BdwVcsStampInitEnd]=vcs.initEndTicks();
                copyResult->stamps[BdwVcsStampWaitStart]=vcs.waitStartTicks();copyResult->stamps[BdwVcsStampBreadcrumb]=vcs.waitEndTicks();
                // Stop the owned engine whatever happened, then unbind. Phase 4:
                // after a CSB context-complete (the context switched out and
                // the engine went idle) on a quiescent engine, that is the
                // verified stop; GDRST only for anything else.
                const bool cleanlyIdle=minimalResets && ran && copyResult->contextComplete && owner.quiescent();
                copyResult->resetAfterSkipped=cleanlyIdle;
                copyResult->resetAfter=cleanlyIdle || owner.resetBcs(BdwBcsResetIntent::StopOwnedWork);
                if(copyResult->resetAfter)copyResult->scanoutRetired=vcsUser?BdwRetirePreparedVcsUser(vcs):BdwRetirePreparedVcsSelftest(vcs);
                if(copyResult->resetAfter)copyResult->patRestored=pat.Release(patMmio);
                copyResult->patUncertain=!copyResult->resetAfter || !copyResult->patRestored || !copyResult->scanoutRetired;
                copyResult->resetUncertain=owner.resetWasUncertain;
                owner.patActive=false;
                // After the PAT lease is released: compare GT PM state with the
                // pre-test snapshot and restore any changed plain PM control.
                BdwVcsPmFinish(held,2);
                copyResult->stamps[BdwVcsStampWorkEnd]=__builtin_ia32_rdtsc();
                vcsCleanStop=vcsUser && ran && copyResult->resetAfter && copyResult->scanoutRetired &&
                    copyResult->patRestored && !copyResult->resetUncertain;
                // The callback's result is the transaction's cleanliness: VCS was
                // stopped, the PAT restored and the lease unbound. Whether the
                // self-test itself passed is reported through copyResult (ran ->
                // copyComplete/contextComplete), so a failed but cleanly
                // recovered self-test does not leave the shared claim uncertain.
                (void)ran;
                return !copyResult->patUncertain && !copyResult->resetUncertain;
            }
#else
            if(vcsCopy)return false;
#endif
            BdwBcsDeviceBackend<CopyOwner> backend(owner,reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6880000ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6101008ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),scratchIndex,copyToken,scratchDma);
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
#if defined(BROOK_GPU_SUBMISSION_TIMING)
            const auto runStart=BdwTimingNow();
#endif
            const bool copied=scanoutCopy?BdwRunPreparedBcsScanout(backend,scratchDma,*copyResult):
                BdwRunRetainedBcs(backend,scratchDma,*copyResult);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
            const auto runEnd=BdwTimingNow();if(runEnd>runStart)copyResult->runTicks=runEnd-runStart;
#endif
#else
            const bool copied=BdwRunRetainedBcs(backend,scratchDma,*copyResult);
#endif
            copyResult->ggttWriteAttempts=backend.ggttWriteAttempts();copyResult->portWriteAttempts=backend.portWriteAttempts();
            copyResult->engineSampled=held.read32(BdwDellBar0+0x22034,copyResult->head) &&
                held.read32(BdwDellBar0+0x22030,copyResult->tail) && held.read32(BdwDellBar0+0x2209c,copyResult->mode) &&
                held.read32(BdwDellBar0+0x2203c,copyResult->control) && held.read32(BdwDellBar0+0x22074,copyResult->acthd) &&
                held.read32(BdwDellBar0+0x22064,copyResult->ipeir) && held.read32(BdwDellBar0+0x22068,copyResult->ipehr) &&
                held.read32(BdwDellBar0+0x22234,copyResult->execlist);
            // A failed submission may still have reached the engine. Reset it
            // before changing the policy back; on uncertainty retain all pages.
            copyResult->resetAfter=owner.resetBcs(BdwBcsResetIntent::StopOwnedWork);
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
            if(scanoutCopy && copyResult->resetAfter)
                copyResult->scanoutRetired=BdwRetirePreparedBcsScanout(backend);
#endif
            if(copyResult->resetAfter)copyResult->patRestored=pat.Release(patMmio);
            copyResult->patUncertain=!copyResult->resetAfter || !copyResult->patRestored ||
                (scanoutCopy && !copyResult->scanoutRetired);
            owner.patActive=false;
            return copied && !copyResult->patUncertain;
        }
        struct Owner {
            Io& io;uint32_t khz;uint64_t origin;uint8_t* cpu;bool resetDone=false;uint32_t cacheLine=0;
            bool owned(){uint32_t a=0;return io.owned() && io.read32(BdwDellBar0+0x130044,a) && a!=0xffffffffu && (a&1);}
            bool allEnginesQuiescent(){return owned();} // checked immediately before callback
            bool quiescent(){return owned();}
            bool monotonicMs(uint64_t& ms){uint64_t ticks=0;if(!io.tsc(ticks) || ticks<origin)return false;ms=(ticks-origin)/khz;return true;}
            void relax(){io.relax();}
            bool resetVerified(){return resetDone && owned();}
            bool csbPrepared(uint64_t hws){return cpu && hws==0x10000000ull && resetVerified();}
            bool flushPage(){
                if(!owned() || !cpu || !cacheLine || cacheLine>4096 || (4096%cacheLine))return false;
                __asm__ volatile("mfence":::"memory");
                for(uint32_t off=0;off<4096;off+=cacheLine)__asm__ volatile("clflush (%0)"::"r"(cpu+off):"memory");
                __asm__ volatile("mfence":::"memory");return owned();
            }
            bool prepareCsbMemory(uint64_t hws){return csbPrepared(hws) && BdwCsbInitializeForReset(cpu,[&](uint8_t*){return flushPage();});}
            bool csbMemoryProducer(uint64_t hws,uint32_t& value){return csbPrepared(hws) && flushPage() && BdwCsbReadProducer(cpu,value);}
            bool invalidateGpuTranslations(){
                BdwGen8TlbMmio<Owner> mmio(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage));
                BdwGen8TlbInvalidate invalidator;return invalidator.Run(mmio);
            }
        } owner{held,khz,0,scratchCpu};
        if(!held.tsc(owner.origin))return false;
        if(initResult) {
            if(!scratchCpu || reinterpret_cast<uintptr_t>(scratchCpu)!=DIRECT_MAP_BASE+scratchDma)return false;
            __asm__ volatile("lfence":::"memory");
            const uint64_t mapped=*reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6880000ull);
            __asm__ volatile("lfence":::"memory");
            if(mapped!=(scratchDma|1))return false;
            uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
            owner.cacheLine=((b>>8)&255)*8;
            if(!(d&(1u<<19)) || owner.cacheLine<16 || owner.cacheLine>4096 ||
               (owner.cacheLine&(owner.cacheLine-1)) || 4096%owner.cacheLine)return false;
            initResult->attempted=true;
            BdwBcsResetMmio<Owner> reset(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
            owner.resetDone=reset.reset()==BdwBcsResetResult::DomainAcknowledged;
            initResult->resetComplete=owner.resetDone;
            if(!owner.resetDone)return false;
            BdwBcsMmio<Owner> bcs(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull));
            initResult->initComplete=bcs.initializeExeclists(0x10000000ull);
            return initResult->initComplete;
        }
        BdwScratchMmio<Owner> mmio(owner,reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6880000ull),
            reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6101008ull),scratchIndex,clearScratch?0:scratchDma);
        if(clearScratch)*scratchResult=BdwClearKnownScratchSlot(mmio,scratchIndex,scratchDma);
        else {
            BdwBcsResetMmio<Owner> reset(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
            auto resetBcs=[&](){return reset.reset()==BdwBcsResetResult::DomainAcknowledged;};
            *scratchResult=BdwResetAndTakeoverScratchSlot(mmio,scratchIndex,scratchDma,resetBcs);
        }
        return scratchResult->complete;
    };
    result=BdwDellForcewakeCriticalWithWork(io,khz,work,scopeCheck || scratchResult || initResult || patResult || copyResult || rcsReset || gtInit || renderResult || vcsReset || vcsPmResult,otherEngines,patRequested,tlbRequested,resetRequested);
    // A render callback that refused before its first device write (entry
    // checks, or GT init NotStarted with no writes) leaves nothing to recover;
    // the worker reruns the still-armed job. Anything later stays uncertain.
    if(renderResult && result.sequence.workAttempted && !result.sequence.workComplete &&
       renderResult->entryRefusal!=BdwRenderRefusalNone && !renderResult->attempted && !renderResult->gtWrites &&
       !renderResult->transaction.patReady && !renderResult->transaction.resetAttempted && !renderResult->pollYieldFailed)
        result.workRefusedUntouched=true;
    // A failed run that was reset (verified), had its binding retired and its
    // PAT restored leaves no device state to recover either.
    if(renderResult && result.sequence.workAttempted && !result.sequence.workComplete && renderResult->retiredAfterFailedRun){
        const auto& t=renderResult->transaction;
        if(t.resetComplete && t.patRestored && !t.uncertain && renderResult->ggttCleared && !renderResult->pollYieldFailed)
            result.workRefusedUntouched=true;
    }
    // VCS (VA-API phase 1) work that refused before any engine write leaves
    // nothing to recover: a VCS reset that reported NotStarted (or failed
    // evidence) with no Uncertain outcome, or a self-test that stopped at a
    // precondition or at a NotStarted reset-before. Never mark the claim
    // uncertain for those, or every later GPU job would refuse.
    if(vcsReset && result.sequence.workAttempted && !result.sequence.workComplete && !vcsReset->uncertain && !vcsReset->complete)
        result.workRefusedUntouched=true;
    // The PM experiment callback returns false only when it refused at its page
    // evidence check, before any write.
    if(vcsPmResult && result.sequence.workAttempted && !result.sequence.workComplete && !vcsPmResult->attempted)
        result.workRefusedUntouched=true;
    if(vcsCopy && copyResult && result.sequence.workAttempted && !result.sequence.workComplete &&
       copyResult->refusalStep && !copyResult->resetUncertain && !copyResult->portWriteAttempts && !copyResult->ggttWriteAttempts &&
       !copyResult->patEstablished)
        result.workRefusedUntouched=true;
    VmmKernelPtCriticalExit(irq);
    claim.finish(result);
}
void VmmDellForcewakeCriticalRun(uint32_t khz,BdwForcewakeAdapterResult& result,bool otherEngines,bool patRequested,bool tlbRequested,bool resetRequested,bool scopeCheck) {
    VmmDellForcewakeCriticalImpl(khz,result,otherEngines,patRequested,tlbRequested,resetRequested,scopeCheck,0,0,nullptr,nullptr,nullptr,false,nullptr,0,nullptr);
}
void VmmDellForcewakeScratchRun(uint32_t khz,uint32_t index,uint64_t dma,BdwForcewakeAdapterResult& result,BdwScratchTakeoverResult& scratch) {
    scratch={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,dma,&scratch,nullptr,nullptr,false,nullptr,0,nullptr);
}
void VmmDellForcewakeBcsInit(uint32_t khz,uint32_t index,uint64_t dma,uint8_t* cpu,BdwForcewakeAdapterResult& result,BdwBcsInitResult& init) {
    init={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,dma,nullptr,cpu,&init,false,nullptr,0,nullptr);
}
void VmmDellForcewakeClearStale(uint32_t khz,uint64_t expected,BdwForcewakeAdapterResult& result,BdwScratchTakeoverResult& clear) {
    clear={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,65536,expected,&clear,nullptr,nullptr,true,nullptr,0,nullptr);
}
void VmmDellForcewakePatTest(uint32_t khz,BdwForcewakeAdapterResult& result,BdwPatTestResult& pat) {
    pat={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,0,0,nullptr,nullptr,nullptr,false,&pat,0,nullptr);
}
void VmmDellBcsCopy(uint32_t khz,uint32_t index,uint64_t token,uint64_t scratch,BdwForcewakeAdapterResult& result,BdwBcsRunResult& copy) {
    copy={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,scratch,nullptr,nullptr,nullptr,false,nullptr,token,&copy);
}
#if defined(BROOK_SSD_AUTO_RECOVERY)
void VmmDellBcsScanout(uint32_t khz,uint32_t index,uint64_t token,uint64_t scratch,BdwForcewakeAdapterResult& result,BdwBcsRunResult& copy) {
    copy={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,scratch,nullptr,nullptr,nullptr,
        false,nullptr,token,&copy,nullptr,nullptr,nullptr,nullptr,true);
}
#endif
#if defined(BROOK_SSD_AUTO_RECOVERY) && defined(BROOK_BDW_SCANOUT_FLIP)
bool VmmDellFlipGgttBind(BdwGgttExtentBind& bind,const BdwGgttExtentLease& lease,uint64_t* snapshot,
                         uint64_t takeoverScratch,BdwFlipGgttDiag& diag) {
    constexpr uint32_t maxPages=16;
    const auto& r=lease.reservation;
    diag.first=r.first;diag.count=r.count;diag.stage=BdwFlipGgttStage::Precondition;
    if(!r.token || !r.count || r.first<65536 || r.count>maxPages*512-(r.first&511u) || SmpCurrentCpuIndex()!=0 ||
       !snapshot || !takeoverScratch || (takeoverScratch&4095) || takeoverScratch>=(uint64_t{1}<<39))return false;
    const uint32_t pageCount=((r.first&511u)+r.count+511u)/512u;
    volatile uint64_t* pages[maxPages]{};
    for(uint32_t i=0;i<pageCount;++i)
        pages[i]=reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+BdwDellGgttBase+(uint64_t((r.first>>9)+i)<<12));
    auto* flush=reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwDellBar0+0x101008);
    struct Owner {
        uint32_t firstPage=0;uint32_t audited=0;bool valid=true;uint64_t scratch=0;uint32_t failedPage=0;
        static bool Audit(uint64_t page){
            KernelAliasMapping identity{},direct{},current{};
            DellI2cCriticalEvidence e{};
            e.aliasesValid=VmmKernelPtAuditPage(page,identity,direct,current) && current.writable;
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu) && cpu.cpu==0;
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return BdwGttEvidenceValid(e);
        }
        // Each PTE page is audited once per critical section (the page-table
        // lock is held throughout), not on every one of ~5,700 accesses.
        bool auditPtePage(uint32_t page,volatile uint64_t* p){
            const uint32_t n=page-firstPage;
            if(!valid || n>=32 || p!=reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+BdwDellGgttBase+(uint64_t(page)<<12)))return false;
            if(!(audited&(1u<<n))){if(!Audit(BdwDellGgttBase+(uint64_t(page)<<12))){valid=false;failedPage=page;return false;}audited|=1u<<n;}
            return true;
        }
        bool owned(){return valid && SmpCurrentCpuIndex()==0;}
        bool readable(){return valid;}
        bool validateLease(const BdwGgttExtentLease& l){
            return BdwGlobalGgttSpace().owns(l.reservation) && !(l.reservation.first&63u) &&
                l.reservation.count>BdwFlipRegs::GuardPages;
        }
        // The extent lies above the 256 MiB CPU aperture (index >= 65536),
        // outside every Brook reservation and the GOP/guard range, and no
        // display plane or Brook engine context references it, so inherited
        // entries are passive translations nobody can use (cf. i915, which
        // rewrites all unreserved GGTT entries at load).
        bool quiescent(){return valid;}
        bool validateTakeover(const BdwGgttExtentLease& l,uint64_t s){return s==scratch && validateLease(l);}
        // GFX_FLSH_CNTL posts the PTE writes, which is what i915 relies on
        // for GGTT updates; the display fetches through them only after a
        // later DSPASURF write and no engine context references them.
        bool invalidateGpuTranslations(){return valid;}
    } owner{r.first>>9,0,true,takeoverScratch,0};
    BdwGgttExtentMmio<Owner> io(owner,lease,pages,pageCount,flush,takeoverScratch);
    return VmmWithKernelPtCritical([&](){
        if(!Owner::Audit(BdwDellBar0+0x101000)){diag.stage=BdwFlipGgttStage::FlushAudit;return false;}
        // Stable twice-read snapshot of the inherited entries.
        for(unsigned pass=0;pass<2;++pass)for(uint32_t i=0;i<r.count;++i){
            uint64_t v=0;
            if(!io.read(r.first+i,v)){
                diag.stage=owner.failedPage?BdwFlipGgttStage::PteAudit:BdwFlipGgttStage::Unstable;
                diag.auditPage=owner.failedPage;return false;
            }
            if(!pass)snapshot[i]=v;
            else if(snapshot[i]!=v){diag.stage=BdwFlipGgttStage::Unstable;diag.firstNonzero=r.first+i;return false;}
        }
        BdwFlipGgttClassify(snapshot,r.first,r.count,diag);
        // All-zero ranges use the plain binder; anything inherited is taken
        // over against the exact snapshot (BindTakenOver re-reads it first).
        const bool ok=diag.takeover?bind.BindTakenOver(lease,io,snapshot,takeoverScratch):bind.Bind(lease,io);
        if(ok){diag.stage=BdwFlipGgttStage::Bound;return true;}
        diag.stage=bind.State()==BdwGgttExtentState::Fresh?BdwFlipGgttStage::Refused:BdwFlipGgttStage::Uncertain;
        if(owner.failedPage){diag.auditPage=owner.failedPage;}
        return false;
    });
}
#endif
void VmmDellVcsSelftest(uint32_t khz,uint32_t index,uint64_t token,uint64_t scratch,BdwForcewakeAdapterResult& result,BdwBcsRunResult& run) {
    vcsCleanStop=false;
    run={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,scratch,nullptr,nullptr,nullptr,false,nullptr,token,&run,
        nullptr,nullptr,nullptr,nullptr,false,nullptr,true);
}
// VCS0 user job (VA-API phase 1c), prepared by BdwPrepareVcsUserJob.
// Phase 4 VCS lane: phase 1 submits and leaves the job running, phase 2
// completes it (breadcrumb, CSB, stop, unbind, PAT restore).
void VmmDellVcsUserLane(uint32_t khz,uint8_t phase,uint32_t index,uint64_t token,uint64_t scratch,BdwForcewakeAdapterResult& result,BdwBcsRunResult& run) {
    run={};if(phase!=1 && phase!=2){result={};return;}
    VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,scratch,nullptr,nullptr,nullptr,false,nullptr,token,&run,
        nullptr,nullptr,nullptr,nullptr,false,nullptr,false,0,nullptr,true,phase);
}
void VmmDellVcsUserRun(uint32_t khz,uint32_t index,uint64_t token,uint64_t scratch,BdwForcewakeAdapterResult& result,BdwBcsRunResult& run) {
    run={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,scratch,nullptr,nullptr,nullptr,false,nullptr,token,&run,
        nullptr,nullptr,nullptr,nullptr,false,nullptr,false,0,nullptr,true);
}
void VmmDellVcsPmOp(uint32_t khz,uint8_t op,BdwForcewakeAdapterResult& result,BdwRcsResetTestResult& r) {
    vcsCleanStop=false;
    r={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,0,0,nullptr,nullptr,nullptr,false,nullptr,0,nullptr,
        nullptr,nullptr,nullptr,nullptr,false,nullptr,false,op,&r);
}
void VmmDellVcsReset(uint32_t khz,BdwForcewakeAdapterResult& result,BdwRcsResetTestResult& reset) {
    vcsCleanStop=false;
    reset={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,0,0,nullptr,nullptr,nullptr,false,nullptr,0,nullptr,
        nullptr,nullptr,nullptr,nullptr,false,&reset);
}
void VmmDellRcsReset(uint32_t khz,BdwForcewakeAdapterResult& result,BdwRcsResetTestResult& reset) {
    reset={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,0,0,nullptr,nullptr,nullptr,false,nullptr,0,nullptr,&reset);
}
void VmmDellRenderGtInit(uint32_t khz,BdwForcewakeAdapterResult& result,BdwRenderGtInitResult& init) {
    init={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,0,0,nullptr,nullptr,nullptr,false,nullptr,0,nullptr,nullptr,&init);
}
void VmmDellRenderRun(uint32_t khz,uint32_t index,uint64_t token,uint64_t scratch,uint32_t (&snapshot)[20*1024],BdwForcewakeAdapterResult& result,BdwRenderRunResult& render) {
    VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,scratch,nullptr,nullptr,nullptr,false,nullptr,token,nullptr,nullptr,nullptr,&render,&snapshot);
}
} // namespace brook
