// Idle C-states and energy-performance bias. See cpu_idle.h.
//
// Wake-source argument: Brook wakes an idle CPU only by interrupt (LAPIC
// timer, reschedule/TLB IPIs, device MSIs); nothing relies on a store to wake
// a halted CPU. MWAIT with IF=1 exits on any interrupt exactly as HLT does,
// and additionally on a write to the monitored line, so it is never less
// responsive than HLT. The sequence `cli; monitor; sti; mwait` keeps the
// interrupt shadow of STI over MWAIT, so an interrupt arriving after STI is
// taken after MWAIT starts and ends it (the same property `sti; hlt` relies
// on). States that stop the LAPIC timer (C3/C6) are refused without ARAT.
// The invariant TSC keeps counting in every C-state used here.
#include "cpu_idle.h"
#include "cpu.h"
#include "smp.h"
#include "spinlock.h"
#include "tsc_clock.h"

namespace brook {
namespace {

struct alignas(64) MonitorLine { volatile uint64_t word; uint8_t pad[56]; };

struct Platform {
    bool decided = false, bareIntel = false, residency = false, epb = false;
    int32_t epbFirmware = -1;
    CpuIdleCaps caps;
};

inline void Cpuid(uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(0u));
}
inline uint64_t Rdtsc() { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (uint64_t(hi) << 32) | lo; }

Platform g_platform;
MonitorLine g_monitor[CpuIdleMaxCpus];
CpuIdlePerCpu g_cpu[CpuIdleMaxCpus];

// Published choice: hint (-1 = HLT) and ECX packed so idle loops read one word.
volatile int64_t g_choice = -1;            // (ecx << 32) | (uint32_t)hint, or -1
volatile uint32_t g_requested = 0;          // CpuIdleState
volatile uint64_t g_epbGen = 0;
volatile uint32_t g_epbValue = 0;
IrqSpinLock g_epbLock;
CpuEpbTrial g_epbTrial;
bool g_epbTrialActive=false;
// IA32_PERF_CTL target-ratio cap trial (bits 15:8). Brook never writes
// PERF_CTL otherwise, so the firmware request is the baseline to restore.
IrqSpinLock g_ratioLock;
CpuEpbTrial g_ratioTrial;
bool g_ratioTrialActive=false;
// The power budget is the other PERF_CTL owner, under the same lock.
CpuPowerBudget g_budget;
bool g_budgetActive=false;
unsigned ReadRatio() { return unsigned((ReadMsr(0x199)>>8)&0xffu); }
unsigned WriteRatio(unsigned value) {
    WriteMsr(0x199,(ReadMsr(0x199)&~0xff00ull)|(uint64_t(value&0xffu)<<8));
    return ReadRatio();
}
volatile int32_t g_epbTarget = -1;
volatile uint64_t g_sampleGen = 0;
volatile bool g_auto = false;
volatile bool g_bootDefaultDone = false;
volatile int64_t g_autoChoice[uint32_t(CpuIdleState::Count)] = {-1, -1, -1, -1, -1};
volatile uint32_t g_autoDeepest = 0;
volatile uint32_t g_khz = 0;
bool g_epbBootApplied = false;
uint64_t g_prevTsc = 0, g_prevPkg[4] = {};

int64_t Pack(const CpuIdleChoice& c) { return c.hint < 0 ? -1 : int64_t((uint64_t(c.ecx) << 32) | uint32_t(c.hint)); }
uint32_t Khz() {
    uint32_t k = __atomic_load_n(&g_khz, __ATOMIC_RELAXED);
    if (!k) { k = TscClockReadStatus().khz; if (k) __atomic_store_n(&g_khz, k, __ATOMIC_RELAXED); }
    return k;
}

const Platform& Plat() {
    if (__atomic_load_n(&g_platform.decided, __ATOMIC_ACQUIRE)) return g_platform;
    Platform p{};
    uint32_t a = 0, b = 0, c = 0, d = 0;
    Cpuid(0, a, b, c, d);
    const uint32_t maxLeaf = a;
    const bool intel = b == 0x756e6547u && d == 0x49656e69u && c == 0x6c65746eu;
    if (intel && maxLeaf >= 1) {
        Cpuid(1, a, b, c, d);
        const bool hypervisor = (c & (1u << 31)) != 0;
        const uint32_t family = (a >> 8) & 0xf;
        const uint32_t model = ((a >> 4) & 0xf) | (((a >> 16) & 0xf) << 4);
        p.bareIntel = !hypervisor && family == 6;
        p.caps.mwait = p.bareIntel && (c & (1u << 3));
        if (p.caps.mwait && maxLeaf >= 5) {
            uint32_t a5 = 0, b5 = 0, c5 = 0, d5 = 0;
            Cpuid(5, a5, b5, c5, d5);
            p.caps.leaf5 = true;
            p.caps.extensions = (c5 & 1u) != 0;
            p.caps.interruptBreak = p.caps.extensions && (c5 & 2u) != 0;
            p.caps.leaf5Edx = d5;
        }
        if (p.bareIntel && maxLeaf >= 6) {
            uint32_t a6 = 0, b6 = 0, c6 = 0, d6 = 0;
            Cpuid(6, a6, b6, c6, d6);
            p.caps.arat = (a6 & (1u << 2)) != 0;
            p.epb = (c6 & (1u << 3)) != 0;
        }
        // Core/package residency MSRs: Broadwell client (0x3d U/Y, 0x47 H).
        p.residency = p.bareIntel && (model == 0x3d || model == 0x47);
        if (p.bareIntel && p.epb) {
            p.epbFirmware = int32_t(ReadMsr(0x1b0) & 0xfu);
            // Linux behaviour: firmware 0 (performance) or 15 becomes 6 (normal).
            const int32_t target = CpuEpbBootTarget(uint64_t(p.epbFirmware));
            if (target >= 0) {
                g_epbValue = uint32_t(target); g_epbTarget = target; g_epbGen = 1; g_epbBootApplied = true;
            }
        }
    }
    g_platform = p;
    __atomic_store_n(&g_platform.decided, true, __ATOMIC_RELEASE);
    return g_platform;
}

} // namespace

