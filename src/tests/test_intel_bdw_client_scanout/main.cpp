#undef NDEBUG
#include <cassert>
#include <cstdint>
#include "intel_bdw_client_scanout.h"
#include "intel_bdw_client_timeline.h"

using namespace brook;

struct FakeDisplay {
    uint32_t frame=0, live=0, base=0, stride=12800, offset=0;
    uint32_t control=BdwFlipRegs::GopControl;
    uint32_t order[4]{};
    uint32_t writes=0;

    uint32_t read32(uint32_t reg) const {
        switch(reg){
        case BdwFlipRegs::FrameCountA:return frame;
        case BdwFlipRegs::SurfaceLive:return live;
        case BdwFlipRegs::Surface:return base;
        case BdwFlipRegs::Stride:return stride;
        case BdwFlipRegs::TileOffset:return offset;
        case BdwFlipRegs::PlaneControl:return control;
        default:assert(false);return 0;
        }
    }
    void write32(uint32_t reg,uint32_t value){
        order[writes++%4]=reg;
        switch(reg){
        case BdwFlipRegs::Surface:base=value;break;
        case BdwFlipRegs::Stride:stride=value;break;
        case BdwFlipRegs::TileOffset:offset=value;break;
        case BdwFlipRegs::PlaneControl:control=value;break;
        default:assert(false);
        }
    }
    void Vblank(){++frame;live=base;}
};

static void CheckTimeline(){
    BdwClientTimeline timeline;
    BdwClientTimelineEntry rows[BdwClientTimeline::Capacity];
    assert(!timeline.Read(rows,BdwClientTimeline::Capacity));
    const uint64_t first=timeline.Admit(10,41,80,false,1000);
    timeline.Flushed(first,1001,1008);
    timeline.Armed(first,1012);
    timeline.Latched(first,1020);
    const uint64_t second=timeline.Admit(10,42,81,true,1021);
    timeline.Flushed(second,1024,1032);
    timeline.Armed(second,1034);
    assert(timeline.Read(rows,2)==2 && !rows[0].retiredNs && !rows[1].latchNs);
    timeline.Latched(second,1040,true);
    timeline.Retired(first,1041,BdwClientRetirement::Replacement,second);
    assert(timeline.Read(rows,2)==2 && rows[0].sequence==first && rows[1].sequence==second);
    assert(rows[0].owner==10 && rows[0].offer==41 && rows[0].buffer==80 && !rows[0].publishOnAp);
    assert(rows[0].admittedNs==1000 && rows[0].flushStartNs==1001 && rows[0].flushEndNs==1008);
    assert(rows[0].armNs==1012 && rows[0].latchNs==1020 && rows[0].retiredNs==1041);
    assert(rows[0].retiredBy==rows[1].sequence && rows[0].retirement==BdwClientRetirement::Replacement);
    assert(rows[1].publishOnAp && rows[1].latchOffWorker && !rows[0].latchOffWorker && rows[1].latchNs<rows[0].retiredNs);
    timeline.Retired(second,1044,BdwClientRetirement::Restore);
    const uint64_t failed=timeline.Admit(11,42,82,true,1045);
    timeline.Retired(failed,1046,BdwClientRetirement::Rejected);
    assert(timeline.Read(rows,2)==2 && rows[0].retirement==BdwClientRetirement::Restore && !rows[0].retiredBy);
    assert(rows[1].retirement==BdwClientRetirement::Rejected && !rows[1].armNs && !rows[1].flushStartNs);

    for(uint32_t i=0;i<BdwClientTimeline::Capacity;++i)timeline.Admit(12,100+i,83,false,2000+i);
    timeline.Retired(first,3000,BdwClientRetirement::Restore);
    timeline.Flushed(second,3001,3002);
    assert(timeline.Read(rows,BdwClientTimeline::Capacity)==BdwClientTimeline::Capacity);
    for(uint32_t i=0;i<BdwClientTimeline::Capacity;++i){
        assert(rows[i].offer==100+i && rows[i].owner==12 && rows[i].buffer==83);
        assert(!rows[i].flushStartNs && !rows[i].retiredNs);
    }
}

