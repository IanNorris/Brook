// Thermal/power sampler: one pinned kernel thread per CPU reads its core's
// DTS and APERF/MPERF every ThermalPeriodMs; the CPU 0 thread also reads the
// package DTS and RAPL energy and publishes a sample. Every MSR is gated on
// CPUID (bare metal, Intel family 6, the matching feature bits, and the
// Broadwell client models for RAPL/residency) so VMs never take a #GP.
#include "thermal_runtime.h"
#include "cpu.h"
#include "cpu_idle.h"
#include "shutdown.h"
#include "smp.h"
#include "apic.h"
#include "process.h"
#include "scheduler.h"
#include "spinlock.h"
#include "kprintf.h"
#include "klog.h"
#include "tsc_clock.h"
#include "intel_bdw_worker_stats.h"

namespace brook {
namespace {

struct Platform {
    bool decided = false, dts = false, pkgDts = false, aperf = false, rapl = false, epb = false, hwp = false, c8910 = false;
    uint32_t model = 0, tjmax = 0, baseMhz = 0;
};
inline void Cpuid(uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(0u));
}
inline uint64_t Rdtsc() { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (uint64_t(hi) << 32) | lo; }

Platform g_platform;
const Platform& Plat() {
    if (g_platform.decided) return g_platform;
    Platform p{};
    uint32_t a = 0, b = 0, c = 0, d = 0;
    Cpuid(0, a, b, c, d);
    const uint32_t maxLeaf = a;
    const bool intel = b == 0x756e6547u && d == 0x49656e69u && c == 0x6c65746eu;
    if (intel && maxLeaf >= 6) {
        Cpuid(1, a, b, c, d);
        const bool hypervisor = (c & (1u << 31)) != 0;
        const uint32_t family = (a >> 8) & 0xf;
        p.model = ((a >> 4) & 0xf) | (((a >> 16) & 0xf) << 4);
        uint32_t a6 = 0, b6 = 0, c6 = 0, d6 = 0;
        Cpuid(6, a6, b6, c6, d6);
        const bool bare = !hypervisor && family == 6;
        p.dts = bare && (a6 & 1u);
        p.pkgDts = bare && (a6 & (1u << 6));
        p.aperf = bare && (c6 & 1u);
        p.epb = bare && (c6 & (1u << 3));
        p.hwp = bare && (a6 & (1u << 7));
        // RAPL, PLATFORM_INFO and C-state residency MSRs: Broadwell client
        // (0x3d U/Y, 0x47 H). C8-C10 package residency: 0x3d only (ULT).
        p.rapl = bare && (p.model == 0x3d || p.model == 0x47);
        p.c8910 = bare && p.model == 0x3d;
        if (p.dts) p.tjmax = ThermalDecodeTjMax(ReadMsr(0x1a2));
        if (p.rapl) p.baseMhz = uint32_t((ReadMsr(0xce) >> 8) & 0xff) * 100;
    }
    p.decided = true;
    g_platform = p;
    return g_platform;
}

uint64_t g_cpuSlot[ThermalMaxCpus];   // ThermalPackCpu, written by the CPU's own thread
IrqSpinLock g_lock;
ThermalRing<ThermalHistory> g_history;
ThermalRing<ThermalGraphHistory> g_graph;
ThermalGraphAccumulator g_accumulator;
ThermalMaxima g_maxima;
uint64_t g_samples = 0, g_misplaced = 0;

struct CpuPrev { uint64_t aperf = 0, mperf = 0, tsc = 0; bool primed = false; };
struct PkgPrev { uint32_t pkg = 0, pp0 = 0, pp1 = 0; uint64_t tsc = 0, unit = 0; bool primed = false; uint64_t gpuSeq = 0, gpuTick = 0; };

void SampleCpu(uint32_t cpu, CpuPrev& prev) {
    const Platform& p = Plat();
    ThermalCpu out{};
    out.valid = true;
    if (p.dts) out.tempC = int16_t(ThermalDecodeDts(ReadMsr(0x19c), p.tjmax));
    out.ratio = uint8_t((ReadMsr(0x198) >> 8) & 0xff);
    if (p.aperf) {
        const uint64_t mperf = ReadMsr(0xe7), aperf = ReadMsr(0xe8), tsc = Rdtsc();
        if (prev.primed) {
            out.mhz = uint16_t(ThermalMhz(aperf - prev.aperf, mperf - prev.mperf, p.baseMhz));
            out.c0Percent = uint8_t(ThermalC0Percent(mperf - prev.mperf, tsc - prev.tsc));
        }
        prev = {aperf, mperf, tsc, true};
    }
    __atomic_store_n(&g_cpuSlot[cpu], ThermalPackCpu(out), __ATOMIC_RELEASE);
}

void Publish(PkgPrev& prev) {
    const Platform& p = Plat();
    ThermalSample s{};
    s.tick = ApicTickCount();
    if (p.pkgDts) s.pkgC = int16_t(ThermalDecodeDts(ReadMsr(0x1b1), p.tjmax));
    const uint32_t n = SmpGetCpuCount() < ThermalMaxCpus ? SmpGetCpuCount() : ThermalMaxCpus;
    ThermalCpu cpus[ThermalMaxCpus]{};
    for (uint32_t i = 0; i < n; ++i) cpus[i] = ThermalUnpackCpu(__atomic_load_n(&g_cpuSlot[i], __ATOMIC_ACQUIRE));
    ThermalSummariseCpus(cpus, n, s);
    bool powerValid = false;
    if (p.rapl) {
        const uint64_t unit = ReadMsr(0x606), tsc = Rdtsc();
        const uint32_t pkg = uint32_t(ReadMsr(0x611)), pp0 = uint32_t(ReadMsr(0x639)), pp1 = uint32_t(ReadMsr(0x641));
        const uint32_t khz = TscClockReadStatus().khz;
        if (prev.primed && khz && tsc > prev.tsc) {
            powerValid = true;
            const uint64_t us = (tsc - prev.tsc) * 1000 / khz;
            s.pkgMw = ThermalEnergyMw(prev.pkg, pkg, unit, us);
            s.coreMw = ThermalEnergyMw(prev.pp0, pp0, unit, us);
            s.gtMw = ThermalEnergyMw(prev.pp1, pp1, unit, us);
        }
        prev.pkg = pkg; prev.pp0 = pp0; prev.pp1 = pp1; prev.tsc = tsc; prev.unit = unit; prev.primed = true;
    }
    if (CpuIdleBudgetActive()) {
        CpuBudgetSample b{};
        b.powerValid = powerValid;
        b.coresMw = s.coreMw;
        b.tempValid = p.pkgDts && s.pkgC > 0;
        b.pkgC = s.pkgC;
        CpuIdleBudgetControl(s.tick, b);
    }
    // Last CAGF a render job sampled, if a job ran in the last 2 s.
    const uint64_t gpuSeq = __atomic_load_n(&BdwGpuMhzSequence(), __ATOMIC_ACQUIRE);
    if (gpuSeq != prev.gpuSeq) { prev.gpuSeq = gpuSeq; prev.gpuTick = s.tick; }
    if (prev.gpuTick && s.tick - prev.gpuTick < 2000)
        s.gpuMhz = uint16_t(__atomic_load_n(&BdwLastGpuMhz(), __ATOMIC_RELAXED));
    const uint64_t flags = IrqSpinLockAcquire(&g_lock);
    g_history.Push(s);
    g_maxima.Note(s);
    ThermalSample point{};
    if (g_accumulator.Add(s, point)) g_graph.Push(point);
    ++g_samples;
    IrqSpinLockRelease(&g_lock, flags);
}

bool EpbTrialEligible()
{
    const auto& p=Plat();
    return p.model==0x3d && p.epb && !p.hwp && p.pkgDts &&
           ShutdownLaunchAllowed() && ShutdownGetAcStatus().source==AcPowerSource::Online &&
           ThermalDecodeDts(ReadMsr(0x1b1),p.tjmax)>0;
}

void Worker(void* arg) {
    const uint32_t cpu = uint32_t(reinterpret_cast<uintptr_t>(arg));
    CpuPrev prev{};
    PkgPrev pkg{};
    for (;;) {
        SchedulerSleepMs(ThermalPeriodMs);
        // Per-core MSRs must be read on their own CPU; skip if the scheduler
        // ever ran this pinned thread elsewhere.
        if (SmpCurrentCpuIndex() != cpu) { __atomic_add_fetch(&g_misplaced, 1, __ATOMIC_RELAXED); continue; }
        SampleCpu(cpu, prev);
        if (CpuIdleEpbTrialActive()) CpuIdleTickEpbTrial(cpu,ApicTickCount(),EpbTrialEligible());
        if (CpuIdleRatioTrialActive()) CpuIdleTickRatioTrial(cpu,ApicTickCount(),EpbTrialEligible());
        if (CpuIdleBudgetActive()) CpuIdleTickBudget(cpu,ApicTickCount(),EpbTrialEligible());
        if (cpu == 0) Publish(pkg);
    }
}

} // namespace

