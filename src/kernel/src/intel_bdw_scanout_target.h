#pragma once
#include <stdint.h>
#include "intel_bdw_blitter.h"
#include "intel_bdw_ppgtt.h"
#include "intel_bdw_display_flip.h"
#include "intel_bdw_zero_copy.h"

namespace brook {
struct CompositorPresentRegion;
// Display surfaces Brook may legitimately leave on primary plane A. The GOP
// surface (GGTT offset 0) is always accepted; kernel flip buffers are added
// once mapped and stay for the boot (their GGTT mapping is boot-lifetime).
// Read lock-free by the per-job display invariant checks.
// Slots 0..2: M3 flip buffers; 3..6: zero-copy renderer images.
constexpr uint32_t BdwScanoutFlipSurfaceSlots=BdwFlipMaxSlots-1;
constexpr uint32_t BdwScanoutSurfaceSlots=BdwScanoutFlipSurfaceSlots+BdwZeroCopyMaxImages;
inline uint32_t g_bdwScanoutSurfaces[BdwScanoutSurfaceSlots]{};
inline bool BdwScanoutSurfaceAccepted(uint32_t surface){
    if(!surface)return true;
    for(auto& s:g_bdwScanoutSurfaces)if(__atomic_load_n(&s,__ATOMIC_ACQUIRE)==surface)return true;
    return false;
}
// Re-arm the firmware GOP surface. MMIO only and lock-free; a no-op unless a
// flip path has published its display mapping. Safe from panic and exception
// context: call before any drawing into the GOP framebuffer and before reset.
void BdwScanoutPanicRestore();

#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
// GPU virtual layout of the private BCS address space for persistent targets.
constexpr uint64_t BdwTargetGopAddress=0x04000000;    // == BdwBcsScanoutAddress
constexpr uint64_t BdwTargetFlipAddress=0x06000000;   // + slot-1 * 32 MiB
constexpr uint64_t BdwTargetStride=0x02000000;
enum class BdwTargetKind : uint8_t {Legacy,PersistentGop,Flip};
struct BdwTargetPlan {
    BdwTargetKind kind=BdwTargetKind::Legacy;
    const BdwPpgttLeaf* leaves=nullptr;uint32_t leafCount=0;
    BdwBlitBuffer destination{};
    BdwBlitRegion repairs[BdwFlipDamageMax]{};uint32_t repairCount=0;
    uint32_t slot=0;BdwFlipDamage damage{};
};
// Present worker only, after the existing BCS admission. Legacy means use
// the per-present GOP mapping; RepairRequired means the present must fail
// with a full software repair (the flip chain could not be trusted).
enum class BdwTargetSelect : uint8_t {Ok,RepairRequired};
BdwTargetSelect BdwScanoutTargetSelect(const CompositorPresentRegion* regions,uint32_t count,BdwTargetPlan& plan);
// After the device transaction (copied = the copy retired cleanly).
// A flip target is committed and armed (or queued); a failure freezes the
// chain, restores the GOP surface and requires a software repair.
bool BdwScanoutTargetFinish(BdwTargetPlan& plan,bool copied);
// Worker idle hook: observe an armed flip's latch and arm a queued frame.
void BdwScanoutFlipPoll();
// Software compositor, holding the scanout mapping lock with no GPU present
// in flight. Away = the display is (or may soon be) reading a kernel buffer,
// so the next software frame must be complete; ReturnToGop then arms the
// fully rewritten GOP.
bool BdwScanoutFlipAwayFromGop();
void BdwScanoutFlipReturnToGop();
// Debug-channel controls (runtime gates, off at every boot).
struct BdwScanoutTargetStatus {
    bool flipBuilt,mapBuilt,mapEnabled,flipRequested,flipActive,flipDisabled,probeSampled,probeOk;
    const char* reason;BdwFlipProbe probe;BdwFlipRefusal refusal;
    uint32_t front,latest,armed,queued,surfaces[BdwFlipMaxSlots-1];
    uint64_t gopReturns,activationUs;
    const char* activationStep;BdwFlipGgttDiag ggtt;
};
bool BdwScanoutFlipProbe(BdwFlipProbe& probe,BdwFlipRefusal& refusal);
bool BdwScanoutFlipArm(const char*& reason);
void BdwScanoutFlipDisarm();
bool BdwScanoutMapEnable(bool on);
void BdwScanoutTargetReadStatus(BdwScanoutTargetStatus& out);

// Zero-copy scanout of renderer images (BROOK_BDW_ZERO_COPY_SCANOUT, which
// requires BROOK_BDW_SCANOUT_FLIP; runtime gate "gpu zerocopy arm").
// See docs/bdw-zero-copy-scanout.md.
struct BdwGpuBuffer;
// Syscall context. Validates and retains the ring for the boot (one
// registration per boot in phase 1). Returns 0 or a negative errno.
int64_t BdwZeroCopyRegister(uint64_t owner,BdwGpuBuffer* const* buffers,uint32_t count,const BdwZeroCopyGeometry& geometry,
                            BdwZeroCopyRefusal& refusal);
// Handoff reservation hooks (sleepable handoff mutex held). Reserve refuses
// to let the GPU write an image the display may fetch.
bool BdwZeroCopyOnReserve(BdwGpuBuffer* buffer);
void BdwZeroCopyOnRelease(BdwGpuBuffer* buffer);
// Present worker, after the BCS admission: Copy = not an image or zero-copy
// inactive (do the normal BCS copy from it), Presented = armed or queued
// with no copy, RepairRequired = fail the present with a software repair.
enum class BdwZeroCopyOutcome : uint8_t {Copy,Presented,RepairRequired};
BdwZeroCopyOutcome BdwZeroCopyPresentSource(BdwGpuBuffer* source);
struct BdwZeroCopyStatus {
    bool built,registered,requested,active,disabled;uint32_t images,freeMask,front,armed,queued;
    uint64_t owner,sequence,setupUs,gopReturns,ages[BdwZeroCopyMaxImages];uint32_t surfaces[BdwZeroCopyMaxImages];
    const char* reason;BdwZeroCopyFallback last;BdwZeroCopyRefusal refusal;BdwZeroCopyStats stats;BdwFlipGgttDiag ggtt;
};
void BdwZeroCopyReadStatus(BdwZeroCopyStatus& out);
bool BdwZeroCopyArm(const char*& reason);
void BdwZeroCopyDisarm();
#endif
}
