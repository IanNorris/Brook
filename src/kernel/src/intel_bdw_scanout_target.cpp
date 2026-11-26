#include "kprintf.h"
// Persistent scanout targets for the Dell Broadwell BCS present path:
//  - BROOK_BDW_PERSISTENT_SCANOUT_MAP: map the firmware GOP into the private
//    BCS address space once (persistent leaf tables) instead of rebuilding
//    ~5,600 destination PTEs and re-reading every GOP GGTT entry per present.
//  - BROOK_BDW_SCANOUT_FLIP: three kernel-owned scanout buffers mapped in
//    the GGTT; each present blits once into a back buffer and arms DSPASURF,
//    latched at vblank. The GOP stays the fallback and the panic surface.
// Both are compiled out by default and runtime-gated off at every boot (the
// gates are debug-channel commands and live only in RAM). See
// docs/bdw-page-flip.md for the evidence, the state machine and the plan.
#include "intel_bdw_scanout_target.h"
#include "intel_bdw_scanout_capture.h"
#include "string.h"
#include "intel_bdw_buffer.h"
#include "intel_bdw_scanout.h"
#include "intel_bdw_oneshot.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_display_probe.h"
#include "intel_bdw_cursor.h"
#include "intel_bdw_client_scanout.h"
#include "gpu_output_abi.h"
#include "desktop_present_policy.h"
#include "intel_bdw_ggtt_extent_bind.h"
#include "intel_bdw_critical.h"
#include "intel_bdw_worker_stats.h"
#include "intel_bdw_forcewake_adapter.h"
#include "compositor.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "native_tsc_observe.h"
#include "tsc_clock.h"
#include "boot_protocol/boot_protocol.h"
#include "acpi.h"
#include "apic.h"
#include "spinlock.h"
#include "scheduler.h"
#include "process.h"
#include "serial.h"
#include "smp.h"
#include "sync/ksemaphore.h"

