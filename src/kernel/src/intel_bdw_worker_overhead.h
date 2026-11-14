#pragma once
#include <stdint.h>

namespace brook {
// Fixed per-job costs of the Broadwell submission worker, in TSC ticks.
// Written only by the gpu-submit worker; read by the debug channel with
// relaxed loads (diagnostic: a read racing a job may mix two jobs' values).
enum BdwOverheadStep : uint32_t {
    BdwOhEntry=0,        // worker entry checks: lease, pages, display, PCI identity
    BdwOhContextBegin,   // context image allocation and owner context load
    BdwOhReprepare,      // retained render state re-prepare
    BdwOhPrivateDma,     // private render DMA inspection
    BdwOhReservation,    // GGTT reservation check and cache policy
    BdwOhPrepare,        // BdwPrepareRetainedSubmission total
    BdwOhTables,         //   table allocation (gemProbeTables.Prepare)
    BdwOhWorkspace,      //   expansion workspace allocation
    BdwOhAttach,         //   PPGTT build / adopt
    BdwOhInspectTables,  //   table DMA inspection
    BdwOhPublish,        //   table and image publication
    BdwOhSyncDevice,     //   CPU cache sync for the device
    BdwOhSnapshotDma,    //   per-page buffer DMA/ownership inspection
    BdwOhArm,            //   arm
    BdwOhScratch,        // scratch reuse
    BdwOhRenderRun,      // VmmDellRenderRun (forcewake critical section incl. GPU time)
    BdwOhRetire,         // retire and timing bookkeeping
    BdwOhFinish,         // context finish and image free/swap
    BdwOhAudit,          // one critical-section re-audit after a GPU-wait yield
    BdwOhAuditTypes,     //   of which: MTRR/PAT memory-type observation
    BdwOhYieldWait,      // one unlocked wait (sleep or spin slice) inside a GPU wait
    BdwOhStepCount
};
struct BdwOverheadSpan {uint64_t count,ticks,maxTicks;};
struct BdwOverheadCounters {
    uint64_t auditBatches,auditBatchFallbacks,workspaceReuses,workspaceAllocs;
};
struct BdwOverheadState {
    BdwOverheadSpan steps[BdwOhStepCount];
    BdwOverheadCounters counters;
    uint32_t resetRequested;
    uint32_t auditBatchDisabled; // runtime switch: 1 = per-page observation (pre-change behaviour)
};
inline BdwOverheadState& BdwOverhead(){static BdwOverheadState s{};return s;}
inline uint64_t BdwOverheadTicks(){uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));return (uint64_t(hi)<<32)|lo;}
// Worker thread only.
inline void BdwOverheadAdd(BdwOverheadStep step,uint64_t ticks){
    auto& s=BdwOverhead();
    if(__atomic_load_n(&s.resetRequested,__ATOMIC_ACQUIRE)){
        for(auto& x:s.steps){__atomic_store_n(&x.count,0,__ATOMIC_RELAXED);__atomic_store_n(&x.ticks,0,__ATOMIC_RELAXED);__atomic_store_n(&x.maxTicks,0,__ATOMIC_RELAXED);}
        __atomic_store_n(&s.counters.auditBatches,0,__ATOMIC_RELAXED);__atomic_store_n(&s.counters.auditBatchFallbacks,0,__ATOMIC_RELAXED);
        __atomic_store_n(&s.counters.workspaceReuses,0,__ATOMIC_RELAXED);__atomic_store_n(&s.counters.workspaceAllocs,0,__ATOMIC_RELAXED);
        __atomic_store_n(&s.resetRequested,0,__ATOMIC_RELEASE);
    }
    if(step>=BdwOhStepCount)return;
    auto& x=s.steps[step];
    __atomic_store_n(&x.count,x.count+1,__ATOMIC_RELAXED);
    __atomic_store_n(&x.ticks,x.ticks+ticks,__ATOMIC_RELAXED);
    if(ticks>x.maxTicks)__atomic_store_n(&x.maxTicks,ticks,__ATOMIC_RELAXED);
}
inline void BdwOverheadCount(uint64_t& counter){__atomic_store_n(&counter,counter+1,__ATOMIC_RELAXED);}
// Scoped span: records ticks from construction to destruction.
struct BdwOverheadScope {
    BdwOverheadStep step;uint64_t start;
    explicit BdwOverheadScope(BdwOverheadStep s):step(s),start(BdwOverheadTicks()){}
    ~BdwOverheadScope(){const uint64_t end=BdwOverheadTicks();BdwOverheadAdd(step,end>start?end-start:0);}
};
inline bool BdwAuditBatchEnabled(){return !__atomic_load_n(&BdwOverhead().auditBatchDisabled,__ATOMIC_RELAXED);}
inline void BdwAuditBatchSet(bool on){__atomic_store_n(&BdwOverhead().auditBatchDisabled,on?0u:1u,__ATOMIC_RELAXED);}
inline void BdwOverheadRequestReset(){__atomic_store_n(&BdwOverhead().resetRequested,1u,__ATOMIC_RELEASE);}
inline const char* BdwOverheadStepName(unsigned i){
    static const char* names[BdwOhStepCount]={"entry","context_begin","reprepare","private_dma","reservation",
        "prepare","tables","workspace","attach","inspect_tables","publish","sync_device","snapshot_dma","arm",
        "scratch","render_run","retire","finish","audit","audit_types","yield_wait"};
    return i<BdwOhStepCount?names[i]:"?";
}
}
