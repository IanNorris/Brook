#pragma once
#include "wm_capture_protection.h"
#include <stdint.h>
#include "intel_bdw_blitter.h"
#include "intel_bdw_ppgtt.h"
#include "intel_bdw_display_flip.h"
#include "intel_bdw_zero_copy.h"
#include "intel_bdw_client_scanout.h"
#include "intel_bdw_client_timeline.h"

struct BrookGpuClientScanoutStatus;

namespace brook {
struct BdwCursorStatus {
    bool requested=false, bound=false, active=false, armed=false, transparentTarget=false;
    uint64_t images=0, cachedArms=0, positions=0, fallbacks=0;
};
bool BdwCursorRequest(bool enable);
BdwCursorStatus BdwCursorReadStatus();
uint64_t BdwCursorRendererQuery(uint64_t owner,bool supportsHandoff,bool& suppress);
void BdwCursorRendererPresented(uint64_t owner,uint64_t epoch);
bool BdwCursorBlocksSoftware();
// Published only after both boot-lifetime cursor buffers are bound. The
// display safety check reads these without taking the scanout lock.
inline uint32_t g_bdwCursorBases[2]{};
inline bool BdwCursorBaseOwned(uint32_t base){
    if(!base)return false;
    for(const auto& owned:g_bdwCursorBases)
        if(__atomic_load_n(&owned,__ATOMIC_ACQUIRE)==base)return true;
    return false;
}
void BdwCursorUpdate(uint64_t leaseOwner,bool visible,int32_t x,int32_t y,
                     const uint32_t* pixels,uint32_t width,uint32_t height);

struct BdwGpuBuffer;
int64_t BdwClientScanoutPresent(uint64_t owner,BdwGpuBuffer*,const BdwClientScanoutLayout&,uint64_t offer,
                                 const CapturePrivacy&);
int64_t BdwClientScanoutStatus(uint64_t owner,BrookGpuClientScanoutStatus&);
int64_t BdwClientScanoutLeave(uint64_t owner);
bool BdwClientScanoutWorkPending();
bool BdwClientScanoutService(bool canPresent);
bool BdwClientScanoutWakeWorker();
bool BdwClientScanoutSetApPublication(bool enabled);
bool BdwClientScanoutApPublication();
bool BdwClientScanoutSetOffWorkerObservation(bool enabled);
bool BdwClientScanoutOffWorkerObservation();
uint32_t BdwClientScanoutReadTimeline(BdwClientTimelineEntry* out,uint32_t max);
// Address and layout are published together. Changes are permitted only
// after the worker proves the mapping is neither programmed nor live.
constexpr uint32_t BdwClientScanoutSlots=3;
inline uint64_t g_bdwClientSurface[BdwClientScanoutSlots]{};
inline bool BdwClientScanoutSurface(uint32_t base,uint32_t& control,uint32_t& stride){
    if(!base)return false;
    for(const auto& entry:g_bdwClientSurface){
        const uint64_t value=__atomic_load_n(&entry,__ATOMIC_ACQUIRE);
        if(uint32_t(value)!=base)continue;
        const BdwClientScanoutLayout layout{uint32_t(value>>32)&~0x80000000u,(value>>63)!=0};
        control=layout.Control();stride=layout.stride;
        return true;
    }
    return false;
}

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
    uint32_t control=0,stride=0;
    return BdwClientScanoutSurface(surface,control,stride);
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
bool BdwScanoutTargetFinish(BdwTargetPlan& plan,bool copied,const CapturePrivacy&);
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
// with no copy, Deferred = keep the current front and drop this frame,
// RepairRequired = fail the present with a software repair.
enum class BdwZeroCopyOutcome : uint8_t {Copy,Presented,RepairRequired,Deferred};
BdwZeroCopyOutcome BdwZeroCopyPresentSource(BdwGpuBuffer* source,const CompositorPresentRegion* regions,
                                             uint32_t regionCount,const CapturePrivacy&);
bool BdwZeroCopyCanHoldFront(uint64_t owner);
void BdwZeroCopyNoteWorkerWait();
// Present thread, before handing a present to the worker: in "flush" cache
// mode CLFLUSH the image's drawn lines there, off the gpu-submit worker.
void BdwZeroCopyPrepareScanout(BdwGpuBuffer* source,const CompositorPresentRegion* regions,uint32_t count);
struct BdwZeroCopyStatus {
    bool built,registered,requested,active,disabled;uint32_t images,freeMask,front,armed,queued;
    uint64_t owner,sequence,setupUs,gopReturns,ages[BdwZeroCopyMaxImages];uint32_t surfaces[BdwZeroCopyMaxImages];
    const char* reason;BdwZeroCopyFallback last;BdwZeroCopyRefusal refusal;BdwZeroCopyStats stats;BdwFlipGgttDiag ggtt;
    bool uncachedRender;uint64_t renderJobsUc,renderJobsWb;
};
void BdwZeroCopyReadStatus(BdwZeroCopyStatus& out);
// Copies up to max recent timeline entries, oldest first; khz converts TSC.
uint32_t BdwZeroCopyReadTimeline(BdwZeroCopyTimelineEntry* out,uint32_t max,uint64_t& khz);
// Syscall context: hand the retained images to a restarted renderer. On 0
// out[0..count) carry a reference each (the caller publishes fds and drops
// them). -2 none registered, -1 not the attached owner, -16 still in use.
int64_t BdwZeroCopyAdopt(uint64_t owner,BdwGpuBuffer** out,uint32_t& count);
bool BdwZeroCopyArm(const char*& reason);
void BdwZeroCopyDisarm();
// Auto-arm (wt cache mode) when the renderer registers its ring. On by
// default; /etc/dell-zero-copy-no-auto opts out at boot and `gpu zerocopy off`
// cancels it for the boot. Arming still requires a passing flip probe.
void BdwZeroCopySetAutoArm(bool enabled);
bool BdwZeroCopyAutoArm();
// Render jobs writing images lease PPAT slot 3 UC (default on). The toggle
// exists only to A/B the coherency fix ("gpu zerocopy uc on|off").
bool BdwZeroCopyUncachedRender();
uint8_t BdwZeroCopyScanoutPat();
// 0 "flush" (default), 1 "uc", 2 "wt"; false for an unknown mode.
bool BdwZeroCopySetCacheMode(uint32_t mode);
const char* BdwZeroCopyCacheModeName();
void BdwZeroCopySetUncachedRender(bool on);
void BdwZeroCopyNoteRender(bool uncached);
// "gpu zerocopy verify": objective coherency check of the front image with
// the pipe A CRC (primary plane source). Two stable CRCs, then a CLFLUSH of
// every line of the front image, then two more. A display that already saw
// the CPU-coherent pixels gives the same CRC; a changed CRC proves the panel
// was scanning stale memory. Needs zero-copy active and an idle desktop
// (the front image must not change during the ~0.25 s measurement).
struct BdwZeroCopyVerify {
    const char* result;uint32_t front;uint64_t sequence;
    uint32_t before[2],after[2],flushedPages;bool stableBefore,stableAfter,changed;
};
bool BdwZeroCopyVerifyFront(BdwZeroCopyVerify& out);

// Debug screenshot of what primary plane A is fetching (DSPASURFLIVE).
// Compositor thread, inside its scanout mapping lock. Gop: the plane shows
// the firmware GOP (or no flip path ever moved it), so read the GOP
// framebuffer. Copied: destination holds width x height packed B,G,R,X rows
// of the live kernel flip buffer, renderer image or direct client buffer
// (X-tiled surfaces detiled). The copied image is held off the display
// queue for the copy (presents are mailboxed, not armed, for ~1-2 frames),
// so it cannot be rewritten underneath. Busy: the image left the display
// during the copy; retry at the next frame. Unsupported: unknown surface,
// plane offset or pixel format.
enum class BdwScanoutCaptureKind : uint8_t {Gop,Copied,Busy,Unsupported};
struct BdwScanoutCaptureInfo {
    uint32_t plane=0,surface=0,control=0,stride=0,flags=0,copyUs=0;
    CapturePrivacy privacy;
};
BdwScanoutCaptureKind BdwScanoutCaptureLive(uint32_t* destination,uint32_t width,uint32_t height,
                                                BdwScanoutCaptureInfo& info,uint64_t protectionEpoch);
// Composites the visible hardware cursor plane (premultiplied ARGB) over a
// captured frame. Returns true when a cursor image was on the plane.
bool BdwScanoutCaptureCursor(uint32_t* destination,uint32_t width,uint32_t height);
#endif
}