static void CheckObserver(){
    BdwClientLatchObserver observer;
    assert(!observer.Enabled() && !observer.QueueWake(true) && !observer.MayPoll(true,false,false));
    observer.SetEnabled(true);
    assert(observer.MayPoll(true,false,false));
    assert(!observer.MayPoll(false,false,false));
    assert(!observer.MayPoll(true,true,false) && !observer.MayPoll(true,false,true));
    assert(!observer.QueueWake(false) && observer.QueueWake(true) && !observer.QueueWake(true));
    observer.SetEnabled(false);
    assert(!observer.QueueWake(true));
    observer.SetEnabled(true);
    assert(!observer.QueueWake(true)); // one wake remains pending through the toggle
    observer.ConsumeWake();
    assert(observer.QueueWake(true));
    observer.ConsumeWake();

    const BdwClientScanoutLayout linear{12800,false}, tiled{13312,true};
    const BdwClientScanoutImage first{0x10000000,1,linear}, next{0x12000000,2,tiled};
    FakeDisplay io;
    BdwClientScanoutFlip flip;
    assert(!flip.CanObserve() && flip.Arm(io,first,0));
    assert(flip.CanObserve() && !flip.Poll(io)); // no later vblank
    io.Vblank();
    assert(flip.Poll(io) && !flip.CanObserve());

    assert(flip.Arm(io,next,0));
    ++io.frame; // vblank count alone cannot release the previous mapping
    assert(flip.CanObserve() && !flip.Poll(io) && flip.Contains(first.base));
    io.live=next.base;io.stride=linear.stride;
    assert(!flip.Poll(io) && flip.Contains(first.base));
    io.stride=tiled.stride;
    assert(flip.Poll(io) && !flip.Contains(first.base) && !flip.CanObserve());

    assert(flip.Arm(io,first,0) && flip.Restore(io,0));
    assert(!flip.CanObserve() && flip.Contains(next.base) && flip.Contains(first.base));
    io.Vblank();
    assert(flip.Poll(io) && !flip.Contains(next.base) && !flip.Contains(first.base));

    assert(flip.Arm(io,first,0));
    io.frame+=4;
    assert(!flip.Poll(io) && !flip.CanObserve() && flip.Contains(first.base));
}

static void CheckRepeatedObserverHandoffs(){
    constexpr BdwClientScanoutLayout linear{12800,false}, tiled{13312,true};
    FakeDisplay io;
    BdwClientScanoutFlip flip;
    BdwClientLatchObserver observer;
    for(unsigned cycle=0;cycle<1024;++cycle){
        const BdwClientScanoutImage first{0x10000000,1+2ull*cycle,linear};
        const BdwClientScanoutImage next{0x12000000,2+2ull*cycle,tiled};
        observer.SetEnabled(true);
        assert(flip.Arm(io,first,0) && observer.QueueWake(flip.CanObserve()));
        assert(!observer.MayPoll(flip.CanObserve(),true,false));
        assert(!observer.MayPoll(flip.CanObserve(),false,true));
        io.Vblank();
        observer.ConsumeWake();
        assert(flip.Contains(first.base) && !flip.Front().base);
        assert(observer.MayPoll(flip.CanObserve(),false,false) && flip.Poll(io));

        assert(flip.Arm(io,next,0) && observer.QueueWake(flip.CanObserve()));
        observer.SetEnabled(false);
        assert(!observer.MayPoll(flip.CanObserve(),false,false));
        observer.SetEnabled(true);
        assert(!observer.QueueWake(flip.CanObserve()));
        if(cycle&1){
            // Exit fullscreen while a replacement is still armed. The AP
            // no longer owns this decision and both images stay pinned.
            assert(flip.Restore(io,0));
            assert(!observer.MayPoll(flip.CanObserve(),false,false));
            assert(!flip.Poll(io) && flip.Contains(first.base) && flip.Contains(next.base));
        }else{
            // Capture or verify can leave the proof to the original poll.
            assert(!observer.MayPoll(flip.CanObserve(),cycle&2,!(cycle&2)));
            io.Vblank();
            assert(flip.Poll(io) && !flip.Contains(first.base) && flip.Contains(next.base));
            assert(flip.Restore(io,0) && !flip.Poll(io) && flip.Contains(next.base));
        }
        observer.ConsumeWake();
        io.Vblank();
        assert(flip.Poll(io) && !flip.Contains(first.base) && !flip.Contains(next.base));
        assert(!flip.Busy() && !observer.QueueWake(flip.CanObserve()));
    }
}

