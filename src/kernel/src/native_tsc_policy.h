#pragma once
#include <stdint.h>

namespace brook {
// Admission and calibration arithmetic only. This does not read the PIT or
// publish a kernel clock. Frequency invariance is not evidence that TSCs on
// different CPUs have synchronized offsets: callers must separately establish
// CPU affinity or cross-CPU synchronization and handle suspend/reset.
template<class Cpuid> bool NativeTscCapabilities(Cpuid cpuid){
    uint32_t a=0,b=0,c=0,d=0;cpuid(0,a,b,c,d);if(a<1)return false;
    cpuid(1,a,b,c,d);if(!(d&(1u<<4)) || (c&(1u<<31)))return false;
    cpuid(0x80000000u,a,b,c,d);if(a<0x80000007u)return false;
    cpuid(0x80000001u,a,b,c,d);if(!(d&(1u<<27)))return false;
    cpuid(0x80000007u,a,b,c,d);return (d&(1u<<8))!=0;
}
// Each PIT channel-2 window counts pitTicks at 1193182 Hz. Measured TSC spans
// also include gate-write latency, clock phase and terminal polling delay. The
// acquisition code must enforce bounded waits, restore hardware state and
// obtain ordered timestamps; this function cannot prove those properties.
// Reject any inconsistent or implausible window instead of selecting a lucky
// sample. A trusted calibration also needs real-hardware verification.
inline bool NativeTscCalibrate(const uint64_t (&ticks)[5],uint32_t pitTicks,uint32_t& khz){
    khz=0;if(!pitTicks || pitTicks>65535)return false;
    uint32_t samples[5]{};
    for(unsigned i=0;i<5;++i){
        if(!ticks[i] || ticks[i]>~uint64_t(0)/1193182ULL)return false;
        const uint64_t f=ticks[i]*1193182ULL/(uint64_t(pitTicks)*1000);
        if(f<1000 || f>20000000)return false;samples[i]=uint32_t(f);
    }
    for(unsigned i=1;i<5;++i){const auto x=samples[i];unsigned j=i;while(j && samples[j-1]>x){samples[j]=samples[j-1];--j;}samples[j]=x;}
    const uint32_t median=samples[2];if(samples[4]-samples[0]>median/100)return false;
    khz=median;return true;
}
// Compute an elapsed nanosecond duration without overflowing intermediate
// multiplication. Caller checks monotonicity across samples and CPUs.
inline bool NativeTscElapsedNs(uint64_t origin,uint64_t now,uint32_t khz,uint64_t& ns){
    if(!khz || khz>20000000 || now<origin)return false;
    const uint64_t delta=now-origin,ms=delta/khz,rem=delta%khz;
    if(ms>~uint64_t(0)/1000000ULL)return false;
    const uint64_t whole=ms*1000000ULL,fraction=rem*1000000ULL/khz;
    if(whole>~uint64_t(0)-fraction)return false;ns=whole+fraction;return true;
}
}
