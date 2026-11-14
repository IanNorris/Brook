#pragma once
#include <stdint.h>
#include "intel_bdw_display_flip.h"

namespace brook {
// Pure policy for zero-copy scanout on the Dell XPS 13 9343 (pipe A, plane A,
// firmware GOP mode 3200x1800 linear XRGB8888, stride 12800). The desktop
// renderer registers a ring of its own protected (non-user-mappable) output
// buffers; the kernel binds them in the GGTT and arms DSPASURF to the one a
// present names, so no copy happens. Nothing here touches hardware; see
// docs/bdw-zero-copy-scanout.md for the design and src/kernel/src/
// intel_bdw_zero_copy.cpp for the worker integration.
//
// Slot 0 is the firmware GOP surface (GGTT offset 0, the panic and fallback
// surface); slots 1..count are renderer images. At most one slot is armed
// (written to DSPASURF, not yet latched) and at most one completed frame is
// queued behind it. The queue is a mailbox: a newer present replaces the
// queued image, which the display has never fetched, and frees it.
//
// An image may be written by the GPU only after BeginWrite, which requires
// that it is not front, armed, queued or already being written. It is
// released back to the renderer when the display stops fetching it, i.e.
// when a later latch makes a different slot front - never at submit time.
constexpr uint32_t BdwZeroCopyMaxImages=4;
constexpr uint32_t BdwZeroCopyWidth=3200,BdwZeroCopyHeight=1800,BdwZeroCopyStride=12800;
constexpr uint32_t BdwZeroCopyFourccXrgb8888=0x34325258u; // DRM 'XR24'
constexpr uint64_t BdwZeroCopyImageBytes=uint64_t(BdwZeroCopyStride)*BdwZeroCopyHeight; // 5625 pages exactly

enum class BdwZeroCopyRefusal : uint8_t {
    None,Count,Geometry,Format,Size,UserMappable,Identity,Duplicate,
};
inline const char* BdwZeroCopyRefusalName(BdwZeroCopyRefusal r){
    switch(r){
    case BdwZeroCopyRefusal::None:return "none";
    case BdwZeroCopyRefusal::Count:return "image-count";
    case BdwZeroCopyRefusal::Geometry:return "geometry";
    case BdwZeroCopyRefusal::Format:return "format";
    case BdwZeroCopyRefusal::Size:return "buffer-size";
    case BdwZeroCopyRefusal::UserMappable:return "user-mappable";
    case BdwZeroCopyRefusal::Identity:return "identity";
    case BdwZeroCopyRefusal::Duplicate:return "duplicate";
    }
    return "unknown";
}
// One renderer buffer offered for scanout, as the kernel sees it.
struct BdwZeroCopyCandidate {
    uint64_t identity=0,bytes=0;
    bool userMappable=true;
};
struct BdwZeroCopyGeometry {uint32_t width=0,height=0,stride=0,fourcc=0;};
// Registration check. A user-mappable buffer must never become a scanout
// surface (a client could then put arbitrary pixels on the panel outside the
// compositor); only protected handoff buffers of exactly the panel mode pass.
inline BdwZeroCopyRefusal BdwZeroCopyEligible(const BdwZeroCopyGeometry& g,const BdwZeroCopyCandidate* c,uint32_t count){
    if(count<3 || count>BdwZeroCopyMaxImages || !c)return BdwZeroCopyRefusal::Count;
    if(g.width!=BdwZeroCopyWidth || g.height!=BdwZeroCopyHeight || g.stride!=BdwZeroCopyStride)return BdwZeroCopyRefusal::Geometry;
    if(g.fourcc!=BdwZeroCopyFourccXrgb8888)return BdwZeroCopyRefusal::Format;
    for(uint32_t i=0;i<count;++i){
        if(c[i].userMappable)return BdwZeroCopyRefusal::UserMappable;
        if(c[i].bytes<BdwZeroCopyImageBytes || c[i].bytes>BdwZeroCopyImageBytes+4095)return BdwZeroCopyRefusal::Size;
        if(!c[i].identity)return BdwZeroCopyRefusal::Identity;
        for(uint32_t j=0;j<i;++j)if(c[j].identity==c[i].identity)return BdwZeroCopyRefusal::Duplicate;
    }
    return BdwZeroCopyRefusal::None;
}

enum class BdwZeroCopyPresent : uint8_t {Rejected,ArmNow,Queued};
class BdwZeroCopyRing {
    uint32_t surfaces_[BdwZeroCopyMaxImages+1]{};
    uint64_t identities_[BdwZeroCopyMaxImages+1]{};
    uint64_t presented_[BdwZeroCopyMaxImages+1]{}; // present sequence that last filled the slot
    uint32_t slots_=0,front_=0,armed_=0,queued_=0,toArm_=0,writing_=0;
    bool hasArmed_=false,hasQueued_=false,hasToArm_=false,uncertain_=false,leaving_=false;
    uint32_t requestFrame_=0;uint64_t requestTicks_=0,sequence_=0,replaced_=0;
    static constexpr uint32_t Bit(uint32_t s){return 1u<<s;}
    bool Busy(uint32_t s)const{
        return s==front_ || (hasArmed_ && armed_==s) || (hasQueued_ && queued_==s) || (hasToArm_ && toArm_==s) || (writing_&Bit(s));
    }
public:
    // surfaces are GGTT byte offsets (256 KiB aligned); identities are the
    // registered buffer identities. The GOP (surface 0) must be live.
    bool Configure(const uint32_t* surfaces,const uint64_t* identities,uint32_t count){
        if(slots_ || !surfaces || !identities || count<3 || count>BdwZeroCopyMaxImages)return false;
        for(uint32_t i=0;i<count;++i){
            if(!surfaces[i] || (surfaces[i]&(BdwFlipRegs::SurfaceAlignment-1)) || !identities[i])return false;
            for(uint32_t j=0;j<i;++j)if(surfaces[j]==surfaces[i] || identities[j]==identities[i])return false;
        }
        surfaces_[0]=0;identities_[0]=0;
        for(uint32_t i=0;i<count;++i){surfaces_[i+1]=surfaces[i];identities_[i+1]=identities[i];presented_[i+1]=0;}
        slots_=count+1;front_=0;writing_=0;sequence_=0;replaced_=0;
        hasArmed_=hasQueued_=hasToArm_=uncertain_=leaving_=false;
        return true;
    }
    bool Configured()const{return slots_!=0;}
    uint32_t Images()const{return slots_?slots_-1:0;}
    uint32_t Surface(uint32_t slot)const{return slot<slots_?surfaces_[slot]:~0u;}
    uint32_t Front()const{return front_;}
    bool HasArmed()const{return hasArmed_;}
    uint32_t ArmedSlot()const{return armed_;}
    bool HasQueued()const{return hasQueued_;}
    uint32_t QueuedSlot()const{return queued_;}
    bool Uncertain()const{return uncertain_;}
    bool Leaving()const{return leaving_;}
    uint64_t Sequence()const{return sequence_;}
    uint64_t Replaced()const{return replaced_;}
    // 1..count for a registered identity, 0 otherwise.
    uint32_t Find(uint64_t identity)const{
        if(!identity)return 0;
        for(uint32_t s=1;s<slots_;++s)if(identities_[s]==identity)return s;
        return 0;
    }
    // Bit i set: image i (slot i+1) may be written now.
    uint32_t FreeMask()const{
        if(!slots_ || uncertain_ || leaving_)return 0;
        uint32_t mask=0;for(uint32_t s=1;s<slots_;++s)if(!Busy(s))mask|=Bit(s-1);
        return mask;
    }
    // Frames presented since the slot's content was last presented; 0 when it
    // never held a presented frame (the renderer must then draw it in full).
    uint64_t Age(uint32_t slot)const{
        return slot && slot<slots_ && presented_[slot]?sequence_-presented_[slot]+1:0;
    }
    // A GPU job reserved the image (render target). Refused unless free.
    bool BeginWrite(uint32_t slot){
        if(!slot || slot>=slots_ || uncertain_ || leaving_ || Busy(slot))return false;
        writing_|=Bit(slot);return true;
    }
    // The job was released without a present (failure or cancel): the image
    // content is unknown and it must be redrawn in full before display.
    void AbandonWrite(uint32_t slot){if(slot<slots_ && (writing_&Bit(slot))){writing_&=~Bit(slot);presented_[slot]=0;}}
    // The render job completed; the frame becomes the newest. ArmNow tells
    // the caller to write DSPASURF at once (then call Armed); Queued means it
    // waits behind the armed flip, replacing (freeing) any older queued one.
    BdwZeroCopyPresent Present(uint32_t slot){
        if(!slot || slot>=slots_ || uncertain_ || leaving_ || !(writing_&Bit(slot)))return BdwZeroCopyPresent::Rejected;
        writing_&=~Bit(slot);presented_[slot]=++sequence_;
        if(hasArmed_ || hasToArm_){
            if(hasQueued_ && queued_!=slot)++replaced_;
            queued_=slot;hasQueued_=true;return BdwZeroCopyPresent::Queued;
        }
        toArm_=slot;hasToArm_=true;return BdwZeroCopyPresent::ArmNow;
    }
    // frame is the counter value read before the DSPASURF write.
    bool Armed(uint32_t slot,uint32_t frame,uint64_t ticks){
        if(!hasToArm_ || slot!=toArm_ || uncertain_)return false;
        hasToArm_=false;armed_=slot;hasArmed_=true;requestFrame_=frame;requestTicks_=ticks;return true;
    }
    // Read the frame counter first, then DSPASURFLIVE. Three frames after the
    // write without that surface live freezes the ring (Uncertain).
    BdwFlipPoll Poll(uint32_t frame,uint32_t live,uint64_t ticks,BdwFlipLatch& out){
        if(uncertain_)return BdwFlipPoll::Uncertain;
        if(!hasArmed_)return BdwFlipPoll::Idle;
        const uint32_t frames=frame-requestFrame_;
        if(!frames)return BdwFlipPoll::Pending;
        if(live!=surfaces_[armed_]){
            if(frames>=3){uncertain_=true;return BdwFlipPoll::Uncertain;}
            return BdwFlipPoll::Pending;
        }
        out={armed_,frames,ticks>requestTicks_?ticks-requestTicks_:0};
        front_=armed_;hasArmed_=false;
        return BdwFlipPoll::Latched;
    }
    // After a latch, the queued frame (if any) is the next to arm.
    bool TakeQueued(uint32_t& slot){
        if(uncertain_ || hasArmed_ || hasToArm_ || !hasQueued_)return false;
        hasQueued_=false;toArm_=slot=queued_;hasToArm_=true;return true;
    }
    // Leave zero-copy (unregister, owner exit, lease expiry, disable): the
    // CPU has fully rewritten the GOP. Drops the queued frame; returns true
    // when DSPASURF must be written with the GOP (then Armed(0,...)).
    bool RequestGop(){
        if(!slots_)return false;
        leaving_=true;hasQueued_=false;
        if(front_==0 && !hasArmed_){hasToArm_=false;return false;}
        if(hasArmed_ && armed_==0)return false;
        toArm_=0;hasToArm_=true;return true;
    }
    // The display provably fetches only the GOP and nothing is armed or being
    // written: the images' GGTT entries may be unbound and the buffers freed.
    bool Quiescent()const{return slots_ && front_==0 && !hasArmed_ && !hasToArm_ && !writing_;}
    // Hardware state can no longer be trusted (latch timeout, a plane change):
    // freeze. Recovery is RestoreGop (MMIO) and a software repaint; the images
    // stay bound until a later latch of the GOP is observed.
    void MarkUncertain(){if(slots_)uncertain_=true;}
    // Uncertain recovery: the GOP was rewritten and observed live again.
    void GopObserved(){if(slots_){uncertain_=false;leaving_=true;front_=0;hasArmed_=hasQueued_=hasToArm_=false;}}
};

// Per-present diagnostics, reported by "gpu zerocopy status".
enum class BdwZeroCopyFallback : uint8_t {
    None,NotBuilt,Disarmed,NotRegistered,NotImage,NotWriting,Uncertain,Leaving,PlaneChanged,Bind,Count_
};
inline const char* BdwZeroCopyFallbackName(BdwZeroCopyFallback f){
    switch(f){
    case BdwZeroCopyFallback::None:return "none";
    case BdwZeroCopyFallback::NotBuilt:return "not-built";
    case BdwZeroCopyFallback::Disarmed:return "disarmed";
    case BdwZeroCopyFallback::NotRegistered:return "not-registered";
    case BdwZeroCopyFallback::NotImage:return "source-not-image";
    case BdwZeroCopyFallback::NotWriting:return "image-not-reserved";
    case BdwZeroCopyFallback::Uncertain:return "uncertain";
    case BdwZeroCopyFallback::Leaving:return "leaving";
    case BdwZeroCopyFallback::PlaneChanged:return "plane-changed";
    case BdwZeroCopyFallback::Bind:return "ggtt-bind";
    case BdwZeroCopyFallback::Count_:break;
    }
    return "unknown";
}
struct BdwZeroCopyStats {
    uint64_t presents=0,armedNow=0,queued=0,replaced=0,latched=0,missedVblanks=0,uncertain=0,refusedReserve=0;
    uint64_t fallback[uint32_t(BdwZeroCopyFallback::Count_)]{};
    uint64_t latchUsSum=0,latchUsMax=0;
    uint64_t latchFrames[4]{}; // 1, 2, 3, >3
    void Latch(uint32_t frames,uint64_t us){
        ++latched;latchUsSum+=us;if(us>latchUsMax)latchUsMax=us;
        ++latchFrames[frames>=4?3:frames?frames-1:0];
        if(frames>1)missedVblanks+=frames-1;
    }
};
}