namespace brook {
namespace {
// UC alias of BAR0+0x70000 (pipe A frame counter and primary plane). Published
// only once a flip path may have written DSPASURF; boot lifetime.
volatile uint32_t* g_planePage=nullptr;
bool g_cursorTouched=false;
bool g_clientPlaneTouched=false;

uint64_t g_restoreBudgetTicks=0;
inline uint64_t Rdtsc(){uint32_t lo,hi;__asm__ volatile("lfence; rdtsc":"=a"(lo),"=d"(hi)::"memory");return (uint64_t(hi)<<32)|lo;}
struct PlaneIo {
    volatile uint32_t* page;
    uint32_t read32(uint32_t offset){return page[(offset&4095)/4];}
    void write32(uint32_t offset,uint32_t value){page[(offset&4095)/4]=value;__asm__ volatile("mfence":::"memory");}
    uint64_t now(){return Rdtsc();}
    void relax(){__asm__ volatile("pause");}
};
}
void BdwScanoutPanicRestore(){
    auto* page=__atomic_load_n(&g_planePage,__ATOMIC_ACQUIRE);
    if(!page)return;
    PlaneIo io{page};
    if(__atomic_load_n(&g_cursorTouched,__ATOMIC_ACQUIRE))BdwCursorDisable(io);
    const uint64_t budget=__atomic_load_n(&g_restoreBudgetTicks,__ATOMIC_RELAXED);
    const uint32_t control=io.read32(BdwFlipRegs::PlaneControl);
    const uint32_t programmed=io.read32(BdwFlipRegs::Surface);
    const uint32_t live=io.read32(BdwFlipRegs::SurfaceLive);
    if(__atomic_load_n(&g_clientPlaneTouched,__ATOMIC_ACQUIRE) &&
       (control==BdwFlipRegs::GopControl || control==(BdwFlipRegs::GopControl|BdwFlipRegs::TiledBit)) &&
       BdwScanoutSurfaceAccepted(programmed) && BdwScanoutSurfaceAccepted(live)){
        BdwClientScanoutFlip restore;
        if(restore.Restore(io,0)){
            const uint64_t start=io.now();
            while(!restore.Poll(io) && io.now()-start<budget)io.relax();
        }
        return;
    }
    (void)BdwFlipRestoreGop(io,0,budget);
}

#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST) && \
    (defined(BROOK_BDW_PERSISTENT_SCANOUT_MAP) || defined(BROOK_BDW_SCANOUT_FLIP))
namespace {
constexpr uint32_t SurfacePages=BdwDellGopPages;
constexpr uint64_t SurfaceBytes=uint64_t(SurfacePages)*4096;
constexpr uint32_t LeafTables=(SurfacePages+511)/512;
struct Leaves {BdwOwnedPage tables[LeafTables]{};BdwPpgttLeaf links[LeafTables]{};uint32_t count=0;bool built=false;};
BdwOwnedPage zeroPage{};bool zeroReady=false;
Leaves gop;uint64_t gopBase=0;BdwGopPtes gopPtes{};BdwPersistentMapPolicy gopPolicy;
bool mapEnabled=false; // runtime gate: "gpu scanout map on"

void Stat(BdwWorkerScanoutEvent event){
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    BdwWorkerStatsScanout(event);
#else
    (void)event;
#endif
}
bool EnsureZero(){
    if(zeroReady)return true;
    if(!BdwPrivatePageAlloc(zeroPage) || !BdwPrivatePagesPublish(&zeroPage,1))return false;
    zeroReady=true;return true;
}
// Pages stay allocated on failure: they are boot-lifetime and never handed to
// the device, so leaking them is the safe outcome.
template<class PageOf> bool BuildLeaves(Leaves& l,uint64_t address,PageOf pageOf,uint32_t pages){
    if(l.built)return true;
    const uint32_t n=(pages+511)/512;
    if(!EnsureZero() || !n || n>LeafTables)return false;
    for(uint32_t i=0;i<n;++i)if(!l.tables[i].dma && !BdwPrivatePageAlloc(l.tables[i]))return false;
    BdwPpgttPage tables[LeafTables]{};
    for(uint32_t i=0;i<n;++i)tables[i]={l.tables[i].dma,l.tables[i].cpu};
    if(!BdwFillPpgttLeaves(zeroPage.dma,tables,n,pageOf,pages,BdwPpgttCache::Uncached,true) ||
       !BdwPrivatePagesPublish(l.tables,n))return false;
    for(uint32_t i=0;i<n;++i)l.links[i]={address+(uint64_t(i)<<21),l.tables[i].dma};
    l.count=n;l.built=true;return true;
}
bool LeavesOwned(const Leaves& l){
    if(!l.built)return false;
    for(uint32_t i=0;i<l.count;++i)if(!BdwPrivatePageOwned(l.tables[i]))return false;
    return true;
}
// Worker only. The cached GOP leaves may be used when the display is still
// in the GOP mode and the first/last GOP PTEs are unchanged; every 64th use
// repeats the complete twice-read of all GOP PTEs (BdwObserveGopBacking).
bool ValidateGop(){
    if(gopPolicy.NeedFull()){
        BdwGopBacking backing{};BdwGopPtes ptes{};
        bool ok=BdwObserveGopBacking(backing) && backing.bytes==SurfaceBytes &&
            backing.physicalBase>=0xdd000000ull && backing.physicalBase<=0xdf800000ull-SurfaceBytes &&
            (!gopBase || backing.physicalBase==gopBase) && BdwObserveGopPtesLive(ptes);
        if(ok && !gop.built){
            const uint64_t base=backing.physicalBase;
            ok=BuildLeaves(gop,BdwTargetGopAddress,[base](uint32_t i){return base+uint64_t(i)*4096;},SurfacePages);
        }
        ok=ok && LeavesOwned(gop);
        if(ok){gopBase=backing.physicalBase;gopPtes=ptes;}
        gopPolicy.Full(ok);
        Stat(ok?BdwWorkerScanoutEvent::MapFull:BdwWorkerScanoutEvent::MapFailed);
        return ok;
    }
    BdwDisplaySnapshot display{};BdwGopPtes ptes{};
    const bool ok=BdwObserveDisplayAccepted(display) && BdwObserveGopPtesLive(ptes) &&
        ptes.first==gopPtes.first && ptes.last==gopPtes.last && LeavesOwned(gop);
    if(ok)gopPolicy.Hit();else gopPolicy.Invalidate();
    Stat(ok?BdwWorkerScanoutEvent::MapHit:BdwWorkerScanoutEvent::MapFailed);
    return ok;
}
void PersistentGopPlan(BdwTargetPlan& plan){
    plan.kind=BdwTargetKind::PersistentGop;plan.leaves=gop.links;plan.leafCount=gop.count;
    plan.destination={BdwTargetGopAddress,SurfaceBytes,12800};
}

#if defined(BROOK_BDW_SCANOUT_FLIP)
// Guards the chain and flip state; never held across allocation, GGTT
// binding or the device transaction. Taken by the present worker and by the
// software compositor (inside its scanout mapping lock, never the reverse).
IrqSpinLock lock{};
// Debug screenshot in progress (BdwScanoutCaptureLive): latches are not
// processed and no new surface is armed, so the image being copied stays
// front (or armed) and no writer can reserve it. Presents still mailbox.
bool captureHold=false;
constexpr uint32_t FlipBuffers=3,ExtentPages=SurfacePages+BdwFlipRegs::GuardPages;
static_assert(FlipBuffers+1<=BdwFlipMaxSlots && (FlipBuffers+1)*LeafTables<=BdwPpgttMaxLeaves);
struct FlipBuffer {uint64_t* dma=nullptr;uint64_t* snapshot=nullptr;Leaves leaves;BdwGgttReservation reservation{};
    BdwGgttExtentBind bind;bool bound=false;CapturePrivacy privacy{};};
BdwOwnedPage takeoverScratch{};bool takeoverReady=false;
BdwFlipGgttDiag ggttDiag{};const char* activationStep="none";
FlipBuffer buffers[FlipBuffers];
BdwFlipChain chain;
BdwPpgttLeaf allLeaves[BdwPpgttMaxLeaves]{};uint32_t allLeafCount=0;
constexpr uint32_t DisplayPages[]={0x60000,0x6f000,0x70000,0x71000,0x72000,0x7f000};
volatile uint32_t* displayRegs[6]{};bool displayMapped=false;
bool flipRequested=false,flipActive=false,flipDisabled=false,probeSampled=false,probeOk=false;
// After a disable the GOP may be stale even once the plane latched it again
// (frames went to kernel buffers): owe the compositor one full repaint.
bool repairOwed=false;
const char* flipReason="not requested";const char* pendingLog=nullptr;
BdwFlipProbe lastProbe{};BdwFlipRefusal lastRefusal=BdwFlipRefusal::NoSample;
uint64_t gopReturns=0,activationUs=0,tscKhz=0;

volatile uint32_t* MapUc(uint64_t physical){
    const auto address=VmmAllocPages(1,VMM_WRITABLE|VMM_NO_EXEC,MemTag::Device,KernelPid);
    if(!address)return nullptr;
    const auto leaf=VmmDetachPage(KernelPageTable,address);
    if(leaf)PmmFreePage(VmmLeafPhysical(leaf));
    if(!VmmMapPage(KernelPageTable,address,PhysicalAddress(physical),
        VMM_WRITABLE|VMM_NO_EXEC|VMM_CACHE_DISABLE|VMM_WRITE_THRU,MemTag::Device,KernelPid)){
        VmmFreePages(address,1);return nullptr;
    }
    return reinterpret_cast<volatile uint32_t*>(address.raw()); // boot-lifetime borrowed MMIO
}
bool MapDisplay(){
    if(displayMapped)return true;
    constexpr uint32_t allowed=(1u<<uint32_t(MemoryType::Reserved))|(1u<<uint32_t(MemoryType::Mmio));
    for(auto offset:DisplayPages){
        BootResourceClassification c{};
        if(!AcpiClassifyBootRange(BdwDellBar0+offset,4096,c) ||
           !((!c.types && c.unreported) || (!c.unreported && c.types && !(c.types&~allowed))))return false;
    }
    for(unsigned i=0;i<6;++i)if(!displayRegs[i] && !(displayRegs[i]=MapUc(BdwDellBar0+DisplayPages[i])))return false;
    displayMapped=true;return true;
}
uint32_t Read(uint32_t offset){
    for(unsigned i=0;i<6;++i)if((offset&~4095u)==DisplayPages[i] && displayRegs[i]){
        __asm__ volatile("lfence":::"memory");return displayRegs[i][(offset&4095)/4];
    }
    return 0xffffffffu;
}
uint64_t AddressOf(uint32_t slot){return slot?BdwTargetFlipAddress+uint64_t(slot-1)*BdwTargetStride:BdwTargetGopAddress;}
void DisableLocked(const char* why){
    if(!flipDisabled){flipDisabled=true;flipReason=why;pendingLog=why;repairOwed=flipActive;}
    chain.MarkUncertain();
    // Best effort, never waits here: re-arm the GOP; the compositor's full
    // software repaint follows from the RepairRequired the caller returns.
    if(displayMapped){PlaneIo io{displayRegs[2]};(void)BdwFlipRestoreGop(io,0,0);}
    Stat(BdwWorkerScanoutEvent::FlipUncertain);
}
void LogPending(){
    if(const char* why=__atomic_exchange_n(&pendingLog,static_cast<const char*>(nullptr),__ATOMIC_ACQ_REL))
        SerialPrintf("gpu-flip: disabled for this boot (%s); BCS copy to the GOP resumes after a full repaint\n",why);
}
// Arm DSPASURF for a slot the chain designated. The plane must still be in
// the GOP mode (no firmware modeset since the probe) or flipping stops.
bool ArmLocked(uint32_t slot){
    if(Read(BdwFlipRegs::PlaneControl)!=BdwFlipRegs::GopControl || Read(BdwFlipRegs::Stride)!=12800){
        DisableLocked("plane-changed");return false;
    }
    PlaneIo io{displayRegs[2]};
    const uint32_t frame=io.read32(BdwFlipRegs::FrameCountA);
    io.write32(BdwFlipRegs::Surface,chain.Surface(slot));
    (void)io.read32(BdwFlipRegs::Surface);
    if(!chain.Armed(slot,frame,Rdtsc())){DisableLocked("arm-rejected");return false;}
    Stat(BdwWorkerScanoutEvent::FlipArmed);
    return true;
}
void PollLocked(){
    if(!flipActive || flipDisabled || captureHold)return;
    if(chain.HasArmed()){
        const uint32_t frame=Read(BdwFlipRegs::FrameCountA),live=Read(BdwFlipRegs::SurfaceLive);
        BdwFlipLatch latch{};
        switch(chain.Poll(frame,live,Rdtsc(),latch)){
        case BdwFlipPoll::Latched:
            CompositorRecordDesktopPresent();
#if defined(BROOK_GPU_SUBMISSION_TIMING)
            BdwWorkerStatsFlipLatched(latch.frames,latch.ticks);
#endif
            break;
        case BdwFlipPoll::Uncertain:DisableLocked("latch-uncertain");return;
        default:return;
        }
    }
    uint32_t queued=0;
    if(chain.TakeQueued(queued))(void)ArmLocked(queued);
}
// Worker only, outside the lock (allocation, cache flushes and GGTT writes).
bool Activate(){
    const uint64_t start=Rdtsc();
    auto fail=[](const char* why){
        const uint64_t f=IrqSpinLockAcquire(&lock);DisableLocked(why);activationStep=why;IrqSpinLockRelease(&lock,f);
        SerialPrintf("gpu-flip: activation failed at %s; ggtt %s buffer=%u first=%u count=%u entries=%u zero=%u valid=%u other=%u nonzero@%u=%lx page=%u\n",
            why,BdwFlipGgttStageName(ggttDiag.stage),ggttDiag.buffer,ggttDiag.first,ggttDiag.count,ggttDiag.entries,
            ggttDiag.zero,ggttDiag.valid,ggttDiag.other,ggttDiag.firstNonzero,ggttDiag.firstNonzeroValue,ggttDiag.auditPage);
        LogPending();return false;
    };
    if(!MapDisplay())return fail("display-map");
    if(!ValidateGop())return false; // transient: retried by the next present
    if(!EnsureZero())return fail("alloc");
    // Distinct from the guard/zero page, which every buffer maps as data.
    if(!takeoverReady){
        if(!BdwPrivatePageAlloc(takeoverScratch) || !BdwPrivatePagesPublish(&takeoverScratch,1))return fail("alloc");
        takeoverReady=true;
    }
    uint32_t surfaces[FlipBuffers]{};
    for(uint32_t b=0;b<FlipBuffers;++b){
        auto& buffer=buffers[b];
        if(!buffer.dma){
            auto* dma=static_cast<uint64_t*>(kmalloc(uint64_t(ExtentPages)*sizeof(uint64_t)));
            if(!dma)return fail("alloc");
            // Not zeroed: every pixel is written by the blitter before the
            // buffer is first armed (it starts fully stale). Flush any dirty
            // lines a previous owner left, so no write-back lands later.
            BdwOwnedPage chunk[64];unsigned n=0;
            for(uint32_t i=0;i<SurfacePages;++i){
                BdwOwnedPage page{};if(!BdwPrivatePageAlloc(page,false))return fail("alloc");
                dma[i]=page.dma;chunk[n++]=page;
                if(n==64 || i+1==SurfacePages){if(!BdwPrivatePagesPublish(chunk,n))return fail("publish");n=0;}
            }
            for(uint32_t i=SurfacePages;i<ExtentPages;++i)dma[i]=zeroPage.dma; // display guard PTEs
            buffer.dma=dma;
        }
        const uint64_t* pages=buffer.dma;
        if(!BuildLeaves(buffer.leaves,AddressOf(b+1),[pages](uint32_t i){return pages[i];},SurfacePages))return fail("leaves");
        // The GGTT lives in stolen memory and survives warm resets: on the
        // Dell every entry above the GOP holds leftovers from an earlier OS
        // (gen 186 survey: no zero entries at 66048+). Bind against a stable
        // snapshot, taking over inherited values as i915 does at load.
        if(!buffer.snapshot && !(buffer.snapshot=static_cast<uint64_t*>(kmalloc(uint64_t(ExtentPages)*sizeof(uint64_t)))))
            return fail("alloc");
        ggttDiag={};ggttDiag.buffer=b;ggttDiag.entries=BdwGlobalGgttSpace().Limit();ggttDiag.count=ExtentPages;
        if(!buffer.reservation.token &&
           !BdwGlobalGgttSpace().reserveExtent(ExtentPages,BdwFlipRegs::SurfaceAlignment/4096,buffer.reservation)){
            ggttDiag.stage=BdwFlipGgttStage::Reserve;return fail("ggtt-space");
        }
        {
            const BdwGgttExtentLease lease{buffer.reservation,buffer.dma};
            buffer.bound=VmmDellFlipGgttUpdate(buffer.bind,lease,buffer.snapshot,takeoverScratch.dma,ggttDiag);
        }
        ggttDiag.entries=BdwGlobalGgttSpace().Limit();
        if(!buffer.bound && ggttDiag.stage==BdwFlipGgttStage::Uncertain)return fail("ggtt-bind-uncertain");
        if(!buffer.bound)return fail("ggtt-bind");
        surfaces[b]=buffer.reservation.first*4096;
    }
    allLeafCount=0;
    for(uint32_t i=0;i<gop.count;++i)allLeaves[allLeafCount++]=gop.links[i];
    for(auto& buffer:buffers)for(uint32_t i=0;i<buffer.leaves.count;++i)allLeaves[allLeafCount++]=buffer.leaves.links[i];
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const bool configured=chain.Configure(0,surfaces,FlipBuffers);
    if(configured){
        // Publish before the first DSPASURF write: the per-job invariant
        // accepts these surfaces and panic/reset can restore the GOP.
        for(uint32_t b=0;b<FlipBuffers;++b)__atomic_store_n(&g_bdwScanoutSurfaces[b],surfaces[b],__ATOMIC_RELEASE);
        __atomic_store_n(&g_restoreBudgetTicks,tscKhz*50,__ATOMIC_RELAXED); // 50 ms: three frames
        __atomic_store_n(&g_planePage,displayRegs[2],__ATOMIC_RELEASE);
        flipActive=true;
        activationUs=tscKhz?(Rdtsc()-start)*1000/tscKhz:0;
    }
    IrqSpinLockRelease(&lock,f);
    if(!configured)return fail("chain");
    {const uint64_t g=IrqSpinLockAcquire(&lock);activationStep="active";IrqSpinLockRelease(&lock,g);}
    SerialPrintf("gpu-flip: active; surfaces %x %x %x; setup %u us\n",surfaces[0],surfaces[1],surfaces[2],unsigned(activationUs));
    return true;
}
void FlipPlan(uint32_t slot,const CompositorPresentRegion* regions,uint32_t count,BdwTargetPlan& plan){
    plan.kind=BdwTargetKind::Flip;plan.slot=slot;plan.leaves=allLeaves;plan.leafCount=allLeafCount;
    plan.destination={AddressOf(slot),SurfaceBytes,12800};
    const uint32_t latest=chain.Latest();
    const BdwBlitBuffer source{AddressOf(latest),SurfaceBytes,12800};
    const auto& stale=chain.Stale(slot);
    plan.repairCount=0;
    bool covered=false;
    for(uint32_t i=0;i<count;++i)
        if(!regions[i].x && !regions[i].y && regions[i].width==3200 && regions[i].height==1800)covered=true;
    if(slot!=latest && !covered){
        if(stale.full)plan.repairs[plan.repairCount++]={source,plan.destination,0,0,0,0,3200,1800};
        else for(uint32_t i=0;i<stale.count;++i){const auto& r=stale.rects[i];
            plan.repairs[plan.repairCount++]={source,plan.destination,r.x,r.y,r.x,r.y,r.width,r.height};}
    }
    for(uint32_t i=0;i<count;++i)plan.damage.Add({regions[i].x,regions[i].y,regions[i].width,regions[i].height});
}
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
// Zero-copy scanout: DSPASURF points at the renderer's own protected output
// images (docs/bdw-zero-copy-scanout.md). Shares the flip lock, the display
// mapping, the probe and the takeover scratch page with the M3 flip; the two
// are mutually exclusive at runtime. Images and their GGTT extents are
// retained for the boot once registered (phase 1: one registration per boot,
// because the extent binder can only retire through the Io that bound it).
constexpr uint32_t ZcExtentPages=SurfacePages+BdwFlipRegs::GuardPages;
static_assert(BdwZeroCopyImageBytes==SurfaceBytes,"zero-copy images are exactly the GOP mode");
struct ZcImage {BdwGpuBuffer* buffer=nullptr;uint64_t identity=0;uint64_t* dma=nullptr;uint64_t* snapshot=nullptr;
    BdwGgttReservation reservation{};BdwGgttExtentBind bind;bool bound=false;CapturePrivacy privacy{};};
ZcImage zcImages[BdwZeroCopyMaxImages];
uint32_t zcCount=0;uint64_t zcOwner=0;
bool zcAutoArm=false;
BdwZeroCopyRing zcRing;
bool zcRequested=false,zcActive=false,zcDisabled=false,zcRepairOwed=false,zcBound=false;
const char* zcReason="not requested";const char* zcPendingLog=nullptr;
BdwZeroCopyStats zcStats{};BdwZeroCopyFallback zcLast=BdwZeroCopyFallback::Disarmed;
BdwZeroCopyRefusal zcRefusal=BdwZeroCopyRefusal::None;
BdwFlipGgttDiag zcGgtt{};uint64_t zcSetupUs=0,zcGopReturns=0;
// Cache policy for render writes to images (runtime, "gpu zerocopy cache"):
// flush = write-back + CLFLUSH of drawn lines before arming (default),
// uc = PPAT slot 3 uncached for jobs writing images, wt = write-through.
enum : uint32_t {ZcCacheFlush=0,ZcCacheUc=1,ZcCacheWt=2};
// Default wt (gen 379: equal to uc, no flush cost); flush is opt-in.
uint32_t zcCacheMode=ZcCacheWt;uint32_t zcFlushedOnce=0;
bool zcVerifyHold=false; // "gpu zerocopy verify": keep the front image still
// Per-present timeline (TSC), "gpu zerocopy timeline": present request,
// DSPASURF write, observed latch. Indexed by present sequence.
constexpr uint32_t ZcTimelineSize=64;
BdwZeroCopyTimelineEntry zcTimeline[ZcTimelineSize]{};uint64_t zcSlotSequence[BdwZeroCopyMaxImages+1]{};
BdwZeroCopyTimelineEntry* ZcTimelineFor(uint32_t slot){
    if(!slot || slot>BdwZeroCopyMaxImages || !zcSlotSequence[slot])return nullptr;
    auto& e=zcTimeline[zcSlotSequence[slot]%ZcTimelineSize];return e.sequence==zcSlotSequence[slot]?&e:nullptr;
}

uint32_t ZcSlotOf(const BdwGpuBuffer* b){
    if(!b)return 0;
    for(uint32_t i=0;i<zcCount;++i)if(zcImages[i].buffer==b)return i+1;
    return 0;
}
// Both images and their trailing safe PTEs remain pinned until reboot.
constexpr uint32_t CursorImagePages=4,CursorExtentPages=CursorImagePages+BdwFlipRegs::GuardPages;
struct CursorBacking {
    BdwOwnedPage pixels[CursorImagePages]{};
    uint64_t dma[CursorExtentPages]{}, snapshot[CursorExtentPages]{};
    BdwGgttReservation reservation{};
    BdwGgttExtentBind bind;
};
CursorBacking cursorBacking[2];
BdwCursorPlane cursorPlane;
BdwCursorHandoff cursorHandoff;
BdwCursorStatus cursorStatus;
bool cursorSetupFailed=false,cursorMayBeVisible=false,zcLeavingForGop=false;
BdwCursorImages cursorImages;

void CursorOffLocked(bool failed=false) {
    const bool enabled=cursorPlane.enabled();
    if(failed && (enabled || cursorHandoff.inProgress()))++cursorStatus.fallbacks;
    cursorHandoff.cancel();
    if(enabled){
        PlaneIo io{displayRegs[2]};
        cursorPlane.disable(io,io.read32(BdwFlipRegs::FrameCountA));
    }
    cursorStatus.active=false;
}
bool CursorPollLocked(){
    if(cursorPlane.blocksSoftware()){
        PlaneIo io{displayRegs[2]};
        if(!cursorPlane.poll(io,io.read32(BdwFlipRegs::FrameCountA))){
            CursorOffLocked(true);
            cursorSetupFailed=true;cursorStatus.requested=false;
            return false;
        }
    }
    if(!cursorPlane.blocksSoftware())cursorMayBeVisible=false;
    if(cursorPlane.ready() && cursorPlane.active())cursorHandoff.latched();
    if(cursorHandoff.inProgress() && !cursorHandoff.owned() && cursorHandoff.expired(ApicTickCount())){
        CursorOffLocked(true);
        cursorStatus.requested=false;
        return false;
    }
    cursorStatus.active=cursorHandoff.owned() && cursorPlane.active();
    return true;
}
// Called only by the BSP submission worker after it has bound the desktop
// ring, so the scratch page, zero page and display mapping are valid.
void CursorBindIfRequested() {
    {const uint64_t f=IrqSpinLockAcquire(&lock);
     const bool setup=cursorStatus.requested && !cursorStatus.bound && !cursorSetupFailed && zcActive;
     IrqSpinLockRelease(&lock,f);
     if(!setup)return;}
    bool ok=true;
    for(auto& image:cursorBacking){
        for(auto& page:image.pixels)if(!BdwPrivatePageAlloc(page)){ok=false;break;}
        if(!ok || !BdwPrivatePagesPublish(image.pixels,CursorImagePages)){ok=false;break;}
        for(uint32_t i=0;i<CursorExtentPages;++i)
            image.dma[i]=i<CursorImagePages ? image.pixels[i].dma : zeroPage.dma;
        constexpr uint32_t alignmentPages=BdwFlipRegs::SurfaceAlignment/4096;
        if(!BdwGlobalGgttSpace().reserveExtent(CursorExtentPages,alignmentPages,image.reservation)){
            ok=false;break;
        }
        const BdwGgttExtentLease lease{image.reservation,image.dma};
        BdwFlipGgttDiag diag{};
        if(!VmmDellFlipGgttUpdate(image.bind,lease,image.snapshot,takeoverScratch.dma,diag)){
            ok=false;break;
        }
    }
    const uint64_t f=IrqSpinLockAcquire(&lock);
    if(ok){
        for(uint32_t slot=0;slot<2;++slot)
            __atomic_store_n(&g_bdwCursorBases[slot],cursorBacking[slot].reservation.first*4096,__ATOMIC_RELEASE);
    }
    cursorStatus.bound=ok;cursorSetupFailed=!ok;
    IrqSpinLockRelease(&lock,f);
}


struct ClientMapping {
    BdwGpuBuffer* buffer=nullptr;
    BdwClientScanoutLayout layout{};
    BdwGgttReservation reservation{};
    BdwGgttExtentBind binding;
    uint64_t* dma=nullptr;
    uint64_t* snapshot=nullptr;
    bool held=false;
    bool quarantined=false;
    CapturePrivacy privacy{};
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    uint64_t timelineSequence=0;
#endif
    uint32_t Base() const {return reservation.first*4096;}
};
ClientMapping clientMappings[BdwClientScanoutSlots];
BdwClientScanoutFlip clientFlip;
struct ClientRequest {
    BdwGpuBuffer* buffer=nullptr;
    BdwClientScanoutLayout layout{};
    uint64_t owner=0,offer=0;
    bool publishOnAp=false;
    BdwClientPublication publication{};
    uint64_t flushTicks=0;
    CapturePrivacy privacy{};
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    uint64_t timelineSequence=0;
#endif
} clientRequest;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
BdwClientTimeline clientTimeline;
uint64_t clientArmedSequence=0;
#endif
KSemaphore clientPublicationWake{};
uint32_t clientPublicationStarted=0;
bool clientPublishOnAp=false;
BdwClientLatchObserver clientObserver;
KSemaphore clientObserverWake{};
uint32_t clientObserverStarted=0;
uint64_t clientPresents=0,clientLatched=0,clientRestores=0,clientFailedOffer=0;
int32_t clientError=0;
bool clientLeaving=false,clientGopRestore=false;

bool ClientOn(){return DesktopPresentPolicy()&BROOK_DESKTOP_POLICY_DIRECT_ON;}
bool ClientShowing(){return clientFlip.Front().base || clientFlip.Armed().base || clientFlip.Busy();}

void ClientRestoreLocked(bool gopTarget){
    if(clientRequest.buffer)clientRequest.publication.Cancel();
    if(!ClientShowing())return;
    clientGopRestore|=gopTarget;
    clientLeaving=true;
    PlaneIo io{displayRegs[2]};
    (void)clientFlip.Restore(io,clientGopRestore?0:zcRing.Surface(zcRing.Front()));
}
bool ClientObservationPendingLocked(){
    return ClientOn() && zcRequested && !zcDisabled && zcActive &&
        !clientLeaving && clientFlip.CanObserve();
}

void ClientPollLocked(bool offWorker=false){
    if(offWorker){
        // Do not change the programmed surface or restore the GOP from
        // this task. A worker poll still handles recovery and takeover.
        if(!clientObserver.MayPoll(ClientObservationPendingLocked(),captureHold,zcVerifyHold))return;
    }else{
        if(!ClientShowing())return;
        if(!ClientOn() || !zcRequested || zcDisabled || !zcActive)
            ClientRestoreLocked(!zcRequested || zcDisabled || !zcActive);
    }
    PlaneIo io{displayRegs[2]};
    const bool restoring=clientFlip.Restoring();
    if(!clientFlip.Poll(io)){
        if(!offWorker && clientFlip.Uncertain() && !restoring)ClientRestoreLocked(true);
        return;
    }
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t successor=restoring?0:clientArmedSequence;
    if(successor)clientTimeline.Latched(successor,ElapsedClockNs(),offWorker);
    clientArmedSequence=0;
#endif
    if(restoring){
        ++clientRestores;
        if(clientGopRestore){
            zcActive=false;zcRing=BdwZeroCopyRing{};zcRepairOwed=true;
        }
        clientGopRestore=clientLeaving=false;
    }else {
        ++clientLatched;
        CompositorRecordDesktopPresent();
    }
    for(auto& mapping:clientMappings){
        if(mapping.held && !clientFlip.Contains(mapping.Base())){
            // Do not tell the renderer to send SAFE before permitting writes.
            BdwGpuBufferEndCpuTransition(mapping.buffer);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
            clientTimeline.Retired(mapping.timelineSequence,ElapsedClockNs(),
                restoring?BdwClientRetirement::Restore:BdwClientRetirement::Replacement,successor);
            mapping.timelineSequence=0;
#endif
            mapping.held=false;
        }
    }
}
void ClientClearMapping(ClientMapping& m){
    auto* old=m.buffer;
    if(m.reservation.token)BdwGlobalGgttSpace().release(m.reservation);
    m.reservation={};
    m.buffer=nullptr;
    (void)m.binding.Recycle();
    BdwGpuBufferUnref(old);
}
bool ClientEvict(ClientMapping& m){
    if(!m.buffer)return true;
    BdwFlipGgttDiag diag{};
    const BdwGgttExtentLease lease{m.reservation,m.dma};
    if(!VmmDellFlipGgttUpdate(m.binding,lease,m.snapshot,takeoverScratch.dma,diag,BdwFlipGgttOperation::Retire)){
        m.quarantined=true;
        return false;
    }
    ClientClearMapping(m);
    return true;
}
bool ClientBind(ClientMapping& m,const ClientRequest& request){
    if(m.buffer==request.buffer && m.layout.stride==request.layout.stride &&
       m.layout.tiled==request.layout.tiled)return true;
    if(!ClientEvict(m))return false;
    constexpr uint32_t maxPages=BdwClientScanoutLayout{16384,false}.Pages()+BdwFlipRegs::GuardPages;
    if(!m.dma)m.dma=static_cast<uint64_t*>(kmalloc(uint64_t(maxPages)*sizeof(uint64_t)));
    if(!m.snapshot)m.snapshot=static_cast<uint64_t*>(kmalloc(uint64_t(maxPages)*sizeof(uint64_t)));
    const uint32_t pages=request.layout.Pages(),extent=pages+BdwFlipRegs::GuardPages;
    uint32_t backingPages=0;
    const uint64_t* source=BdwGpuBufferPages(request.buffer,backingPages);
    if(!m.dma || !m.snapshot || !source || backingPages<pages || !zeroReady || !takeoverReady ||
       !BdwGlobalGgttSpace().reserveExtent(extent,BdwFlipRegs::SurfaceAlignment/4096,m.reservation))return false;
    for(uint32_t p=0;p<extent;++p)m.dma[p]=p<pages?source[p]:zeroPage.dma;
    BdwGpuBufferRef(request.buffer);
    m.buffer=request.buffer;m.layout=request.layout;
    const BdwGgttExtentLease lease{m.reservation,m.dma};
    BdwFlipGgttDiag diag{};
    if(VmmDellFlipGgttUpdate(m.binding,lease,m.snapshot,takeoverScratch.dma,diag,BdwFlipGgttOperation::BindReversible))return true;
    if(m.binding.SafeToRelease())ClientClearMapping(m);
    else m.quarantined=true;
    return false;
}
void ZcFallback(BdwZeroCopyFallback f){zcLast=f;++zcStats.fallback[uint32_t(f)];}
// Permanent for the boot. Best effort, never waits: re-arm the GOP; the
// caller returns RepairRequired so the compositor repaints it in full.
void ZcDisableLocked(const char* why){
    CursorOffLocked(true);
    ClientRestoreLocked(true);
    if(!zcDisabled){zcDisabled=true;zcReason=why;zcPendingLog=why;zcRepairOwed=zcActive;}
    zcRing.MarkUncertain();zcActive=false;
    // The old GOP can still contain a software cursor. Wait for the
    // disable latch before making it visible again.
    if(displayMapped && !cursorPlane.blocksSoftware()){
        PlaneIo io{displayRegs[2]};(void)BdwFlipRestoreGop(io,0,0);
    }
    ++zcStats.uncertain;
}
void ZcLogPending(){
    if(const char* why=__atomic_exchange_n(&zcPendingLog,static_cast<const char*>(nullptr),__ATOMIC_ACQ_REL))
        KPrintf("gpu-zerocopy: disabled for this boot (%s); BCS copy to the GOP resumes after a full repaint\n",why);
}
bool ZcArmLocked(uint32_t slot){
    if(Read(BdwFlipRegs::PlaneControl)!=BdwFlipRegs::GopControl || Read(BdwFlipRegs::Stride)!=BdwZeroCopyStride){
        ZcFallback(BdwZeroCopyFallback::PlaneChanged);ZcDisableLocked("plane-changed");return false;
    }
    PlaneIo io{displayRegs[2]};
    const uint32_t frame=io.read32(BdwFlipRegs::FrameCountA);
    io.write32(BdwFlipRegs::Surface,zcRing.Surface(slot));
    (void)io.read32(BdwFlipRegs::Surface);
    const uint64_t armed=Rdtsc();
    if(!zcRing.Armed(slot,frame,armed)){ZcDisableLocked("arm-rejected");return false;}
    if(auto* e=ZcTimelineFor(slot))e->armTsc=armed;
    return true;
}
void ZcPollLocked(){
    CursorPollLocked();
    if(captureHold)return;
    ClientPollLocked();
    if(zcDisabled && displayMapped && !cursorPlane.blocksSoftware() && Read(BdwFlipRegs::Surface)!=0){
        PlaneIo io{displayRegs[2]};(void)BdwFlipRestoreGop(io,0,0);
    }
    if(!zcActive || zcDisabled || ClientShowing())return;
    if(zcRing.HasArmed()){
        const uint32_t frame=Read(BdwFlipRegs::FrameCountA),live=Read(BdwFlipRegs::SurfaceLive);
        BdwFlipLatch latch{};
        switch(zcRing.Poll(frame,live,Rdtsc(),latch)){
        case BdwFlipPoll::Latched:
            zcStats.Latch(latch.frames,tscKhz?latch.ticks*1000/tscKhz:0);
            CompositorRecordDesktopPresent();
            if(auto* e=ZcTimelineFor(latch.slot)){e->latchTsc=Rdtsc();e->frames=latch.frames;}
            break;
        case BdwFlipPoll::Uncertain:ZcFallback(BdwZeroCopyFallback::Uncertain);ZcDisableLocked("latch-uncertain");return;
        default:return;
        }
    }
    if(zcVerifyHold)return;
    uint32_t pending=0;
    if(zcRing.PendingArm(pending)){(void)ZcArmLocked(pending);return;}
    uint32_t queued=0;
    if(zcRing.TakeQueued(queued))(void)ZcArmLocked(queued);
}
// Worker only, outside the lock: GGTT-bind every registered image once.
bool ZcBind(){
    if(zcBound)return true;
    auto fail=[](const char* why){
        const uint64_t f=IrqSpinLockAcquire(&lock);ZcFallback(BdwZeroCopyFallback::Bind);ZcDisableLocked(why);IrqSpinLockRelease(&lock,f);
        KPrintf("gpu-zerocopy: bind failed at %s; ggtt %s image=%u first=%u count=%u zero=%u valid=%u other=%u page=%u\n",
            why,BdwFlipGgttStageName(zcGgtt.stage),zcGgtt.buffer,zcGgtt.first,zcGgtt.count,zcGgtt.zero,zcGgtt.valid,
            zcGgtt.other,zcGgtt.auditPage);
        ZcLogPending();return false;
    };
    if(!MapDisplay())return fail("display-map");
    if(!ValidateGop())return false; // transient: retried by the next present
    if(!EnsureZero())return fail("alloc");
    if(!takeoverReady){
        if(!BdwPrivatePageAlloc(takeoverScratch) || !BdwPrivatePagesPublish(&takeoverScratch,1))return fail("alloc");
        takeoverReady=true;
    }
    for(uint32_t i=0;i<zcCount;++i){
        auto& image=zcImages[i];
        if(image.bound)continue;
        uint32_t pages=0;const uint64_t* source=BdwGpuBufferPages(image.buffer,pages);
        if(!source || pages<SurfacePages)return fail("image-pages");
        if(!image.dma){
            auto* dma=static_cast<uint64_t*>(kmalloc(uint64_t(ZcExtentPages)*sizeof(uint64_t)));
            if(!dma)return fail("alloc");
            // The buffer is retained for the boot, so these pages stay pinned.
            for(uint32_t p=0;p<SurfacePages;++p)dma[p]=source[p];
            for(uint32_t p=SurfacePages;p<ZcExtentPages;++p)dma[p]=zeroPage.dma; // display guard PTEs
            image.dma=dma;
        }
        if(!image.snapshot && !(image.snapshot=static_cast<uint64_t*>(kmalloc(uint64_t(ZcExtentPages)*sizeof(uint64_t)))))
            return fail("alloc");
        zcGgtt={};zcGgtt.buffer=i;zcGgtt.entries=BdwGlobalGgttSpace().Limit();zcGgtt.count=ZcExtentPages;
        if(!image.reservation.token &&
           !BdwGlobalGgttSpace().reserveExtent(ZcExtentPages,BdwFlipRegs::SurfaceAlignment/4096,image.reservation)){
            zcGgtt.stage=BdwFlipGgttStage::Reserve;return fail("ggtt-space");
        }
        {
            const BdwGgttExtentLease lease{image.reservation,image.dma};
            image.bound=VmmDellFlipGgttUpdate(image.bind,lease,image.snapshot,takeoverScratch.dma,zcGgtt);
        }
        if(!image.bound && zcGgtt.stage==BdwFlipGgttStage::Uncertain)return fail("ggtt-bind-uncertain");
        if(!image.bound)return fail("ggtt-bind");
    }
    zcBound=true;return true;
}
// Worker only: bind (first time) and start a fresh ring on the GOP.
bool ZcActivate(){
    const uint64_t start=Rdtsc();
    if(!ZcBind())return false;
    uint32_t surfaces[BdwZeroCopyMaxImages]{};uint64_t identities[BdwZeroCopyMaxImages]{};
    for(uint32_t i=0;i<zcCount;++i){surfaces[i]=zcImages[i].reservation.first*4096;identities[i]=zcImages[i].identity;}
    const uint64_t f=IrqSpinLockAcquire(&lock);
    bool ok=!zcDisabled && !flipActive && zcRequested && !ClientShowing();
    if(ok){
        zcRing=BdwZeroCopyRing{};
        ok=zcRing.Configure(surfaces,identities,zcCount);
        if(ok){
            // Publish before the first DSPASURF write: the per-job display
            // invariant accepts these surfaces and panic/reset restores the GOP.
            for(uint32_t i=0;i<zcCount;++i)
                __atomic_store_n(&g_bdwScanoutSurfaces[BdwScanoutFlipSurfaceSlots+i],surfaces[i],__ATOMIC_RELEASE);
            __atomic_store_n(&g_restoreBudgetTicks,tscKhz*50,__ATOMIC_RELAXED);
            __atomic_store_n(&g_planePage,displayRegs[2],__ATOMIC_RELEASE);
            zcActive=true;zcReason="active";
            zcSetupUs=tscKhz?(Rdtsc()-start)*1000/tscKhz:0;
        }else ZcDisableLocked("ring");
    }
    IrqSpinLockRelease(&lock,f);ZcLogPending();
    if(ok)KPrintf("gpu-zerocopy: active; %u images; surfaces %x %x %x %x; setup %u us\n",zcCount,
        surfaces[0],surfaces[1],surfaces[2],surfaces[3],unsigned(zcSetupUs));
    return ok;
}
// "gpu zerocopy off": return to the GOP, waiting (bounded) for the latch so
// the renderer never writes an image the display still fetches.
void ZcLeave(){
    {
        const uint64_t f=IrqSpinLockAcquire(&lock);
        if(zcLeavingForGop){IrqSpinLockRelease(&lock,f);return;}
        zcLeavingForGop=true;
        CursorOffLocked();
        IrqSpinLockRelease(&lock,f);
    }
    // Both the present worker and the adopting renderer use this path. Do
    // not reintroduce the old GOP cursor while the hardware can still show.
    const uint64_t start=Rdtsc(),budget=__atomic_load_n(&g_restoreBudgetTicks,__ATOMIC_RELAXED);
    while(BdwCursorBlocksSoftware()){
        if(Rdtsc()-start>=budget){
            const uint64_t f=IrqSpinLockAcquire(&lock);
            zcLeavingForGop=false;
            IrqSpinLockRelease(&lock,f);
            return;
        }
        SchedulerSleepMs(1);
    }
    PlaneIo io{displayRegs[2]};
    const auto restored=BdwFlipRestoreGop(io,0,__atomic_load_n(&g_restoreBudgetTicks,__ATOMIC_RELAXED));
    const uint64_t f=IrqSpinLockAcquire(&lock);
    if(restored.latched){zcActive=false;zcRing=BdwZeroCopyRing{};zcRepairOwed=true;++zcGopReturns;zcReason="off";}
    else ZcDisableLocked("leave-latch-timeout");
    zcLeavingForGop=false;
    IrqSpinLockRelease(&lock,f);ZcLogPending();
}
#endif
#endif
}

BdwTargetSelect BdwScanoutTargetSelect(const CompositorPresentRegion* regions,uint32_t count,BdwTargetPlan& plan){
    plan={};
#if defined(BROOK_BDW_SCANOUT_FLIP)
    bool requested=false,active=false,disabled=false;
    {const uint64_t f=IrqSpinLockAcquire(&lock);PollLocked();
     requested=flipRequested;active=flipActive;disabled=flipDisabled;IrqSpinLockRelease(&lock,f);}
    LogPending();
    if(requested && !active && !disabled){(void)Activate();
        const uint64_t f=IrqSpinLockAcquire(&lock);active=flipActive;IrqSpinLockRelease(&lock,f);}
    const bool gopOk=(active || mapEnabled) && ValidateGop();
    if(active){
        const uint64_t f=IrqSpinLockAcquire(&lock);
        const bool away=!chain.GopCurrent() || flipDisabled;
        if(flipDisabled || !gopOk){
            // The panel may show a kernel buffer: the GOP must be repainted
            // in full by the CPU before anything else is presented.
            bool repair=away;
            if(flipDisabled){
                const bool onGop=Read(BdwFlipRegs::Surface)==0 && Read(BdwFlipRegs::SurfaceLive)==0;
                if(!onGop){PlaneIo io{displayRegs[2]};(void)BdwFlipRestoreGop(io,0,0);}
                repair=!onGop || repairOwed;
                if(onGop)repairOwed=false;
            }else if(!repair)chain.GopWritten();
            IrqSpinLockRelease(&lock,f);LogPending();
            if(repair)return BdwTargetSelect::RepairRequired;
        }else{
            uint32_t slot=0;bool target=false;
            if(flipRequested){slot=chain.AcquireBack();target=slot!=0;}
            else if(chain.Latest()!=0){
                // Leaving flip mode: write the GOP like a back buffer once the
                // display no longer reads it, otherwise keep flipping.
                if(chain.AcquireGop()){slot=0;target=true;}
                else{slot=chain.AcquireBack();target=slot!=0;}
            }
            if(target)FlipPlan(slot,regions,count,plan);
            else if(flipRequested || chain.Latest()!=0){IrqSpinLockRelease(&lock,f);return BdwTargetSelect::RepairRequired;}
            else chain.GopWritten(); // the GOP holds the newest frame: plain GOP present
            IrqSpinLockRelease(&lock,f);
            if(target)return BdwTargetSelect::Ok;
        }
    }
    if(mapEnabled && gopOk)PersistentGopPlan(plan);
#else
    (void)regions;(void)count;
    if(mapEnabled && ValidateGop())PersistentGopPlan(plan);
#endif
    return BdwTargetSelect::Ok;
}
bool BdwScanoutTargetFinish(BdwTargetPlan& plan,bool copied,const CapturePrivacy& privacy){
#if defined(BROOK_BDW_SCANOUT_FLIP)
    if(plan.kind!=BdwTargetKind::Flip)return true;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    bool ok=false,armNow=false;
    if(!copied){chain.Abandon(plan.slot);DisableLocked("copy-failed");}
    else if(!chain.Commit(plan.slot,plan.damage,armNow))DisableLocked("commit-rejected");
    else {
        buffers[plan.slot-1].privacy=privacy;
        if(armNow)ok=ArmLocked(plan.slot);
        else {ok=true;Stat(BdwWorkerScanoutEvent::FlipQueued);}
    }
    IrqSpinLockRelease(&lock,f);LogPending();
    return ok;
#else
    (void)plan;(void)copied;(void)privacy;return true;
#endif
}
void BdwScanoutFlipPoll(){
#if defined(BROOK_BDW_SCANOUT_FLIP)
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    {
        const uint64_t f=IrqSpinLockAcquire(&lock);ZcPollLocked();IrqSpinLockRelease(&lock,f);ZcLogPending();
    }
#endif
    if(!__atomic_load_n(&flipActive,__ATOMIC_ACQUIRE))return;
    const uint64_t f=IrqSpinLockAcquire(&lock);PollLocked();IrqSpinLockRelease(&lock,f);
    LogPending();
#endif
}
bool BdwScanoutFlipAwayFromGop(){
#if defined(BROOK_BDW_SCANOUT_FLIP)
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    // A renderer image is (or may soon be) on the panel: the software frame
    // must be complete before ReturnToGop arms the GOP again.
    if(__atomic_load_n(&zcActive,__ATOMIC_ACQUIRE)){
        const uint64_t f=IrqSpinLockAcquire(&lock);
        const bool away=zcActive && (zcRing.Front()!=0 || zcRing.HasArmed() || zcRing.Uncertain() || ClientShowing());
        IrqSpinLockRelease(&lock,f);
        if(away)return true;
    }
#endif
    if(!__atomic_load_n(&flipActive,__ATOMIC_ACQUIRE))return false;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const bool away=flipDisabled?!(Read(BdwFlipRegs::Surface)==0 && Read(BdwFlipRegs::SurfaceLive)==0):!chain.GopCurrent();
    IrqSpinLockRelease(&lock,f);
    return away;
#else
    return false;
#endif
}
void BdwScanoutFlipReturnToGop(){
#if defined(BROOK_BDW_SCANOUT_FLIP)
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    // Software takeover (lease expiry or renderer exit): the GOP was fully
    // rewritten. Arm it without waiting; the renderer's next image present
    // re-activates zero-copy on a fresh ring (front = GOP).
    if(__atomic_load_n(&zcActive,__ATOMIC_ACQUIRE)){
        const uint64_t f=IrqSpinLockAcquire(&lock);
        if(zcActive){
            CursorOffLocked();
            if(!cursorPlane.blocksSoftware()){
                ClientRestoreLocked(true);PlaneIo io{displayRegs[2]};(void)BdwFlipRestoreGop(io,0,0);
                zcActive=false;zcRing=BdwZeroCopyRing{};++zcGopReturns;if(!zcDisabled)zcReason="software takeover";
            }
        }
        IrqSpinLockRelease(&lock,f);
    }
#endif
    if(!__atomic_load_n(&flipActive,__ATOMIC_ACQUIRE))return;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    if(flipDisabled){PlaneIo io{displayRegs[2]};(void)BdwFlipRestoreGop(io,0,0);}
    else if(chain.RequestGop()){if(ArmLocked(0))++gopReturns;}
    IrqSpinLockRelease(&lock,f);
    LogPending();
#endif
}
bool BdwScanoutFlipProbe(BdwFlipProbe& probe,BdwFlipRefusal& refusal){
    probe={};refusal=BdwFlipRefusal::NoSample;
#if defined(BROOK_BDW_SCANOUT_FLIP)
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.measured || !tsc.consistent || !tsc.khz)return false;
    // Identity, D0, armed recovery reset and the exact GOP plane state are
    // the existing display preconditions; the probe adds read-only samples.
    BdwDisplaySnapshot display{};
    if(!BdwObserveDisplayLive(display) || !BdwDisplayUsesGop(display) || !MapDisplay())return false;
    auto sample=[](BdwFlipProbe& p){
        p.pipeSource=Read(BdwFlipRegs::PipeSource);p.ddiEdp=Read(BdwFlipRegs::DdiEdpControl);
        p.psr=Read(BdwFlipRegs::PsrControl);p.pipeConfA=Read(BdwFlipRegs::PipeConfA);
        p.pipeConfEdp=Read(BdwFlipRegs::PipeConfEdp);p.control=Read(BdwFlipRegs::PlaneControl);
        p.linearOffset=Read(BdwFlipRegs::LinearOffset);p.stride=Read(BdwFlipRegs::Stride);
        p.surface=Read(BdwFlipRegs::Surface);p.tileOffset=Read(BdwFlipRegs::TileOffset);
        p.surfaceLive=Read(BdwFlipRegs::SurfaceLive);p.controlB=Read(BdwFlipRegs::PlaneControlB);
        p.controlC=Read(BdwFlipRegs::PlaneControlC);p.flipCount=Read(BdwFlipRegs::FlipCountA);
    };
    BdwFlipProbe first{},second{};
    sample(first);
    first.frame0=Read(BdwFlipRegs::FrameCountA);const uint64_t t0=Rdtsc();
    SchedulerSleepMs(100);
    first.frame1=Read(BdwFlipRegs::FrameCountA);const uint64_t t1=Rdtsc();
    sample(second);
    first.elapsedUs=t1>t0?(t1-t0)*1000/tsc.khz:0;
    probe=first;
    // Everything but the counters must be stable across the interval.
    second.frame0=first.frame0;second.frame1=first.frame1;second.elapsedUs=first.elapsedUs;second.flipCount=first.flipCount;
    const auto* a=reinterpret_cast<const uint8_t*>(&first);const auto* b=reinterpret_cast<const uint8_t*>(&second);
    bool stable=true;for(unsigned i=0;i<sizeof(first);++i)if(a[i]!=b[i])stable=false;
    refusal=stable?BdwFlipEvaluate(first,BdwFlipExpect{}):BdwFlipRefusal::NoSample;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    lastProbe=probe;lastRefusal=refusal;probeSampled=true;probeOk=refusal==BdwFlipRefusal::None;tscKhz=tsc.khz;
    IrqSpinLockRelease(&lock,f);
    SerialPrintf("gpu-flip: probe %s ddi=%x psr=%x conf=%x ctl=%x stride=%u surf=%x live=%x frames=%u/%u us\n",
        BdwFlipRefusalName(refusal),probe.ddiEdp,probe.psr,probe.pipeConfEdp,probe.control,probe.stride,
        probe.surface,probe.surfaceLive,probe.frame1-probe.frame0,unsigned(probe.elapsedUs));
    return true;
#else
    return false;
#endif
}
bool BdwScanoutFlipArm(const char*& reason){
#if defined(BROOK_BDW_SCANOUT_FLIP)
    if(!BdwBcsScanoutEnabled()){reason="bcs-present-disabled";return false;}
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    {const uint64_t f=IrqSpinLockAcquire(&lock);const bool zc=zcRequested || zcActive;IrqSpinLockRelease(&lock,f);
     if(zc){reason="zerocopy-armed";return false;}}
#endif
    {const uint64_t f=IrqSpinLockAcquire(&lock);const bool active=flipActive,disabled=flipDisabled;
     if(disabled)reason=flipReason;
     if(active && !disabled)flipRequested=true;
     IrqSpinLockRelease(&lock,f);
     if(disabled)return false;
     if(active){reason="armed";return true;}}
    BdwFlipProbe probe{};BdwFlipRefusal refusal{};
    if(!BdwScanoutFlipProbe(probe,refusal)){reason="probe-unavailable";return false;}
    if(refusal!=BdwFlipRefusal::None){reason=BdwFlipRefusalName(refusal);return false;}
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const bool disabled=flipDisabled;
    if(!disabled)flipRequested=true;
    reason=disabled?flipReason:"armed";
    IrqSpinLockRelease(&lock,f);
    return !disabled;
#else
    reason="not built";return false;
#endif
}
void BdwScanoutFlipDisarm(){
#if defined(BROOK_BDW_SCANOUT_FLIP)
    const uint64_t f=IrqSpinLockAcquire(&lock);flipRequested=false;IrqSpinLockRelease(&lock,f);
#endif
}
bool BdwScanoutMapEnable(bool on){
#if defined(BROOK_BDW_PERSISTENT_SCANOUT_MAP)
    __atomic_store_n(&mapEnabled,on,__ATOMIC_RELEASE);return true;
#else
    (void)on;return false;
#endif
}
void BdwScanoutTargetReadStatus(BdwScanoutTargetStatus& out){
    out={};
#if defined(BROOK_BDW_PERSISTENT_SCANOUT_MAP)
    out.mapBuilt=true;
#endif
    out.mapEnabled=__atomic_load_n(&mapEnabled,__ATOMIC_ACQUIRE);
#if defined(BROOK_BDW_SCANOUT_FLIP)
    out.flipBuilt=true;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    out.flipRequested=flipRequested;out.flipActive=flipActive;out.flipDisabled=flipDisabled;
    out.probeSampled=probeSampled;out.probeOk=probeOk;out.reason=flipReason;out.probe=lastProbe;out.refusal=lastRefusal;
    out.front=chain.Front();out.latest=chain.Latest();
    out.armed=chain.HasArmed()?chain.ArmedSlot():~0u;out.queued=chain.HasQueued()?chain.QueuedSlot():~0u;
    for(uint32_t i=0;i<BdwFlipMaxSlots-1;++i)out.surfaces[i]=__atomic_load_n(&g_bdwScanoutSurfaces[i],__ATOMIC_ACQUIRE);
    out.gopReturns=gopReturns;out.activationUs=activationUs;out.activationStep=activationStep;out.ggtt=ggttDiag;
    IrqSpinLockRelease(&lock,f);
#else
    out.reason="not built";
#endif
}
#if defined(BROOK_BDW_SCANOUT_FLIP)
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
namespace {constexpr int64_t ErrBadf=-9,ErrBusy=-16,ErrInval=-22;
bool zcAutoRetrySpawned=false;
// Retries the boot-default arm every 2 s, up to 30 times. Stops as soon as the
// arm is requested, auto-arm is cancelled (gpu zerocopy off), or zero-copy is
// disabled or unregistered.
void ZcAutoArmRetry(void*){
    for(unsigned attempt=1;attempt<=30;++attempt){
        SchedulerSleepMs(2000);
        if(!__atomic_load_n(&zcAutoArm,__ATOMIC_ACQUIRE))return;
        bool stop=false,requested=false;
        {const uint64_t f=IrqSpinLockAcquire(&lock);stop=zcDisabled || !zcCount;requested=zcRequested || zcActive;IrqSpinLockRelease(&lock,f);}
        if(stop || requested)return;
        const char* note="";const bool armed=BdwZeroCopyArm(note);
        if(armed){KPrintf("gpu-zerocopy: boot default arm requested on retry %u (%s)\n",attempt,note);return;}
        if(attempt==1 || attempt%10==0)KPrintf("gpu-zerocopy: boot default arm retry %u refused (%s)\n",attempt,note);
    }
    KPrintf("gpu-zerocopy: boot default arm gave up after 30 retries\n");
}
}
int64_t BdwZeroCopyRegister(uint64_t owner,BdwGpuBuffer* const* images,uint32_t imageCount,const BdwZeroCopyGeometry& geometry,
                            BdwZeroCopyRefusal& refusal){
    refusal=BdwZeroCopyRefusal::Count;
    if(!images || !imageCount || imageCount>BdwZeroCopyMaxImages)return ErrInval;
    BdwZeroCopyCandidate c[BdwZeroCopyMaxImages]{};
    for(uint32_t i=0;i<imageCount;++i){
        if(!images[i])return ErrBadf;
        c[i]={BdwGpuBufferIdentity(images[i]),BdwGpuBufferBytes(images[i]),BdwGpuBufferAllowsUserMapping(images[i])};
    }
    refusal=BdwZeroCopyEligible(geometry,c,imageCount);
    {const uint64_t f=IrqSpinLockAcquire(&lock);zcRefusal=refusal;const bool busy=zcCount!=0;IrqSpinLockRelease(&lock,f);
     if(refusal!=BdwZeroCopyRefusal::None)return ErrInval;
     if(busy)return ErrBusy;}
    // Atomic, outside the spin lock. A later failure leaves earlier marks set,
    // which only makes those images render uncached: still correct.
    for(uint32_t i=0;i<imageCount;++i)if(!BdwGpuBufferMarkScanoutImage(images[i]))return ErrBusy;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    if(zcCount){IrqSpinLockRelease(&lock,f);return ErrBusy;}
    for(uint32_t i=0;i<imageCount;++i){
        BdwGpuBufferRef(images[i]); // retained for the boot (phase 1)
        zcImages[i].buffer=images[i];zcImages[i].identity=c[i].identity;
    }
    zcCount=imageCount;zcOwner=owner;
    IrqSpinLockRelease(&lock,f);
    KPrintf("gpu-zerocopy: registered %u renderer images (owner %lx)\n",imageCount,owner);
    if(__atomic_load_n(&zcAutoArm,__ATOMIC_ACQUIRE)){
        const char* note="";const bool armed=BdwZeroCopyArm(note);
        KPrintf("gpu-zerocopy: boot default arm %s (%s)\n",armed?"requested":"refused",note);
        // gen 423: the first arm at registration can be refused (the flip probe
        // runs before the display has settled), and nothing retried it, so the
        // boot default never took effect. Retry from a kernel thread: the probe
        // sleeps 100 ms, so it must stay off the present path.
        if(!armed && !__atomic_exchange_n(&zcAutoRetrySpawned,true,__ATOMIC_ACQ_REL)){
            if(Process* t=KernelThreadCreate("zc-autoarm",ZcAutoArmRetry,nullptr,3))SchedulerAddProcess(t);
            else KPrintf("gpu-zerocopy: auto-arm retry thread creation failed\n");
        }
    }
    return 0;
}
int64_t BdwZeroCopyAdopt(uint64_t owner,BdwGpuBuffer** out,uint32_t& count){
    count=0;
    uint32_t registered=0;BdwGpuBuffer* images[BdwZeroCopyMaxImages]{};
    {const uint64_t f=IrqSpinLockAcquire(&lock);registered=zcCount;
     for(uint32_t i=0;i<registered;++i)images[i]=zcImages[i].buffer;IrqSpinLockRelease(&lock,f);}
    bool idle[BdwZeroCopyMaxImages]{};
    // gen 401: a renderer job that failed after a verified engine reset marks
    // its ring images recoverably failed. Once that job has retired (no GPU
    // use left) their contents are merely unknown, and the adopting renderer
    // fully redraws each image before it is shown, so the failure is cleared.
    uint32_t cleared=0;
    for(uint32_t i=0;i<registered;++i)if(BdwGpuBufferClearRecoverableFailure(images[i]))++cleared;
    if(cleared)KPrintf("gpu-zerocopy: cleared recoverable failure on %u retained images before adoption\n",cleared);
    for(uint32_t i=0;i<registered;++i)idle[i]=BdwGpuBufferIdle(images[i]);
    switch(BdwZeroCopyAdoptDecision(registered,BdwOffscreenHandoffAttached(owner),idle)){
    case BdwZeroCopyAdoption::NotRegistered:return -2;
    case BdwZeroCopyAdoption::NotAttached:return -1;
    case BdwZeroCopyAdoption::Busy:return -16;
    case BdwZeroCopyAdoption::Adopt:break;
    }
    // The previous owner is gone. If its images may still be on the panel,
    // return to the GOP first (bounded wait for the latch); the software
    // compositor's repaint or the new renderer's first full frame follows.
    bool active=false;{const uint64_t f=IrqSpinLockAcquire(&lock);active=zcActive;IrqSpinLockRelease(&lock,f);}
    if(active)ZcLeave();
    const uint64_t f=IrqSpinLockAcquire(&lock);
    if(zcActive || zcCount!=registered){IrqSpinLockRelease(&lock,f);return -16;}
    const uint64_t previous=zcOwner;
    if(previous!=owner)CursorOffLocked();
    zcOwner=owner;
    for(uint32_t i=0;i<registered;++i){BdwGpuBufferRef(images[i]);out[i]=images[i];}
    count=registered;
    IrqSpinLockRelease(&lock,f);
    KPrintf("gpu-zerocopy: %u retained images adopted by owner %lx (was %lx)\n",registered,owner,previous);
    return 0;
}
bool BdwZeroCopyOnReserve(BdwGpuBuffer* buffer){
    if(!BdwGpuBufferIsScanoutImage(buffer))return true;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    bool ok=true;const uint32_t slot=ZcSlotOf(buffer);
    if(slot && zcActive && !zcDisabled){ok=zcRing.BeginWrite(slot);if(!ok)++zcStats.refusedReserve;}
    IrqSpinLockRelease(&lock,f);
    return ok;
}
void BdwZeroCopyOnRelease(BdwGpuBuffer* buffer){
    if(!BdwGpuBufferIsScanoutImage(buffer))return;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const uint32_t slot=ZcSlotOf(buffer);
    if(slot && zcActive)zcRing.AbandonWrite(slot);
    IrqSpinLockRelease(&lock,f);
}
// "flush" cache mode: render writes stay write-back (fast); before an image
// is armed, every line the renderer drew this frame (the present regions,
// which in ring mode cover the buffer-age repair too) is CLFLUSHed so the
// non-snooping display reads memory. CLFLUSH writes a line back from every
// cache level, the shared LLC included, which the GT writes reach at the
// end-of-batch render-target flush that precedes the fence. An image's
// first present after activation is flushed in full.
void ZcFlushLines(BdwGpuBuffer* buffer,uint64_t start,uint64_t bytes){
    const uint64_t end=start+bytes;
    for(uint64_t line=start&~uint64_t{63};line<end;line+=64){
        const uint64_t page=line>>12;if(page>=SurfacePages)break;
        const uint64_t base=BdwGpuBufferPageKernelAddress(buffer,uint32_t(page));if(!base)continue;
        __asm__ volatile("clflush (%0)"::"r"(base+(line&4095)):"memory");
    }
}
void ZcFlushForScanout(BdwGpuBuffer* buffer,uint32_t slot,const CompositorPresentRegion* regions,uint32_t count){
    const uint64_t start=Rdtsc();uint64_t bytes=0;
    bool full=!regions || !count;
    {const uint64_t f=IrqSpinLockAcquire(&lock);if(slot && slot<=BdwZeroCopyMaxImages && !(zcFlushedOnce&(1u<<slot)))full=true;
     IrqSpinLockRelease(&lock,f);}
    if(full){ZcFlushLines(buffer,0,BdwZeroCopyImageBytes);bytes=BdwZeroCopyImageBytes;}
    else for(uint32_t i=0;i<count;++i){const auto& r=regions[i];
        if(!r.width || !r.height || r.x+r.width>BdwZeroCopyWidth || r.y+r.height>BdwZeroCopyHeight){
            ZcFlushLines(buffer,0,BdwZeroCopyImageBytes);bytes=BdwZeroCopyImageBytes;break;} // unexpected geometry: be safe
        for(uint32_t row=0;row<r.height;++row){
            const uint64_t at=uint64_t(r.y+row)*BdwZeroCopyStride+uint64_t(r.x)*4;
            ZcFlushLines(buffer,at,uint64_t(r.width)*4);bytes+=uint64_t(r.width)*4;
        }
    }
    __asm__ volatile("mfence":::"memory");
    const uint64_t us=tscKhz?(Rdtsc()-start)*1000/tscKhz:0;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    if(slot && slot<=BdwZeroCopyMaxImages)zcFlushedOnce|=1u<<slot;
    ++zcStats.flushes;zcStats.flushBytes+=bytes;zcStats.flushUsSum+=us;if(us>zcStats.flushUsMax)zcStats.flushUsMax=us;
    if(full)++zcStats.fullFlushes;
    IrqSpinLockRelease(&lock,f);
}
void BdwZeroCopyPrepareScanout(BdwGpuBuffer* source,const CompositorPresentRegion* regions,uint32_t count){
    // Present thread (not the gpu-submit worker): the render job has
    // retired, so its lines are in the LLC. Images are flushed from the arm
    // request onwards, each in full the first time.
    if(__atomic_load_n(&zcCacheMode,__ATOMIC_ACQUIRE)!=ZcCacheFlush || !__atomic_load_n(&zcRequested,__ATOMIC_ACQUIRE) ||
       !BdwGpuBufferIsScanoutImage(source))return;
    const uint32_t slot=ZcSlotOf(source);
    if(slot)ZcFlushForScanout(source,slot,regions,count);
}
BdwZeroCopyOutcome BdwZeroCopyPresentSource(BdwGpuBuffer* source,const CompositorPresentRegion* regions,
                                              uint32_t regionCount,const CapturePrivacy& privacy){
    (void)regions;(void)regionCount;
    bool requested=false,active=false,disabled=false;uint32_t registered=0;
    {const uint64_t f=IrqSpinLockAcquire(&lock);ZcPollLocked();
     if(ClientShowing()){
         ClientRestoreLocked(false);
         IrqSpinLockRelease(&lock,f);
         return BdwZeroCopyOutcome::RepairRequired;
     }
     registered=zcCount;requested=zcRequested;active=zcActive;disabled=zcDisabled;IrqSpinLockRelease(&lock,f);}
    ZcLogPending();
    if(active)CursorBindIfRequested();
    if(!registered)return BdwZeroCopyOutcome::Copy;
    const bool image=BdwGpuBufferIsScanoutImage(source) && ZcSlotOf(source);
    if(disabled){
        // As the M3 flip: the GOP must be live again and owes one full repaint.
        const uint64_t f=IrqSpinLockAcquire(&lock);
        const bool onGop=displayMapped && Read(BdwFlipRegs::Surface)==0 && Read(BdwFlipRegs::SurfaceLive)==0;
        if(!onGop && displayMapped && !cursorPlane.blocksSoftware()){
            PlaneIo io{displayRegs[2]};(void)BdwFlipRestoreGop(io,0,0);
        }
        const bool repair=!onGop || zcRepairOwed;
        if(onGop)zcRepairOwed=false;
        IrqSpinLockRelease(&lock,f);
        return repair?BdwZeroCopyOutcome::RepairRequired:BdwZeroCopyOutcome::Copy;
    }
    if(active && (!requested || !image)){
        // Leaving (gpu zerocopy off) or a non-image present (a renderer
        // fallback path): back to the GOP, which is stale, so this present
        // is refused and the compositor repaints in full. Re-armed by the
        // next image present while requested.
        {const uint64_t f=IrqSpinLockAcquire(&lock);ZcFallback(image?BdwZeroCopyFallback::Disarmed:BdwZeroCopyFallback::NotImage);
         IrqSpinLockRelease(&lock,f);}
        ZcLeave();
        return BdwZeroCopyOutcome::RepairRequired;
    }
    if(!active){
        if(!image)return BdwZeroCopyOutcome::Copy;
        bool flip=false;
        {const uint64_t f=IrqSpinLockAcquire(&lock);flip=flipActive || flipRequested;
         if(!requested || flip)ZcFallback(BdwZeroCopyFallback::Disarmed);IrqSpinLockRelease(&lock,f);}
        if(!requested || flip || !ZcActivate())return BdwZeroCopyOutcome::Copy; // the BCS copy from the image is exact
    }
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const uint32_t slot=ZcSlotOf(source);
    if(ClientShowing()){
        ClientRestoreLocked(false);
        IrqSpinLockRelease(&lock,f);
        return BdwZeroCopyOutcome::RepairRequired;
    }
    if(!zcActive || zcDisabled){IrqSpinLockRelease(&lock,f);return BdwZeroCopyOutcome::Copy;}
    bool adopted=false;
    const auto p=zcRing.PresentCompleted(slot,adopted);
    if(p==BdwZeroCopyPresent::Deferred){
        ++zcStats.deferredPresents;
        IrqSpinLockRelease(&lock,f);
        return BdwZeroCopyOutcome::Deferred;
    }
    BdwZeroCopyOutcome outcome=BdwZeroCopyOutcome::Presented;
    if(p==BdwZeroCopyPresent::Rejected){
        ZcFallback(zcRing.Uncertain()?BdwZeroCopyFallback::Uncertain:BdwZeroCopyFallback::NotWriting);
        ZcDisableLocked("image-busy");outcome=BdwZeroCopyOutcome::RepairRequired;
    }else{
        zcImages[slot-1].privacy=privacy;
        ++zcStats.presents;
        if(adopted)++zcStats.adoptedPresents;
        {const uint64_t seq=zcRing.Sequence();auto& e=zcTimeline[seq%ZcTimelineSize];
         e={seq,slot,p==BdwZeroCopyPresent::Queued,Rdtsc(),0,0,0};zcSlotSequence[slot]=seq;}
        if(p==BdwZeroCopyPresent::ArmNow){++zcStats.armedNow;
            if(!zcVerifyHold && !captureHold && !ZcArmLocked(slot))outcome=BdwZeroCopyOutcome::RepairRequired;}
        else ++zcStats.queued;
        zcStats.replaced=zcRing.Replaced();zcLast=BdwZeroCopyFallback::None;
    }
    IrqSpinLockRelease(&lock,f);ZcLogPending();
    return outcome;
}
// A verified, owned front can remain on screen during present back-pressure.
bool BdwZeroCopyCanHoldFront(uint64_t owner){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const bool canHold=owner && zcOwner==owner && zcActive && zcRequested && !zcDisabled &&
        !zcLeavingForGop && !ClientShowing() && zcRing.HoldsFront(Read(BdwFlipRegs::SurfaceLive));
    IrqSpinLockRelease(&lock,f);
    return canHold;
}
void BdwZeroCopyNoteWorkerWait(){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    ++zcStats.workerWaits;
    IrqSpinLockRelease(&lock,f);
}
uint64_t zcRenderUc=0,zcRenderWb=0;
bool BdwZeroCopyUncachedRender(){return __atomic_load_n(&zcCacheMode,__ATOMIC_ACQUIRE)!=ZcCacheFlush;}
uint8_t BdwZeroCopyScanoutPat(){return __atomic_load_n(&zcCacheMode,__ATOMIC_ACQUIRE)==ZcCacheWt?2:0;}
void BdwZeroCopySetUncachedRender(bool on){__atomic_store_n(&zcCacheMode,on?uint32_t(ZcCacheUc):uint32_t(ZcCacheFlush),__ATOMIC_RELEASE);}
bool BdwZeroCopySetCacheMode(uint32_t mode){
    if(mode>ZcCacheWt)return false;
    // Images rendered under another policy may hold dirty WB lines: flush
    // each in full at its next present.
    const uint64_t f=IrqSpinLockAcquire(&lock);__atomic_store_n(&zcCacheMode,mode,__ATOMIC_RELEASE);zcFlushedOnce=0;IrqSpinLockRelease(&lock,f);
    return true;
}
const char* BdwZeroCopyCacheModeName(){
    switch(__atomic_load_n(&zcCacheMode,__ATOMIC_ACQUIRE)){case ZcCacheUc:return "uc";case ZcCacheWt:return "wt";default:return "flush";}
}
void BdwZeroCopyNoteRender(bool uncached){__atomic_add_fetch(uncached?&zcRenderUc:&zcRenderWb,1,__ATOMIC_RELAXED);}
bool BdwZeroCopyVerifyFront(BdwZeroCopyVerify& out){
    out={};out.result="inactive";
    uint32_t front=0;uint64_t sequence=0;BdwGpuBuffer* buffer=nullptr;
    auto snap=[&](uint32_t& f,uint64_t& q){const uint64_t g=IrqSpinLockAcquire(&lock);
        const bool ok=zcActive && !zcDisabled && displayMapped && !ClientShowing();f=ok?zcRing.Front():0;q=zcRing.Sequence();
        IrqSpinLockRelease(&lock,g);return ok;};
    // Hold the panel still: new presents are accepted (and mailboxed) but not
    // armed until the measurement ends. First let any armed flip latch.
    {const uint64_t f=IrqSpinLockAcquire(&lock);zcVerifyHold=true;IrqSpinLockRelease(&lock,f);}
    struct Release {~Release(){const uint64_t f=IrqSpinLockAcquire(&lock);zcVerifyHold=false;ZcPollLocked();IrqSpinLockRelease(&lock,f);}} release;
    for(unsigned i=0;i<20;++i){bool armed=false;
        {const uint64_t f=IrqSpinLockAcquire(&lock);if(zcActive){
            if(zcRing.HasArmed()){const uint32_t frame=Read(BdwFlipRegs::FrameCountA),live=Read(BdwFlipRegs::SurfaceLive);
                BdwFlipLatch latch{};if(zcRing.Poll(frame,live,Rdtsc(),latch)==BdwFlipPoll::Latched)zcStats.Latch(latch.frames,tscKhz?latch.ticks*1000/tscKhz:0);}
            armed=zcRing.HasArmed();}
         IrqSpinLockRelease(&lock,f);}
        if(!armed)break;SchedulerSleepMs(5);}
    if(!snap(front,sequence))return false;
    out.front=front;out.sequence=sequence;
    if(!front){out.result="gop-front";return false;}
    buffer=zcImages[front-1].buffer; // retained for the boot
    // Pipe A CRC, primary plane source (i915 ivb/hsw/bdw: PIPE_CRC_CTL_A
    // 0x60050, bit 31 enable, source bits 30:29 = 0; result 0x60064).
    constexpr uint32_t CrcControl=0x60050,CrcResult=0x60064;
    PlaneIo crc{displayRegs[0]};
    auto sample=[&](){
        // Wait two vblanks so the result covers a whole frame after the change.
        const uint32_t start=Read(BdwFlipRegs::FrameCountA);
        for(unsigned i=0;i<40 && Read(BdwFlipRegs::FrameCountA)-start<2;++i)SchedulerSleepMs(5);
        return crc.read32(CrcResult);
    };
    crc.write32(CrcControl,1u<<31);
    out.before[0]=sample();out.before[1]=sample();
    uint32_t f2=0;uint64_t q2=0;
    if(!snap(f2,q2) || f2!=front){crc.write32(CrcControl,0);out.result="front-changed";return false;}
    // Write back every line of the front image (the display only reads memory).
    uint32_t pages=0;(void)BdwGpuBufferPages(buffer,pages);
    for(uint32_t page=0;page<pages && page<SurfacePages;++page){
        const uint64_t base=BdwGpuBufferPageKernelAddress(buffer,page);if(!base)continue;
        for(uint32_t line=0;line<4096;line+=64)__asm__ volatile("clflush (%0)"::"r"(base+line):"memory");
        ++out.flushedPages;
    }
    __asm__ volatile("mfence":::"memory");
    out.after[0]=sample();out.after[1]=sample();
    crc.write32(CrcControl,0);
    if(!snap(f2,q2) || f2!=front){out.result="front-changed";return false;}
    out.stableBefore=out.before[0]==out.before[1];out.stableAfter=out.after[0]==out.after[1];
    out.changed=out.before[1]!=out.after[0];
    out.result=!out.stableBefore || !out.stableAfter?"crc-unstable (inconclusive)":
        out.changed?"stale-memory (display saw pixels the CPU flush changed)":"coherent";
    return true;
}
uint32_t BdwZeroCopyReadTimeline(BdwZeroCopyTimelineEntry* out,uint32_t max,uint64_t& khz){
    const uint64_t f=IrqSpinLockAcquire(&lock);khz=tscKhz;
    const uint64_t last=zcRing.Sequence();uint32_t n=0;
    for(uint64_t s=last>=ZcTimelineSize?last-ZcTimelineSize+1:1;s<=last && n<max;++s){
        const auto& e=zcTimeline[s%ZcTimelineSize];if(e.sequence==s)out[n++]=e;}
    IrqSpinLockRelease(&lock,f);return n;
}
bool BdwZeroCopyArm(const char*& reason){
    if(!BdwBcsScanoutEnabled()){reason="bcs-present-disabled";return false;}
    {const uint64_t f=IrqSpinLockAcquire(&lock);
     const bool disabled=zcDisabled,flip=flipRequested || flipActive,already=zcRequested;
     if(disabled)reason=zcReason;else if(flip)reason="m3-flip-armed";
     IrqSpinLockRelease(&lock,f);
     if(disabled || flip)return false;
     if(already){reason="armed";return true;}}
    BdwFlipProbe probe{};BdwFlipRefusal refusal{};
    if(!BdwScanoutFlipProbe(probe,refusal)){reason="probe-unavailable";return false;}
    if(refusal!=BdwFlipRefusal::None){reason=BdwFlipRefusalName(refusal);return false;}
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const bool ok=!zcDisabled && !flipRequested && !flipActive;
    if(ok){zcRequested=true;zcFlushedOnce=0;zcReason=zcCount?"armed; activates at the next image present":"armed; waiting for renderer registration";}
    reason=zcReason;
    IrqSpinLockRelease(&lock,f);
    return ok;
}
void BdwZeroCopySetAutoArm(bool enabled){__atomic_store_n(&zcAutoArm,enabled,__ATOMIC_RELEASE);}
bool BdwZeroCopyAutoArm(){return __atomic_load_n(&zcAutoArm,__ATOMIC_ACQUIRE);}
void BdwZeroCopyDisarm(){
    __atomic_store_n(&zcAutoArm,false,__ATOMIC_RELEASE); // an explicit off holds for the boot
    const uint64_t f=IrqSpinLockAcquire(&lock);zcRequested=false;if(!zcDisabled)zcReason="leaving at the next present";
    IrqSpinLockRelease(&lock,f);
}
void BdwZeroCopyReadStatus(BdwZeroCopyStatus& out){
    out={};out.built=true;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    out.registered=zcCount!=0;out.requested=zcRequested;out.active=zcActive;out.disabled=zcDisabled;
    out.images=zcCount;out.freeMask=zcActive?zcRing.FreeMask():(zcDisabled?0:(1u<<zcCount)-1u);
    out.front=zcActive?zcRing.Front():0;out.armed=zcActive && zcRing.HasArmed()?zcRing.ArmedSlot():~0u;
    out.queued=zcActive && zcRing.HasQueued()?zcRing.QueuedSlot():~0u;
    out.owner=zcOwner;out.sequence=zcRing.Sequence();out.setupUs=zcSetupUs;out.gopReturns=zcGopReturns;
    for(uint32_t i=0;i<BdwZeroCopyMaxImages;++i){
        out.ages[i]=zcActive?zcRing.Age(i+1):0;
        out.surfaces[i]=__atomic_load_n(&g_bdwScanoutSurfaces[BdwScanoutFlipSurfaceSlots+i],__ATOMIC_ACQUIRE);
    }
    out.reason=zcReason;out.last=zcLast;out.refusal=zcRefusal;out.stats=zcStats;out.ggtt=zcGgtt;
    IrqSpinLockRelease(&lock,f);
    out.uncachedRender=BdwZeroCopyUncachedRender();
    out.renderJobsUc=__atomic_load_n(&zcRenderUc,__ATOMIC_RELAXED);out.renderJobsWb=__atomic_load_n(&zcRenderWb,__ATOMIC_RELAXED);
}
#else
int64_t BdwZeroCopyRegister(uint64_t,BdwGpuBuffer* const*,uint32_t,const BdwZeroCopyGeometry&,BdwZeroCopyRefusal& r){r=BdwZeroCopyRefusal::None;return -38;}
bool BdwZeroCopyOnReserve(BdwGpuBuffer*){return true;}
void BdwZeroCopyOnRelease(BdwGpuBuffer*){}
BdwZeroCopyOutcome BdwZeroCopyPresentSource(BdwGpuBuffer*,const CompositorPresentRegion*,uint32_t,const CapturePrivacy&){return BdwZeroCopyOutcome::Copy;}
bool BdwZeroCopyCanHoldFront(uint64_t){return false;}
void BdwZeroCopyNoteWorkerWait(){}
void BdwZeroCopyPrepareScanout(BdwGpuBuffer*,const CompositorPresentRegion*,uint32_t){}
uint32_t BdwZeroCopyReadTimeline(BdwZeroCopyTimelineEntry*,uint32_t,uint64_t& khz){khz=0;return 0;}
void BdwZeroCopyReadStatus(BdwZeroCopyStatus& out){out={};out.reason="not built";out.last=BdwZeroCopyFallback::NotBuilt;}
bool BdwZeroCopyArm(const char*& reason){reason="not built";return false;}
void BdwZeroCopyDisarm(){}
void BdwZeroCopySetAutoArm(bool){}
bool BdwZeroCopyAutoArm(){return false;}
bool BdwZeroCopyUncachedRender(){return false;}
uint8_t BdwZeroCopyScanoutPat(){return 0;}
bool BdwZeroCopySetCacheMode(uint32_t){return false;}
const char* BdwZeroCopyCacheModeName(){return "not built";}
void BdwZeroCopySetUncachedRender(bool){}
void BdwZeroCopyNoteRender(bool){}
bool BdwZeroCopyVerifyFront(BdwZeroCopyVerify& out){out={};out.result="not built";return false;}
int64_t BdwZeroCopyAdopt(uint64_t,BdwGpuBuffer**,uint32_t& count){count=0;return -38;}
#endif
#else
int64_t BdwZeroCopyRegister(uint64_t,BdwGpuBuffer* const*,uint32_t,const BdwZeroCopyGeometry&,BdwZeroCopyRefusal& r){r=BdwZeroCopyRefusal::None;return -38;}
bool BdwZeroCopyOnReserve(BdwGpuBuffer*){return true;}
void BdwZeroCopyOnRelease(BdwGpuBuffer*){}
BdwZeroCopyOutcome BdwZeroCopyPresentSource(BdwGpuBuffer*,const CompositorPresentRegion*,uint32_t,const CapturePrivacy&){return BdwZeroCopyOutcome::Copy;}
bool BdwZeroCopyCanHoldFront(uint64_t){return false;}
void BdwZeroCopyNoteWorkerWait(){}
void BdwZeroCopyPrepareScanout(BdwGpuBuffer*,const CompositorPresentRegion*,uint32_t){}
uint32_t BdwZeroCopyReadTimeline(BdwZeroCopyTimelineEntry*,uint32_t,uint64_t& khz){khz=0;return 0;}
void BdwZeroCopyReadStatus(BdwZeroCopyStatus& out){out={};out.reason="not built";out.last=BdwZeroCopyFallback::NotBuilt;}
bool BdwZeroCopyArm(const char*& reason){reason="not built";return false;}
void BdwZeroCopyDisarm(){}
void BdwZeroCopySetAutoArm(bool){}
bool BdwZeroCopyAutoArm(){return false;}
bool BdwZeroCopyUncachedRender(){return false;}
uint8_t BdwZeroCopyScanoutPat(){return 0;}
bool BdwZeroCopySetCacheMode(uint32_t){return false;}
const char* BdwZeroCopyCacheModeName(){return "not built";}
void BdwZeroCopySetUncachedRender(bool){}
void BdwZeroCopyNoteRender(bool){}
bool BdwZeroCopyVerifyFront(BdwZeroCopyVerify& out){out={};out.result="not built";return false;}
int64_t BdwZeroCopyAdopt(uint64_t,BdwGpuBuffer**,uint32_t& count){count=0;return -38;}
#endif
#elif defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
// Neither option built: the default per-present BCS copy is unchanged.
BdwTargetSelect BdwScanoutTargetSelect(const CompositorPresentRegion*,uint32_t,BdwTargetPlan& plan){plan={};return BdwTargetSelect::Ok;}
bool BdwScanoutTargetFinish(BdwTargetPlan&,bool,const CapturePrivacy&){return true;}
void BdwScanoutFlipPoll(){}
bool BdwScanoutFlipAwayFromGop(){return false;}
void BdwScanoutFlipReturnToGop(){}
bool BdwScanoutFlipProbe(BdwFlipProbe& probe,BdwFlipRefusal& refusal){probe={};refusal=BdwFlipRefusal::NoSample;return false;}
bool BdwScanoutFlipArm(const char*& reason){reason="not built";return false;}
void BdwScanoutFlipDisarm(){}
bool BdwScanoutMapEnable(bool){return false;}
void BdwScanoutTargetReadStatus(BdwScanoutTargetStatus& out){out={};out.reason="not built";}
int64_t BdwZeroCopyRegister(uint64_t,BdwGpuBuffer* const*,uint32_t,const BdwZeroCopyGeometry&,BdwZeroCopyRefusal& r){r=BdwZeroCopyRefusal::None;return -38;}
bool BdwZeroCopyOnReserve(BdwGpuBuffer*){return true;}
void BdwZeroCopyOnRelease(BdwGpuBuffer*){}
BdwZeroCopyOutcome BdwZeroCopyPresentSource(BdwGpuBuffer*,const CompositorPresentRegion*,uint32_t,const CapturePrivacy&){return BdwZeroCopyOutcome::Copy;}
bool BdwZeroCopyCanHoldFront(uint64_t){return false;}
void BdwZeroCopyNoteWorkerWait(){}
void BdwZeroCopyPrepareScanout(BdwGpuBuffer*,const CompositorPresentRegion*,uint32_t){}
uint32_t BdwZeroCopyReadTimeline(BdwZeroCopyTimelineEntry*,uint32_t,uint64_t& khz){khz=0;return 0;}
void BdwZeroCopyReadStatus(BdwZeroCopyStatus& out){out={};out.reason="not built";out.last=BdwZeroCopyFallback::NotBuilt;}
bool BdwZeroCopyArm(const char*& reason){reason="not built";return false;}
void BdwZeroCopyDisarm(){}
void BdwZeroCopySetAutoArm(bool){}
bool BdwZeroCopyAutoArm(){return false;}
bool BdwZeroCopyUncachedRender(){return false;}
uint8_t BdwZeroCopyScanoutPat(){return 0;}
bool BdwZeroCopySetCacheMode(uint32_t){return false;}
const char* BdwZeroCopyCacheModeName(){return "not built";}
void BdwZeroCopySetUncachedRender(bool){}
void BdwZeroCopyNoteRender(bool){}
bool BdwZeroCopyVerifyFront(BdwZeroCopyVerify& out){out={};out.result="not built";return false;}
int64_t BdwZeroCopyAdopt(uint64_t,BdwGpuBuffer**,uint32_t& count){count=0;return -38;}
#endif
#if defined(BROOK_SSD_AUTO_RECOVERY) && defined(BROOK_BDW_SCANOUT_FLIP) && defined(BROOK_BDW_ZERO_COPY_SCANOUT) && !defined(BROOK_HOST_TEST)

