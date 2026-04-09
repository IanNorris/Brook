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
