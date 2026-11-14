// Host test: VCS0 video engine bring-up policy, reset sequence and self-test batch.
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>
#include "intel_bdw_video.h"
#include "intel_bdw_bcs_reset.h"
#include "intel_bdw_bcs_init.h"
#include "intel_bdw_context.h"
#include "intel_bdw_submission.h"

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

int main(){
    TestVcsContext();
    TestVcsSubmission();
    TestSelftestGate();
    TestResetSequence();
    TestGate();
    TestBatch();
    std::puts("test_intel_bdw_video: ok");
    return 0;
}
