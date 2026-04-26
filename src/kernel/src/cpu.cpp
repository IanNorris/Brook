#include "cpu.h"
#include "gdt.h"
#include "serial.h"

// ---------------------------------------------------------------------------
// CR0 / CR4 bit definitions
// ---------------------------------------------------------------------------

// CR0
static constexpr uint64_t CR0_MP = (1ULL << 1);  // Monitor co-processor
static constexpr uint64_t CR0_EM = (1ULL << 2);  // Emulation (must be 0 for FPU)
static constexpr uint64_t CR0_NE = (1ULL << 5);  // Numeric Error (native FPU errors)

// CR4
static constexpr uint64_t CR4_OSFXSR    = (1ULL << 9);   // OS supports FXSAVE/FXRSTOR
static constexpr uint64_t CR4_OSXMMEXCPT = (1ULL << 10); // OS handles SIMD FP exceptions
static constexpr uint64_t CR4_OSXSAVE    = (1ULL << 18); // OS uses XSAVE/XRSTOR & enables AVX

static inline uint64_t ReadCr0() { uint64_t v; __asm__ volatile("mov %%cr0,%0":"=r"(v)); return v; }
static inline void     WriteCr0(uint64_t v) { __asm__ volatile("mov %0,%%cr0"::"r"(v)); }
static inline uint64_t ReadCr4() { uint64_t v; __asm__ volatile("mov %%cr4,%0":"=r"(v)); return v; }
static inline void     WriteCr4(uint64_t v) { __asm__ volatile("mov %0,%%cr4"::"r"(v)); }

void CpuInitFpu()
{
    // ---- CR0: enable native x87 FPU, disable software emulation ----
    uint64_t cr0 = ReadCr0();
    cr0 &= ~CR0_EM;   // clear EM — no software emulation
    cr0 |=  CR0_MP;   // set MP  — WAIT/FWAIT raises #NM if TS set
    cr0 |=  CR0_NE;   // set NE  — native FP error reporting (not legacy IRQ13)
    WriteCr0(cr0);

    // ---- FNINIT — put x87 FPU in known default state ----
    __asm__ volatile("fninit");

    // ---- CR4: tell the CPU the OS can handle SSE state ----
    uint64_t cr4 = ReadCr4();
    cr4 |= CR4_OSFXSR;     // OS supports FXSAVE/FXRSTOR (required to use SSE)
    cr4 |= CR4_OSXMMEXCPT; // OS handles #XM (SIMD floating-point exceptions)
    WriteCr4(cr4);

    brook::SerialPrintf("CPU: FPU/SSE enabled (CR0=0x%lx CR4=0x%lx)\n",
                        ReadCr0(), ReadCr4());

    // Report SSE4.2 — glibc ifunc resolvers use it to pick optimised paths.
    uint32_t ecx_val;
    __asm__ volatile("cpuid" : "=c"(ecx_val) : "a"(1) : "ebx", "edx");
    brook::SerialPrintf("CPU: SSE3=%s SSSE3=%s SSE4.1=%s SSE4.2=%s\n",
                        (ecx_val & (1U << 0)) ? "yes" : "no",
                        (ecx_val & (1U << 9)) ? "yes" : "no",
                        (ecx_val & (1U << 19)) ? "yes" : "no",
                        (ecx_val & (1U << 20)) ? "yes" : "no");

    CpuEnableXsaveAvx();
}

