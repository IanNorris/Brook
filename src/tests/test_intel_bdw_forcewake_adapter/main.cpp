#include "intel_bdw_forcewake_adapter.h"
#include <cassert>
#include <vector>
using namespace brook;
struct Fake {
    unsigned evidenceCount=0,denyPage=0,writes=0,reads=0;bool owner=true,stalled=false,failSetPost=false,failClear=false,c0=true;
    bool awake=false,pendingVcs=false,failSecondVecs=false,failBcs=false,failRcs=false,onesBcs=false,onesRcs=false,failed=false;unsigned vecsHeads=0,readsAfterFailure=0;uint64_t ticks=0;std::vector<uint32_t> values;
    bool owned(){return owner;}
    bool evidence(uint64_t page,DellI2cCriticalEvidence& e){assert(page==(evidenceCount<4?BdwForcewakePages[evidenceCount]:BdwOtherEnginePages[evidenceCount-4]));++evidenceCount;
        if(evidenceCount==denyPage)return false;
        e.aliasesValid=e.identityPresent=e.identityMatches=e.directPresent=e.directMatches=e.currentDirectPresent=e.currentDirectMatches=e.cpuValid=true;
        e.identityPat=e.directPat=e.currentDirectPat=0;e.patSlot0=6;e.mtrrType=0;return true;}
    bool tsc(uint64_t& t){if(!stalled)ticks+=1000;t=ticks;return true;}
    void relax(){}
    bool write32(uint64_t a,uint32_t v){assert(a==BdwDellBar0+0xa188);++writes;values.push_back(v);awake=v==0x10001;return !(v==0x10000 && failClear);}
    bool read32(uint64_t a,uint32_t& v){++reads;
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
