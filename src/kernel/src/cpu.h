#pragma once

#include <stdint.h>

// CPU feature initialisation — call once very early in kernel boot, after GdtInit().
//
// Enables:
//   - x87 FPU (CR0: clear EM, set MP, set NE)
//   - SSE / SSE2 (CR4: set OSFXSR + OSXMMEXCPT)
//
// NOTE on interrupt handlers and SSE:
//   idt.cpp and apic.cpp are compiled with -mgeneral-regs-only so that
//   __attribute__((interrupt)) handlers never touch XMM registers.  This means
//   no FXSAVE/FXRSTOR is needed in the ISR path — the interrupted code's FPU
//   state is never disturbed.
//
//   When processes are added, context switches must save/restore SSE state via
//   FXSAVE64/FXRSTOR64 (or XSAVE/XRSTOR if AVX is enabled).

void CpuInitFpu();

// Returns true if CPUID reports SSE2 support (always true on x86-64).
bool CpuHasSse2();

// ---- MSR helpers ----

static constexpr uint32_t MSR_EFER   = 0xC0000080;
static constexpr uint32_t MSR_STAR   = 0xC0000081;
static constexpr uint32_t MSR_LSTAR  = 0xC0000082;
static constexpr uint32_t MSR_CSTAR  = 0xC0000083;  // compat-mode SYSCALL (unused)
static constexpr uint32_t MSR_FMASK  = 0xC0000084;
static constexpr uint32_t MSR_GS_BASE        = 0xC0000101;
static constexpr uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;

static constexpr uint64_t EFER_SCE = (1ULL << 0);  // SysCall Enable

static inline uint64_t ReadMsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
}

static inline void WriteMsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = static_cast<uint32_t>(val);
    uint32_t hi = static_cast<uint32_t>(val >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

// ---- Per-CPU kernel environment ----
// Accessible via SWAPGS → gs:[offset].
// The syscall dispatcher reads SyscallStack, SyscallTable from here.

struct KernelCpuEnv {
    uint64_t selfPtr;       // [gs:0]  — pointer to this struct (for sanity checks)
    uint64_t syscallStack;  // [gs:8]  — top of per-CPU syscall kernel stack
    uint64_t syscallTable;  // [gs:16] — pointer to the active syscall table
    uint64_t savedUserRsp;  // [gs:24] — user RSP saved during SYSCALL
    uint64_t currentPid;    // [gs:32] — PID of the currently running process
};

// Initialise SYSCALL/SYSRET MSRs.  Call after GdtInit().
// lstarEntry is the address of the assembly syscall entry point.
// This function also enables SCE in EFER and sets FMASK to clear IF.
// Does NOT set up GS base or the KernelCpuEnv — that's done later when
// the per-CPU environment and syscall dispatcher are ready.
void CpuInitSyscallMsrs(uint64_t lstarEntry);

// Set the kernel GS base (MSR_KERNEL_GS_BASE) for SWAPGS.
void CpuSetKernelGsBase(KernelCpuEnv* env);
