#include <cassert>
#include <cstdint>
#include "../../kernel/src/native_tsc_policy.h"
using namespace brook;
struct Cpu {
    bool leaf=true,tsc=true,hypervisor=false,extended=true,rdtscp=true,invariant=true;
    void operator()(uint32_t input,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d)const{
        a=b=c=d=0;
        switch(input){case 0:a=leaf?1:0;break;case 1:d=tsc?1u<<4:0;c=hypervisor?1u<<31:0;break;
        case 0x80000000:a=extended?0x80000007:0x80000001;break;
        case 0x80000001:d=rdtscp?1u<<27:0;break;case 0x80000007:d=invariant?1u<<8:0;break;default:assert(false);}
    }
};
int main(){
    assert(NativeTscCapabilities(Cpu{}));
    for(unsigned i=0;i<6;++i){Cpu c;switch(i){case 0:c.leaf=false;break;case 1:c.tsc=false;break;case 2:c.hypervisor=true;break;
        case 3:c.extended=false;break;case 4:c.rdtscp=false;break;case 5:c.invariant=false;break;}assert(!NativeTscCapabilities(c));}
    // 25 ms PIT window, nominal 2 GHz, with sub-percent measurement variation.
    uint64_t ticks[5]={50000000,50010000,49990000,50005000,49995000};uint32_t khz=0;
    assert(NativeTscCalibrate(ticks,29829,khz) && khz>1999000 && khz<2001000);
    ticks[2]=0;assert(!NativeTscCalibrate(ticks,29829,khz) && !khz);ticks[2]=49990000;
    ticks[3]=51000000;assert(!NativeTscCalibrate(ticks,29829,khz) && !khz);ticks[3]=50005000;
    ticks[0]=~uint64_t(0);assert(!NativeTscCalibrate(ticks,29829,khz));ticks[0]=50000000;
    assert(!NativeTscCalibrate(ticks,0,khz) && !NativeTscCalibrate(ticks,65536,khz));
    uint64_t edge[5]={49751000,50000000,50000000,50000000,50249000};assert(NativeTscCalibrate(edge,29829,khz));
    edge[4]=50260000;assert(!NativeTscCalibrate(edge,29829,khz));
    uint64_t low[5]={1001,1001,1001,1001,1001};assert(NativeTscCalibrate(low,1193,khz) && khz>=1000);
    uint64_t high[5]={19996000,19996000,19996000,19996000,19996000};assert(NativeTscCalibrate(high,1193,khz) && khz<=20000000);
    uint64_t ns=9;assert(NativeTscElapsedNs(100,2000100,2000000,ns) && ns==1000000);
    assert(NativeTscElapsedNs(100,1100,2000000,ns) && ns==500);
    assert(!NativeTscElapsedNs(100,99,2000000,ns) && ns==500);
    assert(!NativeTscElapsedNs(0,~uint64_t(0),1,ns) && ns==500);
    assert(!NativeTscElapsedNs(0,1,0,ns) && ns==500);
    const uint64_t nearMaxMs=~uint64_t(0)/1000000;
    assert(NativeTscElapsedNs(0,nearMaxMs*2,2,ns) && ns==nearMaxMs*1000000);
}