void CpuIdleEnter(uint32_t cpu) {
    const auto& plat = Plat();
    // Boot default: the governor (gen 396 A/B: -22% idle package power, no
    // yquake frame-time penalty). Selected once, by CPU 0's first idle entry;
    // `cpu idle state hlt` (or any explicit state) overrides it. VMs and parts
    // without usable MWAIT states stay on HLT (CpuIdleRequestAuto declines).
    if (cpu == 0 && !__atomic_load_n(&g_bootDefaultDone, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_bootDefaultDone, true, __ATOMIC_RELEASE);
        CpuIdleRequestAuto();
    }
    CpuIdlePerCpu* pc = cpu < CpuIdleMaxCpus ? &g_cpu[cpu] : nullptr;
    if (pc) {
        pc->seen = true;
        const uint64_t eg = __atomic_load_n(&g_epbGen, __ATOMIC_ACQUIRE);
        if (plat.epb && eg != __atomic_load_n(&pc->epbGen,__ATOMIC_RELAXED)) {
            const auto flags=IrqSpinLockAcquire(&g_epbLock);
            const auto current=__atomic_load_n(&g_epbGen,__ATOMIC_RELAXED);
            if (g_epbTrial.phase==CpuEpbTrial::Phase::Off && current!=pc->epbGen) {
                WriteMsr(0x1b0, (ReadMsr(0x1b0) & ~0xfull) | (g_epbValue & 0xfu));
                pc->epb=ReadMsr(0x1b0);
                __atomic_store_n(&pc->epbGen,current,__ATOMIC_RELEASE);
            }
            IrqSpinLockRelease(&g_epbLock,flags);
        }
        const uint64_t sg = __atomic_load_n(&g_sampleGen, __ATOMIC_ACQUIRE);
        if (sg != pc->sampleGen) {
            if (plat.residency) {
                pc->coreC3 = ReadMsr(0x3fc); pc->coreC6 = ReadMsr(0x3fd); pc->coreC7 = ReadMsr(0x3fe);
            }
            if (plat.epb) pc->epb = ReadMsr(0x1b0);
            pc->sampleTsc = Rdtsc();
            pc->sampleGen = sg;
        }
    }
    int64_t choice = __atomic_load_n(&g_choice, __ATOMIC_ACQUIRE);
    uint32_t state = __atomic_load_n(&g_requested, __ATOMIC_RELAXED);
    const bool govern = pc && __atomic_load_n(&g_auto, __ATOMIC_ACQUIRE);
    bool held = false;
    const uint64_t t0 = Rdtsc();
    if (govern) {
        held = t0 < pc->holdUntilTsc;
        const uint32_t predicted = CpuIdlePredictUs(pc->history);
        pc->lastPredictUs = predicted;
        uint32_t want = uint32_t(CpuIdleGovern(predicted, held));
        const uint32_t deepest = __atomic_load_n(&g_autoDeepest, __ATOMIC_RELAXED);
        if (want > deepest) want = deepest;
        // Step down to the deepest pre-validated choice at or below `want`.
        while (want > 0 && __atomic_load_n(&g_autoChoice[want], __ATOMIC_RELAXED) < 0) --want;
        state = want;
        choice = want ? __atomic_load_n(&g_autoChoice[want], __ATOMIC_RELAXED) : -1;
        pc->autoSelected[state]++;
        if (held) pc->holds++;
    }
    if (choice < 0 || !pc) {
        __asm__ volatile("sti\n\thlt" ::: "memory");
        if (pc) pc->entries[uint32_t(CpuIdleState::Hlt)]++;
        if (govern) state = uint32_t(CpuIdleState::Hlt);
    } else {
        const uint32_t hint = uint32_t(choice & 0xffffffffu);
        const uint32_t ecx = uint32_t(uint64_t(choice) >> 32);
        __asm__ volatile("cli" ::: "memory");
        __asm__ volatile("monitor" :: "a"(&g_monitor[cpu].word), "c"(0u), "d"(0u) : "memory");
        __asm__ volatile("sti\n\tmwait" :: "a"(hint), "c"(ecx) : "memory");
        pc->entries[state < uint32_t(CpuIdleState::Count) ? state : 0]++;
    }
    if (!pc) return;
    const uint64_t dt = Rdtsc() - t0;
    pc->idleTsc += dt;
    if (govern) {
        const uint32_t khz = Khz();
        const uint64_t us64 = khz ? dt * 1000 / khz : 0;
        const uint32_t us = us64 > 0xffffffffull ? 0xffffffffu : uint32_t(us64);
        CpuIdleHistoryPush(pc->history, us);
        const auto outcome = CpuIdleJudge(CpuIdleState(state), us, held,
                                          CpuIdleState(__atomic_load_n(&g_autoDeepest, __ATOMIC_RELAXED)));
        if (outcome == CpuIdleOutcome::Hit) pc->hits++;
        else if (outcome == CpuIdleOutcome::TooDeep) pc->tooDeep++;
        else pc->tooShallow++;
    }
}

