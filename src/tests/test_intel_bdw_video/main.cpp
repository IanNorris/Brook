// Host test: VCS0 video engine bring-up policy, reset sequence and self-test batch.
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include "intel_bdw_video.h"
#include "intel_bdw_bcs_reset.h"
#include "intel_bdw_bcs_init.h"
#include "intel_bdw_context.h"
#include "intel_bdw_submission.h"
#include "intel_bdw_ioctl_trace.h"

using namespace brook;

namespace {
// Fake VCS + GT MMIO used to drive the shared reset template for VCS.
struct FakeVcs {
    uint32_t resetCtl=0,miMode=0x200,gdrst=0,hwstam=0,mode=0,hws=0,csb=7;
    bool readyOnRequest=true,domainClears=true,catastrophic=false,ownedOk=true;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    uint64_t ms=0;
    bool owned(){return ownedOk;}
    bool quiescent(){return true;}
    bool monotonicMs(uint64_t& v){v=ms++;return true;}
    void relax(){}
    bool readReg(uint32_t reg,uint32_t& v){
        if(reg==0x120d0){v=resetCtl|(catastrophic?4u:0u);return true;}
        if(reg==0x1209c){v=miMode;return true;}
        if(reg==0x941c){v=gdrst;return true;}
        return false;
    }
    bool writeReg(uint32_t reg,uint32_t v){
        writes.push_back({reg,v});
        if(reg==0x120d0){
            if(v==0x10001u && readyOnRequest)resetCtl=1|2;
            else if(v==0x10000u)resetCtl&=~1u;
            return true;
        }
        if(reg==0x941c){gdrst=domainClears?0:v;return true;}
        if(reg==0x1209c){miMode=v|0x200;return true;}
        return false;
    }
};
}

static void TestResetSequence(){
    FakeVcs io;
    assert(BdwResetVcsDomain(io)==BdwBcsResetResult::DomainAcknowledged);
    // Expected i915-style sequence: request reset-ready, GDRST media (4), release request.
    assert(io.writes.size()==3);
    assert(io.writes[0].first==0x120d0 && io.writes[0].second==0x10001u);
    assert(io.writes[1].first==0x941c && io.writes[1].second==4);
    assert(io.writes[2].first==0x120d0 && io.writes[2].second==0x10000u);
    // Catastrophic error: refused before any write.
    FakeVcs bad;bad.catastrophic=true;
    assert(BdwResetVcsDomain(bad)==BdwBcsResetResult::NotStarted && bad.writes.empty());
    // Another domain already resetting: refused before any write.
    FakeVcs busy;busy.gdrst=2;
    assert(BdwResetVcsDomain(busy)==BdwBcsResetResult::NotStarted && busy.writes.empty());
    // Domain never acknowledges: uncertain (a write may have reached the device).
    FakeVcs stuck;stuck.domainClears=false;
    assert(BdwResetVcsDomain(stuck)==BdwBcsResetResult::Uncertain);
    // Never reaches reset-ready: uncertain, and the request is cancelled.
    FakeVcs noready;noready.readyOnRequest=false;
    assert(BdwResetVcsDomain(noready)==BdwBcsResetResult::Uncertain);
    assert(!noready.writes.empty() && noready.writes.back().second==0x10000u);
}