int main(){
    CheckTimeline();
    CheckObserver();
    CheckRepeatedObserverHandoffs();
    constexpr BdwClientScanoutLayout linear{12800,false}, tiled{13312,true};
    assert(linear.Valid() && tiled.Valid() && tiled.Pages()>linear.Pages());
    assert((!BdwClientScanoutLayout{12864,true}.Valid()));
    assert((!BdwClientScanoutLayout{16385,false}.Valid()));
    constexpr BdwClientScanoutImage a{0x10000000,1,linear}, b{0x12000000,2,tiled};
    // Submission cannot arm an unpublished source or retire its lease
    // while the publisher may still be walking the buffer's pages.
    BdwClientPublication completed;
    assert(!completed.Finished() && !completed.CanArm());
    assert(completed.Begin() && !completed.Begin());
    assert(!completed.Finished() && !completed.CanArm());
    completed.Finish(true);
    assert(completed.Finished() && completed.CanArm() && !completed.Begin());

    BdwClientPublication failed;
    assert(failed.Begin());
    failed.Finish(false);
    assert(failed.Finished() && !failed.CanArm());

    // Takeover can cancel before dispatch, while CLFLUSH runs, or after
    // publication but before the BSP commits the flip under its lock.
    BdwClientPublication cancelledBefore;
    cancelledBefore.Cancel();
    assert(cancelledBefore.Begin() && cancelledBefore.Cancelled() && !cancelledBefore.Finished());
    cancelledBefore.Finish(false);
    assert(cancelledBefore.Finished() && !cancelledBefore.CanArm());

    BdwClientPublication cancelledDuring;
    assert(cancelledDuring.Begin() && !cancelledDuring.Cancelled());
    cancelledDuring.Cancel();
    assert(!cancelledDuring.Finished() && !cancelledDuring.CanArm());
    cancelledDuring.Finish(true);
    assert(cancelledDuring.Finished() && !cancelledDuring.CanArm());

    completed.Cancel();
    assert(completed.Finished() && !completed.CanArm());

    FakeDisplay io;
    BdwClientScanoutFlip flip;
    assert(flip.Arm(io,a,0));
    assert(io.order[0]==BdwFlipRegs::Stride && io.order[1]==BdwFlipRegs::TileOffset &&
           io.order[2]==BdwFlipRegs::PlaneControl && io.order[3]==BdwFlipRegs::Surface);
    assert(flip.Contains(a.base) && !flip.Poll(io) && !flip.Arm(io,b,0));
    io.Vblank();
    assert(flip.Poll(io) && flip.Front().identity==a.identity && !flip.Armed().identity);
    assert(flip.Arm(io,b,0) && io.stride==tiled.stride && io.control==tiled.Control());
    assert(flip.Contains(a.base) && flip.Contains(b.base));
    io.Vblank();
    assert(flip.Poll(io) && !flip.Contains(a.base) && flip.Front().identity==b.identity);
    assert(flip.Arm(io,a,0));
    assert(flip.Restore(io,0));
    assert(flip.Contains(b.base) && flip.Contains(a.base) && !flip.Poll(io));
    io.Vblank();
    assert(flip.Poll(io) && !flip.Contains(b.base) && !flip.Contains(a.base));
    assert(io.stride==12800 && io.control==BdwFlipRegs::GopControl && !io.offset);

    // A fallback to the last kernel-owned image also retains a pending
    // client until that image latches, even if the live register was equal.
    constexpr uint32_t kernelFront=0x14000000;
    io.base=io.live=kernelFront;
    assert(flip.Arm(io,a,kernelFront));
    assert(flip.Restore(io,kernelFront) && !flip.Poll(io));
    io.Vblank();
    assert(flip.Poll(io) && !flip.Contains(a.base));
    assert(flip.Arm(io,b,kernelFront));
    io.frame+=4;
    assert(!flip.Poll(io) && flip.Uncertain() && flip.Contains(b.base));
    assert(flip.Restore(io,0) && !flip.Poll(io));
    io.Vblank();
    assert(flip.Poll(io) && !flip.Uncertain());
    assert(flip.Arm(io,a,0));
    io.Vblank();io.stride=13312;
    assert(!flip.Poll(io) && flip.Contains(a.base));
}
