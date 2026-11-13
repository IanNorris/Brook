#pragma once
#include <stdint.h>
#include "intel_bdw_exec_address_space.h"
namespace brook {
struct BdwGpuFile;
struct BdwGpuContext;
struct BdwFence;
struct BdwGpuBuffer;
struct BdwExecSnapshot;
constexpr uint32_t BdwMaxSubmissionFences=256;
// Kernel-copied subset of i915 exec objects. The caller decodes the UAPI,
// rejects unsupported relocation, fence and engine features, and supplies
// these fields; no userspace pointer is dereferenced here. Flags use the
// i915 WRITE(4), SUPPORTS_48B(8), PINNED(16), ASYNC(64), CAPTURE(128)
// values. ASYNC is serialized and CAPTURE is a best-effort hint. This validates object
// ownership/ranges only, not command safety or GPU execution authorization.
struct BdwExecObjectInput {uint32_t handle;uint64_t address,alignment,flags;};
BdwExecSnapshot* BdwExecSnapshotCreate(BdwGpuFile* file,const BdwExecObjectInput* objects,uint32_t count,
                                       uint32_t batchIndex,uint32_t batchStart,uint32_t batchLength);
// Bind before queueing; the snapshot retains the context even if its handle
// is closed. The executor must serialize and complete the context separately.
bool BdwExecSnapshotBindContext(BdwExecSnapshot* snapshot,BdwGpuContext* context);
BdwGpuContext* BdwExecSnapshotContext(const BdwExecSnapshot* snapshot);
// Conservative bounded test for disjoint retained objects and distinct,
// known contexts. False includes unknown or too-large cases.
bool BdwExecSnapshotsIndependent(const BdwExecSnapshot*,const BdwExecSnapshot*);
// Signal fence identities are bound before queueing and retained until the
// snapshot is released. The submission owner must supply distinct, pending
// fences and publish them to sync objects only after successful enqueue.
bool BdwExecSnapshotAddSignalFence(BdwExecSnapshot* snapshot,BdwFence* fence);
// Admission-time relationship only: this snapshot retains both the exact
// writable buffer and the exact pending signal fence. Caller serializes
// snapshot mutation/release and must still establish exclusive presentation.
bool BdwExecSnapshotWritesWithSignal(const BdwExecSnapshot*,const BdwGpuBuffer*,const BdwFence*);
// Returns a retained pending signal for this snapshot's writable buffer, or
// null. Call before queue admission while snapshot mutation is serialized.
BdwFence* BdwExecSnapshotAcquireWriteSignal(const BdwExecSnapshot*,const BdwGpuBuffer*);
// Recognize a reserved buffer even when the request has no usable signal;
// routers must reject that case instead of falling back to ordinary submit.
bool BdwExecSnapshotContainsBuffer(const BdwExecSnapshot*,const BdwGpuBuffer*);
// Protected output is reserved for the compositor handoff; do not classify
// a submission containing one as ordinary application work.
bool BdwExecSnapshotHasProtectedBuffer(const BdwExecSnapshot*);
// Queue lock protects the snapshot and its immutable object list. The returned
// fence is retained and resolves only after this exact submission retires.
// It remains pending for uncertain device work. Null means no match/error.
BdwFence* BdwExecSnapshotAcquireCompletionForBuffer(const BdwExecSnapshot*,const BdwGpuBuffer*);
uint32_t BdwExecSnapshotBufferUseCount(const BdwExecSnapshot*,const BdwGpuBuffer*);
// Before queue admission, bind a previously reserved output to this exact
// writable snapshot and signal fence. Caller owns the output reservation
// beyond snapshot retirement and serializes binding, reserve and release.
// A failed reservation may consume the token; discard that snapshot and use a
// fresh reservation/token after safely releasing it rather than retrying it.
bool BdwExecSnapshotBindHandoff(BdwExecSnapshot*,BdwGpuBuffer*,BdwFence*,uint64_t token);
// Only for a rejected queue admission; caller owns and serializes snapshot.
bool BdwExecSnapshotHandoffUndelivered(const BdwExecSnapshot*,const BdwGpuBuffer*,uint64_t token);
bool BdwExecSnapshotSignalFences(BdwExecSnapshot* snapshot,bool success);
bool BdwExecSnapshotAddWaitFence(BdwExecSnapshot* snapshot,BdwFence* fence);
// 0 pending, 1 ready, 2 failed; fence status can only advance.
uint32_t BdwExecSnapshotDependencies(const BdwExecSnapshot* snapshot);
// Replace an exact fixed store probe with an immutable kernel-owned batch.
// This does not authorize arbitrary commands, DMA, or general 3D submission.
bool BdwExecSnapshotShadowStoreProbe(BdwExecSnapshot* snapshot);
#ifdef BROOK_HOST_TEST
uint64_t BdwExecSnapshotTestBatchKernelAddress(const BdwExecSnapshot* snapshot);
#endif
uint64_t BdwExecSnapshotBatchAddress(const BdwExecSnapshot* snapshot);
uint64_t BdwExecSnapshotBatchLength(const BdwExecSnapshot* snapshot);
uint32_t BdwExecSnapshotPageCount(const BdwExecSnapshot* snapshot);
// Immutable admission metadata suitable for bounded failure reporting. This
// does not read GPU-owned memory or establish that submitted work retired.
uint32_t BdwExecSnapshotObjectCount(const BdwExecSnapshot* snapshot);
// A bounded, best-effort CPU sample taken during admission, before GPU use.
// Concurrent writers can change the batch; this is not an execution trace.
struct BdwBatchSample {uint32_t bytes;uint32_t words[8];};
BdwBatchSample BdwExecSnapshotBatchSample(const BdwExecSnapshot* snapshot);
// A fresh, best-effort CPU observation; GPU/client writes can race this read.
BdwBatchSample BdwExecSnapshotCurrentBatchSample(const BdwExecSnapshot* snapshot);
// Failure-time, bounded CPU-visible samples of explicitly capture-marked
// buffers. GPU or CPU writes can race these reads; bytes are not atomic as a
// whole and may be stale. Requested may exceed count when truncated.
struct BdwCaptureEntry {uint64_t address,size;uint32_t bytes,reserved;uint8_t data[8192];};
struct BdwGpuCapture {uint32_t version,count,requested,reserved;BdwCaptureEntry entries[4];};
static_assert(sizeof(BdwCaptureEntry)==8216 && sizeof(BdwGpuCapture)==32880,"GPU capture in-memory layout");
void BdwExecSnapshotCapture(const BdwExecSnapshot* snapshot,BdwGpuCapture& out);
void BdwExecSnapshotSetOrigin(BdwExecSnapshot* snapshot,uint32_t pid,uint32_t contextId,uint64_t incarnation=0);
void BdwExecSnapshotOrigin(const BdwExecSnapshot* snapshot,uint32_t& pid,uint32_t& contextId);
uint64_t BdwExecSnapshotOwner(const BdwExecSnapshot* snapshot);
// Exact table-page count for the copied, sorted object addresses; the builder
// independently checks all backing pages and the caller's allocation.
bool BdwExecSnapshotTableCount(const BdwExecSnapshot* snapshot,uint32_t& out);
bool BdwExecSnapshotBuildPpgtt(BdwExecSnapshot* snapshot,const BdwPpgttPage (&scratch)[4],
                               const BdwPpgttPage* tables,uint32_t tableCount,
                               BdwPpgttMapping* workspace,uint32_t workspaceCount);
// Prepare matching PPGTT tables and an RCS ring from the same retained
// snapshot. The caller still owns every page, must publish them and establish
// command safety, device ownership, cache/DMA safety and reset handling.
// This does not arm the snapshot or submit anything to hardware.
bool BdwExecSnapshotBuildRenderImage(BdwExecSnapshot* snapshot,const BdwPpgttPage (&scratch)[4],
                                    const BdwPpgttPage* tables,uint32_t tableCount,
                                    BdwPpgttMapping* workspace,uint32_t workspaceCount,
                                    uint64_t completionGgtt,uint32_t nonce,bool gt3,uint32_t (&ring)[56]);
// CPU cache operations only; caller must exclude concurrent CPU access and
// independently establish device cache flushing and DMA ownership. CPU sync
// after GPU work requires a verified retirement/reset.
bool BdwExecSnapshotSyncForDevice(BdwExecSnapshot* snapshot);
bool BdwExecSnapshotSyncForCpu(BdwExecSnapshot* snapshot,bool safelyStopped);
// Mark every retained buffer unusable after a failed/uncertain GPU request or
// an aborted queued request. Reserved and armed snapshots are accepted.
// This does not retire the snapshot or release its pages.
bool BdwExecSnapshotMarkFailed(BdwExecSnapshot* snapshot,bool pending=false);
// Point-in-time Dell VT-d and backing ownership observation only. The caller
// must prove exact PCI/DMAR identity, keep all buffers pinned and serialize
// device/IOMMU changes throughout a later submission; this grants no DMA.
uint32_t BdwExecSnapshotInspectDma(BdwExecSnapshot* snapshot);
// Reserve buffer use before exposing a queued job to waiters. A reserved but
// unsubmitted snapshot may be cancelled and released without a device reset.
// The owner must serialize reserve, preparation, arm and release.
bool BdwExecSnapshotReserve(BdwExecSnapshot* snapshot);
// Arm immediately before the first possibly delivered GPU submission, after
// successful PPGTT construction, CPU cache sync and DMA snapshot. The owner
// must pass true only after separately publishing the pinned PPGTT/scratch
// pages and establishing device cache, command and ownership guarantees.
// These point-in-time checks do not replace the owner's continuous lease. A
// submitted snapshot can be released only with verified retirement/reset;
// otherwise all buffer references remain held until recovery. The owner must
// serialize build/arm/release and keep the snapshot itself alive across calls.
bool BdwExecSnapshotArm(BdwExecSnapshot* snapshot,bool tablesPublished);
// End the buffer-use reservation while retaining the snapshot record. Armed
// work requires a verified stop and CPU cache synchronization; a queued but
// unsubmitted reservation can be retired without a device stop.
bool BdwExecSnapshotRetire(BdwExecSnapshot* snapshot,bool safelyStopped);
bool BdwExecSnapshotRelease(BdwExecSnapshot* snapshot,bool safelyStopped);
}
