// kvmclock.cpp — KVM paravirt clock (CLOCKSOURCE2) support.
//
// The CMOS/LAPIC clock has second-granularity correctness after the RTC
// re-anchor in rtc.cpp, but ns-precision queries (clock_gettime with
// CLOCK_MONOTONIC) still drift within each one-second window.  Under KVM,
// the host exports a pvclock page that converts rdtsc() into nanoseconds
// with guaranteed monotonicity — we use that when available.
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

    // Zero the struct — version must be 0 (even) before the host writes it.
    volatile uint64_t* q = reinterpret_cast<volatile uint64_t*>(g_pvti);
    for (int i = 0; i < 4; i++) q[i] = 0;

    // Write MSR: bit 0 = enable, bits 63..1 = physical address (4-byte aligned).
    WriteMsr(MSR_KVM_SYSTEM_TIME_NEW, phys.raw() | 1ULL);
    g_enabled = true;

    // Do a throwaway read so we can log something useful.
    uint64_t ns = KvmClockReadNs();
    SerialPrintf("KvmClock: enabled at phys 0x%lx, flags=0x%x, t=%lu ns\n",
                 phys.raw(), g_pvti->flags, ns);
    return true;
}

bool KvmClockAvailable()
{
    return g_enabled;
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

} // namespace brook
