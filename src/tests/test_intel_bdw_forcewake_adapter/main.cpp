#include "intel_bdw_forcewake_adapter.h"
#include <cassert>
#include <vector>
using namespace brook;
struct Fake {
    unsigned evidenceCount=0,denyPage=0,writes=0,reads=0;bool owner=true,stalled=false,failSetPost=false,failClear=false,c0=true;
    bool awake=false;uint64_t ticks=0;std::vector<uint32_t> values;
    bool owned(){return owner;}
    bool evidence(uint64_t page,DellI2cCriticalEvidence& e){assert(page==BdwForcewakePages[evidenceCount]);++evidenceCount;
        if(evidenceCount==denyPage)return false;
        e.aliasesValid=e.identityPresent=e.identityMatches=e.directPresent=e.directMatches=e.currentDirectPresent=e.currentDirectMatches=e.cpuValid=true;
        e.identityPat=e.directPat=e.currentDirectPat=0;e.patSlot0=6;e.mtrrType=0;return true;}
    bool tsc(uint64_t& t){if(!stalled)ticks+=1000;t=ticks;return true;}
    void relax(){}
    bool write32(uint64_t a,uint32_t v){assert(a==BdwDellBar0+0xa188);++writes;values.push_back(v);awake=v==0x10001;return !(v==0x10000 && failClear);}
    bool read32(uint64_t a,uint32_t& v){++reads;
        if(a==BdwDellBar0+0x130044){if(failSetPost && writes==1){failSetPost=false;return false;}v=awake?1:0;return true;}
        if(a==BdwDellBar0+0x13805c){v=c0?0:1;return true;}
        if(a==BdwDellBar0+0x2209c){v=1u<<9;return true;}
        assert(a==BdwDellBar0+0x22034 || a==BdwDellBar0+0x22030);v=0;return true;}
};
int main(){
    {Fake f;auto r=BdwDellForcewakeCritical(f,1000);assert(r.aliases && r.clock && r.sequence.acquired && r.sequence.sampled && r.sequence.idle && r.clearDelivered && r.sequence.released);assert(f.values==std::vector<uint32_t>({0x10001,0x10000}));}
    {Fake f;f.denyPage=4;auto r=BdwDellForcewakeCritical(f,1000);assert(!r.aliases && !f.writes && !f.reads);}
    {Fake f;f.stalled=true;auto r=BdwDellForcewakeCritical(f,1000);assert(r.aliases && !r.clock && !f.writes && !f.reads);}
    {Fake f;f.failSetPost=true;auto r=BdwDellForcewakeCritical(f,1000);assert(r.sequence.requested && !r.sequence.acquired && r.sequence.released && f.writes==2);}
    {Fake f;f.c0=false;auto r=BdwDellForcewakeCritical(f,1000);assert(r.sequence.acquired && !r.sequence.threadReady && r.sequence.released && f.writes==2);}
    {Fake f;f.failClear=true;auto r=BdwDellForcewakeCritical(f,1000);assert(!r.clearDelivered && !r.sequence.released && f.writes==2);}
}
