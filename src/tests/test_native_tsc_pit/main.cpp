#include <cassert>
#include <cstdint>
#include "../../kernel/src/native_tsc_pit.h"
using namespace brook;
struct Pit {
    uint8_t gate=0x13;unsigned controls=0,counters=0,pauses=0,reads=0,tscs=0,loaded=0,countdown=0,stuckLowAt=0;
    bool output=true,running=false,stuckHigh=false,stuckLow=false,backward=false,failTsc=false;
    uint8_t readGate(){++reads;if(running && !stuckLow && controls!=stuckLowAt && ++countdown>=2){output=true;running=false;}
        return uint8_t((gate&~0x20u)|((stuckHigh || output)?0x20:0));}
    void writeGate(uint8_t value){
        if((value&1) && !(gate&1) && loaded==2 && !output && !(value&2)){running=true;countdown=0;}
        gate=value;}
    void writeControl(uint8_t value){assert(value==0xb0 && !(gate&3));++controls;loaded=0;output=false;running=false;}
    void writeCounter(uint8_t value){assert(loaded<2 && value==(loaded?0x74:0x85));++loaded;++counters;}
    bool tsc(uint64_t& out){++tscs;if(failTsc)return false;out=backward?100:100+uint64_t(tscs)*50000000;return true;}
    void pause(){++pauses;}
};
int main(){
    Pit p;uint64_t ticks[5]{};assert(NativeTscMeasurePit(p,29829,20,ticks));
    assert((p.gate&~0x20u)==0x13 && p.controls==5 && p.counters==10 && p.tscs==10);
    for(auto t:ticks)assert(t==50000000);
    for(unsigned i=0;i<5;++i){Pit f;uint64_t unchanged[5]={1,2,3,4,5};
        if(i==0)f.stuckHigh=true;if(i==1)f.stuckLow=true;if(i==2)f.backward=true;if(i==3)f.failTsc=true;if(i==4)f.stuckLowAt=3;
        assert(!NativeTscMeasurePit(f,29829,4,unchanged) && (f.gate&~0x20u)==0x13);
        for(unsigned j=0;j<5;++j)assert(unchanged[j]==j+1);
    }
    Pit invalid;assert(!NativeTscMeasurePit(invalid,0,20,ticks) && !invalid.reads);
    assert(!NativeTscMeasurePit(invalid,29829,0,ticks) && !invalid.reads);
}
