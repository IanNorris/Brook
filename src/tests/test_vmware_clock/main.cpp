#include <cassert>
#include <cstdint>
#include "../../kernel/src/vmware_clock_policy.h"
#include "../../kernel/src/vmware_platform.h"

struct Cpu {
    bool hypervisor=true,tsc=true,vendor=true,frequencyLeaf=true,extended=true,rdtscp=true,invariant=true;
    uint32_t khz=2903999;
    void operator()(uint32_t leaf,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d) const {
        a=b=c=d=0;
        switch(leaf){
        case 1:c=hypervisor?(1u<<31):0;d=tsc?(1u<<4):0;break;
        case 0x40000000:a=frequencyLeaf?0x40000010:0x40000000;
            b=vendor?0x61774d56:0;c=0x4d566572;d=0x65726177;break;
        case 0x80000000:a=extended?0x80000007:0x80000001;break;
        case 0x80000001:d=rdtscp?(1u<<27):0;break;
        case 0x80000007:d=invariant?(1u<<8):0;break;
        case 0x40000010:a=khz;break;
        default:assert(false);
        }
    }
};
int main(){
    for(unsigned accepted=0;accepted<2;++accepted){
        unsigned calls=0;
        const bool ready=brook::VmwareEnableLazyTimers([&](uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d){
            ++calls;assert(a==0x564d5868u && b==0xffffffffu && c==49 && d==0x5658);
            b=accepted?0x564d5868u:0xffffffffu;
        });
        assert(calls==1 && ready==bool(accepted));
    }
    assert(brook::VmwareTscFrequencyKhz(Cpu{})==2903999);
    assert(brook::VmwarePlatformPresent(Cpu{}));
    for(unsigned disabled=0;disabled<8;++disabled){
        Cpu cpu;
        switch(disabled){
        case 0:cpu.hypervisor=false;break;case 1:cpu.tsc=false;break;
        case 2:cpu.vendor=false;break;case 3:cpu.frequencyLeaf=false;break;
        case 4:cpu.extended=false;break;case 5:cpu.rdtscp=false;break;
        case 6:cpu.invariant=false;break;case 7:cpu.khz=0;break;
        }
        assert(!brook::VmwareTscFrequencyKhz(cpu));
        if(disabled==0 || disabled==2)assert(!brook::VmwarePlatformPresent(cpu));
    }
    uint64_t out=42;
    assert(brook::VmwareTscElapsedMs(100,7,100+uint64_t(2903999)*10000,2903999,&out)&&out==10007);
    assert(brook::VmwareTscElapsedMs(100,7,100+2903998,2903999,&out)&&out==7);
    assert(!brook::VmwareTscElapsedMs(100,7,99,2903999,&out)&&out==7);
    assert(!brook::VmwareTscElapsedMs(100,7,101,0,&out)&&out==7);
    assert(!brook::VmwareTscElapsedMs(0,0,100,1,nullptr));
    assert(brook::VmwareTscElapsedMs(0,~uint64_t(0)-1,1,1,&out)&&out==~uint64_t(0));
    assert(!brook::VmwareTscElapsedMs(0,~uint64_t(0)-1,2,1,&out)&&out==~uint64_t(0));
}
