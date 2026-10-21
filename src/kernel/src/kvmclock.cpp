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
#include "smp.h"
#include "pvclock_page.h"

namespace brook {

using PvClockVcpuTimeInfo = BrookPvclockInfo;
static_assert(sizeof(PvClockVcpuTimeInfo) == 32, "pvclock layout");

static constexpr uint32_t MSR_KVM_SYSTEM_TIME_NEW = 0x4b564d01;
static constexpr uint32_t KVM_CPUID_BASE          = 0x40000000;
static constexpr uint32_t KVM_FEATURE_CLOCKSOURCE2 = (1u << 3);

static volatile BrookPvclockSlot* g_pvti = nullptr;
static bool g_clockReady[BROOK_PVCLOCK_CPUS]{};
static bool g_enabled = false;
static bool g_stableFeature = false;
static PhysicalAddress g_clockPage;

static inline void Cpuid(uint32_t leaf, uint32_t sub,
                         uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d)
{
#ifdef BROOK_HOST_TEST
    extern void KvmClockTestCpuid(uint32_t, uint32_t, uint32_t&, uint32_t&, uint32_t&, uint32_t&);
    KvmClockTestCpuid(leaf, sub, a, b, c, d);
#else
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(sub));
#endif
}

static uint32_t ClockCpuIndex() {
#ifdef BROOK_HOST_TEST
    extern uint32_t KvmClockTestCpu();
    return KvmClockTestCpu();
#else
    return SmpCurrentCpuIndex();
#endif
}

static bool ClockRdtscpSupported() {
#ifdef BROOK_HOST_TEST
    extern bool KvmClockTestRdtscpSupported();
    return KvmClockTestRdtscpSupported();
#else
    uint32_t a,b,c,d;Cpuid(0x80000000,0,a,b,c,d);
    if (a<0x80000001) return false;
    Cpuid(0x80000001,0,a,b,c,d);
    return (d&(1u<<27))!=0;
#endif
}

static volatile BrookPvclockMetadata* ClockMetadata() {
    return reinterpret_cast<volatile BrookPvclockMetadata*>(g_pvti[0].reserved);
}

bool KvmClockInitOnCpu(uint32_t cpu) {
    if (!g_enabled || !g_pvti || cpu>=BROOK_PVCLOCK_CPUS || cpu!=ClockCpuIndex()) return false;
    if (__atomic_load_n(&g_clockReady[cpu],__ATOMIC_ACQUIRE)) return true;
    const bool rdtscp=ClockRdtscpSupported();
    if (!rdtscp) __atomic_fetch_and(&ClockMetadata()->features,~uint32_t(BROOK_PVCLOCK_RDTSCP),__ATOMIC_RELEASE);
#ifdef BROOK_HOST_TEST
    extern void KvmClockTestRegister(uint32_t,uint64_t,bool);
    KvmClockTestRegister(cpu,g_clockPage.raw()+cpu*BROOK_PVCLOCK_STRIDE,rdtscp);
#else
    if (rdtscp) WriteMsr(0xc0000103u,cpu); // IA32_TSC_AUX: CPU index for the vDSO.
    WriteMsr(MSR_KVM_SYSTEM_TIME_NEW,(g_clockPage.raw()+cpu*BROOK_PVCLOCK_STRIDE)|1ULL);
#endif
    __atomic_store_n(&g_clockReady[cpu],true,__ATOMIC_RELEASE);
    return true;
}

uint64_t KvmClockRegisteredCpuMask() {
    uint64_t mask=0;
    for (uint32_t cpu=0;cpu<BROOK_PVCLOCK_CPUS;++cpu)
        if (__atomic_load_n(&g_clockReady[cpu],__ATOMIC_ACQUIRE)) mask|=uint64_t(1)<<cpu;
    return mask;
}

