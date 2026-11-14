// tsc_clock.cpp — invariant-TSC elapsed clock for bare metal (Dell XPS 13).
//
// Without kvmclock, the reported time used to be the count of delivered LAPIC
// timer interrupts. IRQs-off sections (the Intel GPU path) lose ticks, and on
// the Dell CLOCK_MONOTONIC ran ~11.5% slow under GPU load. The invariant TSC
// runs at a constant rate in every P/C-state, so once its rate is calibrated
// and the CPUs are shown to agree at boot it is a far better elapsed clock.
//
// Calibration: the median of the five 25 ms PIT channel-2 windows taken during
// LAPIC timer calibration (apic.cpp). Synchronisation: the existing bootstrap
// rendezvous (SmpObserveNativeTsc) bounds each AP's TSC offset from the BSP.

#include "tsc_clock.h"
#include "apic.h"
#include "cpu.h"
#include "kvmclock.h"
#include "serial.h"
#include "smp.h"
#include "tick_clock.h"
#include "vdso.h"

namespace brook {
namespace {
constexpr uint32_t MSR_TSC_AUX=0xc0000103u;
bool g_decided=false;
bool g_enabled=false;            // published with release after the fields below
uint64_t g_originTsc=0,g_originNs=0;
TscScale g_scale{};
ClockSelection g_selection{};
ClockSelectionInput g_input{};
uint32_t g_cpus=0,g_syncCpus=0;
alignas(64) volatile uint64_t g_lastNs=0;
alignas(64) uint64_t g_floorHits=0;

inline uint64_t ReadTsc(){
    uint32_t lo,hi;
    __asm__ volatile("lfence; rdtsc; lfence":"=a"(lo),"=d"(hi)::"memory");
    return (uint64_t(hi)<<32)|lo;
}
const char* ExchangeName(NativeTscExchangeResult r){
    switch(r){case NativeTscExchangeResult::Complete:return "complete";case NativeTscExchangeResult::Unavailable:return "unavailable";
    case NativeTscExchangeResult::Timeout:return "timeout";case NativeTscExchangeResult::Abandoned:return "abandoned";
    default:return "invalid";}
}
NativeTscCpuObservation g_cores[63]{};
}

void TscClockSelect(){
    if(g_decided)return;
    g_decided=true;
    auto& in=g_input;
    in.kvmclock=KvmClockAvailable();
    in.vmwareTsc=ApicGetClockSnapshot().vmwareTsc;
    in.cpu=TscReadCpuFeatures([](uint32_t leaf,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d){
        __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(leaf),"c"(0));});
    in.khz=ApicGetTscKhz();ApicGetTscKhzSamples(in.samples);
    g_cpus=SmpGetCpuCount();
    ClockSelection pre{};
    if(ClockSelectWantsSync(in,pre)){
        // APs are still spinning in their bootstrap wait and answer the
        // rendezvous; a single-CPU system has nothing to compare.
        const uint32_t expected=g_cpus?g_cpus-1:0;
        g_syncCpus=expected?SmpObserveNativeTsc(g_cores,63):0;
        in.sync=TscSyncSummarize(g_cores,g_syncCpus,expected);
        in.syncMeasured=true;
        for(uint32_t i=0;i<g_syncCpus;++i)
            SerialPrintf("CLOCK_SYNC cpu=%u result=%s exchanges=%u ap_minus_bsp_ticks=[%ld,%ld] round_trip=%lu\n",
                g_cores[i].cpu,ExchangeName(g_cores[i].result),g_cores[i].exchanges,g_cores[i].interval.lower,
                g_cores[i].interval.upper,g_cores[i].interval.roundTrip);
    }
    g_selection=ClockSelect(in);
    if(g_selection.kind==ClockSourceKind::Tsc){
        uint64_t flags;
        __asm__ volatile("pushfq; pop %0; cli":"=r"(flags)::"memory");
        // Continue the delivered-tick millisecond domain: no consumer that
        // already read ApicTickCount()/CLOCK_MONOTONIC can observe a step back.
        g_originNs=ApicTickCount()*1000000ULL;
        g_originTsc=ReadTsc();
        g_scale=g_selection.scale;
        __atomic_store_n(&g_lastNs,g_originNs,__ATOMIC_RELAXED);
        if(in.cpu.rdtscp)WriteMsr(MSR_TSC_AUX,0);
        __atomic_store_n(&g_enabled,true,__ATOMIC_RELEASE);
        ApicSelectTscElapsedClock();
        if(flags&0x200)__asm__ volatile("sti":::"memory");
        const bool vdso=g_selection.vdso &&
            VdsoPublishTscClock(g_originTsc,g_originNs,g_scale.mul,g_scale.shift);
        g_selection.vdso=vdso;
        SerialPrintf("CLOCK_SOURCE tsc khz=%u sync_max_ns=%lu warp_free=%u cpus=%u vdso=%u hypervisor=%u mul=%u shift=%d\n",
            in.khz,g_selection.syncMaxNs,in.sync.warpFree?1u:0u,g_cpus,vdso?1u:0u,in.cpu.hypervisor?1u:0u,
            g_scale.mul,int(g_scale.shift));
        return;
    }
    if(in.syncMeasured && !in.sync.complete)
        SerialPrintf("CLOCK_SOURCE %s reason=%s cpu=%u result=%s measured=%u/%u khz=%u\n",ClockSourceName(g_selection.kind),
            g_selection.reason,in.sync.failedCpu,ExchangeName(in.sync.failure),g_syncCpus,g_cpus?g_cpus-1:0,in.khz);
    else if(g_selection.syncMaxNs)
        SerialPrintf("CLOCK_SOURCE %s reason=%s sync_max_ns=%lu khz=%u\n",ClockSourceName(g_selection.kind),
            g_selection.reason,g_selection.syncMaxNs,in.khz);
    else
        SerialPrintf("CLOCK_SOURCE %s reason=%s khz=%u invariant=%u hypervisor=%u\n",ClockSourceName(g_selection.kind),
            g_selection.reason,in.khz,in.cpu.invariant?1u:0u,in.cpu.hypervisor?1u:0u);
}

