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

namespace brook {
namespace {

struct alignas(64) MonitorLine { volatile uint64_t word; uint8_t pad[56]; };

struct Platform {
    bool decided = false, bareIntel = false, residency = false, epb = false;
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
volatile int32_t g_epbTarget = -1;
volatile uint64_t g_sampleGen = 0;

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
    }
    g_platform = p;
    __atomic_store_n(&g_platform.decided, true, __ATOMIC_RELEASE);
    return g_platform;
}

} // namespace

void CpuIdleEnter(uint32_t cpu) {
    const auto& plat = Plat();
    CpuIdlePerCpu* pc = cpu < CpuIdleMaxCpus ? &g_cpu[cpu] : nullptr;
    if (pc) {
        pc->seen = true;
        const uint64_t eg = __atomic_load_n(&g_epbGen, __ATOMIC_ACQUIRE);
        if (plat.epb && eg != pc->epbGen) {
            WriteMsr(0x1b0, (ReadMsr(0x1b0) & ~0xfull) | (g_epbValue & 0xfu));
            pc->epb = ReadMsr(0x1b0);
            pc->epbGen = eg;
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
    const int64_t choice = __atomic_load_n(&g_choice, __ATOMIC_ACQUIRE);
    const uint64_t t0 = Rdtsc();
    if (choice < 0 || !pc) {
        __asm__ volatile("sti\n\thlt" ::: "memory");
        if (pc) pc->entries[uint32_t(CpuIdleState::Hlt)]++;
    } else {
        const uint32_t hint = uint32_t(choice & 0xffffffffu);
        const uint32_t ecx = uint32_t(uint64_t(choice) >> 32);
        __asm__ volatile("cli" ::: "memory");
        __asm__ volatile("monitor" :: "a"(&g_monitor[cpu].word), "c"(0u), "d"(0u) : "memory");
        __asm__ volatile("sti\n\tmwait" :: "a"(hint), "c"(ecx) : "memory");
        const uint32_t state = __atomic_load_n(&g_requested, __ATOMIC_RELAXED);
        pc->entries[state < uint32_t(CpuIdleState::Count) ? state : 0]++;
    }
    if (pc) pc->idleTsc += Rdtsc() - t0;
}

CpuIdleChoice CpuIdleRequest(CpuIdleState state) {
    const auto choice = CpuIdleSelect(Plat().caps, state);
    __atomic_store_n(&g_requested, uint32_t(choice.state), __ATOMIC_RELAXED);
    const int64_t packed = choice.hint < 0 ? -1 : int64_t((uint64_t(choice.ecx) << 32) | uint32_t(choice.hint));
    __atomic_store_n(&g_choice, packed, __ATOMIC_RELEASE);
    // Wake idle CPUs parked in MWAIT so they pick up the new hint promptly.
    for (uint32_t i = 0; i < CpuIdleMaxCpus; ++i) __atomic_add_fetch(&g_monitor[i].word, 1, __ATOMIC_RELEASE);
    return choice;
}

bool CpuIdleRequestEpb(uint32_t value) {
    if (!Plat().epb || value > 15) return false;
    __atomic_store_n(&g_epbValue, value, __ATOMIC_RELAXED);
    __atomic_store_n(&g_epbTarget, int32_t(value), __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_epbGen, 1, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < CpuIdleMaxCpus; ++i) __atomic_add_fetch(&g_monitor[i].word, 1, __ATOMIC_RELEASE);
    return true;
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
    if (plat.bareIntel) out.miscEnable = ReadMsr(0x1a0);
}

void CpuIdleResetCounters() {
    for (uint32_t i = 0; i < CpuIdleMaxCpus; ++i) {
        for (auto& e : g_cpu[i].entries) e = 0;
        g_cpu[i].idleTsc = 0;
    }
}

} // namespace brook