static bool DetectKvm(uint32_t& features, uint32_t* hints = nullptr)
{
    uint32_t a, b, c, d;
    Cpuid(KVM_CPUID_BASE, 0, a, b, c, d);
    // Expect "KVMKVMKVM\0\0\0" little-endian: ebx='KVMK', ecx='VMKV', edx='M\0\0\0'
    if (b != 0x4b4d564bu || c != 0x564b4d56u || d != 0x0000004du) return false;
    uint32_t fb, fc, fd;
    Cpuid(KVM_CPUID_BASE + 1, 0, features, fb, fc, fd);
    if (hints) *hints = fd;
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

    g_pvti = reinterpret_cast<volatile BrookPvclockSlot*>(PhysToVirt(phys).raw());

    // Sanitize the entire allocation before host registration: the read-only
    // user alias must never expose stale kernel data outside the pvclock ABI.
    volatile uint64_t* q = reinterpret_cast<volatile uint64_t*>(g_pvti);
    for (int i = 0; i < 4096 / 8; i++) q[i] = 0;
    g_clockPage = phys; // Permanent reference while KVM may write this page.

    auto* metadata=ClockMetadata();
    metadata->magic=BROOK_PVCLOCK_MAGIC;metadata->version=BROOK_PVCLOCK_VERSION;
    metadata->stride=BROOK_PVCLOCK_STRIDE;metadata->cpus=BROOK_PVCLOCK_CPUS;
    metadata->features=ClockRdtscpSupported()?BROOK_PVCLOCK_RDTSCP:0;
    g_stableFeature = (features & (1u << 24)) != 0;
    g_enabled = true;
    if (!KvmClockInitOnCpu(0)) {g_enabled=false;return false;}

    // Do a throwaway read so we can log something useful.
    uint64_t ns = KvmClockReadNs();
    SerialPrintf("KvmClock: enabled at phys 0x%lx, flags=0x%x, t=%lu ns\n",
                 phys.raw(), g_pvti[0].clock.flags, ns);
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
static bool ReadClockNs(uint64_t* result,bool stable)
{
    if (!result || !g_enabled || !g_pvti || (stable && !g_stableFeature)) return false;
    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        const uint32_t cpu=ClockCpuIndex();
        if (cpu>=BROOK_PVCLOCK_CPUS || !__atomic_load_n(&g_clockReady[cpu],__ATOMIC_ACQUIRE)) return false;
        const auto* page=&g_pvti[cpu].clock;
        const uint32_t first = __atomic_load_n(&page->version, __ATOMIC_ACQUIRE);
        if (first & 1) { __asm__ volatile("pause"); continue; }
        const uint64_t timestamp = page->tsc_timestamp;
        const uint64_t base = page->system_time;
        const uint32_t multiplier = page->tsc_to_system_mul;
        const int shift = page->tsc_shift;
        const uint8_t flags = page->flags;
        // Sample TSC inside the validated snapshot, with CPU and compiler
        // ordering on both sides. A concurrent hypervisor update retries all.
        const uint64_t tsc = OrderedTsc();
        const uint32_t second = __atomic_load_n(&page->version, __ATOMIC_ACQUIRE);
        if (cpu!=ClockCpuIndex() || first != second || (second & 1)) continue;
        if ((stable && !(flags & 1)) || !multiplier || tsc < timestamp || shift < -63 || shift > 63) return false;
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
bool KvmClockReadStableNs(uint64_t* result) {return ReadClockNs(result,true);}
uint64_t KvmClockReadNs() {
    if (!g_enabled || !g_pvti) return 0;
    const uint32_t cpu=ClockCpuIndex();
    if (cpu>=BROOK_PVCLOCK_CPUS || !__atomic_load_n(&g_clockReady[cpu],__ATOMIC_ACQUIRE)) return 0;
    // Preserve the legacy retrying API: a transient host update must not publish
    // zero to syscall/RTC consumers. Consumers needing bounded failure use the
    // separate KvmClockReadStableNs interface.
    uint64_t result=0;
    while (!ReadClockNs(&result,false)) __asm__ volatile("pause");
    return result;
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
static_assert(__builtin_offsetof(KvmStealRecord, preempted) == 16, "KVM preemption hint ABI");
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

bool KvmStealReadPreempted(uint32_t cpu,uint8_t* output)
{
    if(!output || !KvmStealAvailable(cpu) || !g_stealRecords)return false;
    *output=__atomic_load_n(&g_stealRecords[cpu].preempted,__ATOMIC_ACQUIRE);return true;
}

void KvmVirtualizationRead(uint32_t cpus, KvmVirtualizationStatus* output)
{
    if (!output) return;
    *output = {};
    output->cpus = cpus < KvmStealCpus ? cpus : KvmStealCpus;
    output->present = DetectKvm(output->features, &output->hints);
    if (!output->present) return;
    for (uint32_t cpu = 0; cpu < output->cpus; ++cpu) {
        // Acquire pairs with registration publication. Records stay pinned;
        // the host can update this byte at any time, outside versioned steal
        // reads. Never clear it or participate in the PV TLB flush protocol.
        if (!KvmStealAvailable(cpu) || !g_stealRecords) continue;
        const uint64_t mask = uint64_t(1) << cpu;
        output->registeredStealMask |= mask;
        const uint8_t hint = __atomic_load_n(&g_stealRecords[cpu].preempted, __ATOMIC_ACQUIRE);
        if (hint & 1u) output->preemptedHintMask |= mask; // KVM_VCPU_PREEMPTED
        if (hint & ~1u) output->otherHintMask |= mask;
    }
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