void TscClockInitOnCpu(uint32_t cpu){
    if(TscClockAvailable() && g_input.cpu.rdtscp)WriteMsr(MSR_TSC_AUX,cpu);
}

bool TscClockAvailable(){return __atomic_load_n(&g_enabled,__ATOMIC_ACQUIRE);}

uint64_t TscClockReadNs(){
    if(!TscClockAvailable())return 0;
    const uint64_t ns=TscClockValue(ReadTsc(),g_originTsc,g_originNs,g_scale);
    const uint64_t published=TickClockPublishFloor(&g_lastNs,ns);
    if(published!=ns)__atomic_add_fetch(&g_floorHits,1,__ATOMIC_RELAXED);
    return published;
}

TscClockStatus TscClockReadStatus(){
    TscClockStatus s{};
    s.kind=g_selection.kind;s.reason=g_selection.reason;s.decided=g_decided;
    s.hypervisor=g_input.cpu.hypervisor;s.invariant=g_input.cpu.invariant;s.rdtscp=g_input.cpu.rdtscp;
    s.vdso=g_selection.vdso;s.khz=g_input.khz;s.cpus=g_cpus;s.syncCpus=g_syncCpus;
    s.syncFailedCpu=g_input.sync.failedCpu;s.warpFree=g_input.sync.warpFree;s.syncMaxNs=g_selection.syncMaxNs;s.syncMaxTicks=g_input.sync.maxTicks;
    s.originTsc=g_originTsc;s.originNs=g_originNs;s.scale=g_scale;
    s.floorHits=__atomic_load_n(&g_floorHits,__ATOMIC_RELAXED);
    return s;
}
}
