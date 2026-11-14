#pragma once
// Kernel-only MSR sampling for the render-clock thermal guard and the
// `gpu clock status` power readout. Every MSR read is gated on CPUID: bare
// metal (no hypervisor bit), Intel family 6, package thermal management
// (CPUID.06H:EAX[6]) for the temperature MSRs, and a Broadwell client model
// (0x3d/0x47) for RAPL, so QEMU/KVM never takes a #GP from an absent MSR.
#include <stdint.h>
#include "cpu.h"
#include "kprintf.h"
#include "intel_bdw_render_clock.h"

namespace brook {
struct BdwClockPlatform { bool decided=false,thermal=false,rapl=false; };
inline void BdwClockCpuid(uint32_t leaf,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d){
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(leaf),"c"(0u));
}
inline uint64_t BdwClockTsc(){uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));return (uint64_t(hi)<<32)|lo;}
inline const BdwClockPlatform& BdwClockPlatformInfo(){
    static BdwClockPlatform p{};
    if(p.decided)return p;
    uint32_t a=0,b=0,c=0,d=0;BdwClockCpuid(0,a,b,c,d);
    const uint32_t maxLeaf=a;const bool intel=b==0x756e6547u && d==0x49656e69u && c==0x6c65746eu;
    if(intel && maxLeaf>=6){
        BdwClockCpuid(1,a,b,c,d);
        const bool hypervisor=(c&(1u<<31))!=0;
        const uint32_t family=(a>>8)&0xf,model=((a>>4)&0xf)|(((a>>16)&0xf)<<4);
        uint32_t a6=0,b6=0,c6=0,d6=0;BdwClockCpuid(6,a6,b6,c6,d6);
        p.thermal=!hypervisor && family==6 && (a6&(1u<<6))!=0;
        p.rapl=!hypervisor && family==6 && (model==0x3d || model==0x47);
    }
    p.decided=true;return p;
}
inline int32_t BdwClockReadTempC(){
    if(!BdwClockPlatformInfo().thermal)return -1;
    return BdwClockDecodeTempC(ReadMsr(0x1b1),ReadMsr(0x1a2));
}
// Called by the gpu-submit worker before each render job; samples at most
// every 50 ms and latches the 500 MHz fallback at the thermal limit.
inline void BdwClockSampleThermal(uint64_t tscKhz){
    auto& s=BdwClock();
    const uint64_t now=BdwClockTsc();
    if(tscKhz && s.lastSampleTsc && now-s.lastSampleTsc<tscKhz*50)return;
    s.lastSampleTsc=now;
    const int32_t t=BdwClockReadTempC();
    if(t<0)return;
    if(BdwClockNoteTemp(s,t))
        KPrintf("gpu-clock: package %d C >= limit %d C; render clock request back to %u MHz until the next 'gpu clock' command\n",
            t,s.limitC,BdwClockDefaultMhz);
}
struct BdwClockEnergy { bool valid=false; uint32_t pkg=0,pp1=0; uint64_t unit=0,tsc=0; };
inline BdwClockEnergy BdwClockReadEnergy(){
    BdwClockEnergy e{};
    if(!BdwClockPlatformInfo().rapl)return e;
    e.unit=ReadMsr(0x606);e.pkg=uint32_t(ReadMsr(0x611));e.pp1=uint32_t(ReadMsr(0x641));e.tsc=BdwClockTsc();e.valid=true;
    return e;
}
}