void ThermalSamplerStart() {
    const Platform& p = Plat();
    if (!p.dts) { KLog("thermal: sampler unavailable (no bare-metal digital thermal sensor)"); return; }
    const uint32_t n = SmpGetCpuCount() < ThermalMaxCpus ? SmpGetCpuCount() : ThermalMaxCpus;
    for (uint32_t cpu = 0; cpu < n; ++cpu) {
        Process* t = KernelThreadCreate("thermal", Worker, reinterpret_cast<void*>(uintptr_t(cpu)), 3);
        if (!t) { KLog("thermal: could not start sampler on cpu %u", cpu); continue; }
        t->cpuAffinity = int32_t(cpu);
        SchedulerAddProcess(t);
    }
    KLog("thermal: sampler on %u cpus, TjMax %u C, base %u MHz, rapl %d", n, p.tjmax, p.baseMhz, p.rapl ? 1 : 0);
}

int ThermalStartEpbTrial(unsigned bias,unsigned seconds)
{
    if (bias>15 || !seconds || seconds>300) return -22;
    if (!EpbTrialEligible()) return -95;
    return CpuIdleStartEpbTrial(ApicTickCount(),SmpGetCpuCount(),bias,seconds*1000);
}

int ThermalStartRatioTrial(unsigned ratio,unsigned seconds)
{
    if (!seconds || seconds>300) return -22;
    if (!EpbTrialEligible()) return -95;
    // Cap only: between the minimum efficiency ratio and the maximum
    // non-turbo ratio from MSR_PLATFORM_INFO. Never request turbo.
    const uint64_t info=ReadMsr(0xce);
    const unsigned maxNonTurbo=unsigned((info>>8)&0xff),minRatio=unsigned((info>>40)&0xff);
    if (!maxNonTurbo || !minRatio || ratio<minRatio || ratio>maxNonTurbo) return -22;
    return CpuIdleStartRatioTrial(ApicTickCount(),SmpGetCpuCount(),ratio,seconds*1000);
}