static bool PublishClientRequest(bool onAp,bool canPresent){
    uint64_t f=IrqSpinLockAcquire(&lock);
    if(!clientRequest.buffer || clientRequest.publishOnAp!=onAp ||
       !clientRequest.publication.Begin()){
        IrqSpinLockRelease(&lock,f);
        return false;
    }
    const ClientRequest request=clientRequest;
    IrqSpinLockRelease(&lock,f);

    // The request retains the BO and excludes new GPU access. The render
    // fence follows the render/data cache flush, but the display does not
    // snoop the coherent CPU/GT LLC. Publish every page mapped to the
    // display, including tile and pitch padding. GGTT guard PTEs use a
    // separate zero page. Unrelated GPU jobs may run on the AP path.
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t start=Rdtsc();
#endif
    const bool flush=canPresent && !request.publication.Cancelled();
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t flushStart=flush?ElapsedClockNs():0;
#endif
    const bool published=flush && BdwGpuBufferSyncCpuCachePages(request.buffer,request.layout.Pages());
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t flushEnd=flush?ElapsedClockNs():0;
#endif
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t ticks=Rdtsc()-start;
#endif
    f=IrqSpinLockAcquire(&lock);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    clientRequest.flushTicks=ticks;
    clientTimeline.Flushed(request.timelineSequence,flushStart,flushEnd);
#endif
    clientRequest.publication.Finish(published);
    IrqSpinLockRelease(&lock,f);
    return true;
}