static void TestGate(){
    BdwVideoGate g;
    assert(g.stage()==BdwVideoStage::Off && !g.enabled() && !g.advertiseBsd());
    assert(g.mayProbe()==BdwVideoRefusal::GateOff && g.mayReset()==BdwVideoRefusal::GateOff);
    assert(g.setEnabled(true) && g.stage()==BdwVideoStage::Enabled);
    assert(g.mayProbe()==BdwVideoRefusal::None);
    assert(g.mayReset()==BdwVideoRefusal::ProbeFailed);  // must probe first
    // A failed probe stays Enabled and counts a refusal.
    g.probed(false,{});assert(g.stage()==BdwVideoStage::Enabled && g.refusals()==1);
    // Busy engine: probe succeeds but reset is refused.
    BdwVcsSample busy{};busy.sampled=true;busy.quiescent=false;
    g.probed(true,busy);assert(g.stage()==BdwVideoStage::Probed && g.mayReset()==BdwVideoRefusal::NotQuiescent);
    BdwVcsSample idle{};idle.sampled=true;idle.idle=true;idle.quiescent=true;
    g.probed(true,idle);assert(g.mayReset()==BdwVideoRefusal::None);
    g.resetDone(false,false);assert(g.stage()==BdwVideoStage::Probed && g.lastRefusal()==BdwVideoRefusal::ResetRefused);
    g.resetDone(true,false);assert(g.stage()==BdwVideoStage::ResetOk && g.resets()==2);
    // Off and on again keeps the stage reached (it was proven this boot).
    assert(g.setEnabled(false) && g.stage()==BdwVideoStage::Off && g.mayProbe()==BdwVideoRefusal::GateOff);
    assert(g.setEnabled(true) && g.stage()==BdwVideoStage::Enabled);
    // Uncertain reset quarantines for the boot; the gate cannot be re-enabled.
    g.probed(true,idle);g.resetDone(false,true);
    assert(g.stage()==BdwVideoStage::Quarantined && !g.enabled());
    assert(!g.setEnabled(true) && g.mayProbe()==BdwVideoRefusal::Quarantined && g.mayReset()==BdwVideoRefusal::Quarantined);
    assert(!g.advertiseBsd());
}

static void TestBatch(){
    uint32_t b[8]{};
    assert(BdwBuildVcsSelftestBatch(b,8,0x10000040ull,0x5a5a1234u)==6);
    assert(b[0]==0 && b[1]==0x10400002u && b[2]==0x10000040u && b[3]==0 && b[4]==0x5a5a1234u && b[5]==0x05000000u);
    assert(BdwBuildVcsSelftestBatch(b,5,0x1000,1)==0);           // capacity
    assert(BdwBuildVcsSelftestBatch(b,8,0x1002,1)==0);           // alignment
    assert(BdwBuildVcsSelftestBatch(b,8,uint64_t{1}<<32,1)==0);  // GGTT is 4 GiB
    assert(BdwBuildVcsSelftestBatch(b,8,0x1000,0)==0);           // zero would not prove a write
    assert(BdwBuildVcsSelftestBatch(nullptr,8,0x1000,1)==0);
}

static void TestVcsContext(){
    BdwContext64 c{};c.contextGgtt=0x10002000;c.ringGgtt=0x10010000;c.pml4Dma=0x123000;c.ringBytes=4096;c.tailBytes=24;c.contextId=7;
    static uint32_t bcs[2048],vcs[2048];uint64_t db=0,dv=0;
    assert(BdwBuildBcsContext64(c,bcs,db) && BdwBuildVcsContext64(c,vcs,dv) && db==dv);
    unsigned differ=0;
    for(unsigned i=0;i<2048;++i){
        if(bcs[i]==vcs[i])continue;
        ++differ;assert((bcs[i]&0xff000u)==0x22000u && vcs[i]==((bcs[i]&~0xff000u)|0x12000u));
    }
    assert(differ==22); // 11 + 9 LRI register addresses plus the two pointers
    BdwContext64 bad=c;bad.contextId=0;
    assert(!BdwBuildVcsContext64(bad,vcs,dv));
}