int ThermalSetCpuBudget(uint32_t coresMw,int32_t tempC)
{
    if (!coresMw && !tempC) { CpuIdleCancelBudget(); return 0; }
    const CpuBudgetConfig config{coresMw,tempC};
    if (!CpuPowerBudget::Valid(config)) return -22;
    if (!EpbTrialEligible()) return -95;
    // Cap range: minimum efficiency ratio .. max non-turbo - 1 (a request at
    // max non-turbo still allows turbo). Release returns to the firmware value.
    const uint64_t info=ReadMsr(0xce);
    const unsigned maxNonTurbo=unsigned((info>>8)&0xff),minRatio=unsigned((info>>40)&0xff);
    if (!minRatio || maxNonTurbo<=minRatio) return -95;
    return CpuIdleSetBudget(ApicTickCount(),SmpGetCpuCount(),config,minRatio,maxNonTurbo);
}

bool ThermalReadSnapshot(ThermalSnapshot& out) {
    const Platform& p = Plat();
    out = {};
    out.available = p.dts;
    out.rapl = p.rapl;
    out.aperf = p.aperf;
    out.tjmax = p.tjmax;
    out.baseMhz = p.baseMhz;
    out.cpuCount = SmpGetCpuCount() < ThermalMaxCpus ? SmpGetCpuCount() : ThermalMaxCpus;
    if (!p.dts) return false;
    for (uint32_t i = 0; i < out.cpuCount; ++i) out.cpus[i] = ThermalUnpackCpu(__atomic_load_n(&g_cpuSlot[i], __ATOMIC_ACQUIRE));
    const uint64_t flags = IrqSpinLockAcquire(&g_lock);
    if (const ThermalSample* l = g_history.Latest()) out.latest = *l;
    out.maxima = g_maxima;
    out.samples = g_samples;
    IrqSpinLockRelease(&g_lock, flags);
    out.misplaced = __atomic_load_n(&g_misplaced, __ATOMIC_RELAXED);
    return true;
}

