#pragma once
#include <stdint.h>

namespace brook {
// Pure policy for Broadwell primary-plane page flipping on the Dell XPS 13
// 9343 (pipe A, eDP transcoder, firmware GOP mode 3200x1800 linear XRGB8888).
// Nothing here touches hardware; callers supply register samples and perform
// every MMIO access. See docs/bdw-page-flip.md for the evidence and the plan.
//
// Register offsets are BAR0 (GTTMMADR) relative and follow Linux i915
// (i9xx_plane_regs.h, intel_display_reg_defs.h, intel_psr_regs.h):
struct BdwFlipRegs {
    // Pipe A timing generator and frame counter (always-on power well).
    static constexpr uint32_t PipeSource=0x6001c;      // PIPESRC(A): (w-1)<<16 | (h-1)
    static constexpr uint32_t DdiEdpControl=0x6f400;   // TRANS_DDI_FUNC_CTL(EDP)
    static constexpr uint32_t PsrControl=0x6f800;      // EDP_PSR_CTL (BDW: transcoder EDP + 0x800)
    static constexpr uint32_t PipeConfA=0x70008;       // PIPECONF(transcoder A)
    static constexpr uint32_t PipeStatA=0x70024;       // PIPEASTAT (legacy; informational only)
    static constexpr uint32_t FrameCountA=0x70040;     // PIPE_FRMCOUNT_G4X(A), increments at vblank start
    static constexpr uint32_t FlipCountA=0x70044;      // PIPE_FLIPCOUNT_G4X(A)
    // Primary plane A.
    static constexpr uint32_t PlaneControl=0x70180;    // DSPACNTR
    static constexpr uint32_t LinearOffset=0x70184;    // DSPALINOFF
    static constexpr uint32_t Stride=0x70188;          // DSPASTRIDE (bytes when linear)
    static constexpr uint32_t Surface=0x7019c;         // DSPASURF, armed by write, latched at vblank
    static constexpr uint32_t TileOffset=0x701a4;      // DSPATILEOFF
    static constexpr uint32_t SurfaceLive=0x701ac;     // DSPASURFLIVE: surface the plane is fetching
    static constexpr uint32_t PlaneControlB=0x71180,PlaneControlC=0x72180;
    static constexpr uint32_t PipeConfEdp=0x7f008;     // PIPECONF(transcoder EDP)
    // DSPACNTR inherited from the Dell GOP: enable, XRGB8888 (format 0110),
    // linear, no gamma, no rotation. Exactly the value the existing
    // per-job display invariant (BdwDisplayUsesGop) already requires.
    static constexpr uint32_t GopControl=0x98000000u,TiledBit=1u<<10,EnableBit=1u<<31;
    // Display surfaces need 256 KiB alignment plus 128 guard PTEs after the
    // surface for the SNB-BDW VT-d prefetch workaround (i915 does the same).
    static constexpr uint32_t SurfaceAlignment=256*1024,GuardPages=128;
};

// One point-in-time read-only sample. frame0 and frame1 bracket an interval
// of elapsedUs microseconds measured by the caller (TSC).
struct BdwFlipProbe {
    uint32_t pipeSource=0,ddiEdp=0,psr=0,pipeConfA=0,pipeConfEdp=0;
    uint32_t control=0,linearOffset=0,stride=0,surface=0,tileOffset=0,surfaceLive=0;
    uint32_t controlB=0,controlC=0;
    uint32_t frame0=0,frame1=0,flipCount=0;
    uint64_t elapsedUs=0;
};
enum class BdwFlipRefusal : uint8_t {
    None,NoSample,EdpTranscoder,EdpPipeSelect,TranscoderDisabled,PsrEnabled,PlaneDisabled,
    PlaneControl,Tiled,Stride,Surface,SurfaceNotLatched,Offset,OtherPlane,PipeSource,
    FrameCounterStalled,FrameRate,
};
inline const char* BdwFlipRefusalName(BdwFlipRefusal r){
    switch(r){
    case BdwFlipRefusal::None:return "none";
    case BdwFlipRefusal::NoSample:return "no-sample";
    case BdwFlipRefusal::EdpTranscoder:return "edp-transcoder-disabled";
    case BdwFlipRefusal::EdpPipeSelect:return "edp-input-not-pipe-a";
    case BdwFlipRefusal::TranscoderDisabled:return "transcoder-disabled";
    case BdwFlipRefusal::PsrEnabled:return "psr-enabled";
    case BdwFlipRefusal::PlaneDisabled:return "plane-disabled";
    case BdwFlipRefusal::PlaneControl:return "plane-control";
    case BdwFlipRefusal::Tiled:return "tiled";
    case BdwFlipRefusal::Stride:return "stride";
    case BdwFlipRefusal::Surface:return "surface-not-gop";
    case BdwFlipRefusal::SurfaceNotLatched:return "surface-not-latched";
    case BdwFlipRefusal::Offset:return "plane-offset";
    case BdwFlipRefusal::OtherPlane:return "other-pipe-plane-enabled";
    case BdwFlipRefusal::PipeSource:return "pipe-source-size";
    case BdwFlipRefusal::FrameCounterStalled:return "frame-counter-stalled";
    case BdwFlipRefusal::FrameRate:return "frame-rate";
    }
    return "unknown";
}
struct BdwFlipExpect {uint32_t width=3200,height=1800,strideBytes=12800,gopSurface=0;};
// Every check must pass before any display register write. The first
// failing check is returned so the fallback can log one precise reason.
inline BdwFlipRefusal BdwFlipEvaluate(const BdwFlipProbe& p,const BdwFlipExpect& e){
    // eDP on Haswell/Broadwell is driven by the dedicated EDP transcoder.
    // EDP input select (bits 14:12): 0 = pipe A always on, 4 = pipe A on/off.
    if(!(p.ddiEdp&(1u<<31)))return BdwFlipRefusal::EdpTranscoder;
    const uint32_t select=(p.ddiEdp>>12)&7;
    if(select!=0 && select!=4)return BdwFlipRefusal::EdpPipeSelect;
    if((p.pipeConfEdp&0xc0000000u)!=0xc0000000u)return BdwFlipRefusal::TranscoderDisabled; // enable + state
    // PSR can stop the frame counter and keeps the link idle: never flip under it.
    if(p.psr&(1u<<31))return BdwFlipRefusal::PsrEnabled;
    if(!(p.control&BdwFlipRegs::EnableBit))return BdwFlipRefusal::PlaneDisabled;
    if(p.control&BdwFlipRegs::TiledBit)return BdwFlipRefusal::Tiled;
    if(p.control!=BdwFlipRegs::GopControl)return BdwFlipRefusal::PlaneControl;
    if(p.stride!=e.strideBytes)return BdwFlipRefusal::Stride;
    if(p.surface!=e.gopSurface)return BdwFlipRefusal::Surface;
    if(p.surfaceLive!=p.surface)return BdwFlipRefusal::SurfaceNotLatched;
    if(p.linearOffset || p.tileOffset)return BdwFlipRefusal::Offset;
    if((p.controlB|p.controlC)&BdwFlipRegs::EnableBit)return BdwFlipRefusal::OtherPlane;
    if(p.pipeSource!=(((e.width-1)<<16)|(e.height-1)))return BdwFlipRefusal::PipeSource;
    const uint32_t frames=p.frame1-p.frame0; // wraps modulo 2^32
    if(!frames || !p.elapsedUs)return BdwFlipRefusal::FrameCounterStalled;
    // Accept 45-125 Hz: the panel is nominally 60 Hz; DRRS or a rate change
    // outside this window means our vblank model is wrong.
    const uint64_t milliHz=uint64_t(frames)*1000000000ull/p.elapsedUs;
    if(milliHz<45000 || milliHz>125000)return BdwFlipRefusal::FrameRate;
    return BdwFlipRefusal::None;
}

// Damage bookkeeping in screen pixels. Overflow collapses to one bounding
// box, which is conservative (copies more, never less).
struct BdwFlipRect {uint32_t x=0,y=0,width=0,height=0;};
constexpr uint32_t BdwFlipDamageMax=32;
struct BdwFlipDamage {
    BdwFlipRect rects[BdwFlipDamageMax]{};
    uint32_t count=0;
    bool full=false;
    void Clear(){count=0;full=false;}
    void SetFull(){count=0;full=true;}
    bool Empty()const{return !full && !count;}
    static bool Contains(const BdwFlipRect& a,const BdwFlipRect& b){
        return b.x>=a.x && b.y>=a.y && b.x+b.width<=a.x+a.width && b.y+b.height<=a.y+a.height;
    }
    void Collapse(){
        if(full || !count)return;
        uint32_t x0=~0u,y0=~0u,x1=0,y1=0;
        for(uint32_t i=0;i<count;++i){const auto& r=rects[i];
            if(r.x<x0)x0=r.x;
            if(r.y<y0)y0=r.y;
            if(r.x+r.width>x1)x1=r.x+r.width;
            if(r.y+r.height>y1)y1=r.y+r.height;}
        rects[0]={x0,y0,x1-x0,y1-y0};count=1;
    }
    void Add(const BdwFlipRect& r){
        if(full || !r.width || !r.height)return;
        for(uint32_t i=0;i<count;++i)if(Contains(rects[i],r))return;
        if(count==BdwFlipDamageMax){Collapse();
            if(!Contains(rects[0],r)){
                const uint32_t x0=r.x<rects[0].x?r.x:rects[0].x,y0=r.y<rects[0].y?r.y:rects[0].y;
                const uint32_t ax=rects[0].x+rects[0].width,ay=rects[0].y+rects[0].height;
                const uint32_t x1=r.x+r.width>ax?r.x+r.width:ax,y1=r.y+r.height>ay?r.y+r.height:ay;
                rects[0]={x0,y0,x1-x0,y1-y0};
            }
            return;
        }
        rects[count++]=r;
    }
    void Merge(const BdwFlipDamage& other){
        if(other.full){SetFull();return;}
        for(uint32_t i=0;i<other.count;++i)Add(other.rects[i]);
    }
};

// Buffer rotation and retirement. Slot 0 is the firmware GOP surface; slots
// 1..count are kernel scanout buffers. At most one flip is armed (written to
// DSPASURF and not yet latched) and at most one completed frame is queued
// behind it (mailbox: a newer frame rewrites the queued buffer in place).
//
// A buffer may be written only when it is neither front, armed, queued nor
// otherwise retained; the queued buffer is the one exception, because the
// display has never seen it. Latch proof: the frame counter has advanced past
// the value sampled before the last DSPASURF write, and DSPASURFLIVE equals
// that write. At vblank start the hardware latches the last armed surface.
//
// Staleness is relative to the newest complete frame ("latest"), not to the
// front: stale(X) covers every pixel where X may differ from latest. Writing
// a frame into X therefore repairs stale(X) from latest, then applies damage.
constexpr uint32_t BdwFlipMaxSlots=4; // GOP + up to three buffers
enum class BdwFlipPoll : uint8_t {Idle,Pending,Latched,Uncertain};
struct BdwFlipLatch {uint32_t slot=0,frames=0;uint64_t ticks=0;};
class BdwFlipChain {
    uint32_t surfaces_[BdwFlipMaxSlots]{};
    BdwFlipDamage stale_[BdwFlipMaxSlots]{};
    uint32_t slots_=0,front_=0,armed_=0,queued_=0,latest_=0,toArm_=0,retained_=1,acquired_=0;
    bool hasArmed_=false,hasQueued_=false,hasToArm_=false,uncertain_=false;
    uint32_t requestFrame_=0;uint64_t requestTicks_=0;
    static constexpr uint32_t Bit(uint32_t s){return 1u<<s;}
public:
    // surfaces are GGTT byte offsets; the GOP surface must be the live one.
    bool Configure(uint32_t gopSurface,const uint32_t* surfaces,uint32_t count){
        if(slots_ || !surfaces || count<2 || count>BdwFlipMaxSlots-1 || (gopSurface&(BdwFlipRegs::SurfaceAlignment-1)))return false;
        for(uint32_t i=0;i<count;++i){
            if(!surfaces[i] || (surfaces[i]&(BdwFlipRegs::SurfaceAlignment-1)) || surfaces[i]==gopSurface)return false;
            for(uint32_t j=0;j<i;++j)if(surfaces[j]==surfaces[i])return false;
        }
        surfaces_[0]=gopSurface;stale_[0].Clear();
        for(uint32_t i=0;i<count;++i){surfaces_[i+1]=surfaces[i];stale_[i+1].SetFull();}
        slots_=count+1;front_=latest_=0;retained_=Bit(0);acquired_=0;
        hasArmed_=hasQueued_=hasToArm_=uncertain_=false;
        return true;
    }
    bool Configured()const{return slots_!=0;}
    uint32_t Slots()const{return slots_;}
    uint32_t Surface(uint32_t slot)const{return slot<slots_?surfaces_[slot]:~0u;}
    uint32_t Front()const{return front_;}
    uint32_t Latest()const{return latest_;}
    bool HasArmed()const{return hasArmed_;}
    uint32_t ArmedSlot()const{return armed_;}
    bool HasQueued()const{return hasQueued_;}
    uint32_t QueuedSlot()const{return queued_;}
    bool Uncertain()const{return uncertain_;}
    // Front or anything that may still be scanned out is retained.
    bool Retained(uint32_t slot)const{return slot<slots_ && (retained_&Bit(slot));}
    bool Reusable(uint32_t slot)const{
        return slot && slot<slots_ && !uncertain_ && !(retained_&Bit(slot)) && !(acquired_&Bit(slot)) &&
            !(hasQueued_ && queued_==slot) && !(hasToArm_ && toArm_==slot);
    }
    const BdwFlipDamage& Stale(uint32_t slot)const{return stale_[slot<slots_?slot:0];}
    // Next kernel buffer to write, or 0. Prefers a free buffer; otherwise
    // takes back the queued (never displayed) one, which already holds latest.
    uint32_t AcquireBack(){
        if(!slots_ || uncertain_ || acquired_ || hasToArm_)return 0;
        for(uint32_t n=1;n<slots_;++n){
            const uint32_t s=(latest_+n-1)%(slots_-1)+1; // round-robin after latest
            if(Reusable(s)){acquired_|=Bit(s);return s;}
        }
        if(hasQueued_ && queued_){hasQueued_=false;acquired_|=Bit(queued_);return queued_;}
        return 0;
    }
    // Leaving flip mode: the GOP becomes the next target once the display
    // has stopped reading it (it is neither front, armed nor retained).
    bool AcquireGop(){
        if(!slots_ || uncertain_ || acquired_ || hasToArm_ || (retained_&Bit(0)) || (hasQueued_ && !queued_))return false;
        acquired_|=Bit(0);return true;
    }
    // The copy into slot retired: slot now holds latest + damage. armNow
    // tells the caller to write DSPASURF at once; otherwise the frame is
    // queued behind the armed flip (and replaces any older queued frame).
    bool Commit(uint32_t slot,const BdwFlipDamage& damage,bool& armNow){
        armNow=false;
        if(slot>=slots_ || uncertain_ || !(acquired_&Bit(slot)))return false;
        acquired_&=~Bit(slot);
        for(uint32_t s=0;s<slots_;++s){if(s==slot)stale_[s].Clear();else stale_[s].Merge(damage);}
        latest_=slot;
        if(hasArmed_){queued_=slot;hasQueued_=true;return true;}
        toArm_=slot;hasToArm_=true;armNow=true;return true;
    }
    // A copy that did not retire cleanly leaves the chain unknowable.
    void Abandon(uint32_t slot){if(slot<slots_){acquired_&=~Bit(slot);stale_[slot].SetFull();}uncertain_=true;}
    // After a latch, the queued frame (if any) becomes the one to arm.
    bool TakeQueued(uint32_t& slot){
        if(uncertain_ || hasArmed_ || !hasQueued_ || hasToArm_)return false;
        hasQueued_=false;toArm_=slot=queued_;hasToArm_=true;return true;
    }
    // Software takeover: the CPU fully rewrote the GOP, which becomes latest.
    // Returns true when DSPASURF must be written (then call Armed(0,...)).
    // A buffer already armed stays retained because it may have latched.
    bool RequestGop(){
        if(!slots_ || acquired_)return false;
        for(uint32_t s=1;s<slots_;++s)stale_[s].SetFull();
        stale_[0].Clear();latest_=0;hasQueued_=false;
        if(front_==0 && !hasArmed_){hasToArm_=false;return false;}
        toArm_=0;hasToArm_=true;return true;
    }
    // frame is the counter value read before the DSPASURF write.
    bool Armed(uint32_t slot,uint32_t frame,uint64_t ticks){
        if(!hasToArm_ || slot!=toArm_ || uncertain_)return false;
        hasToArm_=false;armed_=slot;hasArmed_=true;retained_|=Bit(slot);
        requestFrame_=frame;requestTicks_=ticks;return true;
    }
    // Read the frame counter first, then DSPASURFLIVE. Three frames after
    // the write without that surface live is Uncertain and freezes the chain.
    BdwFlipPoll Poll(uint32_t frame,uint32_t live,uint64_t ticks,BdwFlipLatch& out){
        if(uncertain_)return BdwFlipPoll::Uncertain;
        if(!hasArmed_)return BdwFlipPoll::Idle;
        const uint32_t frames=frame-requestFrame_;
        if(!frames)return BdwFlipPoll::Pending;
        if(live!=surfaces_[armed_]){
            if(frames>=3){uncertain_=true;return BdwFlipPoll::Uncertain;}
            return BdwFlipPoll::Pending; // counter and latch sampled across a vblank edge
        }
        out={armed_,frames,ticks>requestTicks_?ticks-requestTicks_:0};
        front_=armed_;hasArmed_=false;retained_=Bit(front_);
        return BdwFlipPoll::Latched;
    }
    void MarkUncertain(){if(slots_)uncertain_=true;}
    // The GOP (already latest) was written outside the chain, e.g. by the
    // per-present BCS path after leaving flip mode: buffers lost track of it.
    void GopWritten(){if(latest_==0)for(uint32_t s=1;s<slots_;++s)stale_[s].SetFull();}
    // Will the panel show the newest frame from the GOP (front or armed)?
    bool GopCurrent()const{return latest_==0 && (hasArmed_?armed_==0:front_==0);}
};

// Diagnostics for binding one flip buffer's GGTT extent (reported by
// "gpu flip status"). Stage names the step that stopped the bind.
enum class BdwFlipGgttStage : uint8_t {
    None,Reserve,Precondition,FlushAudit,PteAudit,Unstable,Refused,Uncertain,Bound,
};
inline const char* BdwFlipGgttStageName(BdwFlipGgttStage s){
    switch(s){
    case BdwFlipGgttStage::None:return "none";
    case BdwFlipGgttStage::Reserve:return "reserve";
    case BdwFlipGgttStage::Precondition:return "precondition";
    case BdwFlipGgttStage::FlushAudit:return "flush-page-audit";
    case BdwFlipGgttStage::PteAudit:return "pte-page-audit";
    case BdwFlipGgttStage::Unstable:return "snapshot-unstable";
    case BdwFlipGgttStage::Refused:return "bind-refused";
    case BdwFlipGgttStage::Uncertain:return "bind-uncertain";
    case BdwFlipGgttStage::Bound:return "bound";
    }
    return "unknown";
}
struct BdwFlipGgttDiag {
    BdwFlipGgttStage stage=BdwFlipGgttStage::None;
    uint32_t buffer=0,first=0,count=0,entries=0,auditPage=0;
    // Snapshot of the inherited entries (twice-read): zero, valid (bit 0),
    // other nonzero; the first nonzero index and its value.
    uint32_t zero=0,valid=0,other=0,firstNonzero=0;uint64_t firstNonzeroValue=0;
    bool takeover=false;
};
// Classify a stable snapshot of the extent before binding.
inline void BdwFlipGgttClassify(const uint64_t* values,uint32_t first,uint32_t count,BdwFlipGgttDiag& d){
    d.zero=d.valid=d.other=0;d.firstNonzero=0;d.firstNonzeroValue=0;
    for(uint32_t i=0;i<count;++i){const uint64_t v=values[i];
        if(!v){++d.zero;continue;}
        if(v&1)++d.valid;else ++d.other;
        if(!d.firstNonzeroValue){d.firstNonzero=first+i;d.firstNonzeroValue=v;}
    }
    d.takeover=d.valid || d.other;
}

// Minimal panic/reset restore of the firmware surface. Lock-free and
// MMIO-only: one store arms the GOP surface; then a bounded spin waits for
// DSPASURFLIVE to report it so subsequent panic drawing into the firmware
// framebuffer is visible. The order matters: restore first, draw second.
struct BdwFlipRestoreResult {bool attempted=false,written=false,latched=false;};
template<class Io> BdwFlipRestoreResult BdwFlipRestoreGop(Io& io,uint32_t gopSurface,uint64_t budgetTicks){
    BdwFlipRestoreResult r{};r.attempted=true;
    const uint32_t control=io.read32(BdwFlipRegs::PlaneControl);
    // A disabled plane or a firmware modeset (control changed) means there is
    // nothing safe to arm; never write anything else.
    if(control==0xffffffffu || control!=BdwFlipRegs::GopControl)return r;
    if(io.read32(BdwFlipRegs::SurfaceLive)==gopSurface && io.read32(BdwFlipRegs::Surface)==gopSurface){
        r.latched=true;return r;
    }
    io.write32(BdwFlipRegs::Surface,gopSurface);
    (void)io.read32(BdwFlipRegs::Surface); // posting read
    r.written=true;
    const uint64_t start=io.now();
    for(;;){
        if(io.read32(BdwFlipRegs::SurfaceLive)==gopSurface){r.latched=true;return r;}
        const uint64_t now=io.now();
        if(now<start || now-start>=budgetTicks)return r;
        io.relax();
    }
}

// Persistent GOP mapping revalidation cadence. Every present re-reads the
// display registers and the first/last GOP PTE; every Nth present (and the
// first) repeats the full twice-read of all GOP PTEs.
class BdwPersistentMapPolicy {
    uint32_t sinceFull_=0;bool valid_=false;
public:
    static constexpr uint32_t FullEvery=64;
    bool NeedFull()const{return !valid_ || sinceFull_>=FullEvery;}
    void Full(bool ok){valid_=ok;sinceFull_=0;}
    void Hit(){++sinceFull_;}
    void Invalidate(){valid_=false;}
    bool Valid()const{return valid_;}
};
}