// Enable XSAVE-managed CPU state so AVX/AVX2 instructions don't #UD in user
// space.  Sets CR4.OSXSAVE and writes XCR0 = 0x7 (x87 | SSE | AVX).
//
// CAVEAT: We currently keep using fxsave/fxrstor in the scheduler context
// switch and in the APIC profiler IRQ stub.  fxsave only saves x87 + SSE
// (XMM0-15 lower 128 bits) — the YMM upper halves are NOT saved across
// context switches.  This means YMM bits 128-255 of process A can leak
// into process B if the scheduler interleaves them.  For Brook (hobby OS,
// no security boundary, mostly single-threaded GUI apps) this is an
// accepted compromise to unblock AVX-using clients (GIMP etc).  Proper
// fix is to grow FxsaveArea to 1088B alignas(64) and switch the asm to
// xsave64/xrstor64 with state mask 0x7 — tracked separately.
void CpuEnableXsaveAvx()
{
    // CPUID.1.ECX[26] = XSAVE supported by hardware.  qemu64 model lacks
    // it; -cpu host (KVM) and -cpu Skylake-Client and friends have it.
    uint32_t cpuid1_ecx = 0, cpuid1_edx = 0, cpuid1_ebx = 0;
    __asm__ volatile("cpuid"
                     : "=b"(cpuid1_ebx), "=c"(cpuid1_ecx), "=d"(cpuid1_edx)
                     : "a"(1));
    if (!(cpuid1_ecx & (1U << 26)))
    {
        brook::SerialPrintf("CPU: XSAVE not supported by HW; AVX disabled\n");
        return;
    }

    // Set CR4.OSXSAVE — required before XGETBV/XSETBV and before CPUID
    // reports OSXSAVE=1 to user-space (which glibc ifunc resolvers gate on).
    uint64_t cr4 = ReadCr4();
    cr4 |= CR4_OSXSAVE;
    WriteCr4(cr4);

    // Read current XCR0 (just for logging).
    uint32_t xcr0_lo = 0, xcr0_hi = 0;
    __asm__ volatile("xgetbv"
                     : "=a"(xcr0_lo), "=d"(xcr0_hi)
                     : "c"(0));

    // Always enable x87 (bit 0) + SSE (bit 1).  Add AVX (bit 2) iff
    // CPUID.1.ECX[28] is set — required by Intel SDM (don't enable a
    // state component the CPU doesn't advertise).
    uint32_t newXcr0 = xcr0_lo | 0x3;
    bool hasAvx = (cpuid1_ecx & (1U << 28)) != 0;
    if (hasAvx) newXcr0 |= 0x4;

    __asm__ volatile("xsetbv"
                     :
                     : "a"(newXcr0), "d"(0u), "c"(0u));

    // Also read AVX2 from CPUID.7.0.EBX[5] for diagnostic logging.
    uint32_t leaf7_ebx = 0;
    __asm__ volatile("cpuid"
                     : "=b"(leaf7_ebx)
                     : "a"(7u), "c"(0u)
                     : "edx");

    brook::SerialPrintf("CPU: XSAVE enabled (CR4.OSXSAVE=1, XCR0=0x%x, AVX=%s, AVX2=%s)\n",
                        newXcr0,
                        hasAvx ? "yes" : "no",
                        (leaf7_ebx & (1U << 5)) ? "yes" : "no");
}

bool CpuHasSse2()
{
    // CPUID leaf 1, EDX bit 26 = SSE2.  On x86-64 this is always true,
    // but check explicitly for correctness.
    uint32_t edx;
    __asm__ volatile("cpuid" : "=d"(edx) : "a"(1) : "ebx", "ecx");
    return (edx >> 26) & 1;
}

bool CpuHasRdrand()
{
    // CPUID leaf 1, ECX bit 30 = RDRAND support.
    uint32_t ecx;
    __asm__ volatile("cpuid" : "=c"(ecx) : "a"(1) : "ebx", "edx");
    return (ecx >> 30) & 1;
}

// ---------------------------------------------------------------------------
// SYSCALL / SYSRET MSR setup
// ---------------------------------------------------------------------------

void CpuInitSyscallMsrs(uint64_t lstarEntry)
{
    // STAR MSR: [63:48] = SYSRET CS/SS base, [47:32] = SYSCALL CS
    // SYSCALL: CS = STAR[47:32], SS = STAR[47:32] + 8
    // SYSRET:  SS = STAR[63:48] + 8, CS = STAR[63:48] + 16 (64-bit mode)
    uint64_t star = (static_cast<uint64_t>(GDT_STAR_USER) << 48) |
                    (static_cast<uint64_t>(GDT_STAR_KERNEL) << 32);
    WriteMsr(MSR_STAR, star);

    // LSTAR: the RIP that SYSCALL jumps to.
    WriteMsr(MSR_LSTAR, lstarEntry);

    // CSTAR: compatibility-mode SYSCALL target.  We don't support compat mode;
    // set to 0 so it faults immediately if somehow reached.
    WriteMsr(MSR_CSTAR, 0);

    // FMASK: RFLAGS bits to clear on SYSCALL.
    // Clear IF (bit 9) so interrupts are disabled on entry.
    // Clear TF (bit 8) so we don't single-step the kernel entry.
    // Clear DF (bit 10) so string ops go forward.
    WriteMsr(MSR_FMASK, (1ULL << 9) | (1ULL << 8) | (1ULL << 10));

    // Enable the SCE (SysCall Enable) bit in EFER.
    uint64_t efer = ReadMsr(MSR_EFER);
    efer |= EFER_SCE;
    WriteMsr(MSR_EFER, efer);

    brook::SerialPrintf("CPU: SYSCALL/SYSRET enabled (STAR=0x%016lx LSTAR=0x%016lx)\n",
                        star, lstarEntry);
}

void CpuSetKernelGsBase(KernelCpuEnv* env)
{
    env->selfPtr = reinterpret_cast<uint64_t>(env);
    // Set GS.base so the kernel can access env via gs: directly.
    // MSR_KERNEL_GS_BASE is what SWAPGS swaps with — set to 0 (user GS).
    WriteMsr(MSR_GS_BASE, reinterpret_cast<uint64_t>(env));
    WriteMsr(MSR_KERNEL_GS_BASE, 0);
}
