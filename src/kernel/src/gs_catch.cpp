#include "gs_catch.h"

#ifndef BROOK_HOST_TEST

#include "cpu.h"        // ReadMsr / WriteMsr / MSR_GS_BASE / MSR_KERNEL_GS_BASE
#include "smp.h"        // SmpResolveCpuNoGs
#include "serial.h"     // SerialPrintf
#include "panic.h"      // KernelPanic

namespace brook {

namespace {

struct GsCatchRecord {
    const char* site;
    void*       ra;
    uint64_t    badActive;
    uint64_t    shadow;
    uint32_t    apic;
    uint32_t    idx;
};

// Lock-free BSS ring: written GS-free (no locks, no gs-relative access) so it is
// safe to record even before recovery.  Dumpable from a debugger / panic.
constexpr uint32_t GS_CATCH_RING = 32;
GsCatchRecord     g_gsCatchRing[GS_CATCH_RING] = {};
volatile uint32_t g_gsCatchCount = 0;   // monotonic total hits

// Reading IA32_GS_BASE never dereferences gs, so it is safe even when the base
// is bogus.  Kernel GS base is a canonical-high KernelCpuEnv* (bit 63 set); the
// user base is 0 (bit 63 clear) — the same discriminator the paranoid-swapgs
// macros use (EDX bit 31 after rdmsr).
__attribute__((no_instrument_function))
inline bool GsBaseIsKernel()
{
    return (ReadMsr(MSR_GS_BASE) >> 63) != 0;
}

}  // namespace

__attribute__((no_instrument_function, noinline))
void GsBaseCatchAtScene(const char* site, void* ra)
{
    if (__builtin_expect(GsBaseIsKernel(), 1))
        return;

    // Imbalance detected.  Freeze maskable interrupts BEFORE recovery: an IRQ in
    // this window would itself run with the bad GS and #PF at gs:176, and a
    // preempt/reschedule could migrate the thread so the ApicGetId->wrmsr
    // recovery targets the wrong CPU's env.  (NMI/SMI are not masked by cli, but
    // the two recovery writes below run back-to-back with nothing between them.)
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");

    // Re-check after cli: a legitimate concurrent path may have restored the
    // kernel GS base between the first read and the cli.
    if (GsBaseIsKernel())
    {
        __asm__ volatile("push %0\n\tpopfq" :: "r"(flags) : "memory", "cc");
        return;
    }

    // Capture the scene before mutating anything.
    uint64_t badActive = ReadMsr(MSR_GS_BASE);
    uint64_t shadow    = ReadMsr(MSR_KERNEL_GS_BASE);

    uint8_t  apic = 0;
    uint32_t idx  = 0xFFFFFFFFu;
    KernelCpuEnv* env = nullptr;
    SmpResolveCpuNoGs(&apic, &idx, &env);

    // Deterministic recovery: write the correct per-CPU env to the ACTIVE base
    // and restore the shadow to 0 (the CpuSetKernelGsBase invariant).  Writing
    // both back-to-back avoids leaving active==shadow==env, which a later
    // user-return swapgs would leak to userspace.  A bare swapgs is NOT used: in
    // the double-swap case both bases are already 0, so swapgs cannot repair it.
    if (env)
    {
        WriteMsr(MSR_GS_BASE, reinterpret_cast<uint64_t>(env));
        WriteMsr(MSR_KERNEL_GS_BASE, 0);
        __asm__ volatile("" ::: "memory");   // no gs-relative load hoisted above the fix
    }

    uint32_t slot = __atomic_fetch_add(&g_gsCatchCount, 1, __ATOMIC_RELAXED) % GS_CATCH_RING;
    g_gsCatchRing[slot] = GsCatchRecord{ site, ra, badActive, shadow, apic, idx };

    __asm__ volatile("push %0\n\tpopfq" :: "r"(flags) : "memory", "cc");

    // From here gs:176 is valid again (if env resolved) so the report path is
    // safe.  If env did NOT resolve we cannot touch any gs-relative code.
    if (!env)
    {
#if GS_CATCH_PANIC
        __asm__ volatile("cli\n\thlt");   // truly doomed — do not triple-fault
#endif
        return;
    }

#if GS_CATCH_PANIC
    KernelPanic("BRO-178 GS imbalance @ %s ra=%p apic=%u idx=%u "
                "active=0x%lx shadow=0x%lx "
                "(shadow==env => missed kernel swapgs; "
                "shadow==0 => double swapgs / clobbered env)",
                site, ra, apic, idx, badActive, shadow);
#else
    SerialPrintf("BRO-178 GS-CATCH #%u @ %s ra=%p apic=%u idx=%u "
                 "active=0x%lx shadow=0x%lx\n",
                 __atomic_load_n(&g_gsCatchCount, __ATOMIC_RELAXED),
                 site, ra, apic, idx, badActive, shadow);
#endif
}

}  // namespace brook

#endif  // BROOK_HOST_TEST
