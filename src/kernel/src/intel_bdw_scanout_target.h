#pragma once
#include <stdint.h>
#include "intel_bdw_blitter.h"
#include "intel_bdw_ppgtt.h"
#include "intel_bdw_display_flip.h"

namespace brook {
struct CompositorPresentRegion;
// Display surfaces Brook may legitimately leave on primary plane A. The GOP
// surface (GGTT offset 0) is always accepted; kernel flip buffers are added
// once mapped and stay for the boot (their GGTT mapping is boot-lifetime).
// Read lock-free by the per-job display invariant checks.
inline uint32_t g_bdwScanoutSurfaces[BdwFlipMaxSlots-1]{};
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
};
bool BdwScanoutFlipProbe(BdwFlipProbe& probe,BdwFlipRefusal& refusal);
bool BdwScanoutFlipArm(const char*& reason);
void BdwScanoutFlipDisarm();
bool BdwScanoutMapEnable(bool on);
void BdwScanoutTargetReadStatus(BdwScanoutTargetStatus& out);
#endif
}