CpuIdleChoice CpuIdleRequest(CpuIdleState state) {
    __atomic_store_n(&g_auto, false, __ATOMIC_RELEASE);
    const auto choice = CpuIdleSelect(Plat().caps, state);
    __atomic_store_n(&g_requested, uint32_t(choice.state), __ATOMIC_RELAXED);
    const int64_t packed = choice.hint < 0 ? -1 : int64_t((uint64_t(choice.ecx) << 32) | uint32_t(choice.hint));
    __atomic_store_n(&g_choice, packed, __ATOMIC_RELEASE);
    // Wake idle CPUs parked in MWAIT so they pick up the new hint promptly.
    for (uint32_t i = 0; i < CpuIdleMaxCpus; ++i) __atomic_add_fetch(&g_monitor[i].word, 1, __ATOMIC_RELEASE);
    return choice;
}

void CpuIdleRequestAuto() {
    const auto& caps = Plat().caps;
    uint32_t deepest = 0;
    for (uint32_t i = 1; i < uint32_t(CpuIdleState::Count); ++i) {
        const auto c = CpuIdleSelect(caps, CpuIdleState(i));
        // Only exact (unclamped) states are offered to the governor.
        const bool exact = c.hint >= 0 && uint32_t(c.state) == i;
        __atomic_store_n(&g_autoChoice[i], exact ? Pack(c) : int64_t(-1), __ATOMIC_RELAXED);
        if (exact) deepest = i;
    }
    // The governor never picks plain C1; C1E is its shallowest MWAIT state.
    __atomic_store_n(&g_autoDeepest, deepest, __ATOMIC_RELAXED);
    Khz();
    __atomic_store_n(&g_auto, deepest > 0, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < CpuIdleMaxCpus; ++i) __atomic_add_fetch(&g_monitor[i].word, 1, __ATOMIC_RELEASE);
}