static void ClientObservationWorker(void*){
    for(;;){
        KSemaphoreWait(&clientObserverWake);
        // Approximately one vblank plus scheduling slack at 60 Hz.
        // Always leave the worker's original polling path available. A
        // stalled display or a long capture cannot keep this task awake.
        const uint64_t start=ApicTickCount();
        for(uint32_t poll=0;poll<BdwClientLatchObserver::MaxPolls;++poll){
            if(poll && ApicTickCount()-start>=BdwClientLatchObserver::PollWindowMs)break;
            const uint64_t f=IrqSpinLockAcquire(&lock);
            if(!poll)clientObserver.ConsumeWake();
            ClientPollLocked(true);
            const bool pending=clientObserver.Enabled() && ClientObservationPendingLocked();
            IrqSpinLockRelease(&lock,f);
            if(!pending || poll+1==BdwClientLatchObserver::MaxPolls)break;
            SchedulerSleepMs(1);
        }
    }
}

static bool InitClientObservationWorker(){
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&clientObserverStarted,&expected,1,false,
                                    __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return expected==2;
    KSemaphoreInit(&clientObserverWake,0);
    auto* worker=KernelThreadCreate("gpu-latch",ClientObservationWorker,nullptr);
    if(worker){
        worker->cpuAffinity=1;
        SchedulerAddProcess(worker);
    }
    __atomic_store_n(&clientObserverStarted,worker?2u:0u,__ATOMIC_RELEASE);
    return worker!=nullptr;
}