static void TestVcsSubmission(){
    BdwBcsLaunch l{{0x10002000,0x10010000,0x123000,16384,56,1},0x10000,0x10007000,0x1234};
    static uint32_t ctx[2048],bctx[2048];uint32_t batch[8]{},ring[14]{},bbatch[12]{},bring[14]{};uint64_t d=0,bd=0;
    assert(BdwBuildVcsSelftest(l,0x11000,0x5555,ctx,batch,ring,d));
    // Ring identical to the BCS template for the same launch.
    const BdwBlitRegion region{{0x11000,4096,64},{0x12000,4096,64},0,0,0,0,16,4};
    assert(BdwBuildBcsRegions(l,&region,1,bctx,bbatch,bring,bd) && bd==d);
    for(unsigned i=0;i<14;++i)assert(ring[i]==bring[i]);
    assert(batch[0]==0 && batch[1]==0x10000002u && batch[2]==0x11000 && batch[3]==0 && batch[4]==0x5555 && batch[5]==0x05000000u);
    assert(ctx[1024+2]==0x12244 && bctx[1024+2]==0x22244);
    assert(!BdwBuildVcsSelftest(l,0x11000,0,ctx,batch,ring,d));        // zero data
    assert(!BdwBuildVcsSelftest(l,0x11000,0x1234,ctx,batch,ring,d));   // data == nonce
    assert(!BdwBuildVcsSelftest(l,0x10000,0x5555,ctx,batch,ring,d));   // data on the batch page
    assert(!BdwBuildVcsSelftest(l,0x11004,0x5555,ctx,batch,ring,d));   // unaligned
}
static void TestSelftestGate(){
    BdwVideoGate g;BdwVcsSample idle{};idle.sampled=idle.idle=idle.quiescent=true;
    assert(g.maySelftest()==BdwVideoRefusal::GateOff);
    g.setEnabled(true);assert(g.maySelftest()==BdwVideoRefusal::ProbeFailed);
    g.probed(true,idle);assert(g.maySelftest()==BdwVideoRefusal::None);
    g.selftestDone(false,false,BdwVideoRefusal::PrepareFailed);       // refused before any write
    assert(g.stage()==BdwVideoStage::Probed && g.lastRefusal()==BdwVideoRefusal::PrepareFailed);
    g.selftestDone(true,true,BdwVideoRefusal::None);
    assert(g.stage()==BdwVideoStage::SelftestOk && g.selftests()==2 && g.selftestPasses()==1 && !g.advertiseBsd());
    assert(g.maySelftest()==BdwVideoRefusal::None);                   // repeatable
    g.selftestDone(false,true,BdwVideoRefusal::Mismatch);             // engine touched: quarantine
    assert(g.stage()==BdwVideoStage::Quarantined && g.maySelftest()==BdwVideoRefusal::Quarantined && !g.setEnabled(true));
}

static void TestPmRestore(){
    struct Io {uint32_t regs[0x140000/4+16]{};bool failWrite=false;
        bool read(uint32_t r,uint32_t& v){v=regs[r/4];return true;}
        bool write(uint32_t r,uint32_t v){if(failWrite)return true;regs[r/4]=v;return true;}};
    static Io io;BdwGtPmRecord rec{};
    for(unsigned i=0;i<BdwGtPmRegCount;++i){rec.before.value[i]=0x1000+i;io.regs[BdwGtPmRegs[i]/4]=0x1000+i;}
    rec.before.valid=true;rec.after=rec.before;
    // A VCS operation changed RP_CONTROL (restorable), RPSTAT1 (status) and VCS GFX_MODE (engine, report only).
    auto index=[](uint32_t reg){for(unsigned i=0;i<BdwGtPmRegCount;++i)if(BdwGtPmRegs[i]==reg)return i;assert(false);return 0u;};
    const unsigned rp=index(0xa024),st=index(0xa01c),gm=index(0x1229c);
    rec.after.value[rp]=0xdead;rec.after.value[st]=0x1234;rec.after.value[gm]=0x8000;
    io.regs[0xa024/4]=0xdead;
    BdwGtPmRestore(io,rec);
    assert(rec.changedMask==((1u<<rp)|(1u<<st)|(1u<<gm)));
    assert(rec.restoredMask==(1u<<rp) && !rec.restoreFailedMask && io.regs[0xa024/4]==0x1000+rp);
    assert(io.regs[0x1229c/4]==0x1000+gm); // engine register never written
    // A write that does not take is reported as failed.
    io.failWrite=true;io.regs[0xa024/4]=0xdead;BdwGtPmRestore(io,rec);
    assert(rec.restoreFailedMask==(1u<<rp) && !rec.restoredMask);
    // Invalid snapshots restore nothing.
    BdwGtPmRecord none{};BdwGtPmRestore(io,none);assert(!none.changedMask);
    assert(BdwGtCagfMhz(10u<<7)==500 && BdwGtCagfMhz(8u<<7)==400);
    assert(BdwGtPmRegRestorable(0xa090) && !BdwGtPmRegRestorable(0xa01c) && !BdwGtPmRegRestorable(0x1209c));
}

