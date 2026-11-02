#include "native_tsc_observe.h"
#include "native_tsc_policy.h"
#include "native_tsc_pit.h"
#include "portio.h"
#include "smp.h"
#include "vfs.h"
#include "serial.h"
namespace brook {
#ifdef BROOK_HOST_TEST
void NativeTscTestCpuid(uint32_t,uint32_t&,uint32_t&,uint32_t&,uint32_t&);
uint8_t NativeTscTestReadGate();void NativeTscTestWriteGate(uint8_t);void NativeTscTestWriteControl(uint8_t);
void NativeTscTestWriteCounter(uint8_t);bool NativeTscTestRead(uint64_t&);void NativeTscTestPause();
#endif
namespace {
NativeTscObservation observation{};
struct Pit {
    uint8_t readGate(){
#ifdef BROOK_HOST_TEST
        return NativeTscTestReadGate();
#else
        return inb(0x61);
#endif
    }
    void writeGate(uint8_t v){
#ifdef BROOK_HOST_TEST
        NativeTscTestWriteGate(v);
#else
        outb(0x61,v);
#endif
    }
    void writeControl(uint8_t v){
#ifdef BROOK_HOST_TEST
        NativeTscTestWriteControl(v);
#else
        outb(0x43,v);
#endif
    }
    void writeCounter(uint8_t v){
#ifdef BROOK_HOST_TEST
        NativeTscTestWriteCounter(v);
#else
        outb(0x42,v);
#endif
    }
    bool tsc(uint64_t& result){
#ifdef BROOK_HOST_TEST
        return NativeTscTestRead(result);
#else
        uint32_t lo,hi,aux;__asm__ volatile("rdtscp; lfence":"=a"(lo),"=d"(hi),"=c"(aux)::"memory");
        result=(uint64_t(hi)<<32)|lo;return true;
#endif
    }
    void pause(){
#ifdef BROOK_HOST_TEST
        NativeTscTestPause();
#else
        __asm__ volatile("pause");
#endif
    }
};
}
void NativeTscObserveConfigure(){
    if(observation.attempted)return;
    auto* marker=VfsOpen("/etc/native-tsc-observe",0);if(!marker)return;VfsClose(marker);
    observation.requested=true;observation.attempted=true;
    if(SmpCurrentCpuIndex()!=0){observation.reason="not BSP";return;}
    observation.capable=NativeTscCapabilities([](uint32_t leaf,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d){
#ifdef BROOK_HOST_TEST
        NativeTscTestCpuid(leaf,a,b,c,d);
#else
        __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(leaf),"c"(0));
#endif
    });
    if(!observation.capable){observation.reason="native invariant TSC unavailable";return;}
    // APIC initialization has finished its PIT calibration; this runs before
    // drivers or the scheduler can own PIT channel 2. No clock is enabled.
    Pit pit;observation.pitTicks=29829;
    observation.measured=NativeTscMeasurePit(pit,observation.pitTicks,500000,observation.samples);
    if(!observation.measured){observation.reason="PIT measurement failed";return;}
    observation.consistent=NativeTscCalibrate(observation.samples,observation.pitTicks,observation.khz);
    observation.reason=observation.consistent?"observed only":"samples inconsistent";
    if(observation.consistent){const uint32_t cpus=SmpGetCpuCount();observation.coreExpected=cpus?cpus-1:0;
        observation.coreCount=SmpObserveNativeTsc(observation.cores,63);}
    SerialPrintf("NATIVE_TSC_OBSERVE measured=%u consistent=%u khz=%u reason=%s\n",
        unsigned(observation.measured),unsigned(observation.consistent),observation.khz,observation.reason);
}
const NativeTscObservation& NativeTscObserveRead(){return observation;}
}