bool BdwClientScanoutSetOffWorkerObservation(bool enabled){
    if(enabled && (SmpGetCpuCount()<2 || !InitClientObservationWorker()))return false;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    clientObserver.SetEnabled(enabled);
    const bool wake=clientObserver.QueueWake(ClientObservationPendingLocked());
    IrqSpinLockRelease(&lock,f);
    if(wake)KSemaphoreSignal(&clientObserverWake);
    return true;
}

bool BdwClientScanoutOffWorkerObservation(){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const bool enabled=clientObserver.Enabled();
    IrqSpinLockRelease(&lock,f);
    return enabled;
}

static void ClientPublicationWorker(void*){
    for(;;){
        KSemaphoreWait(&clientPublicationWake);
        // Only the BSP worker binds or arms, after rechecking display
        // ownership, cancellation and the submission queue's health.
        if(PublishClientRequest(true,true))(void)BdwClientScanoutWakeWorker();
    }
}

static bool InitClientPublicationWorker(){
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&clientPublicationStarted,&expected,1,false,
                                    __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return expected==2;
    KSemaphoreInit(&clientPublicationWake,0);
    auto* worker=KernelThreadCreate("gpu-publish",ClientPublicationWorker,nullptr);
    if(worker){
        const uint32_t cpus=SmpGetCpuCount();
        if(cpus>1)worker->cpuAffinity=cpus-1;
        SchedulerAddProcess(worker);
    }
    __atomic_store_n(&clientPublicationStarted,worker?2u:0u,__ATOMIC_RELEASE);
    return worker!=nullptr;
}

