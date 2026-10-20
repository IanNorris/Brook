// kvmclock.cpp — KVM paravirt clock (CLOCKSOURCE2) support.
//
// Stable pvclock supplies elapsed nanoseconds independently of timer delivery.
// Per-vCPU steal records separately report host descheduling; pvclock alone
// cannot distinguish elapsed wall time from actual vCPU running time.
//
// Reference: Documentation/virt/kvm/msr.rst in the Linux kernel, and
// pvclock_vcpu_time_info in include/uapi/linux/pvclock-abi.h.

#include "kvmclock.h"
#include "cpu.h"
#include "serial.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"

namespace brook {

struct PvClockVcpuTimeInfo {
    uint32_t version;
    uint32_t pad0;
    uint64_t tsc_timestamp;
    uint64_t system_time;
    uint32_t tsc_to_system_mul;
    int8_t   tsc_shift;
    uint8_t  flags;
    uint8_t  pad[2];
};
static_assert(sizeof(PvClockVcpuTimeInfo) == 32, "pvclock layout");

static constexpr uint32_t MSR_KVM_SYSTEM_TIME_NEW = 0x4b564d01;
static constexpr uint32_t KVM_CPUID_BASE          = 0x40000000;
static constexpr uint32_t KVM_FEATURE_CLOCKSOURCE2 = (1u << 3);

static volatile PvClockVcpuTimeInfo* g_pvti = nullptr;
static bool g_enabled = false;
static bool g_stableFeature = false;
static PhysicalAddress g_clockPage;

static inline void Cpuid(uint32_t leaf, uint32_t sub,
                         uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d)
{
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(sub));
}

static inline uint64_t Rdtsc()
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
}

static bool DetectKvm(uint32_t& features)
{
    uint32_t a, b, c, d;
    Cpuid(KVM_CPUID_BASE, 0, a, b, c, d);
    // Expect "KVMKVMKVM\0\0\0" little-endian: ebx='KVMK', ecx='VMKV', edx='M\0\0\0'
    if (b != 0x4b4d564bu || c != 0x564b4d56u || d != 0x0000004du) return false;
    uint32_t fb, fc, fd;
    Cpuid(KVM_CPUID_BASE + 1, 0, features, fb, fc, fd);
    return true;
}

bool KvmClockInit()
{
    uint32_t features = 0;
    if (!DetectKvm(features))
    {
        SerialPuts("KvmClock: hypervisor is not KVM; using LAPIC-derived clock\n");
        return false;
    }
    if (!(features & KVM_FEATURE_CLOCKSOURCE2))
    {
        SerialPuts("KvmClock: KVM_FEATURE_CLOCKSOURCE2 not advertised\n");
        return false;
    }

    PhysicalAddress phys = PmmAllocPage(MemTag::KernelData);
    if (phys.raw() == 0)
    {
        SerialPuts("KvmClock: PmmAllocPage failed\n");
        return false;
    }

    g_pvti = reinterpret_cast<volatile PvClockVcpuTimeInfo*>(PhysToVirt(phys).raw());

    // Sanitize the entire allocation before host registration: the read-only
    // user alias must never expose stale kernel data outside the pvclock ABI.
    volatile uint64_t* q = reinterpret_cast<volatile uint64_t*>(g_pvti);
    for (int i = 0; i < 4096 / 8; i++) q[i] = 0;
    g_clockPage = phys; // Permanent reference while KVM may write this page.

    // Write MSR: bit 0 = enable, bits 63..1 = physical address (4-byte aligned).
    WriteMsr(MSR_KVM_SYSTEM_TIME_NEW, phys.raw() | 1ULL);
    g_stableFeature = (features & (1u << 24)) != 0;
    g_enabled = true;

    // Do a throwaway read so we can log something useful.
    uint64_t ns = KvmClockReadNs();
    SerialPrintf("KvmClock: enabled at phys 0x%lx, flags=0x%x, t=%lu ns\n",
                 phys.raw(), g_pvti->flags, ns);
    return true;
}

bool KvmClockStableAvailable() { return g_enabled && g_stableFeature; }

bool KvmClockAvailable()
{
    return g_enabled;
}

PhysicalAddress KvmClockPublicPage()
{
    return g_enabled && g_stableFeature ? g_clockPage : PhysicalAddress{};
}

uint64_t KvmClockReadNs()
{
    if (!g_enabled || !g_pvti) return 0;

    uint32_t v1, v2;
    uint64_t tsc_ts, sys_t;
    uint32_t mul;
    int8_t   shift;

    // Lockless read protocol: retry while the version is odd or changed.
    for (;;)
    {
        v1 = __atomic_load_n(&g_pvti->version, __ATOMIC_ACQUIRE);
        if (v1 & 1) { __asm__ volatile("pause"); continue; }

        tsc_ts = g_pvti->tsc_timestamp;
        sys_t  = g_pvti->system_time;
        mul    = g_pvti->tsc_to_system_mul;
        shift  = static_cast<int8_t>(g_pvti->tsc_shift);

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        v2 = g_pvti->version;
        if (v1 == v2 && !(v1 & 1)) break;
    }

    uint64_t tsc = Rdtsc();
    uint64_t delta = (tsc > tsc_ts) ? (tsc - tsc_ts) : 0;

    if (shift < 0)      delta >>= -shift;
    else if (shift > 0) delta <<=  shift;

    unsigned __int128 p = static_cast<unsigned __int128>(delta) * mul;
    return sys_t + static_cast<uint64_t>(p >> 32);
}

