// Dell/Broadwell GPU critical-section work, moved verbatim from
// memory/virtual_memory.cpp. The VMM supplies only the kernel page-table
// critical section and the alias audit; all device logic lives here.
#include "intel_bdw_critical.h"
#include "memory/virtual_memory.h"
#include "cpu.h"
#include "dell_i2c_id_policy.h"
#include "intel_vtd_probe_policy.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_ggtt_mmio.h"
#include "intel_bdw_display_probe.h"
#include "intel_bdw_forcewake_ack.h"
#include "intel_bdw_forcewake_adapter.h"
#include "intel_bdw_scratch_mmio.h"
#include "intel_bdw_bcs_mmio.h"
#include "intel_bdw_rcs_reset_mmio.h"
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
#include "scheduler.h"
#include "smp.h"

namespace brook {
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
                                        uint32_t scratchIndex,uint64_t scratchDma,BdwScratchTakeoverResult* scratchResult,uint8_t* scratchCpu,BdwBcsInitResult* initResult,bool clearScratch,BdwPatTestResult* patResult,uint64_t copyToken,BdwBcsRunResult* copyResult,BdwRcsResetTestResult* rcsReset=nullptr,BdwRenderGtInitResult* gtInit=nullptr,BdwRenderRunResult* renderResult=nullptr,uint32_t (*snapshot)[20*1024]=nullptr,bool scanoutCopy=false) {
    static BdwForcewakeClaim claim;result={};
    if(!claim.acquire())return;
    struct Io {
        bool tlbRequested,resetRequested,scratchRequested,initRequested,patChangeRequested,copyRequested,rcsResetRequested,gtInitRequested,renderRequested;
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
            if(gtInitRequested && (page==BdwOtherEnginePages[0] || page==BdwPrivatePatPage || page==BdwResetPage || page==BdwDellBar0+0xb000) && !current.writable)e.aliasesValid=false;
            if(renderRequested && (page==BdwOtherEnginePages[0] || page==BdwPrivatePatPage || page==BdwResetPage || page==BdwDellBar0+0xb000 || page==0xf6880000ull || page==0xf6101000ull) && !current.writable)e.aliasesValid=false;
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu) && cpu.cpu==0;
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
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
        void Wait(){SchedulerSleepMs(1);}
        void Lock(){*irq=VmmKernelPtCriticalEnter();}
        bool Audit(){
            if(SmpCurrentCpuIndex()!=0)return false;
            for(auto page:BdwForcewakePages){DellI2cCriticalEvidence e{};if(!evidence(page,e) || !BdwGttEvidenceValid(e))return false;}
            for(auto page:BdwOtherEnginePages){DellI2cCriticalEvidence e{};if(!evidence(page,e) || !BdwGttEvidenceValid(e))return false;}
            constexpr uint64_t extra[]={BdwDellBar0,BdwPrivatePatPage,BdwResetPage,BdwDellBar0+0xb000,0xf6880000ull,0xf6101000ull};
            for(auto page:extra){DellI2cCriticalEvidence e{};if(!evidence(page,e) || !BdwGttEvidenceValid(e))return false;}
            return true;
        }
        void Poison(){mappingsValid=false;}
    } io{tlbRequested,resetRequested,scratchResult!=nullptr,initResult!=nullptr,patResult!=nullptr,copyResult!=nullptr,rcsReset!=nullptr,gtInit!=nullptr,renderResult!=nullptr};
    uint64_t irq=VmmKernelPtCriticalEnter();io.irq=&irq;
    // Read-only exercise of the callback boundary. The callback remains under
    // the same page-table lock and forcewake claim as the initial samples.
    auto work=[&](Io& held,const BdwForcewakeResult& initial){
        uint32_t ack=0,lo=0,hi=0;
        if(!(held.read32(BdwDellBar0+0x130044,ack) && ack!=0xffffffffu && (ack&1) &&
            held.read32(BdwDellBar0+0x40e0,lo) && held.read32(BdwDellBar0+0x40e4,hi) &&
            lo==initial.patLo && hi==initial.patHi))return false;
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
            if(!snapshot || (scratchIndex!=65544 && scratchIndex!=65573) || !scratchDma || !copyToken)return false;
            const bool rebased=scratchIndex==65573;
            constexpr uint64_t pages[]={BdwDellBar0,BdwOtherEnginePages[0],BdwPrivatePatPage,BdwResetPage,
                BdwDellBar0+0xb000,0xf6880000ull,0xf6101000ull};
            for(auto page:pages){DellI2cCriticalEvidence e{};if(!held.evidence(page,e) || !BdwGttEvidenceValid(e))return false;}
            GtOwner gt{held,khz};
            struct RenderOwner {
                Io& io;GtOwner& gt;BdwRenderRunResult& result;uint32_t khz,line;uint64_t origin;uint32_t budget=4000000;
                bool patActive=false,gtReady=false,fault=false;
                bool firstCompletionPoll=true,firstRetirementPoll=true;
                bool owned(){return gt.owned();}
                bool quiescent(){
                    struct Reader {RenderOwner& o;bool owned(){return o.owned();}
                        bool read(uint32_t r,uint32_t& v){return o.io.read32(BdwDellBar0+r,v);}} reader{*this};
                    return owned() && BdwObserveEngine(reader,0x2000).submissionQuiescent;
                }
                bool allEnginesQuiescent(){return quiescent();} // others held idle under claim
                bool monotonicMs(uint64_t& ms){uint64_t t=0;if(!io.tsc(t) || t<origin)return false;ms=(t-origin)/khz;return true;}
                bool inTime(){uint64_t ms=0;return budget && --budget && monotonicMs(ms) && ms<2000;}
                bool pollContinue(){
                    if(!owned() || !inTime())return false;
                    if(result.pollYieldRequested){
                        if(!BdwYieldGpuWait(io)){result.pollYieldFailed=true;fault=true;return false;}
                        ++result.pollYields;
                    }
                    return owned() && inTime();
                }
                bool immediatePollAllowed(){return owned() && inTime();}
#if defined(BROOK_GPU_SUBMISSION_TIMING)
                void sampleRenderFrequency(){
                    if(!result.sampleFrequency || result.frequencySamples==result.FrequencySampleLimit)return;
                    uint64_t now=0;
                    if(!io.tsc(now))return;
                    const auto count=result.frequencySamples;
                    if(count && now-result.frequency[count-1].ticks<uint64_t{khz}*5)return;
                    // The forcewake claim already audits the 0xa000 MMIO page.
                    uint32_t status=~uint32_t{0};
                    if(!io.read32(BdwDellBar0+0xa01c,status))return; // GEN6_RPSTAT1
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
                    if(!BdwFirstGpuPoll(*this,firstCompletionPoll))return false;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
                    sampleRenderFrequency();
#endif
                    return true;
                }
                bool retirementContinue(){return BdwFirstGpuPoll(*this,firstRetirementPoll);}
                void relax(){io.relax();}
                bool renderStateReady(){return owned() && gtReady;}
                bool restoreRenderState(){
                    BdwRenderGtMmio<GtOwner> mmio(gt,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[0]),
                        reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage),reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
                    const auto r=BdwInitializeRenderGt(mmio);gtReady=r.status==BdwRenderGtInitStatus::Applied;
                    if(r.status==BdwRenderGtInitStatus::Uncertain)fault=true;
                    return gtReady;
                }
                bool gpuPatLease(){return patActive && owned();}
                bool preparePrivatePat(uint64_t descriptor){return gpuPatLease() && (descriptor&0x119)==0x119;}
                bool readPrivatePat(uint32_t& l,uint32_t& h){return owned() && io.read32(BdwDellBar0+0x40e0,l) && io.read32(BdwDellBar0+0x40e4,h);}
                bool safePatTransition(uint32_t l,uint32_t h,uint32_t nl,uint32_t nh){
                    return owned() && l==0x03030303u && h==0x03030303u && nl==0x03030307u && nh==0x0303030bu;
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
                    if(!owned() || !p || !count || count>128)return false;
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
                bool publishExtraTables(BdwOwnedPage* p,unsigned count){if(!owned() || !p || !count || count>128)return false;
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
                    :o(owner),backend(b),result(r),scratch(s),snapshot(snap),rebased(second),pat(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage)){}
                bool initializeGt(){return BdwTimedTransaction(result,0,[&](){return o.restoreRenderState();});}
                bool establishPat(){return BdwTimedTransaction(result,1,[&](){const bool ok=lease.Establish(pat,0x119);o.patActive=ok;
                    if(lease.State()==BdwPatLeaseState::Quarantined)o.fault=true;return ok;});}
#if defined(BROOK_BDW_RPS_TRIAL)
                bool restoreFrequency=false;
                uint32_t savedFrequency=0;
                void requestTrialFrequency(){
                    constexpr uint32_t requestReg=0xa008; // GEN6_RPNSWREQ
                    constexpr uint32_t trialBin=10; // 500 MHz, in 50 MHz bins
                    constexpr uint32_t ratioMask=0x7f000000u;
                    if(!result.tryFrequency || !o.owned())return;
                    DellI2cCriticalEvidence evidence{};
                    if(!o.io.evidence(BdwRenderFrequencyCapsPage,evidence) || !BdwGttEvidenceValid(evidence))return;
                    if(!o.io.read32(BdwDellBar0+BdwRenderFrequencyCaps,result.trialFrequencyCaps))return;
                    const uint32_t caps=result.trialFrequencyCaps;
                    const uint32_t minBin=(caps>>16)&0xff;
                    const uint32_t maxBin=caps&0xff;
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
                    return ok;});}
                bool saveContext(){return BdwTimedTransaction(result,3,[&](){return rebased?BdwCopyRebasedRenderContext(backend,snapshot):BdwCopyRetainedRenderContext(backend,snapshot);});}
                bool resetAfter(){return BdwTimedTransaction(result,4,[&](){
                    if(!o.resetRcs(BdwBcsResetIntent::StopOwnedWork))return false;
                    if(result.ggttClearRequested && result.contextComplete){
                        result.ggttCleared=BdwRetireRetainedRenderAfterReset(backend);
                        if(!result.ggttCleared){o.fault=true;return false;}
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
        if(!scratchDma || (scanoutCopy?(!copyResult || scratchIndex<65536 || scratchIndex>65536+512-8):scratchIndex!=65536))
            return false;
        const uint64_t scratchPages[]={0xf6880000ull,(scratchResult || copyResult)?0xf6101000ull:BdwResetPage,BdwResetPage};
        for(uint64_t page:scratchPages) {
            DellI2cCriticalEvidence e{};if(!held.evidence(page,e) || !BdwGttEvidenceValid(e))return false;
        }
        if(copyResult) {
            if(!copyToken)return false;
            struct CopyOwner {
                Io& io;uint32_t khz,line;uint64_t origin;uint32_t budget=4000000;
                bool patActive=false,scanout=false;
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
                    return owned() && lo==0x03030303u && hi==0x03030303u &&
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
                    BdwBcsResetMmio<CopyOwner> reset(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),
                        reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
                    return reset.reset(intent)==BdwBcsResetResult::DomainAcknowledged;
                }
                bool quiescent(){
                    struct Reader {CopyOwner& o;bool owned(){return o.owned();}
                        bool read(uint32_t r,uint32_t& v){return o.io.read32(BdwDellBar0+r,v);}} reader{*this};
                    return owned() && BdwObserveEngine(reader,0x22000).submissionQuiescent;
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
                    return io.write32(BdwDellBar0+0x223a0,0x07000000u|(slot<<8)) &&
                        io.read32(BdwDellBar0+0x223a0,posted) && posted!=0xffffffffu && ((posted>>8)&7u)==slot && owned();
                }
            } owner{held,khz,0,0};
            owner.scanout=scanoutCopy;
            uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
            owner.line=((b>>8)&255)*8;
            if(!(d&(1u<<19)) || owner.line<16 || owner.line>4096 || (owner.line&(owner.line-1)) ||
               4096%owner.line || !held.tsc(owner.origin))return false;
            copyResult->resetBeforeAttempted=true;copyResult->resetBefore=owner.resetBcs();
            if(!copyResult->resetBefore)return false;
            BdwPatMmio<CopyOwner> patMmio(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage),scanoutCopy);
            BdwPatLease pat;copyResult->patEstablished=pat.Establish(patMmio,0x119,scanoutCopy);
            if(!copyResult->patEstablished){copyResult->patUncertain=pat.State()==BdwPatLeaseState::Quarantined;return false;}
            owner.patActive=true;
            BdwBcsDeviceBackend<CopyOwner> backend(owner,reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6880000ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6101008ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),scratchIndex,copyToken,scratchDma);
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
            const bool copied=scanoutCopy?BdwRunPreparedBcsScanout(backend,scratchDma,*copyResult):
                BdwRunRetainedBcs(backend,scratchDma,*copyResult);
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
    result=BdwDellForcewakeCriticalWithWork(io,khz,work,scopeCheck || scratchResult || initResult || patResult || copyResult || rcsReset || gtInit || renderResult,otherEngines,patRequested,tlbRequested,resetRequested);
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
