#include <cassert>
#include "intel_bdw_forcewake.h"
using namespace brook;
struct Fake {
    bool owner=true,ackInitial=false,failSet=false,failAcquire=false,failThread=false,failRelease=false,failSample=false,moving=false;
    bool badClock=false,allOnes=false,badPoll=false,late=false,reverse=false,pendingSet=false,appliedSet=false,changingContext=false,pendingContext=false,listMode=false,validRing=false,stoppedRing=false;uint64_t time=0;unsigned clocks=0;
    unsigned writes=0,samples=0,ackReads=0,contextReads=0;uint32_t commands[2]{};
    bool owned(){return owner;}
    bool read(uint32_t reg,uint32_t& v){
        if(reg==0x130044){++ackReads;v=(allOnes || (badPoll && writes==1))?0xffffffffu:(writes==0?(ackInitial?1:0):writes==1?(!failAcquire && !failSet?1:0):(failRelease?1:0));return true;}
        if(reg==0x13805c){v=failThread?1:0;return true;}
        if(failSample)return false;
        if(reg==0x22034){v=(moving && samples++?8:0);return true;}
        if(reg==0x22234){v=(pendingContext || (changingContext && contextReads++))?8:1;return true;}
        if(reg==0x2229c){v=listMode?(1u<<15):0;return true;}
        if(reg==0x2203c){v=validRing?1:0;return true;}
        if(reg==0x22030 || reg==0x22038 || reg==0x22238){v=0;return true;}
        if(reg==0x2209c){v=(1u<<9)|(stoppedRing?(1u<<8):0);return true;}
        assert(false);return false;
    }
    bool write(uint32_t reg,uint32_t v){assert(reg==0xa188 && writes<2);commands[writes++]=v;
        if(writes==1){pendingSet=failSet;if(!failSet)appliedSet=true;return !failSet;}
        if(pendingSet){appliedSet=true;pendingSet=false;}appliedSet=false;return true;}
    bool monotonicMs(uint64_t& value){if(badClock)return false;++clocks;
        if(late && clocks==3)time+=100;
        value=(reverse && clocks==4)?time-2:time++;return true;}
    void relax(){}
};
int main(){
    {Fake f;auto r=BdwObserveBcs(f);assert(r.requested && r.acquired && r.threadReady && r.sampled && r.idle && r.released);
        assert(f.writes==2 && f.commands[0]==0x00010001 && f.commands[1]==0x00010000);}
    {Fake f;f.ackInitial=true;auto r=BdwObserveBcs(f);assert(!r.requested && !f.writes);}
    {Fake f;f.allOnes=true;auto r=BdwObserveBcs(f);assert(!r.requested && !f.writes);}
    {Fake f;f.badPoll=true;auto r=BdwObserveBcs(f);assert(r.requested && !r.acquired && r.released && f.writes==2);}
    {Fake f;f.owner=false;auto r=BdwObserveBcs(f);assert(!r.requested && !f.writes);}
    {Fake f;f.failSet=true;auto r=BdwObserveBcs(f);assert(r.requested && !r.acquired && r.released && f.writes==2 && !f.pendingSet && !f.appliedSet);}
    {Fake f;f.failAcquire=true;auto r=BdwObserveBcs(f);assert(!r.acquired && r.released && f.writes==2);}
    {Fake f;f.failThread=true;auto r=BdwObserveBcs(f);assert(r.acquired && !r.threadReady && r.released && f.writes==2);}
    {Fake f;f.failRelease=true;auto r=BdwObserveBcs(f);assert(r.sampled && !r.released && f.writes==2);}
    {Fake f;f.failSample=true;auto r=BdwObserveBcs(f);assert(!r.sampled && r.released);}
    {Fake f;f.badClock=true;auto r=BdwObserveBcs(f);assert(!r.requested && !f.writes);}
    {Fake f;f.late=true;auto r=BdwObserveBcs(f);assert(r.requested && !r.acquired && r.released);}
    {Fake f;f.failAcquire=true;f.reverse=true;auto r=BdwObserveBcs(f);assert(r.requested && !r.acquired && r.released && f.ackReads==4);}
    {Fake f;f.pendingContext=true;auto r=BdwObserveBcs(f);assert(r.sampled && r.idle && r.first.execlistLo==8 && r.released);}
    {Fake f;f.listMode=true;auto r=BdwObserveEngine(f,0x22000);
        assert(r.submissionQuiescent && BdwExeclistObservationReady(r,true) && !BdwExeclistObservationReady(r,false));}
    {Fake f;auto r=BdwObserveEngine(f,0x22000);assert(r.submissionQuiescent && !BdwExeclistObservationReady(r,true));}
    {Fake f;f.listMode=true;f.stoppedRing=true;auto r=BdwObserveEngine(f,0x22000);
        assert(r.submissionQuiescent && !BdwExeclistObservationReady(r,true));}
    {Fake f;f.listMode=true;f.failSample=true;auto r=BdwObserveEngine(f,0x22000);assert(!BdwExeclistObservationReady(r,true));}
    {Fake f;f.listMode=true;f.changingContext=true;auto r=BdwObserveEngine(f,0x22000);assert(!BdwExeclistObservationReady(r,true));}
    {Fake f;f.listMode=true;f.pendingContext=true;auto r=BdwObserveBcs(f);assert(r.idle && !r.submissionQuiescent);}
    {Fake f;f.validRing=true;auto r=BdwObserveBcs(f);assert(r.idle && !r.submissionQuiescent);}
    {Fake f;f.changingContext=true;auto r=BdwObserveBcs(f);assert(r.sampled && !r.idle && r.released);}
    {Fake f;f.moving=true;auto r=BdwObserveBcs(f);assert(r.sampled && !r.idle && r.released);}
}