void CpuIdleLatencyHint(uint32_t us) {
    if (!__atomic_load_n(&g_auto, __ATOMIC_RELAXED)) return;
    const uint32_t cpu = SmpCurrentCpuIndex();
    const uint32_t khz = Khz();
    if (cpu >= CpuIdleMaxCpus || !khz) return;
    const uint64_t until = Rdtsc() + uint64_t(us) * khz / 1000;
    if (until > g_cpu[cpu].holdUntilTsc) g_cpu[cpu].holdUntilTsc = until;
}

bool CpuIdleRequestEpb(uint32_t value) {
    if (!Plat().epb || value > 15) return false;
    const auto flags=IrqSpinLockAcquire(&g_epbLock);
    g_epbTrial.Cancel(CpuEpbTrial::Outcome::Superseded);
    const bool active=g_epbTrial.phase!=CpuEpbTrial::Phase::Off;
    __atomic_store_n(&g_epbTrialActive,active,__ATOMIC_RELEASE);
    if (!active) {
        __atomic_store_n(&g_epbValue,value,__ATOMIC_RELAXED);
        __atomic_store_n(&g_epbTarget,int32_t(value),__ATOMIC_RELAXED);
        __atomic_add_fetch(&g_epbGen,1,__ATOMIC_RELEASE);
    }
    IrqSpinLockRelease(&g_epbLock,flags);
    // A manual request ends the trial first. Require an explicit retry if
    // temporary values still need restoring on their owning CPUs.
    if (active) return false;
    for (uint32_t i = 0; i < CpuIdleMaxCpus; ++i) __atomic_add_fetch(&g_monitor[i].word, 1, __ATOMIC_RELEASE);
    return true;
}

int CpuIdleStartEpbTrial(uint64_t now,unsigned count,unsigned bias,uint32_t durationMs)
{
    if (!Plat().epb) return -95;
    if (!count || count>CpuEpbTrial::MaxCpus || bias>15 ||
        durationMs<1000 || durationMs>300000) return -22;

    const auto flags=IrqSpinLockAcquire(&g_epbLock);
    const auto generation=__atomic_load_n(&g_epbGen,__ATOMIC_RELAXED);
    bool unresolved=g_epbTrial.phase!=CpuEpbTrial::Phase::Off;
    for (unsigned i=0;i<count;++i)
        unresolved|=__atomic_load_n(&g_cpu[i].epbGen,__ATOMIC_ACQUIRE)!=generation;
    const bool started=!unresolved && g_epbTrial.Start(now,count,bias,durationMs);
    if (started) __atomic_store_n(&g_epbTrialActive,true,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_epbLock,flags);
    return started?0:-16;
}

bool CpuIdleEpbTrialActive()
{
    return __atomic_load_n(&g_epbTrialActive,__ATOMIC_ACQUIRE);
}

void CpuIdleTickEpbTrial(unsigned cpu,uint64_t now,bool eligible)
{
    const auto flags=IrqSpinLockAcquire(&g_epbLock);
    g_epbTrial.Tick(cpu,now,eligible,[](){return ReadMsr(0x1b0)&0xfu;},[](unsigned value){
        WriteMsr(0x1b0,(ReadMsr(0x1b0)&~0xfull)|value);
        return ReadMsr(0x1b0)&0xfu;
    });
    __atomic_store_n(&g_epbTrialActive,g_epbTrial.phase!=CpuEpbTrial::Phase::Off,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_epbLock,flags);
}