// Regression for the gen 392 stall: the caller side of a VCS step never holds
// the diagnostic lease; only the worker takes it, for the step itself, and a
// busy lease leaves the step queued and untouched.
static void TestStepSlot(){
    uint32_t diagnosticOwner=0;unsigned deviceSteps=0;
    BdwVideoStepSlot slot;
    auto workerService=[&](){
        if(!slot.claim())return false;
        uint32_t e=0;
        if(!__atomic_compare_exchange_n(&diagnosticOwner,&e,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)){slot.requeue();return false;}
        ++deviceSteps;                                  // the device step, under the lease
        __atomic_store_n(&diagnosticOwner,0,__ATOMIC_RELEASE);
        slot.complete();return true;};
    assert(!slot.claim() && !slot.pending());           // nothing queued
    assert(slot.begin() && !slot.begin());              // one request at a time
    slot.publish();assert(slot.pending() && diagnosticOwner==0);
    diagnosticOwner=1;                                   // a present/render prepare holds the lease
    assert(!workerService() && slot.pending() && !deviceSteps); // requeued, untouched
    diagnosticOwner=0;
    assert(workerService() && deviceSteps==1 && slot.state()==BdwVideoStepSlot::Done && diagnosticOwner==0);
    assert(!slot.withdraw());                            // a started step cannot be withdrawn
    slot.finish();assert(slot.state()==BdwVideoStepSlot::Idle);
    // Timeout before the worker started: withdrawn, never runs.
    assert(slot.begin());slot.publish();assert(slot.withdraw());
    assert(!workerService() && deviceSteps==1 && slot.state()==BdwVideoStepSlot::Idle);
}

namespace brook {BdwIoctlTrace& BdwIoctlTraceState(){static BdwIoctlTrace t;return t;}}
static void TestIoctlTrace(){
    auto& t=BdwIoctlTraceState();
    t.record(1,0x46,16,5,0);assert(t.next==0);              // disabled: nothing recorded
    t.enabled=1;
    for(uint32_t i=0;i<300;++i)t.record(7,0x69,64,0x802u|i,-22);
    assert(t.next==300 && t.dropped==44);
    const auto& last=t.entries[299%BdwIoctlTraceCapacity];
    assert(last.seq==300 && last.pid==7 && last.nr==0x69 && last.detail==(0x802u|299) && last.result==-22);
    t.reset();assert(t.next==0 && t.dropped==0 && t.entries[0].seq==0);
    assert(std::string(BdwIoctlName(0x69))=="I915_GEM_EXECBUFFER2" && std::string(BdwIoctlName(0x46))=="I915_GETPARAM");
    assert(!BdwVideoUserSubmissionReady());                 // no executor in this build
}

int main(){
    TestIoctlTrace();
    TestStepSlot();
    TestPmRestore();
    TestVcsContext();
    TestVcsSubmission();
    TestSelftestGate();
    TestResetSequence();
    TestGate();
    TestBatch();
    std::puts("test_intel_bdw_video: ok");
    return 0;
}
