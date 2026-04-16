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
}

bool CpuHasSse2()
{
    // CPUID leaf 1, EDX bit 26 = SSE2.  On x86-64 this is always true,
    // but check explicitly for correctness.
    uint32_t edx;
    __asm__ volatile("cpuid" : "=d"(edx) : "a"(1) : "ebx", "ecx");
    return (edx >> 26) & 1;
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