void CpuIdleCancelEpbTrial()
{
    const auto flags=IrqSpinLockAcquire(&g_epbLock);
    g_epbTrial.Cancel();
    __atomic_store_n(&g_epbTrialActive,g_epbTrial.phase!=CpuEpbTrial::Phase::Off,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_epbLock,flags);
}

int CpuIdleStartRatioTrial(uint64_t now,unsigned count,unsigned ratio,uint32_t durationMs)
{
    if (!count || count>CpuEpbTrial::MaxCpus || !ratio || ratio>255 ||
        durationMs<1000 || durationMs>300000) return -22;
    const auto flags=IrqSpinLockAcquire(&g_ratioLock);
    const bool started=g_budget.phase==CpuPowerBudget::Phase::Off &&
                       g_ratioTrial.Start(now,count,ratio,durationMs,255);
    if (started) __atomic_store_n(&g_ratioTrialActive,true,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_ratioLock,flags);
    return started?0:-16;
}

bool CpuIdleRatioTrialActive()
{
    return __atomic_load_n(&g_ratioTrialActive,__ATOMIC_ACQUIRE);
}

void CpuIdleTickRatioTrial(unsigned cpu,uint64_t now,bool eligible)
{
    const auto flags=IrqSpinLockAcquire(&g_ratioLock);
    g_ratioTrial.Tick(cpu,now,eligible,ReadRatio,WriteRatio);
    __atomic_store_n(&g_ratioTrialActive,g_ratioTrial.phase!=CpuEpbTrial::Phase::Off,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_ratioLock,flags);
}

void CpuIdleCancelRatioTrial()
{
    const auto flags=IrqSpinLockAcquire(&g_ratioLock);
    g_ratioTrial.Cancel();
    __atomic_store_n(&g_ratioTrialActive,g_ratioTrial.phase!=CpuEpbTrial::Phase::Off,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_ratioLock,flags);
}

CpuEpbTrial CpuIdleReadRatioTrial()
{
    const auto flags=IrqSpinLockAcquire(&g_ratioLock);
    const auto trial=g_ratioTrial;
    IrqSpinLockRelease(&g_ratioLock,flags);
    return trial;
}

int CpuIdleSetBudget(uint64_t now,unsigned count,const CpuBudgetConfig& config,unsigned minRatio,unsigned maxNonTurbo)
{
    if (!CpuPowerBudget::Valid(config) || !count || count>CpuPowerBudget::MaxCpus) return -22;
    const auto flags=IrqSpinLockAcquire(&g_ratioLock);
    int result=-16;
    if (g_budget.phase==CpuPowerBudget::Phase::Snapshot || g_budget.phase==CpuPowerBudget::Phase::Active)
        result=g_budget.config.external!=config.external?-16:g_budget.SetConfig(config)?0:-22;
    else if (g_budget.phase==CpuPowerBudget::Phase::Off && g_ratioTrial.phase==CpuEpbTrial::Phase::Off)
        result=g_budget.Start(now,count,config,minRatio,maxNonTurbo)?0:-22;
    __atomic_store_n(&g_budgetActive,g_budget.phase!=CpuPowerBudget::Phase::Off,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_ratioLock,flags);
    return result;
}

bool CpuIdleBudgetActive()
{
    return __atomic_load_n(&g_budgetActive,__ATOMIC_ACQUIRE);
}

void CpuIdleTickBudget(unsigned cpu,uint64_t now,bool eligible)
{
    const auto flags=IrqSpinLockAcquire(&g_ratioLock);
    g_budget.Tick(cpu,now,eligible,ReadRatio,WriteRatio);
    __atomic_store_n(&g_budgetActive,g_budget.phase!=CpuPowerBudget::Phase::Off,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_ratioLock,flags);
}

void CpuIdleBudgetControl(uint64_t now,const CpuBudgetSample& sample)
{
    const auto flags=IrqSpinLockAcquire(&g_ratioLock);
    g_budget.Control(now,sample);
    IrqSpinLockRelease(&g_ratioLock,flags);
}