static uint64_t OrderedTsc()
{
#ifdef BROOK_HOST_TEST
    extern uint64_t KvmClockTestTsc();
    return KvmClockTestTsc();
#else
    uint32_t low, high;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) :: "memory");
    return uint64_t(low) | (uint64_t(high) << 32);
#endif
}
bool KvmClockReadStableNs(uint64_t* result)
{
    if (!result || !g_enabled || !g_pvti || !g_stableFeature) return false;
    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        const uint32_t first = __atomic_load_n(&g_pvti->version, __ATOMIC_ACQUIRE);
        if (first & 1) { __asm__ volatile("pause"); continue; }
        const uint64_t timestamp = g_pvti->tsc_timestamp;
        const uint64_t base = g_pvti->system_time;
        const uint32_t multiplier = g_pvti->tsc_to_system_mul;
        const int shift = g_pvti->tsc_shift;
        const uint8_t flags = g_pvti->flags;
        // Sample TSC inside the validated snapshot, with CPU and compiler
        // ordering on both sides. A concurrent hypervisor update retries all.
        const uint64_t tsc = OrderedTsc();
        const uint32_t second = __atomic_load_n(&g_pvti->version, __ATOMIC_ACQUIRE);
        if (first != second || (second & 1)) continue;
        if (!(flags & 1) || !multiplier || tsc < timestamp || shift < -63 || shift > 63) return false;
        uint64_t delta = tsc - timestamp;
        if (shift < 0) delta >>= -shift;
        else if (shift > 0) {
            if (delta > (~uint64_t(0) >> shift)) return false;
            delta <<= shift;
        }
        const uint64_t elapsed = static_cast<uint64_t>((static_cast<unsigned __int128>(delta) * multiplier) >> 32);
        if (base > ~uint64_t(0) - elapsed) return false;
        *result = base + elapsed;
        return true;
    }
    return false;
}

// KVM ABI: one aligned record per vCPU, sharing one permanently pinned page.
// Never expose this host-writable kernel page through a userspace alias.
struct alignas(64) KvmStealRecord {
    uint64_t steal;
    uint32_t version, flags;
    uint8_t preempted, pad0[3];
    uint32_t pad[11];
};
static_assert(sizeof(KvmStealRecord) == 64, "KVM steal ABI");
static constexpr uint32_t KvmStealCpus = 64;
static_assert(KvmStealCpus * sizeof(KvmStealRecord) == 4096, "steal record page capacity");
static volatile KvmStealRecord* g_stealRecords = nullptr;
static PhysicalAddress g_stealPage;
static bool g_stealEnabled[KvmStealCpus]{};

bool KvmStealInit(uint32_t cpu)
{
    if (cpu >= KvmStealCpus) return false;
    if (__atomic_load_n(&g_stealEnabled[cpu], __ATOMIC_ACQUIRE)) return true;
    uint32_t features = 0;
    if (!DetectKvm(features) || !(features & (1u << 5))) return false;
    if (cpu == 0 && !g_stealRecords) {
        const auto page = PmmAllocPage(MemTag::KernelData);
        if (!page.raw()) return false;
        auto* words = reinterpret_cast<volatile uint64_t*>(PhysToVirt(page).raw());
        for (uint32_t n = 0; n < 4096 / 8; ++n) words[n] = 0;
        g_stealPage = page;
        g_stealRecords = reinterpret_cast<volatile KvmStealRecord*>(words);
    }
    if (!g_stealRecords) return false;
    // Called on the vCPU owning this MSR, after BSP publication/activation.
    WriteMsr(0x4b564d03, (g_stealPage.raw() + cpu * sizeof(KvmStealRecord)) | 1ULL);
    __atomic_store_n(&g_stealEnabled[cpu], true, __ATOMIC_RELEASE);
    SerialPrintf("KvmSteal: CPU%u registered\n", cpu);
    return true;
}

bool KvmStealAvailable(uint32_t cpu)
{
    return cpu < KvmStealCpus && __atomic_load_n(&g_stealEnabled[cpu], __ATOMIC_ACQUIRE);
}

bool KvmStealReadNs(uint32_t cpu, uint64_t* result)
{
    if (!result || !KvmStealAvailable(cpu) || !g_stealRecords) return false;
    const auto* page = &g_stealRecords[cpu];
    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        const uint32_t first = __atomic_load_n(&page->version, __ATOMIC_ACQUIRE);
        if (first & 1) { __asm__ volatile("pause"); continue; }
        const uint64_t value = __atomic_load_n(&page->steal, __ATOMIC_RELAXED);
        const uint32_t flags = __atomic_load_n(&page->flags, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t second = __atomic_load_n(&page->version, __ATOMIC_ACQUIRE);
        if (first != second || (second & 1)) continue;
        if (flags) return false; // Reserved by the current ABI.
        *result = value;
        return true;
    }
    return false;
}

} // namespace brook