bool BdwClientScanoutSetApPublication(bool enabled){
    if(enabled && (SmpGetCpuCount()<2 || !InitClientPublicationWorker()))return false;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    __atomic_store_n(&clientPublishOnAp,enabled,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&lock,f);
    return true;
}
bool BdwClientScanoutApPublication(){
    return __atomic_load_n(&clientPublishOnAp,__ATOMIC_ACQUIRE);
}

int64_t BdwClientScanoutPresent(uint64_t owner,BdwGpuBuffer* buffer,const BdwClientScanoutLayout& layout,uint64_t offer,
                                 const CapturePrivacy& privacy){
    if(!owner || !buffer || !offer || !layout.Valid() || !BdwGpuBufferAllowsUserMapping(buffer) ||
       BdwGpuBufferBytes(buffer)<uint64_t(layout.stride)*1800)return -22;
    if(!ClientOn())return -95;
    if(!BdwClientScanoutWakeWorker())return -19;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const bool publishOnAp=BdwClientScanoutApPublication();
    int64_t result=-11;
    if(zcOwner!=owner)result=-1;
    else if(zcActive && zcRequested && !zcDisabled && !zcVerifyHold && !captureHold && !clientRequest.buffer &&
            !clientLeaving && !clientFlip.Busy() && !zcRing.HasArmed() && !zcRing.HasQueued() &&
            (!cursorPlane.enabled() || cursorStatus.active) && BdwGpuBufferBeginCpuTransition(buffer)){
        BdwGpuBufferRef(buffer);
        clientRequest={buffer,layout,owner,offer,publishOnAp};
        clientRequest.privacy=privacy;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
        clientRequest.timelineSequence=clientTimeline.Admit(owner,offer,BdwGpuBufferIdentity(buffer),
                                                            publishOnAp,ElapsedClockNs());
#endif
        ++clientPresents;
        result=0;
    }
    IrqSpinLockRelease(&lock,f);
    if(!result){
        if(publishOnAp)KSemaphoreSignal(&clientPublicationWake);
        else (void)BdwClientScanoutWakeWorker();
    }
    return result;
}
uint32_t BdwClientScanoutReadTimeline(BdwClientTimelineEntry* out,uint32_t max){
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const uint32_t count=clientTimeline.Read(out,max);
    IrqSpinLockRelease(&lock,f);
    return count;
#else
    (void)out;(void)max;
    return 0;
#endif
}
int64_t BdwClientScanoutStatus(uint64_t owner,BrookGpuClientScanoutStatus& out){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    if(owner!=zcOwner){IrqSpinLockRelease(&lock,f);return -1;}
    const uint32_t flags=(clientFlip.Uncertain()?BROOK_GPU_CLIENT_SCANOUT_UNCERTAIN:0) |
        (clientLeaving?BROOK_GPU_CLIENT_SCANOUT_RESTORING:0);
    out={BROOK_GPU_CLIENT_SCANOUT_VERSION,flags,clientError,0,clientFlip.Front().identity,
         clientFlip.Armed().identity,BdwGpuBufferIdentity(clientRequest.buffer),clientFailedOffer,
         clientPresents,clientLatched,clientRestores};
    IrqSpinLockRelease(&lock,f);
    return 0;
}
int64_t BdwClientScanoutLeave(uint64_t owner){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    if(owner!=zcOwner){IrqSpinLockRelease(&lock,f);return -1;}
    if(clientRequest.buffer)clientLeaving=true;
    ClientRestoreLocked(false);
    IrqSpinLockRelease(&lock,f);
    return 0;
}
bool BdwClientScanoutWorkPending(){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const bool pending=clientRequest.buffer &&
        (!clientRequest.publishOnAp || clientRequest.publication.Finished());
    IrqSpinLockRelease(&lock,f);
    return pending;
}
bool BdwClientScanoutService(bool canPresent){
    ClientRequest request{};
    ClientMapping* choice=nullptr;
    {
        uint64_t f=IrqSpinLockAcquire(&lock);
        if(clientRequest.buffer && !clientRequest.publishOnAp && !clientRequest.publication.Finished()){
            IrqSpinLockRelease(&lock,f);
            (void)PublishClientRequest(false,canPresent);
            f=IrqSpinLockAcquire(&lock);
        }
        if(clientRequest.publication.Finished())request=clientRequest;
        if(request.buffer && request.publication.CanArm() && canPresent && !clientLeaving && zcActive && !zcDisabled && ClientOn()){
            for(auto& m:clientMappings){
                if(m.held || m.quarantined)continue;
                if(m.buffer==request.buffer){choice=&m;break;}
                if(!choice || !m.buffer)choice=&m;
            }
            if(choice)__atomic_store_n(&g_bdwClientSurface[choice-clientMappings],0,__ATOMIC_RELEASE);
        }
        IrqSpinLockRelease(&lock,f);
    }
    if(!request.buffer)return false;
    // The BSP worker takes no WM or compositor locks. The syscall checks
    // scene eligibility before admission and software takeover revokes it.
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t start=Rdtsc();
#endif
    const bool bound=choice && ClientBind(*choice,request);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t boundAt=Rdtsc();
#endif
    bool accepted=false,wakeObserver=false;
    {
        const uint64_t f=IrqSpinLockAcquire(&lock);
        if(bound && clientRequest.publication.CanArm() && !clientLeaving && zcActive && zcRequested && !zcDisabled &&
           zcOwner==request.owner && ClientOn() && !zcVerifyHold && !captureHold && !zcRing.HasArmed() && !zcRing.HasQueued()){
            const uint64_t descriptor=uint64_t(choice->Base())|(uint64_t(request.layout.stride)<<32)|
                (request.layout.tiled?(uint64_t{1}<<63):0);
            __atomic_store_n(&g_bdwClientSurface[choice-clientMappings],descriptor,__ATOMIC_RELEASE);
            __atomic_store_n(&g_clientPlaneTouched,true,__ATOMIC_RELEASE);
            PlaneIo io{displayRegs[2]};
            accepted=clientFlip.Arm(io,{choice->Base(),BdwGpuBufferIdentity(request.buffer),request.layout},
                                    zcRing.Surface(zcRing.Front()));
        }
        if(accepted){
            choice->held=true;
            choice->privacy=request.privacy;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
            clientArmedSequence=choice->timelineSequence=request.timelineSequence;
            clientTimeline.Armed(clientArmedSequence,ElapsedClockNs());
#endif
            wakeObserver=clientObserver.QueueWake(true);
        }else{
            BdwGpuBufferEndCpuTransition(request.buffer);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
            clientTimeline.Retired(request.timelineSequence,ElapsedClockNs(),BdwClientRetirement::Rejected);
#endif
            clientError=bound?-11:-5;clientFailedOffer=request.offer;
            if(!ClientShowing())clientLeaving=false;
        }
        clientRequest={};
        IrqSpinLockRelease(&lock,f);
    }
    if(wakeObserver)KSemaphoreSignal(&clientObserverWake);
    BdwGpuBufferUnref(request.buffer);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t end=Rdtsc();
    const uint64_t times[]={boundAt-start,request.flushTicks,end-boundAt,end-start+request.flushTicks};
    BdwWorkerStatsClientScanout(times,accepted);
#endif
    return true;
}