void CpuIdleBudgetSetCap(uint64_t now,unsigned cap)
{
    const auto flags=IrqSpinLockAcquire(&g_ratioLock);
    g_budget.SetCap(now,cap);
    IrqSpinLockRelease(&g_ratioLock,flags);
}

void CpuIdleCancelBudget(bool external)
{
    const auto flags=IrqSpinLockAcquire(&g_ratioLock);
    if (g_budget.config.external==external) g_budget.Cancel();
    __atomic_store_n(&g_budgetActive,g_budget.phase!=CpuPowerBudget::Phase::Off,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_ratioLock,flags);
}

CpuPowerBudget CpuIdleReadBudget()
{
    const auto flags=IrqSpinLockAcquire(&g_ratioLock);
    const auto budget=g_budget;
    IrqSpinLockRelease(&g_ratioLock,flags);
    return budget;
}

CpuEpbTrial CpuIdleReadEpbTrial()
{
    const auto flags=IrqSpinLockAcquire(&g_epbLock);
    const auto trial=g_epbTrial;
    IrqSpinLockRelease(&g_epbLock,flags);
    return trial;
}

void CpuIdleRequestSample() {
    __atomic_add_fetch(&g_sampleGen, 1, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < CpuIdleMaxCpus; ++i) __atomic_add_fetch(&g_monitor[i].word, 1, __ATOMIC_RELEASE);
}

void CpuIdleReadStatus(CpuIdleStatus& out) {
    const auto& plat = Plat();
    out = CpuIdleStatus{};
    out.caps = plat.caps;
    out.bareIntel = plat.bareIntel;
    out.residencyMsrs = plat.residency;
    out.epbSupported = plat.epb;
    out.requested = CpuIdleState(__atomic_load_n(&g_requested, __ATOMIC_RELAXED));
    out.choice = CpuIdleSelect(plat.caps, out.requested);
    if (__atomic_load_n(&g_choice, __ATOMIC_ACQUIRE) < 0) out.choice = CpuIdleChoice{};
    out.epbTarget = __atomic_load_n(&g_epbTarget, __ATOMIC_RELAXED);
    out.epbFirmware = plat.epbFirmware;
    out.epbBootApplied = g_epbBootApplied;
    out.autoMode = __atomic_load_n(&g_auto, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < CpuIdleMaxCpus; ++i) {
        out.cpus[i] = g_cpu[i];
        if (g_cpu[i].seen) out.cpuCount = i + 1;
    }
    out.tsc = Rdtsc();
    if (plat.residency) {
        out.pkgC2 = ReadMsr(0x60d); out.pkgC3 = ReadMsr(0x3f8); out.pkgC6 = ReadMsr(0x3f9); out.pkgC7 = ReadMsr(0x3fa);
        out.pkgCstConfig = ReadMsr(0xe2);
        out.turboRatioLimit = ReadMsr(0x1ad);
    }
    out.prevTsc = g_prevTsc; out.prevPkgC2 = g_prevPkg[0]; out.prevPkgC3 = g_prevPkg[1];
    out.prevPkgC6 = g_prevPkg[2]; out.prevPkgC7 = g_prevPkg[3];
    g_prevTsc = out.tsc; g_prevPkg[0] = out.pkgC2; g_prevPkg[1] = out.pkgC3; g_prevPkg[2] = out.pkgC6; g_prevPkg[3] = out.pkgC7;
    if (plat.bareIntel) out.miscEnable = ReadMsr(0x1a0);
}

void CpuIdleResetCounters() {
    for (uint32_t i = 0; i < CpuIdleMaxCpus; ++i) {
        for (auto& e : g_cpu[i].entries) e = 0;
        for (auto& e : g_cpu[i].autoSelected) e = 0;
        g_cpu[i].idleTsc = 0;
        g_cpu[i].hits = g_cpu[i].tooDeep = g_cpu[i].tooShallow = g_cpu[i].holds = 0;
    }
}

} // namespace brook
