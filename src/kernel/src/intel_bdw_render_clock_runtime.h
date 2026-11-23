#pragma once
// Kernel-only MSR sampling for the render-clock thermal guard and the
// `gpu clock status` power readout. Every MSR read is gated on CPUID: bare
// metal (no hypervisor bit), Intel family 6, package thermal management
// (CPUID.06H:EAX[6]) for the temperature MSRs, and a Broadwell client model
// (0x3d/0x47) for RAPL, so QEMU/KVM never takes a #GP from an absent MSR.
#include <stdint.h>
#include "cpu.h"
#include "kprintf.h"
#include "spinlock.h"
#include "intel_bdw_render_clock.h"

namespace brook {
struct BdwClockPlatform {bool thermal=false,rapl=false;};
inline void BdwClockCpuid(uint32_t leaf,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d){
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(leaf),"c"(0u));
}
inline uint64_t BdwClockTsc(){uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));return (uint64_t(hi)<<32)|lo;}
inline BdwClockPlatform BdwClockPlatformInfo(){
    static uint32_t features=0;
    uint32_t flags=__atomic_load_n(&features,__ATOMIC_ACQUIRE);
    constexpr uint32_t Decided=1,Thermal=2,Rapl=4;
    if(!(flags&Decided)){
        flags=Decided;
        uint32_t a=0,b=0,c=0,d=0;BdwClockCpuid(0,a,b,c,d);
        const uint32_t maxLeaf=a;const bool intel=b==0x756e6547u && d==0x49656e69u && c==0x6c65746eu;
        if(intel && maxLeaf>=6){
            BdwClockCpuid(1,a,b,c,d);
            const bool hypervisor=(c&(1u<<31))!=0;
            const uint32_t family=(a>>8)&0xf,model=((a>>4)&0xf)|(((a>>16)&0xf)<<4);
            uint32_t a6=0,b6=0,c6=0,d6=0;BdwClockCpuid(6,a6,b6,c6,d6);
            if(!hypervisor && family==6){
                if(a6&(1u<<6))flags|=Thermal;
                if(model==0x3d || model==0x47)flags|=Rapl;
            }
        }
        __atomic_store_n(&features,flags,__ATOMIC_RELEASE);
    }
    return {bool(flags&Thermal),bool(flags&Rapl)};
}
inline int32_t BdwClockReadTempC(){
    if(!BdwClockPlatformInfo().thermal)return -1;
    return BdwClockDecodeTempC(ReadMsr(0x1b1),ReadMsr(0x1a2));
}
// The clock command and gpu-submit worker can run on different CPUs.
// Keep the policy, thermal observations and last job request coherent.
struct BdwClockRuntime {
    SpinLock lock{};
    BdwClockState state{};
};
inline BdwClockRuntime& BdwClockRuntimeState(){static BdwClockRuntime runtime{};return runtime;}
inline BdwClockState BdwClockReadStatus(){
    auto& runtime=BdwClockRuntimeState();
    SpinLockAcquire(&runtime.lock);
    const auto status=runtime.state;
    SpinLockRelease(&runtime.lock);
    return status;
}
inline void BdwClockConfigure(uint32_t mhz){
    auto& runtime=BdwClockRuntimeState();
    SpinLockAcquire(&runtime.lock);
    if(mhz)BdwClockSetMhz(runtime.state,mhz);
    else BdwClockSetAuto(runtime.state);
    SpinLockRelease(&runtime.lock);
}
inline void BdwClockConfigureBalance(uint32_t mhz){
    auto& runtime=BdwClockRuntimeState();
    SpinLockAcquire(&runtime.lock);
    BdwClockSetBalance(runtime.state,mhz);
    SpinLockRelease(&runtime.lock);
}
inline void BdwClockRecordCaps(uint32_t caps){
    if(!caps || caps==0xffffffffu)return;
    auto& runtime=BdwClockRuntimeState();
    SpinLockAcquire(&runtime.lock);
    runtime.state.lastCaps=caps;
    SpinLockRelease(&runtime.lock);
}

// Sample package temperature at most every 50 ms. A zero return admits a
// fused-cap read without changing the software request register.
inline uint32_t BdwClockRequestForJob(bool deviceAdmitted,const BdwClockLoadSample& load,uint64_t tscKhz){
    auto& runtime=BdwClockRuntimeState();
    const uint64_t now=BdwClockTsc();
    SpinLockAcquire(&runtime.lock);
    auto& s=runtime.state;
    int change=0;
    if(!tscKhz || !s.lastSampleTsc || now-s.lastSampleTsc>=tscKhz*50){
        s.lastSampleTsc=now;
        const int32_t temp=BdwClockReadTempC();
        const uint64_t nowMs=tscKhz?now/tscKhz:0;
        if(temp>=0)change=BdwClockNoteTemp(s,temp,nowMs);
        else s.lastTempC=-1;
    }
    const uint32_t bin=BdwClockSelectRequest(s,deviceAdmitted,load,tscKhz);
    s.lastRequestedBin=bin;
    const int32_t temp=s.lastTempC,limit=s.limitC;
    const unsigned recoveries=unsigned(s.recoveries);
    SpinLockRelease(&runtime.lock);
    if(change>0)
        KPrintf("gpu-clock: package %d C >= limit %d C; render clock request back to %u MHz until %d C for %u ms\n",
            temp,limit,BdwClockDefaultMhz,limit-BdwClockRecoverMarginC,unsigned(BdwClockRecoverMs));
    else if(change<0)
        KPrintf("gpu-clock: package %d C; thermal guard recovered (recovery %u)\n",temp,recoveries);
    return bin;
}
struct BdwClockEnergy { bool valid=false; uint32_t pkg=0,pp1=0; uint64_t unit=0,tsc=0; };
inline BdwClockEnergy BdwClockReadEnergy(){
    BdwClockEnergy e{};
    if(!BdwClockPlatformInfo().rapl)return e;
    e.unit=ReadMsr(0x606);e.pkg=uint32_t(ReadMsr(0x611));e.pp1=uint32_t(ReadMsr(0x641));e.tsc=BdwClockTsc();e.valid=true;
    return e;
}
}
