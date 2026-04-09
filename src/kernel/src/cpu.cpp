#include "cpu.h"
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
}

bool CpuHasSse2()
{
    // CPUID leaf 1, EDX bit 26 = SSE2.  On x86-64 this is always true,
    // but check explicitly for correctness.
    uint32_t edx;
    __asm__ volatile("cpuid" : "=d"(edx) : "a"(1) : "ebx", "ecx");
    return (edx >> 26) & 1;
}