template <uint32_t N>
static uint32_t CopyRing(const ThermalRing<N>& ring, ThermalSample* out, uint32_t capacity) {
    if (!out || !capacity) return 0;
    const uint64_t flags = IrqSpinLockAcquire(&g_lock);
    const uint32_t n = ring.count < capacity ? ring.count : capacity;
    const uint32_t skip = ring.count - n; // keep the newest n
    for (uint32_t i = 0; i < n; ++i) out[i] = ring.At(skip + i);
    IrqSpinLockRelease(&g_lock, flags);
    return n;
}
uint32_t ThermalReadHistory(ThermalSample* out, uint32_t capacity) { return CopyRing(g_history, out, capacity); }
uint32_t ThermalReadGraph(ThermalSample* out, uint32_t capacity) { return CopyRing(g_graph, out, capacity); }

void ThermalResetMaxima() {
    const uint64_t flags = IrqSpinLockAcquire(&g_lock);
    g_maxima = {};
    IrqSpinLockRelease(&g_lock, flags);
}

ThermalPowerRegs ThermalReadPowerRegs() {
    const Platform& p = Plat();
    ThermalPowerRegs r{};
    r.model = p.model;
    r.cpu = SmpCurrentCpuIndex();
    r.hwpSupported = p.hwp;
    r.epbSupported = p.epb;
    r.c8910 = p.c8910;
    if (!p.rapl) return r;
    r.valid = true;
    r.miscEnable = ReadMsr(0x1a0);
    r.perfCtl = ReadMsr(0x199);
    r.perfStatus = ReadMsr(0x198);
    if (p.epb) r.epb = ReadMsr(0x1b0);
    r.platformInfo = ReadMsr(0xce);
    r.turboRatioLimit = ReadMsr(0x1ad);
    r.pkgCstConfig = ReadMsr(0xe2);
    r.raplPowerUnit = ReadMsr(0x606);
    r.pkgPowerLimit = ReadMsr(0x610);
    r.pkgPowerInfo = ReadMsr(0x614);
    uint32_t a = 0, b = 0, c = 0, d = 0;
    Cpuid(0, a, b, c, d);
    if (a >= 5) { Cpuid(5, a, b, c, d); r.mwaitLeaf5Edx = d; }
    r.pkgC2 = ReadMsr(0x60d);
    r.pkgC3 = ReadMsr(0x3f8);
    r.pkgC6 = ReadMsr(0x3f9);
    r.pkgC7 = ReadMsr(0x3fa);
    if (p.c8910) { r.pkgC8 = ReadMsr(0x630); r.pkgC9 = ReadMsr(0x631); r.pkgC10 = ReadMsr(0x632); }
    r.coreC3 = ReadMsr(0x3fc);
    r.coreC6 = ReadMsr(0x3fd);
    r.coreC7 = ReadMsr(0x3fe);
    r.tsc = Rdtsc();
    return r;
}

} // namespace brook
