#pragma once
#include <stdint.h>
#include "intel_bdw_forcewake.h"
#include "intel_bdw_ggtt_space.h"
#include "intel_bdw_scratch_takeover.h"
#include "intel_bdw_bcs_init.h"
#include "intel_bdw_render_transaction.h"
struct BrookGpuPresentProbe;
namespace brook {
struct BdwGpuBuffer;struct BdwFence;
// Snapshot already-admitted application submissions; caller excludes new
// producer writes and retains the source through the consumer's completion.
int64_t BdwAcquireApplicationBufferFence(BdwGpuBuffer*,BdwFence**);
bool BdwOffscreenRendererOwnedBy(uint64_t owner);
// Builds and releases private command images only; does not access hardware.
bool BdwPreparePrivateImages();
bool BdwPreparePrivateRenderImages(uint32_t (&staging)[20*1024]);
// One boot-lifetime allocation; caller serializes these calls. No GPU mapping
// or submission is performed and a released allocation cannot be recreated.
bool BdwRetainPrivateBcsImages(uint32_t ggttEntries);
struct BdwOwnedPage;
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
struct BdwGopBacking;
struct CompositorPresentRegion;
struct BdwBcsExecutionBackend;
struct BdwBcsRunResult;
bool BdwPrepareBcsScanout(BdwGpuBuffer*,uint64_t token,const BdwGopBacking&,
    const CompositorPresentRegion*,uint32_t count,BdwGgttReservation&,BdwOwnedPage&);
bool BdwRunPreparedBcsScanout(BdwBcsExecutionBackend&,uint64_t scratchDma,BdwBcsRunResult&);
bool BdwRetirePreparedBcsScanout(BdwBcsExecutionBackend&);
bool BdwReleasePreparedBcsScanout();
bool BdwBcsScanoutEnabled();
bool BdwExecuteBcsScanout(BdwGpuBuffer*,uint64_t token,const CompositorPresentRegion*,uint32_t count);
struct BdwScanoutCopyStats {uint64_t requested=0,submitted=0,full=0,partial=0;uint32_t lastStage=0;};
BdwScanoutCopyStats BdwReadScanoutCopyStats();
#endif
// Retained render allocation only; callers serialize against the BCS owner.
bool BdwRetainPrivateRenderImages(uint32_t ggttEntries,bool ppgttProbe=false,bool storeProbe=false);
bool BdwReleasePrivateRenderImages();
bool BdwRetainedRenderReservation(BdwGgttReservation& out);
uint32_t BdwInspectPrivateRenderDma();
// Point-in-time VT-d and private PMM inspection for additional render-table
// pages; the caller owns the array and must maintain its lifetime and exclusion.
uint32_t BdwInspectAdditionalRenderDma(const BdwOwnedPage* pages,unsigned count);
bool BdwPinRetainedRenderScratch(BdwOwnedPage& out,BdwGgttReservation& reservation);
#ifndef BROOK_HOST_TEST
struct BdwExecSnapshot;
struct BdwSubmissionPreparation;
struct BdwSubmissionOutcome;
struct BdwWorkerProbeResult {uint32_t submitted=0,completed=0;bool failed=false;uint32_t values[3]{},contextHashes[2]{};};
bool BdwTestSubmissionWorker(BdwWorkerProbeResult& result);
bool BdwTestContextSubmissionWorker(BdwWorkerProbeResult& result);
// Experimental fixed-store syscall probe only. This does not advertise or
// accept general i915 execution and requires a separate boot marker.
bool BdwStoreProbeEnabled();
int64_t BdwSubmitStoreProbe(void*,BdwExecSnapshot* snapshot);
bool BdwRenderExperimentEnabled();
int64_t BdwSubmitRenderExperiment(void*,BdwExecSnapshot* snapshot);
int64_t BdwBeginRenderPreparation(void*,uint64_t owner,BdwSubmissionPreparation& entry);
void BdwEndRenderPreparation(void*,BdwSubmissionPreparation& entry);
struct BdwGpuBuffer;
int64_t BdwOffscreenHandoffControl(uint64_t owner,uint64_t op,uint64_t arg,BdwGpuBuffer* buffer);
struct WmDesktopLayers;
struct WmDesktopLayer;
// On success takes ownership of the heap-allocated, pinned snapshot.
int64_t BdwOffscreenRegisterScene(uint64_t owner,WmDesktopLayers* layers,uint64_t* token);
struct CompositorBackdropInfo;
int64_t BdwOffscreenRegisterBackdrop(uint64_t owner,const CompositorBackdropInfo& info);
int64_t BdwOffscreenRegisterChrome(uint64_t owner,const WmDesktopLayer& layer);
struct CompositorPresentRegion;
int64_t BdwOffscreenHandoffPresentProbe(uint64_t owner,::BrookGpuPresentProbe& probe,bool lease=false,bool region=false,
                                       bool collectSamples=true,bool fromQueue=false);
int64_t BdwQueueOffscreenPresent(uint64_t owner,uint64_t ticket,uint32_t width,uint32_t height,uint32_t stride,
                                 const CompositorPresentRegion* regions,uint32_t count);
int64_t BdwOffscreenPresentResult(uint64_t owner,uint64_t ticket);
void BdwOffscreenPresentCounts(uint64_t& accepted,uint64_t& presented);
int64_t BdwOffscreenHandoffPresentRegions(uint64_t owner,uint64_t ticket,const CompositorPresentRegion* regions,uint32_t count,
                                         bool fromQueue=false);
void BdwOffscreenHandoffOwnerExit(uint64_t owner);
bool BdwOffscreenHandoffSubmit(void*,BdwExecSnapshot*,int64_t* result);
// Fixed-test worker executor. The worker retains the snapshot; no userspace
// submission is authorized by this entry point.
BdwSubmissionOutcome BdwExecuteRetainedWorkerSnapshot(void*,BdwExecSnapshot* snapshot);
// Fixed, kernel-authored context-switch diagnostic only; never install as a
// userspace submission callback without independent command isolation.
BdwSubmissionOutcome BdwExecuteRetainedContextWorkerSnapshot(void*,BdwExecSnapshot* snapshot);
// Caller owns the serialized render/device lease and retains the snapshot.
// These helpers do not validate commands or authorize a userspace submission.
bool BdwPrepareRetainedSubmission(BdwExecSnapshot* snapshot);
bool BdwRetireRetainedSubmission(BdwExecSnapshot* snapshot);
bool BdwSetupRetainedGemProbe(bool externalTables);
bool BdwFinishRetainedGemProbe(bool safelyStopped);
bool BdwReleaseRetainedGemProbe();
void BdwFailRetainedGemProbe();
bool BdwReprepareRetainedGemProbe();
bool BdwReprepareRetainedSubmission(const uint32_t (*saved)[20*1024],uint32_t nonce);
bool BdwReuseRetainedRenderScratch(BdwOwnedPage& out,BdwGgttReservation& reservation);
#endif
struct BdwRenderExecutionBackend;
struct BdwForcewakeAdapterResult;
struct BdwRenderRunResult {
    bool admitted=false,attempted=false,breadcrumb=false,contextComplete=false;
    bool ggttClearRequested=false,ggttCleared=false;
    bool gemReleaseRequested=false,gemReleased=false;
    bool externalTablesRequested=false;
    bool gemQueued=false;
    bool gemIoctlDecoded=false;
    bool gemIoctlSubmitted=false;
    bool pollYieldRequested=false,pollYieldFailed=false;
    uint32_t pollYields=0;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    // Diagnostic TSC durations for GT, PAT setup, run, save, reset/unbind,
    // PAT restore. Zero or backwards-clock intervals are omitted.
    uint64_t transactionTicks[6]{};
    uint64_t issueToBreadcrumbTicks=0,breadcrumbToRetirementTicks=0;
#endif
    BdwRenderTransactionResult transaction{};
    uint32_t ggttWriteAttempts=0,portWriteAttempts=0;
    uint32_t preflightStage=0,dmaStatus=0;
    uint32_t contextHash=0;
    bool engineSampled=false;
    uint32_t head=0,tail=0,mode=0,control=0,acthd=0,ipeir=0,ipehr=0,execlist=0;
};
bool BdwRunRetainedRender(BdwRenderExecutionBackend& backend,uint64_t scratchDma,BdwRenderRunResult& result);
bool BdwRetireRetainedRenderAfterReset(BdwRenderExecutionBackend& backend);
bool BdwTestRenderRun(BdwForcewakeAdapterResult& out,BdwRenderRunResult& result);
// Explicitly opted-in first-use bootstrap; this does not initialize the GPU.
bool BdwAutomaticRenderRequested();
bool BdwTestRenderRepeat(BdwForcewakeAdapterResult& out,BdwRenderRunResult& result);
bool BdwTestRebasedRenderRun(BdwForcewakeAdapterResult& out,BdwRenderRunResult& result);
// Explicitly opted-in Intel file-interface testing; no rendering readiness is
// implied. The node must remain unavailable without exact Dell admission.
bool BdwIntelUserNodeEnabled();
bool BdwCopyRetainedRenderContext(BdwRenderExecutionBackend& backend,uint32_t (&out)[20*1024]);
bool BdwRetainRebasedRenderImages(const uint32_t (&saved)[20*1024]);
bool BdwRebasedRenderReservation(BdwGgttReservation& out);
uint32_t BdwInspectRebasedRenderDma();
bool BdwPinRebasedRenderScratch(BdwOwnedPage& out,BdwGgttReservation& reservation);
bool BdwRunRebasedRender(BdwRenderExecutionBackend& backend,uint64_t scratchDma,BdwRenderRunResult& result);
bool BdwCopyRebasedRenderContext(BdwRenderExecutionBackend& backend,uint32_t (&out)[20*1024]);
struct BdwBcsExecutionBackend;
struct BdwBcsRunResult;
// Caller has already checked/pinned all pages and holds the device lease.
// Never call while relying on PMM locks; retained pages stay pinned even on
// success, because a context-complete result alone does not reclaim them.
bool BdwRunRetainedBcs(BdwBcsExecutionBackend& backend,uint64_t scratchDma,BdwBcsRunResult& result);
// Snapshot only. It does not grant GPU access or release the retained page.
bool BdwRetainedBcsScratch(BdwOwnedPage& out);
// Caller serializes against retain/release; this is a one-way boot-lifetime
// pin before any scratch PTE write can be attempted.
bool BdwPinRetainedScratchForTakeover(BdwOwnedPage& out,BdwGgttReservation& reservation);
// Caller holds engine exclusion; only valid for the already pinned scratch.
// These perform PMM checks and must not run while holding the VMM page-table lock.
bool BdwPrepareRetainedScratchStatus();
bool BdwReadRetainedScratchProducer(uint32_t& producer);
bool BdwReleasePrivateBcsImages();
// Point-in-time inspection only; does not authorize hardware DMA.
enum BdwDmaSnapshotStatus : uint32_t {
    BdwDmaSnapshotOk=0, BdwDmaNotPrepared=1, BdwDmaResetUnarmed=2,
    BdwDmaWrongCpu=3, BdwDmaMissingVtdEvidence=4, BdwDmaFirstVtdRead=5,
    BdwDmaFirstVtdRestricted=6, BdwDmaPageAddress=7, BdwDmaPageOwner=8,
    BdwDmaPageShared=9, BdwDmaPageAlias=10, BdwDmaSecondVtdRead=11,
    BdwDmaSecondVtdRestricted=12, BdwDmaVtdChanged=13, BdwDmaUnavailable=14,
    BdwDmaReservationLost=15
};
// A passing snapshot is not a DMA authorization.
uint32_t BdwInspectPrivateBcsDma();
bool BdwRetainedBcsReservation(BdwGgttReservation& out);
struct BdwGgttReservedPtes;
struct BdwRcsGgttPtes;
bool BdwObserveReservedGgtt(BdwGgttReservedPtes& out);
bool BdwObserveRcsGgtt(BdwRcsGgttPtes& out);
struct BdwGopPtes;
bool BdwObserveGopPtesLive(BdwGopPtes& out);
struct BdwGopBacking;
bool BdwObserveGopBacking(BdwGopBacking& out);
struct BdwDisplaySnapshot;
struct BdwGgttSurvey;
bool BdwObserveGgttSurvey(uint32_t first,BdwGgttSurvey& out);
bool BdwObserveDisplayLive(BdwDisplaySnapshot& out);
struct BdwForcewakeAdapterResult;
struct BdwRcsResetTestResult {bool attempted=false,complete=false,uncertain=false;};
bool BdwTestRcsReset(BdwForcewakeAdapterResult& out,BdwRcsResetTestResult& reset);
struct BdwRenderGtInitResult;
bool BdwTestRenderGtInit(BdwForcewakeAdapterResult& out,BdwRenderGtInitResult& init);
struct BdwPatTestResult;
bool BdwTestPrivatePat(BdwForcewakeAdapterResult& out,BdwPatTestResult& pat);
bool BdwTestBcsCopy(BdwForcewakeAdapterResult& out,BdwBcsRunResult& copy);
bool BdwObserveForcewakeLive(BdwForcewakeAdapterResult& out,bool otherEngines=false,bool scopeCheck=false);
bool BdwTestTlbInvalidate(BdwForcewakeAdapterResult& out);
bool BdwTestBcsReset(BdwForcewakeAdapterResult& out);
bool BdwTestScratchTakeover(BdwForcewakeAdapterResult& out,BdwScratchTakeoverResult& scratch);
bool BdwTestBcsInitialize(BdwForcewakeAdapterResult& out,BdwBcsInitResult& init);
bool BdwClearPriorScratch(BdwForcewakeAdapterResult& out,BdwScratchTakeoverResult& clear);
struct BdwGopPtePlan { uint64_t firstIndex=0,lastIndex=0,firstAddress=0,lastAddress=0; };
struct BootProtocol;
struct BdwGopObservation {
    bool prepareRequested=false,prepareBuilt=false,renderPrepareRequested=false,renderPrepareBuilt=false;
    bool bcsPresentRequested=false;
    bool retainRequested=false,retainBuilt=false;
    bool renderRetainRequested=false,renderRetainBuilt=false;
    bool inventory=false,planned=false,probeRequested=false,powerD0=false,pagesAdmitted=false,sampled=false;
    BdwGopPtePlan plan{};uint64_t firstPte=0,lastPte=0;
    bool ackRequested=false,ackPageAdmitted=false,ackSampled=false;uint32_t ackRaw=0;
    bool wakeRequested=false,otherEnginesRequested=false,otherEnginePagesAdmitted=false,patRequested=false,patPageAdmitted=false,wakePagesAdmitted=false,wakeClockReady=false,wakeAliases=false,wakeClock=false,wakeClearDelivered=false;
    BdwForcewakeResult wake{};
};
void BdwGopObserveConfigure(const BootProtocol* boot);
const BdwGopObservation& BdwGopObserveRead();
// The extents supplied here must be independently verified by the caller.
// This performs arithmetic only and grants no MMIO or allocation rights.
inline bool BdwPlanGopPtes(uint64_t bar0,uint64_t bar0Bytes,uint64_t bar2,uint64_t bar2Bytes,
                           uint8_t msac,uint16_t ggc,uint64_t framebuffer,uint32_t width,
                           uint32_t height,uint32_t stride,BdwGopPtePlan& out) {
    constexpr uint64_t MiB=1024ull*1024, page=4096;
    if(!bar0 || !bar2 || (bar0&(16*MiB-1)) || (bar2&(256*MiB-1)) ||
       bar0Bytes!=16*MiB || bar2Bytes!=256*MiB || (msac&0x1f)!=1 || msac==0xff || ggc==0xffff ||
       ((ggc>>6)&3)!=3 || !width || !height || uint64_t(width)*4>stride || !framebuffer)return false;
    if(bar0>~uint64_t{0}-bar0Bytes || bar2>~uint64_t{0}-bar2Bytes || framebuffer<bar2)return false;
    if(bar0<bar2+bar2Bytes && bar2<bar0+bar0Bytes)return false;
    const uint64_t bytes=uint64_t(stride)*height;
    const uint64_t offset=framebuffer-bar2;
    if(!bytes || offset>=bar2Bytes || bytes>bar2Bytes-offset)return false;
    const uint64_t first=offset/page,last=(offset+bytes-1)/page;
    constexpr uint64_t gttOffset=8*MiB,gttSize=8*MiB;
    if(first>=gttSize/8 || last>=gttSize/8)return false;
    BdwGopPtePlan result{first,last,bar0+gttOffset+first*8,bar0+gttOffset+last*8};
    out=result;return true;
}
}
