#include "../../kernel/src/intel_bdw_display_flip.h"
#include "../../kernel/src/intel_bdw_ppgtt.h"
#include <cassert>
#include <cstring>
#include <vector>
using namespace brook;

static BdwFlipProbe GoodProbe(){
    BdwFlipProbe p{};
    p.ddiEdp=(1u<<31)|(0u<<12);p.pipeConfEdp=0xc0000000u;p.psr=0;
    p.control=0x98000000u;p.stride=12800;p.surface=0;p.surfaceLive=0;
    p.pipeSource=(3199u<<16)|1799u;p.frame0=1000;p.frame1=1006;p.elapsedUs=100000; // 60 Hz
    return p;
}
static void Probe(){
    const BdwFlipExpect e{};
    assert(BdwFlipEvaluate(GoodProbe(),e)==BdwFlipRefusal::None);
    auto p=GoodProbe();p.ddiEdp=0;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::EdpTranscoder);
    p=GoodProbe();p.ddiEdp|=4u<<12;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::None); // pipe A on/off
    p=GoodProbe();p.ddiEdp|=5u<<12;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::EdpPipeSelect); // pipe B
    p=GoodProbe();p.pipeConfEdp=0x80000000u;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::TranscoderDisabled);
    p=GoodProbe();p.psr=1u<<31;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::PsrEnabled);
    p=GoodProbe();p.control=0;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::PlaneDisabled);
    p=GoodProbe();p.control|=1u<<10;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::Tiled);
    p=GoodProbe();p.control|=1u<<30;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::PlaneControl);
    p=GoodProbe();p.stride=12288;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::Stride);
    p=GoodProbe();p.surface=0x4000000;p.surfaceLive=0x4000000;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::Surface);
    p=GoodProbe();p.surfaceLive=0x40000;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::SurfaceNotLatched);
    p=GoodProbe();p.tileOffset=1;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::Offset);
    p=GoodProbe();p.controlB=0x80000000u;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::OtherPlane);
    p=GoodProbe();p.pipeSource=(1919u<<16)|1079u;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::PipeSource);
    p=GoodProbe();p.frame1=p.frame0;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::FrameCounterStalled);
    p=GoodProbe();p.frame0=0xfffffffeu;p.frame1=4;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::None); // wrap
    p=GoodProbe();p.frame1=p.frame0+2;assert(BdwFlipEvaluate(p,e)==BdwFlipRefusal::FrameRate); // 20 Hz
    for(unsigned r=0;r<=unsigned(BdwFlipRefusal::FrameRate);++r)
        assert(std::strcmp(BdwFlipRefusalName(BdwFlipRefusal(r)),"unknown"));
}
static void Damage(){
    BdwFlipDamage d{};assert(d.Empty());
    d.Add({0,0,0,10});assert(d.Empty());
    d.Add({10,10,100,100});d.Add({20,20,10,10});assert(d.count==1); // contained
    for(uint32_t i=0;i<40;++i)d.Add({i*50,1000,10,10});
    // Overflow collapses conservatively: bounded count, every rect still covered.
    assert(d.count<=BdwFlipDamageMax && d.count<33 && !d.full);
    auto covered=[&](const BdwFlipRect& r){for(uint32_t i=0;i<d.count;++i)if(BdwFlipDamage::Contains(d.rects[i],r))return true;return false;};
    assert(covered({10,10,100,100}));
    for(uint32_t i=0;i<40;++i)assert(covered({i*50,1000,10,10}));
    BdwFlipDamage one{};for(uint32_t i=0;i<BdwFlipDamageMax+1;++i)one.Add({i*20,0,10,10});
    assert(one.count==1 && one.rects[0].x==0 && one.rects[0].width==BdwFlipDamageMax*20+10);
    BdwFlipDamage f{};f.SetFull();d.Merge(f);assert(d.full && !d.Empty());
    d.Add({1,1,1,1});assert(d.full && d.count==0);
}
static constexpr uint32_t A=0x4000000,B=0x6000000,C=0x8000000;
static void Rotation(){
    BdwFlipChain c;const uint32_t s3[]={A,B,C};
    const uint32_t bad[]={A,A};assert(!c.Configure(0,bad,2));
    const uint32_t unaligned[]={A,B+4096};assert(!c.Configure(0,unaligned,2));
    assert(c.Configure(0,s3,3) && !c.Configure(0,s3,3));
    for(uint32_t s=1;s<4;++s)assert(c.Stale(s).full && c.Reusable(s));
    assert(c.Front()==0 && !c.Reusable(0));
    BdwFlipDamage d{};d.Add({0,0,100,100});
    // Frame 1 into slot 1: needs a full repair from the GOP (latest==0).
    uint32_t b=c.AcquireBack();assert(b==1 && c.Latest()==0 && c.Stale(1).full);
    assert(c.AcquireBack()==0); // one acquisition at a time
    assert(c.Commit(b,d)==1 && c.Latest()==1);
    assert(c.Stale(1).Empty() && c.Stale(0).count==1 && c.Stale(2).full);
    assert(!c.Armed(2,10,100) && c.Armed(1,10,100));
    assert(c.Retained(0) && c.Retained(1) && !c.Reusable(1));
    BdwFlipLatch latch{};
    assert(c.Poll(10,0,110,latch)==BdwFlipPoll::Pending);        // no vblank yet
    assert(c.Poll(11,0,120,latch)==BdwFlipPoll::Pending);        // edge: counter before latch
    assert(c.Poll(11,A,130,latch)==BdwFlipPoll::Latched && latch.slot==1 && latch.frames==1 && latch.ticks==30);
    assert(c.Front()==1 && !c.Retained(0) && c.Retained(1) && c.Poll(12,A,140,latch)==BdwFlipPoll::Idle);
    // Frame 2 into slot 2 while 1 is front; then frame 3 queued behind it.
    b=c.AcquireBack();assert(b==2);
    assert(c.Commit(b,d)==2 && c.Armed(2,12,150));
    b=c.AcquireBack();assert(b==3 && c.Stale(3).full);
    assert(c.Commit(b,d)==0 && c.HasQueued() && c.QueuedSlot()==3); // queued, not armed
    assert(c.TakeQueued()==0); // still armed
    // Mailbox: a fourth frame takes the queued buffer back (front 1 and armed 2 retained).
    b=c.AcquireBack();assert(b==3 && !c.HasQueued() && c.Stale(3).Empty());
    BdwFlipDamage d2{};d2.Add({500,500,10,10});
    assert(c.Commit(b,d2)==0 && c.HasQueued());
    assert(c.Stale(1).count==2 && c.Stale(2).count==2); // d then d2 relative to latest 3
    assert(!c.Reusable(1) && !c.Reusable(2) && !c.Reusable(3) && c.AcquireBack()==3);
    assert(c.Commit(3,BdwFlipDamage{})==0);
    assert(c.Poll(13,B,160,latch)==BdwFlipPoll::Latched && latch.slot==2);
    assert(c.Reusable(1) && !c.Reusable(3)); // the queued slot is not free
    assert(c.TakeQueued()==3 && c.AcquireBack()==0); // arm before more work
    assert(c.Armed(3,13,170) && c.AcquireBack()==1);
    assert(c.Poll(16,B,200,latch)==BdwFlipPoll::Uncertain && c.Uncertain()); // 3 frames, wrong surface
    assert(c.AcquireBack()==0 && !c.Reusable(2));
}
static void SoftwareTakeover(){
    BdwFlipChain c;const uint32_t s2[]={A,B};assert(c.Configure(0,s2,2));
    BdwFlipDamage d{};d.Add({0,0,10,10});BdwFlipLatch latch{};
    assert(!c.RequestGop()); // already live: nothing to write
    uint32_t b=c.AcquireBack();assert(c.Commit(b,d)==b && c.Armed(b,1,0));
    assert(c.Poll(2,A,1,latch)==BdwFlipPoll::Latched);
    b=c.AcquireBack();assert(b==2 && c.Commit(b,d)==2 && c.Armed(2,2,2));
    // Takeover while 2 is armed and may already have latched: retain 1, 2 and the GOP.
    assert(c.RequestGop() && c.Latest()==0 && c.Stale(1).full && c.Stale(2).full && c.Stale(0).Empty());
    assert(c.AcquireBack()==0 && c.Armed(0,2,3));
    assert(c.Retained(0) && c.Retained(1) && c.Retained(2) && !c.Reusable(1) && !c.Reusable(2));
    assert(c.Poll(3,B,4,latch)==BdwFlipPoll::Pending);       // 2 latched first; GOP still pending
    assert(c.Poll(4,0,5,latch)==BdwFlipPoll::Latched && latch.slot==0 && c.Front()==0);
    assert(c.Reusable(1) && c.Reusable(2));
    // Abandoned copies freeze the chain.
    b=c.AcquireBack();c.Abandon(b);assert(c.Uncertain() && c.AcquireBack()==0);
}
struct FakeDisplay {
    uint32_t control=0x98000000u,surface=A,live=A;
    unsigned latchAfter=3,reads=0,writes=0;uint64_t clock=0;
    uint32_t read32(uint32_t off){
        if(off==BdwFlipRegs::PlaneControl)return control;
        if(off==BdwFlipRegs::Surface)return surface;
        if(off==BdwFlipRegs::SurfaceLive){if(++reads>=latchAfter)live=surface;return live;}
        return 0;
    }
    void write32(uint32_t off,uint32_t v){assert(off==BdwFlipRegs::Surface);surface=v;++writes;}
    uint64_t now(){return clock;}
    void relax(){++clock;}
};
static void PanicRestore(){
    FakeDisplay d{};
    auto r=BdwFlipRestoreGop(d,0,100);
    assert(r.attempted && r.written && r.latched && d.surface==0 && d.live==0 && d.writes==1);
    // The restore is idempotent: a second call writes nothing.
    r=BdwFlipRestoreGop(d,0,100);assert(r.latched && !r.written && d.writes==1);
    // No vblank (pipe stopped): bounded, reports not latched, still armed GOP.
    FakeDisplay stuck{};stuck.latchAfter=~0u;
    r=BdwFlipRestoreGop(stuck,0,50);assert(r.written && !r.latched && stuck.surface==0 && stuck.clock>=50);
    // A changed plane (firmware modeset) is never written.
    FakeDisplay other{};other.control=0x98000400u;
    r=BdwFlipRestoreGop(other,0,50);assert(!r.written && other.writes==0 && other.surface==A);
}
static void Persistent(){
    BdwPersistentMapPolicy p;assert(p.NeedFull());
    p.Full(true);assert(!p.NeedFull() && p.Valid());
    for(uint32_t i=0;i<BdwPersistentMapPolicy::FullEvery-1;++i)p.Hit();
    assert(!p.NeedFull());p.Hit();assert(p.NeedFull());
    p.Full(false);assert(p.NeedFull() && !p.Valid());
}
alignas(4096) static uint8_t memory[16][4096];
alignas(4096) static uint8_t leafMemory[3][4096];
static void Leaves(){
    // Persistent leaves: 1100 pages need three tables; tail entries hit scratch.
    BdwPpgttPage leaves[3];for(unsigned i=0;i<3;++i)leaves[i]={0x900000+4096ull*i,leafMemory[i]};
    auto pageOf=[](uint32_t i){return 0x10000000ull+uint64_t(i)*4096;};
    assert(!BdwFillPpgttLeaves(0x123000,leaves,2,pageOf,1100,BdwPpgttCache::Uncached,true));
    assert(BdwFillPpgttLeaves(0x123000,leaves,3,pageOf,1100,BdwPpgttCache::Uncached,true));
    const auto* w=reinterpret_cast<const uint64_t*>(leafMemory[2]);
    assert(w[1100-1024-1]==(pageOf(1099)|0x1b) && w[1100-1024]==(0x123000|0x81));
    assert(!BdwFillPpgttLeaves(0x900000,leaves,3,pageOf,1100,BdwPpgttCache::Uncached,true)); // scratch alias
    // Link them into an image beside an ordinary sorted mapping.
    BdwPpgttPage scratch[4],tables[8];
    for(unsigned i=0;i<4;++i)scratch[i]={0x100000+4096ull*i,memory[i]};
    for(unsigned i=0;i<8;++i)tables[i]={0x200000+4096ull*i,memory[i+4]};
    const BdwPpgttMapping maps[]={{0x10000,0x400000,false},{0x2000000,0x401000,false}};
    const BdwPpgttLeaf links[]={{0x4000000,leaves[0].dma},{0x4200000,leaves[1].dma},{0x4400000,leaves[2].dma}};
    assert(BdwBuildPpgttImage(scratch,tables,5,maps,2,true,links,3));
    const auto* pml4=reinterpret_cast<const uint64_t*>(tables[0].cpu);
    const uint64_t pdptDma=pml4[0]&~4095ull;assert(pdptDma==tables[1].dma);
    const auto* pd=reinterpret_cast<const uint64_t*>(tables[2].cpu);
    for(unsigned i=0;i<3;++i)assert(pd[(0x4000000>>21)+i]==(leaves[i].dma|3));
    assert(pd[(0x4000000>>21)+3]==(scratch[1].dma|3));
    // A mapping inside a leaf interval, a misaligned or duplicate leaf, or a
    // leaf aliasing a table page is refused.
    const BdwPpgttMapping inside[]={{0x10000,0x400000,false},{0x4001000,0x401000,false}};
    assert(!BdwBuildPpgttImage(scratch,tables,8,inside,2,true,links,3));
    const BdwPpgttLeaf misaligned[]={{0x4001000,leaves[0].dma}};
    assert(!BdwBuildPpgttImage(scratch,tables,8,maps,2,true,misaligned,1));
    const BdwPpgttLeaf duplicate[]={{0x4000000,leaves[0].dma},{0x4000000,leaves[1].dma}};
    assert(!BdwBuildPpgttImage(scratch,tables,8,maps,2,true,duplicate,2));
    const BdwPpgttLeaf aliasing[]={{0x4000000,tables[5].dma}};
    assert(!BdwBuildPpgttImage(scratch,tables,8,maps,2,true,aliasing,1));
    // A leaf in a different GiB needs its own page directory.
    const BdwPpgttLeaf far[]={{0x40000000,leaves[0].dma}};
    assert(!BdwBuildPpgttImage(scratch,tables,5,maps,2,true,far,1));
    assert(BdwBuildPpgttImage(scratch,tables,6,maps,2,true,far,1));
}
int main(){
    Probe();Damage();Rotation();SoftwareTakeover();PanicRestore();Persistent();Leaves();
    return 0;
}