bool BdwCursorRequest(bool enable){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const bool ok=!enable || !cursorSetupFailed;
    cursorStatus.requested=enable && ok;
    if(!enable){CursorOffLocked();ClientRestoreLocked(false);}
    IrqSpinLockRelease(&lock,f);
    if(ok)CompositorWake();
    return ok;
}
BdwCursorStatus BdwCursorReadStatus(){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    auto status=cursorStatus;
    status.armed=cursorPlane.enabled();
    status.transparentTarget=status.armed && cursorImages.transparent();
    IrqSpinLockRelease(&lock,f);
    return status;
}
uint64_t BdwCursorRendererQuery(uint64_t owner,bool supportsHandoff,bool& suppress){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    CursorPollLocked();
    const bool eligible=supportsHandoff && owner && owner==zcOwner &&
        cursorStatus.requested && cursorStatus.bound && zcActive && zcRequested &&
        !zcDisabled && !zcLeavingForGop;
    const uint64_t now=eligible?ApicTickCount():0;
    const bool wake=eligible && cursorPlane.ready() && cursorHandoff.prepare(now);
    const uint64_t epoch=eligible ?
        cursorHandoff.offer(zcRing.Sequence(),now,cursorPlane.active() && cursorPlane.ready()):0;
    const bool hardwareMayBeVisible=cursorMayBeVisible && cursorPlane.blocksSoftware();
    suppress=eligible?cursorHandoff.suppress(hardwareMayBeVisible):hardwareMayBeVisible;
    const uint64_t ack=cursorHandoff.awaitingPresent()?epoch:0;
    IrqSpinLockRelease(&lock,f);
    if(wake)CompositorWake();
    return ack;
}
void BdwCursorRendererPresented(uint64_t owner,uint64_t epoch){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    const bool wake=owner && owner==zcOwner && zcActive && zcRequested && !zcDisabled &&
        cursorHandoff.presented(epoch,zcRing.Sequence());
    IrqSpinLockRelease(&lock,f);
    if(wake)CompositorWake();
}
bool BdwCursorBlocksSoftware(){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    CursorPollLocked();
    const bool blocks=cursorMayBeVisible && cursorPlane.blocksSoftware();
    IrqSpinLockRelease(&lock,f);
    return blocks;
}
void BdwCursorUpdate(uint64_t leaseOwner,bool visible,int32_t x,int32_t y,
                     const uint32_t* pixels,uint32_t width,uint32_t height){
    const uint64_t f=IrqSpinLockAcquire(&lock);
    struct Unlock {uint64_t flags;~Unlock(){IrqSpinLockRelease(&lock,flags);}} unlock{f};
    if(!CursorPollLocked()){
        ClientRestoreLocked(false);return;
    }
    if(!cursorStatus.requested || !cursorStatus.bound || !zcActive || !zcRequested ||
       !leaseOwner || leaseOwner!=zcOwner || !cursorHandoff.inProgress()) {
        if(visible)ClientRestoreLocked(false);
        CursorOffLocked();return;
    }
    if(visible && (!pixels || !width || !height || width>64 || height>64)){
        ClientRestoreLocked(false);CursorOffLocked(true);return;
    }
    if(!cursorPlane.ready())return;
    const bool probe=cursorHandoff.probing();
    if(probe && cursorPlane.enabled())return;
    PlaneIo io{displayRegs[2]};
    const uint32_t frame=io.read32(BdwFlipRegs::FrameCountA);
    if(frame==0xffffffffu)return;
    const bool activating=!probe && !cursorHandoff.owned();
    if(activating){
        // The transparent probe stays on while the renderer clears its old
        // cursor. Only the live slot proves the old pixels are gone.
        const uint32_t front=zcRing.Front();
        if(!front || io.read32(BdwFlipRegs::SurfaceLive)!=zcRing.Surface(front) ||
           !cursorHandoff.mayEnable(zcSlotSequence[front]))return;
    }

    const uint64_t key=probe || !visible ? 0 : BdwCursorImageKey(pixels,width,height);
    // Fetch a transparent image on screen to ensure the surface can latch.
    if(!key)x=y=0;
    const uint32_t slot=cursorImages.select(key,probe || activating);
    const bool upload=cursorImages.needsUpload(slot,key);
    auto& image=cursorBacking[slot];
    if(upload){
        for(uint32_t row=0;row<64;++row){
            auto* dest=reinterpret_cast<uint32_t*>(image.pixels[row/16].cpu)+(row%16)*64;
            for(uint32_t col=0;col<64;++col){
                const uint32_t argb=key && row<height && col<width ? pixels[row*64+col] : 0;
                const uint32_t a=argb>>24;
                auto scale=[&](unsigned shift){return (((argb>>shift)&255u)*a+127)/255;};
                dest[col]=(a<<24)|(scale(16)<<16)|(scale(8)<<8)|scale(0);
            }
        }
    }
    if(activating)cursorHandoff.arm(ApicTickCount());
    cursorMayBeVisible|=key!=0;
    __atomic_store_n(&g_cursorTouched,true,__ATOMIC_RELEASE);
    const auto previous=cursorPlane.position();
    if((upload && !BdwPrivatePagesPublish(image.pixels,CursorImagePages)) ||
       !cursorPlane.update(io,image.reservation.first*4096,frame,x,y)){
        CursorOffLocked(true);cursorSetupFailed=true;cursorStatus.requested=false;
        ClientRestoreLocked(false);return;
    }
    if(upload)++cursorStatus.images;
    else if(!cursorPlane.ready())++cursorStatus.cachedArms;
    else if(previous!=cursorPlane.position())++cursorStatus.positions;
    cursorImages.selectProgrammed(slot,key);
}

