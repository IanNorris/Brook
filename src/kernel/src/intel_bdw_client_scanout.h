#pragma once

#include "intel_bdw_display_flip.h"

namespace brook {

struct BdwClientScanoutLayout {
    uint32_t stride=0;
    bool tiled=false;

    uint32_t Control() const {
        return BdwFlipRegs::GopControl | (tiled ? BdwFlipRegs::TiledBit : 0);
    }
    bool Valid() const {
        return stride>=12800 && stride<=16384 && !(stride%(tiled ? 512 : 64));
    }
    constexpr uint32_t Pages() const {
        // 1800 rows is already aligned to the eight-row X tile height.
        return (uint64_t(stride)*1800+4095)/4096;
    }
};

// Both workers and takeover use the display lock. The request retains the
// buffer and its exclusive transition until the submission worker consumes a
// finished publication, including when takeover cancels it during the flush.
class BdwClientPublication {
    enum class Phase : uint8_t { Queued, Flushing, Finished };
    Phase phase_=Phase::Queued;
    bool cancelled_=false, published_=false;
public:
    bool Begin() {
        if(phase_!=Phase::Queued)return false;
        phase_=Phase::Flushing;
        return true;
    }
    void Finish(bool published) {
        published_=published;
        phase_=Phase::Finished;
    }
    void Cancel(){cancelled_=true;}
    bool Cancelled() const {return cancelled_;}
    bool Finished() const {return phase_==Phase::Finished;}
    bool CanArm() const {return Finished() && published_ && !cancelled_;}
};

struct BdwClientScanoutImage {
    uint32_t base=0;
    uint64_t identity=0;
    BdwClientScanoutLayout layout{12800,false};
};

template<class Io>
void BdwClientScanoutArm(Io& io,const BdwClientScanoutImage& image) {
    io.write32(BdwFlipRegs::Stride,image.layout.stride);
    // On Broadwell this register is DSPOFFSET, shared by linear and X-tiled.
    io.write32(BdwFlipRegs::TileOffset,0);
    io.write32(BdwFlipRegs::PlaneControl,image.layout.Control());
    io.write32(BdwFlipRegs::Surface,image.base);
    (void)io.read32(BdwFlipRegs::Surface);
}

// One in-flight hardware flip. Software may stage one submission elsewhere,
// but may only call Arm while there is no previous pending write. Restore can
// replace a pending write, so it tracks both old client addresses until GOP
// is confirmed after a vblank. No timeout releases an uncertain address.
class BdwClientScanoutFlip {
    BdwClientScanoutImage front_{}, armed_{};
    uint32_t original_=0, frame_=0, restoreBase_=0;
    bool pending_=false, restoring_=false, uncertain_=false;

public:
    bool Busy() const {return pending_ || restoring_ || uncertain_;}
    bool Uncertain() const {return uncertain_;}
    bool CanObserve() const {return pending_ && !restoring_ && !uncertain_;}
    bool Restoring() const {return restoring_;}
    const BdwClientScanoutImage& Front() const {return front_;}
    const BdwClientScanoutImage& Armed() const {return armed_;}
    bool Contains(uint32_t base) const {
        return base && (front_.base==base || armed_.base==base);
    }

    template<class Io>
    bool Arm(Io& io,const BdwClientScanoutImage& image,uint32_t knownKernelFront) {
        if(Busy() || !image.base || image.base%BdwFlipRegs::SurfaceAlignment ||
           !image.identity || !image.layout.Valid() || front_.base==image.base)return false;
        const uint32_t live=io.read32(BdwFlipRegs::SurfaceLive);
        const uint32_t frame=io.read32(BdwFlipRegs::FrameCountA);
        const auto expected=front_.base?front_.layout:BdwClientScanoutLayout{12800,false};
        if(frame==0xffffffffu || live!=(front_.base?front_.base:knownKernelFront) ||
           io.read32(BdwFlipRegs::Surface)!=live || io.read32(BdwFlipRegs::TileOffset) ||
           io.read32(BdwFlipRegs::PlaneControl)!=expected.Control() ||
           io.read32(BdwFlipRegs::Stride)!=expected.stride)return false;
        original_=live;
        frame_=frame;
        armed_=image;
        pending_=true;
        BdwClientScanoutArm(io,image);
        return true;
    }

    template<class Io>
    bool Restore(Io& io,uint32_t base) {
        if(restoring_ && restoreBase_==base)return true;
        const uint32_t frame=io.read32(BdwFlipRegs::FrameCountA);
        const uint32_t control=io.read32(BdwFlipRegs::PlaneControl);
        if(frame==0xffffffffu || (control&~BdwFlipRegs::TiledBit)!=BdwFlipRegs::GopControl){
            uncertain_=true;return false;
        }
        restoring_=true;
        frame_=frame;
        restoreBase_=base;
        BdwClientScanoutArm(io,{base,0,{12800,false}});
        return true;
    }

    template<class Io>
    bool Poll(Io& io) {
        if(!pending_ && !restoring_)return false;
        const uint32_t live=io.read32(BdwFlipRegs::SurfaceLive);
        const uint32_t frame=io.read32(BdwFlipRegs::FrameCountA);
        if(frame==0xffffffffu || live==0xffffffffu){uncertain_=true;return false;}
        const uint32_t elapsed=frame-frame_;
        if(restoring_){
            // Require a later vblank even when the old live value was GOP.
            // A client write might have been armed just before Restore.
            if(elapsed && live==restoreBase_ && io.read32(BdwFlipRegs::Surface)==restoreBase_ &&
               io.read32(BdwFlipRegs::PlaneControl)==BdwFlipRegs::GopControl &&
               io.read32(BdwFlipRegs::Stride)==12800 && !io.read32(BdwFlipRegs::TileOffset)){
                *this=BdwClientScanoutFlip{};
                return true;
            }
        }else if(!uncertain_ && elapsed && live==armed_.base &&
                 io.read32(BdwFlipRegs::Surface)==armed_.base &&
                 io.read32(BdwFlipRegs::PlaneControl)==armed_.layout.Control() &&
                 io.read32(BdwFlipRegs::Stride)==armed_.layout.stride && !io.read32(BdwFlipRegs::TileOffset)){
            front_=armed_;
            armed_={};
            pending_=false;
            return true;
        }else if(live!=original_ && live!=armed_.base){
            uncertain_=true;
        }
        if(elapsed>=4)uncertain_=true;
        return false;
    }
};

// All transitions hold the scanout lock. A single pending wake suffices
// because any observer poll uses the currently armed flip.
class BdwClientLatchObserver {
public:
    static constexpr uint32_t PollWindowMs=24;
    static constexpr uint32_t MaxPolls=PollWindowMs; // one poll per 1 ms sleep
    bool Enabled() const {return enabled_;}
    void SetEnabled(bool enabled){enabled_=enabled;}
    bool MayPoll(bool pending,bool captureHold,bool verifyHold) const {
        return enabled_ && pending && !captureHold && !verifyHold;
    }
    bool QueueWake(bool eligible){
        if(!enabled_ || !eligible || wakeQueued_)return false;
        wakeQueued_=true;
        return true;
    }
    void ConsumeWake(){wakeQueued_=false;}

private:
    bool enabled_=false,wakeQueued_=false;
};

} // namespace brook
