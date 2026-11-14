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
#include "intel_bdw_scanout.h"
#include "intel_bdw_oneshot.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_display_probe.h"
#include "intel_bdw_ggtt_extent_bind.h"
#include "intel_bdw_critical.h"
#include "intel_bdw_worker_stats.h"
#include "intel_bdw_forcewake_adapter.h"
#include "compositor.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "native_tsc_observe.h"
#include "boot_protocol/boot_protocol.h"
#include "acpi.h"
#include "spinlock.h"
#include "scheduler.h"
#include "serial.h"
#include "smp.h"

namespace brook {
namespace {
// UC alias of BAR0+0x70000 (pipe A frame counter and primary plane). Published
// only once a flip path may have written DSPASURF; boot lifetime.
volatile uint32_t* g_planePage=nullptr;
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
    (void)BdwFlipRestoreGop(io,0,__atomic_load_n(&g_restoreBudgetTicks,__ATOMIC_RELAXED));
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
constexpr uint32_t FlipBuffers=3,ExtentPages=SurfacePages+BdwFlipRegs::GuardPages;
static_assert(FlipBuffers+1<=BdwFlipMaxSlots && (FlipBuffers+1)*LeafTables<=BdwPpgttMaxLeaves);
struct FlipBuffer {uint64_t* dma=nullptr;Leaves leaves;BdwGgttReservation reservation{};BdwGgttExtentBind bind;bool bound=false;};
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
    if(!flipActive || flipDisabled)return;
    if(chain.HasArmed()){
        const uint32_t frame=Read(BdwFlipRegs::FrameCountA),live=Read(BdwFlipRegs::SurfaceLive);
        BdwFlipLatch latch{};
        switch(chain.Poll(frame,live,Rdtsc(),latch)){
        case BdwFlipPoll::Latched:
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
        const uint64_t f=IrqSpinLockAcquire(&lock);DisableLocked(why);IrqSpinLockRelease(&lock,f);LogPending();return false;
    };
    if(!MapDisplay())return fail("display-map");
    if(!ValidateGop())return false; // transient: retried by the next present
    if(!EnsureZero())return fail("alloc");
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
        // A range left valid by an earlier boot is refused before any write;
        // keep that reservation forever (never reuse stale indices) and try
        // the next aligned extent.
        for(unsigned attempt=0;!buffer.bound && attempt<3;++attempt){
            if(!buffer.reservation.token &&
               !BdwGlobalGgttSpace().reserveExtent(ExtentPages,BdwFlipRegs::SurfaceAlignment/4096,buffer.reservation))return fail("ggtt-space");
            const BdwGgttExtentLease lease{buffer.reservation,buffer.dma};
            if(VmmDellFlipGgttBind(buffer.bind,lease,false,true)){buffer.bound=true;break;}
            if(buffer.bind.State()!=BdwGgttExtentState::Fresh)return fail("ggtt-bind-uncertain");
            SerialPrintf("gpu-flip: GGTT extent %u+%u not free; quarantined\n",buffer.reservation.first,ExtentPages);
            buffer.reservation={};(void)buffer.bind.Recycle();
        }
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
bool BdwScanoutTargetFinish(BdwTargetPlan& plan,bool copied){
#if defined(BROOK_BDW_SCANOUT_FLIP)
    if(plan.kind!=BdwTargetKind::Flip)return true;
    const uint64_t f=IrqSpinLockAcquire(&lock);
    bool ok=false,armNow=false;
    if(!copied){chain.Abandon(plan.slot);DisableLocked("copy-failed");}
    else if(!chain.Commit(plan.slot,plan.damage,armNow))DisableLocked("commit-rejected");
    else if(armNow)ok=ArmLocked(plan.slot);
    else {ok=true;Stat(BdwWorkerScanoutEvent::FlipQueued);}
    IrqSpinLockRelease(&lock,f);LogPending();
    return ok;
#else
    (void)plan;(void)copied;return true;
#endif
}
void BdwScanoutFlipPoll(){
#if defined(BROOK_BDW_SCANOUT_FLIP)
    if(!__atomic_load_n(&flipActive,__ATOMIC_ACQUIRE))return;
    const uint64_t f=IrqSpinLockAcquire(&lock);PollLocked();IrqSpinLockRelease(&lock,f);
    LogPending();
#endif
}
bool BdwScanoutFlipAwayFromGop(){
#if defined(BROOK_BDW_SCANOUT_FLIP)
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
    out.gopReturns=gopReturns;out.activationUs=activationUs;
    IrqSpinLockRelease(&lock,f);
#else
    out.reason="not built";
#endif
}
#elif defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
// Neither option built: the default per-present BCS copy is unchanged.
BdwTargetSelect BdwScanoutTargetSelect(const CompositorPresentRegion*,uint32_t,BdwTargetPlan& plan){plan={};return BdwTargetSelect::Ok;}
bool BdwScanoutTargetFinish(BdwTargetPlan&,bool){return true;}
void BdwScanoutFlipPoll(){}
bool BdwScanoutFlipAwayFromGop(){return false;}
void BdwScanoutFlipReturnToGop(){}
bool BdwScanoutFlipProbe(BdwFlipProbe& probe,BdwFlipRefusal& refusal){probe={};refusal=BdwFlipRefusal::NoSample;return false;}
bool BdwScanoutFlipArm(const char*& reason){reason="not built";return false;}
void BdwScanoutFlipDisarm(){}
bool BdwScanoutMapEnable(bool){return false;}
void BdwScanoutTargetReadStatus(BdwScanoutTargetStatus& out){out={};out.reason="not built";}
#endif
}
