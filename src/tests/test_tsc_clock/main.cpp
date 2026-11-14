#include <cassert>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>
#include <string_view>
#include "../../kernel/src/tsc_clock_policy.h"
#include "../../kernel/src/tick_clock.h"
using namespace brook;

namespace {
// Exact reference: delta * 1e9 / hz, in 128 bits.
uint64_t Reference(uint64_t delta,uint64_t hz){
    return uint64_t((static_cast<unsigned __int128>(delta)*1000000000u)/hz);
}
// The vDSO's pvclock arithmetic, transcribed from src/kernel/vdso/clock.c.
bool VdsoValue(uint64_t tsc,uint64_t timestamp,uint64_t base,uint32_t mul,int shift,uint64_t& out){
    if(!mul || tsc<timestamp || shift<-63 || shift>63)return false;
    uint64_t delta=tsc-timestamp;
    if(shift<0)delta>>=-shift;
    else if(shift>0){if(delta>(~uint64_t(0)>>shift))return false;delta<<=shift;}
    const uint64_t elapsed=uint64_t((static_cast<unsigned __int128>(delta)*mul)>>32);
    if(base>~uint64_t(0)-elapsed)return false;
    out=base+elapsed;return true;
}

void Conversion(){
    const uint64_t rates[]={1000000ULL,14318180ULL,999999999ULL,1000000000ULL,1999999999ULL,2000000000ULL,
        2394626000ULL,2400000000ULL,2904748000ULL,3999999999ULL,4294967296ULL,5000000000ULL,20000000000ULL};
    for(auto hz:rates){
        TscScale s{};assert(TscScaleFromHz(hz,s));
        assert(s.mul>=0x80000000u);                 // normalised multiplier
        // Relative error of the scale must be far below 1 ppm.
        const uint64_t deltas[]={0,1,2,3,1000,hz/1000,hz,hz*60,hz*3600,hz*86400,hz*86400*365};
        for(auto d:deltas){
            uint64_t ns=0;assert(TscScaleApply(d,s,ns));
            const uint64_t ref=Reference(d,hz);
            const uint64_t diff=ns>ref?ns-ref:ref-ns;
            // shift<0 drops low TSC bits (sub-ns); multiplier truncation <1e-9.
            const uint64_t allowed=2+ref/1000000000ULL;
            if(diff>allowed){std::fprintf(stderr,"hz=%llu delta=%llu ns=%llu ref=%llu\n",
                (unsigned long long)hz,(unsigned long long)d,(unsigned long long)ns,(unsigned long long)ref);assert(false);}
            // Kernel and vDSO agree exactly for the same read.
            uint64_t v=0;assert(VdsoValue(1000+d,1000,777,s.mul,s.shift,v) && v==777+ns);
            assert(TscClockValue(1000+d,1000,777,s)==777+ns);
        }
        // Monotonic in delta.
        uint64_t prev=0;
        for(uint64_t d=0;d<200000;d+=7){uint64_t ns=0;assert(TscScaleApply(d,s,ns));assert(ns>=prev);prev=ns;}
    }
    TscScale s{};
    assert(!TscScaleFromHz(0,s) && !s.mul);assert(!TscScaleFromHz(999999,s));assert(!TscScaleFromHz(20000000001ULL,s));
    // Overflow safety: maximum delta never wraps, a left shift that would is refused.
    assert(TscScaleFromHz(2394626000ULL,s));
    uint64_t ns=0;assert(TscScaleApply(~uint64_t(0),s,ns) && ns>7000000000000000000ULL);
    TscScale left{0x80000000u,4};assert(!TscScaleApply(~uint64_t(0),left,ns));
    assert(TscScaleApply(~uint64_t(0)>>4,left,ns));
    TscScale bad{0,0};assert(!TscScaleApply(1,bad,ns));
    // Reads behind the origin clamp; saturation instead of wrapping.
    assert(TscClockValue(5,10,1234,s)==1234 && TscClockValue(10,10,1234,s)==1234);
    assert(TscClockValue(~uint64_t(0),0,~uint64_t(0)-5,s)==~uint64_t(0));
    // Ticks-to-ns helper.
    assert(TscTicksToNs(2394626,2394626)==1000000);assert(TscTicksToNs(1173,2394598)==489);
    assert(TscTicksToNs(~uint64_t(0),1)==~uint64_t(0));assert(TscTicksToNs(5,0)==~uint64_t(0));
}

// Per-CPU TSC offsets within the admitted bound: every CPU's raw clock value
// may trail another's, but published (floored) values never go backwards,
// neither per thread nor across threads ordered through memory.
void CrossCpuMonotonic(){
    TscScale s{};assert(TscScaleFromHz(2394626000ULL,s));
    const int64_t offsets[4]={0,-1173,850,-40};      // ticks; ~0.5 us
    volatile uint64_t floor=5000000;                 // origin ns
    std::atomic<uint64_t> tsc{1000000};std::atomic<uint64_t> handoff{0};std::atomic<bool> bad{false};
    auto worker=[&](unsigned cpu){
        uint64_t last=0;
        for(unsigned i=0;i<200000;++i){
            const uint64_t t=tsc.fetch_add(3,std::memory_order_relaxed);
            const uint64_t local=uint64_t(int64_t(t)+offsets[cpu]);
            const uint64_t value=TickClockPublishFloor(&floor,TscClockValue(local,1000000,5000000,s));
            if(value<last)bad=true;last=value;
            // A value another thread published before we read must not exceed ours.
            const uint64_t seen=handoff.load(std::memory_order_acquire);
            const uint64_t after=TickClockPublishFloor(&floor,TscClockValue(uint64_t(int64_t(tsc.load())+offsets[cpu]),1000000,5000000,s));
            if(after<seen)bad=true;
            uint64_t cur=handoff.load();while(cur<after && !handoff.compare_exchange_weak(cur,after)){}
        }
    };
    std::vector<std::thread> threads;for(unsigned c=0;c<4;++c)threads.emplace_back(worker,c);
    for(auto& t:threads)t.join();
    assert(!bad);
    // Without the floor the same offsets would step back: prove the test bites.
    const uint64_t ahead=TscClockValue(2000000+850,1000000,5000000,s);
    const uint64_t behind=TscClockValue(2000000+10-1173,1000000,5000000,s);
    assert(behind<ahead);
    volatile uint64_t f2=0;TickClockPublishFloor(&f2,ahead);assert(TickClockPublishFloor(&f2,behind)==ahead);
}

NativeTscCpuObservation Core(uint32_t cpu,int64_t lo,int64_t hi,NativeTscExchangeResult r=NativeTscExchangeResult::Complete){
    NativeTscCpuObservation c{};c.cpu=cpu;c.exchanges=r==NativeTscExchangeResult::Complete?5:0;c.result=r;
    c.interval.lower=lo;c.interval.upper=hi;c.interval.roundTrip=uint64_t(hi-lo);return c;
}
ClockSelectionInput Good(){
    ClockSelectionInput in{};in.cpu.tsc=in.cpu.invariant=in.cpu.rdtscp=true;
    in.khz=2394626;const uint32_t samples[5]={2394598,2394626,2394700,2394610,2394650};
    for(unsigned i=0;i<5;++i)in.samples[i]=samples[i];
    // The physical Dell's recorded AP-minus-BSP intervals.
    static NativeTscCpuObservation cores[3]={Core(1,-198,1173),Core(2,-141,1119),Core(3,-204,1194)};
    in.sync=TscSyncSummarize(cores,3,3);in.syncMeasured=true;return in;
}

void Selection(){
    auto sel=ClockSelect(Good());
    assert(sel.kind==ClockSourceKind::Tsc && sel.vdso && sel.scale.mul && sel.syncMaxNs==498);
    assert(!std::string_view(ClockSourceName(sel.kind)).compare("tsc"));
    // kvmclock and VMware keep priority.
    {auto in=Good();in.kvmclock=true;auto s=ClockSelect(in);assert(s.kind==ClockSourceKind::Kvmclock && !s.scale.mul);
     ClockSelection pre{};assert(!ClockSelectWantsSync(in,pre));}
    {auto in=Good();in.vmwareTsc=true;assert(ClockSelect(in).kind==ClockSourceKind::VmwareTsc);}
    // CPU features.
    {auto in=Good();in.cpu.invariant=false;auto s=ClockSelect(in);assert(s.kind==ClockSourceKind::ApicTicks && !std::string_view(s.reason).compare("no-invariant-tsc"));}
    {auto in=Good();in.cpu.tsc=false;assert(ClockSelect(in).kind==ClockSourceKind::ApicTicks);}
    {auto in=Good();in.cpu.rdtscp=false;auto s=ClockSelect(in);assert(s.kind==ClockSourceKind::Tsc && !s.vdso);}
    {auto in=Good();in.cpu.hypervisor=true;auto s=ClockSelect(in);assert(s.kind==ClockSourceKind::Tsc &&
        !std::string_view(s.reason).compare("invariant-tsc-under-hypervisor"));}
    // Calibration.
    {auto in=Good();in.khz=0;assert(ClockSelect(in).kind==ClockSourceKind::ApicTicks);}
    {auto in=Good();in.samples[3]=0;assert(ClockSelect(in).kind==ClockSourceKind::ApicTicks);}
    {auto in=Good();in.samples[4]=2394626+2394626/100;auto s=ClockSelect(in);
     assert(s.kind==ClockSourceKind::ApicTicks && !std::string_view(s.reason).compare("calibration-inconsistent"));}
    {auto in=Good();in.samples[4]=in.samples[0]+uint32_t(uint64_t(in.khz)*TscCalibrationSpreadPpm/1000000);
     assert(ClockSelect(in).kind==ClockSourceKind::Tsc);}
    {auto in=Good();in.khz=2394000;assert(ClockSelect(in).kind==ClockSourceKind::ApicTicks);}
    {auto in=Good();for(auto& s:in.samples)s=500;in.khz=500;assert(ClockSelect(in).kind==ClockSourceKind::ApicTicks);}
    // Synchronisation.
    {auto in=Good();in.syncMeasured=false;assert(ClockSelect(in).kind==ClockSourceKind::ApicTicks);}
    {NativeTscCpuObservation c[3]={Core(1,-198,1173),Core(2,0,0,NativeTscExchangeResult::Timeout),Core(3,-204,1194)};
     auto in=Good();in.sync=TscSyncSummarize(c,3,3);assert(!in.sync.complete && in.sync.failedCpu==2 &&
        in.sync.failure==NativeTscExchangeResult::Timeout);
     auto s=ClockSelect(in);assert(s.kind==ClockSourceKind::ApicTicks && !std::string_view(s.reason).compare("sync-exchange-failed"));}
    {auto in=Good();NativeTscCpuObservation c[2]={Core(1,-1,1),Core(2,-1,1)};in.sync=TscSyncSummarize(c,2,3);
     assert(!in.sync.complete);assert(ClockSelect(in).kind==ClockSourceKind::ApicTicks);}
    {NativeTscCpuObservation c[1]={Core(1,5,-5)};auto in=Good();in.sync=TscSyncSummarize(c,1,1);assert(!in.sync.complete);}
    // Offsets: between the vDSO and admission bounds the kernel clock is used
    // without the vDSO; beyond admission, fall back.
    {NativeTscCpuObservation c[2]={Core(1,-198,1173),Core(2,50,1200)};  // AP2 observed ahead: a warp
     auto in=Good();in.sync=TscSyncSummarize(c,2,2);assert(in.sync.complete && !in.sync.warpFree);
     auto s=ClockSelect(in);assert(s.kind==ClockSourceKind::Tsc && !s.vdso);}
    {NativeTscCpuObservation c[1]={Core(1,-1200,-3)};auto in=Good();in.sync=TscSyncSummarize(c,1,1);
     assert(!in.sync.warpFree && !ClockSelect(in).vdso);}
    {NativeTscCpuObservation c[1]={Core(1,-7000,7000)};auto in=Good();in.sync=TscSyncSummarize(c,1,1);  // wide VM round trip
     auto s=ClockSelect(in);assert(in.sync.warpFree && s.kind==ClockSourceKind::Tsc && s.vdso && s.syncMaxNs==2923);}
    {const int64_t t=-int64_t(TscSyncAdmitNs+1000)*2394626/1000000;NativeTscCpuObservation c[1]={Core(1,t,t+100)};
     auto in=Good();in.sync=TscSyncSummarize(c,1,1);auto s=ClockSelect(in);
     assert(s.kind==ClockSourceKind::ApicTicks && s.syncMaxNs>TscSyncAdmitNs && !std::string_view(s.reason).compare("sync-offset-too-large"));}
    // Single CPU: nothing to compare.
    {auto in=Good();in.sync=TscSyncSummarize(nullptr,0,0);assert(in.sync.complete && in.sync.warpFree);auto s=ClockSelect(in);
     assert(s.kind==ClockSourceKind::Tsc && s.syncMaxNs==0);}
}

void Features(){
    struct Cpu{bool hyp=false,inv=true,rdtscp=true;uint32_t maxExt=0x80000008u;
        void operator()(uint32_t leaf,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d)const{a=b=c=d=0;
            if(leaf==0)a=0xd;else if(leaf==1){d=1u<<4;c=hyp?1u<<31:0;}else if(leaf==0x80000000u)a=maxExt;
            else if(leaf==0x80000001u)d=rdtscp?1u<<27:0;else if(leaf==0x80000007u){assert(maxExt>=leaf);d=inv?1u<<8:0;}}};
    auto f=TscReadCpuFeatures(Cpu{});assert(f.tsc && f.invariant && f.rdtscp && !f.hypervisor);
    Cpu h;h.hyp=true;assert(TscReadCpuFeatures(h).hypervisor);
    Cpu old;old.maxExt=0x80000001u;f=TscReadCpuFeatures(old);assert(!f.invariant && f.rdtscp);
}
}

int main(){
    Conversion();CrossCpuMonotonic();Selection();Features();
    std::puts("test_tsc_clock: OK");return 0;
}
