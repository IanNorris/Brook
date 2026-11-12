#include "intel_bdw_forcewake_adapter.h"
#include "intel_bdw_wait_yield.h"
#include <cassert>
#include <vector>
using namespace brook;
struct Fake {
    unsigned evidenceCount=0,denyPage=0,writes=0,reads=0;bool owner=true,stalled=false,failSetPost=false,failClear=false,c0=true;
    bool awake=false,pendingVcs=false,failSecondVecs=false,failBcs=false,failRcs=false,onesBcs=false,onesRcs=false,failed=false,wantPat=false,wantOther=false,changingPat=false,wantTlb=false,failTlb=false,wantReset=false,failReset=false,changingCsb=false;unsigned vecsHeads=0,readsAfterFailure=0,patReads=0,tlbWrites=0,resetWrites=0,csbReads=0;uint32_t resetCtl=0;uint64_t ticks=0;std::vector<uint32_t> values;
    bool owned(){return owner;}
    bool evidence(uint64_t page,DellI2cCriticalEvidence& e){assert(page==(evidenceCount<4?BdwForcewakePages[evidenceCount]:wantPat && evidenceCount==(wantOther?7u:4u)?BdwPrivatePatPage:wantReset && evidenceCount==8?BdwResetPage:BdwOtherEnginePages[evidenceCount-4]));++evidenceCount;
        if(evidenceCount==denyPage)return false;
        e.aliasesValid=e.identityPresent=e.identityMatches=e.directPresent=e.directMatches=e.currentDirectPresent=e.currentDirectMatches=e.cpuValid=true;
        e.identityPat=e.directPat=e.currentDirectPat=0;e.patSlot0=6;e.mtrrType=0;return true;}
    bool tsc(uint64_t& t){if(!stalled)ticks+=1000;t=ticks;return true;}
    void relax(){}
    bool write32(uint64_t a,uint32_t v){if(!owner)return false;if(wantTlb && (a==BdwDellBar0+0x4260 || a==BdwDellBar0+0x4264 || a==BdwDellBar0+0x426c || a==BdwDellBar0+0x4270)){
        assert(awake && v==1);++tlbWrites;return !failTlb;}
        if(wantReset && (a==BdwDellBar0+0x220d0 || a==BdwDellBar0+0x941c)){
            assert(awake);++resetWrites;if(a==BdwDellBar0+0x220d0 && v==0x10001)resetCtl=2;return !failReset;}
        assert(a==BdwDellBar0+0xa188);++writes;values.push_back(v);awake=v==0x10001;return !(v==0x10000 && failClear);}
    bool read32(uint64_t a,uint32_t& v){if(!owner)return false;++reads;
        if(wantReset && a==BdwDellBar0+0x220d0){assert(awake);v=resetCtl;return true;}
        if(wantReset && a==BdwDellBar0+0x223a0){assert(awake);v=changingCsb && ++csbReads==2?6:7;return true;}
        if(wantReset && a==BdwDellBar0+0x941c){assert(awake);v=0;return true;}
        if(wantTlb && (a==BdwDellBar0+0x4260 || a==BdwDellBar0+0x4264 || a==BdwDellBar0+0x426c || a==BdwDellBar0+0x4270)){assert(awake);v=0;return true;}
        if(a==BdwDellBar0+0x40e0 || a==BdwDellBar0+0x40e4){assert(wantPat && awake);v=(a==BdwDellBar0+0x40e0?0x12345678u:0xabcdef01u);if(changingPat && ++patReads==3)v^=1;return true;}
        if(a==BdwDellBar0+0x130044){if(failSetPost && writes==1){failSetPost=false;return false;}v=awake?1:0;return true;}
        if(a==BdwDellBar0+0x13805c){v=c0?0:1;return true;}
        if(failed)++readsAfterFailure;
        constexpr uint32_t bases[]={0x22000,0x2000,0x12000,0x1a000};
        for(auto base:bases) {
            if(a==BdwDellBar0+base+0x34 && ((failBcs && base==0x22000) || (failRcs && base==0x2000))){failed=true;return false;}
            if(a==BdwDellBar0+base+0x34 && ((onesBcs && base==0x22000) || (onesRcs && base==0x2000))){failed=true;v=0xffffffffu;return true;}
            if(a==BdwDellBar0+base+0x9c){v=1u<<9;return true;}
            if(a==BdwDellBar0+base+0x34 && base==0x1a000 && ++vecsHeads==2 && failSecondVecs)return false;
            if(a==BdwDellBar0+base+0x29c){v=(pendingVcs && base==0x12000)?1u<<15:0;return true;}
            if(a==BdwDellBar0+base+0x234){v=(pendingVcs && base==0x12000)?8:1;return true;}
            if(a==BdwDellBar0+base+0x30 || a==BdwDellBar0+base+0x34 || a==BdwDellBar0+base+0x38 ||
               a==BdwDellBar0+base+0x3c || a==BdwDellBar0+base+0x238){v=0;return true;}
        }
        assert(false);return false;}
};
int main(){
    {Fake f;f.wantPat=f.wantOther=true;
      auto work=[](Fake& device,const BdwForcewakeResult&){
          struct Yield {Fake& f;bool held=true;
              bool MayYield(){return held && f.owner;}void Unlock(){held=false;}void Wait(){}void Lock(){held=true;}
              bool Audit(){return false;}void Poison(){f.owner=false;}} io{device};
          return BdwYieldGpuWait(io);
      };
      auto r=BdwDellForcewakeCriticalWithWork(f,1000,work,true,true,true);
      assert(r.sequence.workAttempted && !r.sequence.workComplete && !r.clearDelivered && !r.sequence.released && f.writes==1);
      BdwForcewakeClaim claim;assert(claim.acquire());claim.finish(r);assert(!claim.acquire());}
    {Fake f;f.wantPat=f.wantOther=true;unsigned calls=0;
      auto work=[&](Fake& device,const BdwForcewakeResult& state){++calls;assert(device.awake && device.writes==1 && state.patSampled && state.otherEnginesQuiescent);return true;};
      auto r=BdwDellForcewakeCriticalWithWork(f,1000,work,true,true,true);
      assert(calls==1 && r.sequence.workAttempted && r.sequence.workComplete && r.sequence.released && !f.awake);
      BdwForcewakeClaim c;assert(c.acquire());c.finish(r);assert(c.acquire());}
    {Fake f;f.wantPat=f.wantOther=true;unsigned calls=0;
      auto work=[&](Fake& device,const BdwForcewakeResult&){++calls;assert(device.awake);return false;};
      auto r=BdwDellForcewakeCriticalWithWork(f,1000,work,true,true,true);
      assert(calls==1 && r.sequence.workAttempted && !r.sequence.workComplete && r.sequence.released);
      BdwForcewakeClaim c;assert(c.acquire());c.finish(r);assert(!c.acquire());}
    {Fake f;f.wantPat=f.wantOther=f.pendingVcs=true;unsigned calls=0;
      auto work=[&](Fake&,const BdwForcewakeResult&){++calls;return true;};
      auto r=BdwDellForcewakeCriticalWithWork(f,1000,work,true,true,true);
      assert(!calls && !r.sequence.workAttempted && r.sequence.released);}
    {Fake f;f.wantPat=f.wantOther=f.wantReset=true;auto r=BdwDellForcewakeCritical(f,1000,true,true,false,true);
      assert(r.sequence.resetAttempted && r.sequence.resetComplete && r.sequence.resetCsbSampled && r.sequence.resetCsb==7 && r.sequence.released && f.resetWrites==3 && f.evidenceCount==9);}
    {Fake f;f.wantPat=f.wantOther=f.wantReset=f.changingCsb=true;auto r=BdwDellForcewakeCritical(f,1000,true,true,false,true);
      assert(r.sequence.resetComplete && !r.sequence.resetCsbSampled && r.sequence.released);}
    {Fake f;f.wantPat=f.wantOther=f.wantReset=f.failReset=true;auto r=BdwDellForcewakeCritical(f,1000,true,true,false,true);
      assert(r.sequence.resetAttempted && !r.sequence.resetComplete && r.sequence.released);
      BdwForcewakeClaim c;assert(c.acquire());c.finish(r);assert(!c.acquire());}
    {Fake f;f.wantPat=f.wantOther=f.wantReset=f.pendingVcs=true;auto r=BdwDellForcewakeCritical(f,1000,true,true,false,true);
      assert(!r.sequence.resetAttempted && !f.resetWrites && r.sequence.released);}
    {Fake f;f.wantPat=f.wantOther=f.wantTlb=true;auto r=BdwDellForcewakeCritical(f,1000,true,true,true);
      assert(r.sequence.tlbAttempted && r.sequence.tlbComplete && r.sequence.released && f.tlbWrites==4);}
    {Fake f;f.wantPat=f.wantOther=f.wantTlb=f.failTlb=true;auto r=BdwDellForcewakeCritical(f,1000,true,true,true);
      assert(r.sequence.tlbAttempted && !r.sequence.tlbComplete && r.sequence.released && f.tlbWrites==1);
      BdwForcewakeClaim c;assert(c.acquire());c.finish(r);assert(!c.acquire());}
    {Fake f;f.wantPat=f.wantOther=f.wantTlb=f.pendingVcs=true;auto r=BdwDellForcewakeCritical(f,1000,true,true,true);
      assert(!r.sequence.tlbAttempted && !f.tlbWrites && r.sequence.released);}
    {BdwForcewakeClaim claim;BdwForcewakeAdapterResult r{};assert(claim.acquire() && !claim.acquire());
      claim.finish(r);assert(claim.acquire());r.sequence.requested=true;r.clearDelivered=true;r.sequence.released=true;
      claim.finish(r);assert(claim.acquire());r.sequence.released=false;claim.finish(r);assert(!claim.acquire());
      r.sequence.requested=false;claim.finish(r);assert(!claim.acquire());}
    {Fake f;f.wantPat=true;auto r=BdwDellForcewakeCritical(f,1000,false,true);assert(r.sequence.patSampled && r.sequence.patLo==0x12345678 && r.sequence.patHi==0xabcdef01 && r.sequence.released);}
    {Fake f;f.wantPat=f.wantOther=true;auto r=BdwDellForcewakeCritical(f,1000,true,true);assert(r.sequence.patSampled && r.sequence.otherEnginesQuiescent && r.sequence.released && f.evidenceCount==8);}
    {Fake f;f.wantPat=true;f.changingPat=true;auto r=BdwDellForcewakeCritical(f,1000,false,true);assert(!r.sequence.patSampled && r.sequence.released);}
    {Fake f;f.wantPat=true;f.denyPage=5;auto r=BdwDellForcewakeCritical(f,1000,false,true);assert(!r.aliases && !f.writes && !f.reads);}
    {Fake f;auto r=BdwDellForcewakeCritical(f,1000);assert(r.aliases && r.clock && r.sequence.acquired && r.sequence.sampled && r.sequence.idle && r.clearDelivered && r.sequence.released);assert(f.values==std::vector<uint32_t>({0x10001,0x10000}));}
    {Fake f;f.denyPage=4;auto r=BdwDellForcewakeCritical(f,1000);assert(!r.aliases && !f.writes && !f.reads);}
    {Fake f;f.stalled=true;auto r=BdwDellForcewakeCritical(f,1000);assert(r.aliases && !r.clock && !f.writes && !f.reads);}
    {Fake f;f.failSetPost=true;auto r=BdwDellForcewakeCritical(f,1000);assert(r.sequence.requested && !r.sequence.acquired && r.sequence.released && f.writes==2);}
    {Fake f;f.c0=false;auto r=BdwDellForcewakeCritical(f,1000);assert(r.sequence.acquired && !r.sequence.threadReady && r.sequence.released && f.writes==2);}
    {Fake f;auto r=BdwDellForcewakeCritical(f,1000,true);assert(r.sequence.otherEnginesSampled && r.sequence.otherEnginesQuiescent && r.sequence.released && f.evidenceCount==7);}
    {Fake f;f.denyPage=7;auto r=BdwDellForcewakeCritical(f,1000,true);assert(!r.aliases && !f.writes && !f.reads);}
    {Fake f;f.pendingVcs=true;auto r=BdwDellForcewakeCritical(f,1000,true);assert(r.sequence.otherEnginesSampled && !r.sequence.otherEnginesQuiescent && !r.sequence.other[1].submissionQuiescent && r.sequence.released);}
    {Fake f;f.failBcs=true;auto r=BdwDellForcewakeCritical(f,1000,true);assert(!r.sequence.otherEnginesSampled && r.sequence.released && f.readsAfterFailure==0);}
    {Fake f;f.failRcs=true;auto r=BdwDellForcewakeCritical(f,1000,true);assert(!r.sequence.otherEnginesSampled && r.sequence.released && f.readsAfterFailure==0);}
    {Fake f;f.onesBcs=true;auto r=BdwDellForcewakeCritical(f,1000,true);assert(!r.sequence.otherEnginesSampled && r.sequence.released && f.readsAfterFailure==0);}
    {Fake f;f.onesRcs=true;auto r=BdwDellForcewakeCritical(f,1000,true);assert(!r.sequence.otherEnginesSampled && r.sequence.released && f.readsAfterFailure==0);}
    {Fake f;f.failSecondVecs=true;auto r=BdwDellForcewakeCritical(f,1000,true);assert(!r.sequence.otherEnginesSampled && !r.sequence.otherEnginesQuiescent && r.sequence.released);}
    {Fake f;f.failClear=true;auto r=BdwDellForcewakeCritical(f,1000);assert(!r.clearDelivered && !r.sequence.released && f.writes==2);}
}
