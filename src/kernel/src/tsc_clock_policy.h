#pragma once
#include <stdint.h>
#include "native_tsc_rendezvous.h"

namespace brook {
// Pure arithmetic and admission policy for the kernel's invariant-TSC elapsed
// clock. Host-tested; no CPUID, MSR or TSC access happens here.
//
// The conversion is the KVM pvclock one (ns = ((delta << shift) * mul) >> 32
// with a negative shift meaning a right shift), so the vDSO's existing pvclock
// reader computes exactly the same value as the kernel for the same TSC read.
struct TscScale { uint32_t mul=0; int8_t shift=0; };

// Linux kvm_get_time_scale(NSEC_PER_SEC, hz): mul ends up in [2^31, 2^32).
inline bool TscScaleFromHz(uint64_t hz,TscScale& out){
    out={};
    if(hz<1000000ULL || hz>20000000000ULL)return false;
    constexpr uint64_t High=0xffffffff00000000ULL;
    uint64_t scaled=1000000000ULL,tps64=hz;int shift=0;
    while(tps64>scaled*2 || (tps64&High)){tps64>>=1;--shift;}
    uint32_t tps32=uint32_t(tps64);
    while(tps32<=scaled || (scaled&High)){
        if((scaled&High) || (tps32&0x80000000u))scaled>>=1;else tps32<<=1;
        ++shift;
    }
    if(!tps32 || scaled>=tps32)return false;
    out.mul=uint32_t((scaled<<32)/tps32);out.shift=int8_t(shift);
    return out.mul!=0;
}

// Overflow-safe: a positive shift that would lose high bits is rejected and
// the 64x32 product is formed in 128 bits.
inline bool TscScaleApply(uint64_t delta,const TscScale& s,uint64_t& ns){
    if(!s.mul || s.shift<-63 || s.shift>63)return false;
    if(s.shift<0)delta>>=-s.shift;
    else if(s.shift>0){if(delta>(~uint64_t(0)>>s.shift))return false;delta<<=s.shift;}
    ns=uint64_t((static_cast<unsigned __int128>(delta)*s.mul)>>32);return true;
}

// Absolute clock value for one TSC read. A read behind the origin (a CPU whose
// TSC trails the BSP's by less than the admitted sync bound, sampled just after
// selection) clamps to the origin instead of wrapping.
inline uint64_t TscClockValue(uint64_t tsc,uint64_t originTsc,uint64_t originNs,const TscScale& s){
    if(tsc<=originTsc)return originNs;
    uint64_t elapsed=0;
    if(!TscScaleApply(tsc-originTsc,s,elapsed) || elapsed>~uint64_t(0)-originNs)return ~uint64_t(0);
    return originNs+elapsed;
}

// Saturating; no 128-bit division (the kernel has no libgcc).
inline uint64_t TscTicksToNs(uint64_t ticks,uint32_t khz){
    if(!khz)return ~uint64_t(0);
    const uint64_t ms=ticks/khz,rem=ticks%khz;
    if(ms>~uint64_t(0)/1000000ULL)return ~uint64_t(0);
    const uint64_t whole=ms*1000000ULL,fraction=rem*1000000ULL/khz;
    return whole>~uint64_t(0)-fraction?~uint64_t(0):whole+fraction;
}

// Cross-CPU synchronisation evidence from the bootstrap rendezvous: for each
// AP the intersected interval bounds (AP - BSP) in TSC ticks. The largest
// |bound| is a proven upper bound on that AP's offset at boot. Each exchange
// is also a warp test (BSP read, AP read, BSP read): an interval excluding
// zero means some exchange observed time going backwards between CPUs.
struct TscSyncSummary {
    bool complete=false,warpFree=false;uint32_t failedCpu=0;
    NativeTscExchangeResult failure=NativeTscExchangeResult::Complete;
    uint64_t maxTicks=0;
};
inline uint64_t TscAbs(int64_t v){return v<0?uint64_t(0)-uint64_t(v):uint64_t(v);}
inline TscSyncSummary TscSyncSummarize(const NativeTscCpuObservation* cores,uint32_t count,uint32_t expected){
    TscSyncSummary s{};s.warpFree=true;
    if(count!=expected || (count && !cores)){s.warpFree=false;s.failure=NativeTscExchangeResult::Unavailable;return s;}
    for(uint32_t i=0;i<count;++i){
        const auto& c=cores[i];
        if(c.result!=NativeTscExchangeResult::Complete || !c.exchanges || c.interval.lower>c.interval.upper){
            s.failedCpu=c.cpu?c.cpu:i+1;
            s.failure=c.result==NativeTscExchangeResult::Complete?NativeTscExchangeResult::Invalid:c.result;
            return s;
        }
        if(c.interval.lower>0 || c.interval.upper<0)s.warpFree=false;
        const uint64_t lo=TscAbs(c.interval.lower),hi=TscAbs(c.interval.upper);
        const uint64_t bound=lo>hi?lo:hi;
        if(bound>s.maxTicks)s.maxTicks=bound;
    }
    s.complete=true;return s;
}

enum class ClockSourceKind:uint8_t {ApicTicks,Kvmclock,VmwareTsc,Tsc};
inline const char* ClockSourceName(ClockSourceKind k){
    switch(k){case ClockSourceKind::Kvmclock:return "kvmclock";case ClockSourceKind::VmwareTsc:return "vmware-tsc";
    case ClockSourceKind::Tsc:return "tsc";default:return "apic-ticks";}
}

struct TscCpuFeatures {
    bool tsc=false,hypervisor=false,rdtscp=false,invariant=false;
};
template<class Cpuid> TscCpuFeatures TscReadCpuFeatures(Cpuid cpuid){
    TscCpuFeatures f{};uint32_t a=0,b=0,c=0,d=0;
    cpuid(0u,a,b,c,d);if(a<1)return f;
    cpuid(1u,a,b,c,d);f.tsc=(d&(1u<<4))!=0;f.hypervisor=(c&(1u<<31))!=0;
    cpuid(0x80000000u,a,b,c,d);const uint32_t maxExt=a;
    if(maxExt>=0x80000001u){cpuid(0x80000001u,a,b,c,d);f.rdtscp=(d&(1u<<27))!=0;}
    if(maxExt>=0x80000007u){cpuid(0x80000007u,a,b,c,d);f.invariant=(d&(1u<<8))!=0;}
    return f;
}

// Admission bound on the proven offset. The kernel clock keeps a global
// monotonic floor, so it tolerates a residual offset; the vDSO fast path has
// no floor and is enabled only when no exchange observed a cross-CPU warp.
constexpr uint64_t TscSyncAdmitNs=20000;
// The five boot PIT windows must agree to within this fraction of the median.
constexpr uint32_t TscCalibrationSpreadPpm=5000;

struct ClockSelectionInput {
    bool kvmclock=false,vmwareTsc=false;
    TscCpuFeatures cpu{};
    uint32_t khz=0;uint32_t samples[5]{};
    bool syncMeasured=false;TscSyncSummary sync{};
};
struct ClockSelection {
    ClockSourceKind kind=ClockSourceKind::ApicTicks;
    const char* reason="unselected";
    uint64_t syncMaxNs=0;bool vdso=false;
    TscScale scale{};
};

// First phase: everything except cross-CPU synchronisation. Returns true when
// the caller should run the rendezvous and call ClockSelect with its summary.
inline bool ClockSelectWantsSync(const ClockSelectionInput& in,ClockSelection& out){
    out={};
    if(in.kvmclock){out.kind=ClockSourceKind::Kvmclock;out.reason="kvm-pvclock";return false;}
    if(in.vmwareTsc){out.kind=ClockSourceKind::VmwareTsc;out.reason="vmware-backdoor";return false;}
    if(!in.cpu.tsc){out.reason="no-tsc";return false;}
    if(!in.cpu.invariant){out.reason="no-invariant-tsc";return false;}
    if(!in.khz){out.reason="uncalibrated";return false;}
    uint32_t lo=~uint32_t(0),hi=0;
    for(auto s:in.samples){if(!s){out.reason="calibration-sample-missing";return false;}if(s<lo)lo=s;if(s>hi)hi=s;}
    if(in.khz<lo || in.khz>hi){out.reason="calibration-median-outside-samples";return false;}
    if(uint64_t(hi-lo)*1000000ULL>uint64_t(in.khz)*TscCalibrationSpreadPpm){out.reason="calibration-inconsistent";return false;}
    if(!TscScaleFromHz(uint64_t(in.khz)*1000ULL,out.scale)){out.reason="calibration-out-of-range";return false;}
    out.reason="pending-sync";return true;
}

inline ClockSelection ClockSelect(const ClockSelectionInput& in){
    ClockSelection out{};
    if(!ClockSelectWantsSync(in,out))return out;
    const TscScale scale=out.scale;
    out.scale={};
    if(!in.syncMeasured){out.reason="sync-unmeasured";return out;}
    if(!in.sync.complete){out.reason="sync-exchange-failed";return out;}
    out.syncMaxNs=TscTicksToNs(in.sync.maxTicks,in.khz);
    if(out.syncMaxNs>TscSyncAdmitNs){out.reason="sync-offset-too-large";return out;}
    out.kind=ClockSourceKind::Tsc;out.scale=scale;
    out.vdso=in.cpu.rdtscp && in.sync.warpFree;
    out.reason=in.cpu.hypervisor?"invariant-tsc-under-hypervisor":"invariant-tsc";
    return out;
}
}