#else
int64_t BdwClientScanoutPresent(uint64_t,BdwGpuBuffer*,const BdwClientScanoutLayout&,uint64_t,const CapturePrivacy&){return -38;}
int64_t BdwClientScanoutStatus(uint64_t,BrookGpuClientScanoutStatus&){return -38;}
int64_t BdwClientScanoutLeave(uint64_t){return -38;}
bool BdwClientScanoutWorkPending(){return false;}
bool BdwClientScanoutService(bool){return false;}
bool BdwClientScanoutSetApPublication(bool){return false;}
bool BdwClientScanoutApPublication(){return false;}
bool BdwClientScanoutSetOffWorkerObservation(bool){return false;}
bool BdwClientScanoutOffWorkerObservation(){return false;}
uint32_t BdwClientScanoutReadTimeline(BdwClientTimelineEntry*,uint32_t){return 0;}
bool BdwCursorRequest(bool){return false;}
BdwCursorStatus BdwCursorReadStatus(){return {};}
uint64_t BdwCursorRendererQuery(uint64_t,bool,bool& suppress){suppress=false;return 0;}
void BdwCursorRendererPresented(uint64_t,uint64_t){}
bool BdwCursorBlocksSoftware(){return false;}
void BdwCursorUpdate(uint64_t,bool,int32_t,int32_t,const uint32_t*,uint32_t,uint32_t){}
#endif


#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST) && defined(BROOK_BDW_SCANOUT_FLIP)
namespace {
// One identified live image: a kernel flip buffer (physical pages), or a
// renderer image / direct client buffer (BdwGpuBuffer pages; a client buffer
// is referenced for the copy because client mappings can be recycled).
struct CaptureSource {
    uint32_t plane=BROOK_DEBUG_FRAME_PLANE_UNKNOWN,slot=0,pages=0;
    BdwGpuBuffer* buffer=nullptr;const uint64_t* dma=nullptr;bool referenced=false;
};
uint8_t* CapturePage(const CaptureSource& s,uint32_t page){
    if(page>=s.pages)return nullptr;
    if(s.buffer)return reinterpret_cast<uint8_t*>(BdwGpuBufferPageKernelAddress(s.buffer,page));
    return s.dma && s.dma[page]?reinterpret_cast<uint8_t*>(PhysToVirt(PhysicalAddress(s.dma[page])).raw()):nullptr;
}
// Lock held: the image is still front or armed, so no writer can own it.
bool CaptureHeldLocked(const CaptureSource& s,uint32_t surface){
    (void)surface;
    if(s.plane==BROOK_DEBUG_FRAME_PLANE_FLIP)
        return flipActive && !flipDisabled && !chain.Uncertain() &&
            (chain.Front()==s.slot || (chain.HasArmed() && chain.ArmedSlot()==s.slot));
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    if(s.plane==BROOK_DEBUG_FRAME_PLANE_ZERO_COPY)
        return zcActive && !zcDisabled && !zcRing.Uncertain() &&
            (zcRing.Front()==s.slot || (zcRing.HasArmed() && zcRing.ArmedSlot()==s.slot));
    if(s.plane==BROOK_DEBUG_FRAME_PLANE_CLIENT)return clientFlip.Contains(surface);
#endif
    return false;
}
// Lock held. Matches the live surface against every surface Brook arms.
CaptureSource CaptureFindLocked(uint32_t live,CapturePrivacy& privacy){
    CaptureSource s{};
    for(uint32_t b=0;b<FlipBuffers;++b)
        if(buffers[b].bound && buffers[b].dma && buffers[b].reservation.first*4096==live){
            privacy=buffers[b].privacy;
            s.plane=BROOK_DEBUG_FRAME_PLANE_FLIP;s.slot=b+1;s.dma=buffers[b].dma;s.pages=SurfacePages;return s;
        }
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    for(uint32_t i=0;i<zcCount;++i)
        if(zcImages[i].bound && zcImages[i].buffer && zcImages[i].reservation.first*4096==live){
            uint32_t pages=0;(void)BdwGpuBufferPages(zcImages[i].buffer,pages); // retained for the boot
            privacy=zcImages[i].privacy;
            s.plane=BROOK_DEBUG_FRAME_PLANE_ZERO_COPY;s.slot=i+1;s.buffer=zcImages[i].buffer;s.pages=pages;return s;
        }
    for(auto& m:clientMappings)
        if(m.buffer && m.reservation.token && m.Base()==live){
            uint32_t pages=0;(void)BdwGpuBufferPages(m.buffer,pages);
            BdwGpuBufferRef(m.buffer);
            privacy=m.privacy;
            s.plane=BROOK_DEBUG_FRAME_PLANE_CLIENT;s.slot=live;s.buffer=m.buffer;s.pages=pages;s.referenced=true;return s;
        }
#endif
    return s;
}
void CaptureFlushPages(const CaptureSource& s,uint32_t pages){
    // GT/BCS writes with UC/WT PTEs bypass the CPU caches, and a previous
    // capture may have cached older lines: invalidate before reading. The
    // CPU never writes these lines, so the flush only discards clean copies
    // (or writes back LLC lines a WB render left, as the scanout flush does).
    for(uint32_t page=0;page<pages;++page){
        const uint8_t* base=CapturePage(s,page);if(!base)continue;
        for(uint32_t line=0;line<4096;line+=64)__asm__ volatile("clflush (%0)"::"r"(base+line):"memory");
    }
    __asm__ volatile("mfence":::"memory");
}
}

BdwScanoutCaptureKind BdwScanoutCaptureLive(uint32_t* destination,uint32_t width,uint32_t height,
                                                BdwScanoutCaptureInfo& info,uint64_t protectionEpoch){
    info={};
    if(!destination || !width || !height || width>4096 || height>2160)return BdwScanoutCaptureKind::Unsupported;
    CaptureSource source{};uint32_t live=0,control=0,stride=0,offset=0;
    {
        const uint64_t f=IrqSpinLockAcquire(&lock);
        // No flip path has ever mapped the display: DSPASURF was never
        // written, so the firmware GOP is the plane surface.
        if(!displayMapped){IrqSpinLockRelease(&lock,f);info.plane=BROOK_DEBUG_FRAME_PLANE_UNREAD;return BdwScanoutCaptureKind::Gop;}
        if(captureHold){IrqSpinLockRelease(&lock,f);return BdwScanoutCaptureKind::Busy;}
        live=Read(BdwFlipRegs::SurfaceLive);control=Read(BdwFlipRegs::PlaneControl);stride=Read(BdwFlipRegs::Stride);
        offset=Read(BdwFlipRegs::TileOffset)|Read(BdwFlipRegs::LinearOffset);
        info.surface=live;info.control=control;info.stride=stride;
        if(live==0xffffffffu){IrqSpinLockRelease(&lock,f);return BdwScanoutCaptureKind::Unsupported;}
        if(!live){IrqSpinLockRelease(&lock,f);info.plane=BROOK_DEBUG_FRAME_PLANE_GOP;return BdwScanoutCaptureKind::Gop;}
        source=CaptureFindLocked(live,info.privacy);
        info.plane=source.plane;
        const bool held=source.plane!=BROOK_DEBUG_FRAME_PLANE_UNKNOWN &&
            info.privacy.permits(protectionEpoch) && CaptureHeldLocked(source,live);
        if(held)captureHold=true;
        IrqSpinLockRelease(&lock,f);
        if(!held){
            if(source.referenced)BdwGpuBufferUnref(source.buffer);
            return source.plane==BROOK_DEBUG_FRAME_PLANE_UNKNOWN?BdwScanoutCaptureKind::Unsupported:BdwScanoutCaptureKind::Busy;
        }
    }
    BdwCaptureLayout layout{};
    bool valid=BdwCaptureLayoutFrom(control,stride,offset,width,height,layout) && layout.pages<=source.pages;
    if(valid){
        const uint64_t start=Rdtsc();
        CaptureFlushPages(source,layout.pages);
        valid=BdwCaptureCopy(destination,layout,[&](uint32_t page){return static_cast<const uint8_t*>(CapturePage(source,page));});
        if(layout.tiled)info.flags|=BROOK_DEBUG_FRAME_INFO_DETILED;
        if(layout.swapRb)info.flags|=BROOK_DEBUG_FRAME_INFO_SWAPPED_RB;
        info.copyUs=tscKhz?uint32_t((Rdtsc()-start)*1000/tscKhz):0;
    }
    bool held=false;
    {
        const uint64_t f=IrqSpinLockAcquire(&lock);
        held=CaptureHeldLocked(source,live);
        captureHold=false;
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
        ZcPollLocked();
#endif
        PollLocked();
        IrqSpinLockRelease(&lock,f);
    }
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    ZcLogPending();
#endif
    LogPending();
    if(source.referenced)BdwGpuBufferUnref(source.buffer);
    if(!valid)return BdwScanoutCaptureKind::Unsupported;
    return held?BdwScanoutCaptureKind::Copied:BdwScanoutCaptureKind::Busy;
}

bool BdwScanoutCaptureCursor(uint32_t* destination,uint32_t width,uint32_t height){
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    static uint32_t pixels[64*64]; // compositor thread only
    int32_t x=0,y=0;
    {
        const uint64_t f=IrqSpinLockAcquire(&lock);
        bool visible=false;
        if(displayMapped && cursorStatus.bound && Read(BdwCursorRegs::Control)){
            const uint32_t base=Read(BdwCursorRegs::Live)&BdwCursorRegs::AddressMask,position=Read(BdwCursorRegs::Position);
            for(uint32_t slot=0;slot<2 && !visible;++slot){
                if(!base || cursorBacking[slot].reservation.first*4096!=base)continue;
                for(uint32_t row=0;row<64;++row)
                    memcpy(pixels+row*64,reinterpret_cast<const uint32_t*>(cursorBacking[slot].pixels[row/16].cpu)+(row%16)*64,64*4);
                visible=true;
            }
            BdwCaptureCursorPosition(position,x,y);
        }
        IrqSpinLockRelease(&lock,f);
        if(!visible)return false;
    }
    BdwCaptureCursorComposite(destination,width,height,pixels,x,y);
    return true;
#else
    (void)destination;(void)width;(void)height;return false;
#endif
}
#endif

}
