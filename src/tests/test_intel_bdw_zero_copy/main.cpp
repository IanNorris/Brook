#include "../../kernel/src/intel_bdw_zero_copy.h"
#include "../../kernel/src/compositor_present_lease.h"
#include <cassert>
#include <cstring>
using namespace brook;

static const BdwZeroCopyGeometry Mode{3200,1800,12800,BdwZeroCopyFourccXrgb8888};
static void Eligibility(){
    BdwZeroCopyCandidate c[4]={{1,BdwZeroCopyImageBytes,false},{2,BdwZeroCopyImageBytes,false},
                               {3,BdwZeroCopyImageBytes,false},{4,BdwZeroCopyImageBytes,false}};
    assert(BdwZeroCopyEligible(Mode,c,4)==BdwZeroCopyRefusal::None);
    assert(BdwZeroCopyEligible(Mode,c,3)==BdwZeroCopyRefusal::None);
    assert(BdwZeroCopyEligible(Mode,c,2)==BdwZeroCopyRefusal::Count);
    assert(BdwZeroCopyEligible(Mode,c,5)==BdwZeroCopyRefusal::Count);
    assert(BdwZeroCopyEligible(Mode,nullptr,4)==BdwZeroCopyRefusal::Count);
    auto g=Mode;g.width=1920;assert(BdwZeroCopyEligible(g,c,4)==BdwZeroCopyRefusal::Geometry);
    g=Mode;g.stride=12864;assert(BdwZeroCopyEligible(g,c,4)==BdwZeroCopyRefusal::Geometry);
    g=Mode;g.fourcc=0x34325241u;assert(BdwZeroCopyEligible(g,c,4)==BdwZeroCopyRefusal::Format); // AR24
    // The security property: a client-mappable buffer is never scanned out.
    auto d=c[2];c[2].userMappable=true;assert(BdwZeroCopyEligible(Mode,c,4)==BdwZeroCopyRefusal::UserMappable);c[2]=d;
    c[1].bytes=BdwZeroCopyImageBytes-4096;assert(BdwZeroCopyEligible(Mode,c,4)==BdwZeroCopyRefusal::Size);
    c[1].bytes=BdwZeroCopyImageBytes+4096;assert(BdwZeroCopyEligible(Mode,c,4)==BdwZeroCopyRefusal::Size);
    c[1].bytes=BdwZeroCopyImageBytes;
    c[3].identity=0;assert(BdwZeroCopyEligible(Mode,c,4)==BdwZeroCopyRefusal::Identity);
    c[3].identity=2;assert(BdwZeroCopyEligible(Mode,c,4)==BdwZeroCopyRefusal::Duplicate);
    for(unsigned r=0;r<=unsigned(BdwZeroCopyRefusal::Duplicate);++r)
        assert(std::strcmp(BdwZeroCopyRefusalName(BdwZeroCopyRefusal(r)),"unknown"));
    for(unsigned f=0;f<unsigned(BdwZeroCopyFallback::Count_);++f)
        assert(std::strcmp(BdwZeroCopyFallbackName(BdwZeroCopyFallback(f)),"unknown"));
    static_assert(BdwZeroCopyImageBytes==5625ull*4096,"panel mode is exactly 5625 pages");
}
static const uint32_t S[4]={0x10000000,0x12000000,0x14000000,0x16000000};
static const uint64_t I[4]={11,12,13,14};
static BdwZeroCopyRing Ring(uint32_t n=4){BdwZeroCopyRing r;assert(r.Configure(S,I,n));return r;}
static void Configure(){
    BdwZeroCopyRing r;
    assert(!r.Configure(S,I,2));
    const uint32_t bad[3]={0x10000000,0x10001000,0x14000000};assert(!r.Configure(bad,I,3)); // unaligned
    const uint32_t dup[3]={0x10000000,0x10000000,0x14000000};assert(!r.Configure(dup,I,3));
    const uint32_t zero[3]={0,0x12000000,0x14000000};assert(!r.Configure(zero,I,3)); // GOP surface is not an image
    const uint64_t did[3]={1,1,2};assert(!r.Configure(S,did,3));
    assert(r.Configure(S,I,4) && !r.Configure(S,I,4));
    assert(r.Images()==4 && r.Front()==0 && r.FreeMask()==0xf);
    assert(r.Find(13)==3 && !r.Find(99) && !r.Find(0));
    for(uint32_t s=1;s<=4;++s)assert(r.Age(s)==0); // never presented: full draw
}
// Present one frame into slot s and let it latch immediately.
static void Frame(BdwZeroCopyRing& r,uint32_t s,uint32_t& counter){
    assert(r.BeginWrite(s));
    const auto p=r.Present(s);
    if(p==BdwZeroCopyPresent::ArmNow)assert(r.Armed(s,counter,0));
    BdwFlipLatch l{};
    while(r.HasArmed()){
        ++counter;
        assert(r.Poll(counter,r.Surface(r.ArmedSlot()),counter*16,l)==BdwFlipPoll::Latched);
        uint32_t q=0;if(r.TakeQueued(q))assert(r.Armed(q,counter,0));
    }
}
static void Rotation(){
    auto r=Ring();uint32_t counter=100;
    Frame(r,1,counter);
    assert(r.Front()==1 && r.FreeMask()==0xe && r.Age(1)==1);
    Frame(r,2,counter);
    // Released on latch, not on submit: slot 1 only became free when 2 latched.
    assert(r.Front()==2 && r.FreeMask()==0xd && r.Age(1)==2 && r.Age(2)==1);
    Frame(r,1,counter);assert(r.Front()==1 && r.Age(2)==2 && r.Age(1)==1);
}
static void NeverWriteDisplayed(){
    auto r=Ring();uint32_t counter=0;
    Frame(r,1,counter);
    assert(!r.BeginWrite(1));           // front
    assert(r.BeginWrite(2) && r.Present(2)==BdwZeroCopyPresent::ArmNow && r.Armed(2,counter,0));
    assert(!r.BeginWrite(2));           // armed (may latch at any vblank)
    assert(r.FreeMask()==0xc);          // front 1 and armed 2 are busy
    assert(r.BeginWrite(3) && r.Present(3)==BdwZeroCopyPresent::Queued);
    assert(!r.BeginWrite(3));           // queued
    assert(r.BeginWrite(4));assert(!r.BeginWrite(4)); // already being written
    assert(r.FreeMask()==0);            // pipelined: renderer waits for a latch
    assert(r.Present(1)==BdwZeroCopyPresent::Rejected); // front, not reserved
    // Mailbox: 4 replaces the queued 3, which the display never fetched.
    assert(r.Present(4)==BdwZeroCopyPresent::Queued && r.Replaced()==1);
    assert(r.FreeMask()==0x4);          // 3 is free again
    BdwFlipLatch l{};uint32_t q=0;
    ++counter;assert(r.Poll(counter,S[1],1,l)==BdwFlipPoll::Latched && r.Front()==2);
    assert(r.TakeQueued(q) && q==4 && r.Armed(4,counter,2));
    assert(r.FreeMask()==0x5);          // 1 released by the latch of 2; 3 free
}
static void Latch(){
    auto r=Ring();uint32_t counter=50;BdwFlipLatch l{};
    assert(r.Poll(counter,0,0,l)==BdwFlipPoll::Idle);
    assert(r.BeginWrite(1) && r.Present(1)==BdwZeroCopyPresent::ArmNow);
    assert(!r.Armed(2,counter,0) && r.Armed(1,counter,1000));
    assert(r.Poll(counter,0,1500,l)==BdwFlipPoll::Pending);           // same frame
    assert(r.Poll(counter+1,0,1600,l)==BdwFlipPoll::Pending);         // counter ahead of latch sample
    assert(r.Poll(counter+1,S[0],2000,l)==BdwFlipPoll::Latched && l.slot==1 && l.frames==1 && l.ticks==1000);
    assert(r.Front()==1);
    // Three frames without the armed surface live freezes the ring.
    assert(r.BeginWrite(2) && r.Present(2)==BdwZeroCopyPresent::ArmNow && r.Armed(2,counter+1,0));
    assert(r.Poll(counter+3,S[0],0,l)==BdwFlipPoll::Pending);
    assert(r.Poll(counter+4,S[0],0,l)==BdwFlipPoll::Uncertain && r.Uncertain());
    assert(r.FreeMask()==0 && !r.BeginWrite(3) && r.Present(3)==BdwZeroCopyPresent::Rejected);
    assert(!r.Quiescent());
    r.GopObserved();assert(!r.Uncertain() && r.Quiescent() && r.FreeMask()==0);
    // Counter wrap.
    auto w=Ring();
    assert(w.BeginWrite(1) && w.Present(1)==BdwZeroCopyPresent::ArmNow && w.Armed(1,0xffffffffu,0));
    assert(w.Poll(0,S[0],0,l)==BdwFlipPoll::Latched && l.frames==1);
}
static void Leave(){
    auto r=Ring();uint32_t counter=0;
    Frame(r,1,counter);
    assert(r.BeginWrite(2) && r.Present(2)==BdwZeroCopyPresent::ArmNow && r.Armed(2,counter,0));
    assert(r.BeginWrite(3) && r.Present(3)==BdwZeroCopyPresent::Queued);
    assert(r.BeginWrite(4));
    // Leaving with an image armed: the GOP is armed next; the queued frame is dropped.
    assert(r.RequestGop() && r.Leaving() && !r.HasQueued() && r.FreeMask()==0);
    assert(!r.BeginWrite(3));
    BdwFlipLatch l{};uint32_t q=0;
    ++counter;assert(r.Poll(counter,S[1],0,l)==BdwFlipPoll::Latched && r.Front()==2);
    assert(!r.TakeQueued(q));
    assert(r.Armed(0,counter,0));
    assert(!r.Quiescent());                      // the GOP is armed, not latched
    ++counter;assert(r.Poll(counter,0,0,l)==BdwFlipPoll::Latched && r.Front()==0);
    assert(!r.Quiescent());                      // image 4 is still being written by the GPU
    r.AbandonWrite(4);assert(r.Quiescent() && r.Age(4)==0);
    // Leaving when the GOP is already front needs no write.
    auto g=Ring();assert(!g.RequestGop() && g.Quiescent());
}
static void Abandon(){
    auto r=Ring();uint32_t counter=0;
    Frame(r,1,counter);Frame(r,2,counter);
    assert(r.Age(1)==2);
    assert(r.BeginWrite(1));r.AbandonWrite(1);
    assert(r.Age(1)==0 && (r.FreeMask()&1));     // content unknown: redraw fully
    r.AbandonWrite(3);assert(r.Age(3)==0);       // not writing: no effect
}
static void Stats(){
    BdwZeroCopyStats s{};
    s.Latch(1,500);s.Latch(2,20000);s.Latch(7,90000);
    assert(s.latched==3 && s.latchFrames[0]==1 && s.latchFrames[1]==1 && s.latchFrames[3]==1);
    assert(s.missedVblanks==1+6 && s.latchUsMax==90000 && s.latchUsSum==110500);
}
static void Adoption(){
    const bool idle[4]={true,true,true,true},busy[4]={true,false,true,true};
    assert(BdwZeroCopyAdoptDecision(0,true,idle)==BdwZeroCopyAdoption::NotRegistered);
    assert(BdwZeroCopyAdoptDecision(4,false,idle)==BdwZeroCopyAdoption::NotAttached); // the old renderer is still attached
    assert(BdwZeroCopyAdoptDecision(4,true,busy)==BdwZeroCopyAdoption::Busy);         // a job or ticket still holds one
    assert(BdwZeroCopyAdoptDecision(4,true,nullptr)==BdwZeroCopyAdoption::Busy);
    assert(BdwZeroCopyAdoptDecision(4,true,idle)==BdwZeroCopyAdoption::Adopt);
    const bool three[3]={true,true,true};assert(BdwZeroCopyAdoptDecision(3,true,three)==BdwZeroCopyAdoption::Adopt);
}
// "gpu zerocopy verify" holds the panel still: an ArmNow present is left
// pending, later presents mailbox behind it, and nothing the display may
// fetch becomes writable; on release the pending frame is armed first.
static void Hold(){
    auto r=Ring();uint32_t counter=10,pending=0,q=0;BdwFlipLatch l{};
    Frame(r,1,counter);
    assert(!r.PendingArm(pending));
    assert(r.BeginWrite(2) && r.Present(2)==BdwZeroCopyPresent::ArmNow);  // held: not written to DSPASURF
    assert(r.PendingArm(pending) && pending==2 && r.Front()==1);
    assert(r.BeginWrite(3) && r.Present(3)==BdwZeroCopyPresent::Queued);
    assert(r.BeginWrite(4) && r.Present(4)==BdwZeroCopyPresent::Queued && r.Replaced()==1); // mailbox
    assert(r.FreeMask()==0x4);                     // only 3 (replaced) is free: 1 front, 2 pending, 4 queued
    assert(!r.TakeQueued(q));                      // nothing jumps ahead of the pending frame
    assert(r.Armed(pending,counter,0));            // release
    ++counter;assert(r.Poll(counter,S[1],0,l)==BdwFlipPoll::Latched && r.Front()==2);
    assert(r.TakeQueued(q) && q==4 && r.Armed(4,counter,0));
}
static void Backpressure(){
    auto ring=Ring();
    uint32_t frame=20,queued=0;
    Frame(ring,1,frame);
    bool adopted=true;
    // One handoff ticket already existed before activation; the slot is
    // free and the ticket proves completion, so adoption can present it.
    assert(ring.PresentCompleted(2,adopted)==BdwZeroCopyPresent::ArmNow && adopted);
    assert(ring.Armed(2,frame,0));
    assert(ring.BeginWrite(3) && ring.Present(3)==BdwZeroCopyPresent::Queued);
    assert(ring.BeginWrite(4) && !ring.FreeMask());

    CompositorPresentLease lease;
    assert(lease.Acquire(7,100,COMPOSITOR_PRESENT_LEASE_MS));
    for(uint64_t now=120;now<=740;now+=20){
        for(uint32_t slot=1;slot<=4;++slot)assert(!ring.BeginWrite(slot));
        assert(ring.PresentCompleted(1,adopted)==BdwZeroCopyPresent::Deferred && !adopted);
        assert(ring.PresentCompleted(2,adopted)==BdwZeroCopyPresent::Deferred && !adopted);
        assert(ring.PresentCompleted(3,adopted)==BdwZeroCopyPresent::Deferred && !adopted);
        assert(ring.Front()==1 && ring.HoldsFront(S[0]) && !ring.HoldsFront(0));
        assert(!ring.Leaving() && !ring.Uncertain() && ring.Sequence()==3);
        // A completed deferred ticket may renew only its own live lease.
        assert(!lease.Renew(8,now,COMPOSITOR_PRESENT_LEASE_MS));
        assert(lease.Renew(7,now,COMPOSITOR_PRESENT_LEASE_MS));
    }
    BdwFlipLatch latch{};
    assert(ring.Poll(++frame,S[1],0,latch)==BdwFlipPoll::Latched);
    assert(ring.Front()==2 && (ring.FreeMask()&1));
    assert(ring.TakeQueued(queued) && queued==3 && ring.Armed(3,frame,0));
    assert(ring.BeginWrite(1) && ring.PresentCompleted(1,adopted)==BdwZeroCopyPresent::Queued && !adopted);

    // A real latch failure is rejected (not deferred) and the GOP remains
    // the recovery surface. An expired lease cannot be resurrected.
    assert(ring.Poll(frame+3,S[1],0,latch)==BdwFlipPoll::Uncertain);
    assert(!ring.HoldsFront(S[1]));
    assert(ring.PresentCompleted(4,adopted)==BdwZeroCopyPresent::Rejected);
    assert(!lease.Renew(7,1240,COMPOSITOR_PRESENT_LEASE_MS) && lease.Expire(1240));
    ring.AbandonWrite(4);
    ring.GopObserved();
    assert(ring.Front()==0 && ring.Quiescent());
}
int main(){
    Backpressure();
    Hold();
    Adoption();
    Eligibility();Configure();Rotation();NeverWriteDisplayed();Latch();Leave();Abandon();Stats();
    return 0;
}
