#pragma once

// fpu_irq.h — preserve userspace (and kernel) x87/SSE/AVX state across interrupt
// handlers (BRO-187).
//
// The Brook kernel is built with -msse2/-mfpmath=sse, so interrupt-handler C
// code uses XMM/YMM registers. An asynchronous interrupt that preempts a thread
// with live vector state (e.g. libLLVM/Mesa global constructors holding pointers
// in xmm4) would have that state clobbered by the handler and corrupted on
// return. context_switch.S saves/restores FPU per-thread, but that happens AFTER
// the ISR C code already clobbered the live registers, and a no-context-switch
// interrupt return never saves/restores at all.
//
// These macros, used in the naked ISR wrappers, XSAVE the interrupted FPU state
// onto an aligned slot on the current kernel stack on entry (before any kernel
// vector use) and XRSTOR it on exit (after all handler C calls). Because the
// slot lives on the interrupted thread's kernel stack, it is naturally
// thread-local and survives a context switch inside the handler, and the scheme
// is re-entrant.
//
// Register usage (must be honoured by the host stub):
//   * Runs AFTER all 15 GPRs are pushed and AFTER GS_PARANOID_ENTRY_EBX, so the
//     interrupted r14/r15 are safely saved on the stack and free to use as
//     scratch. r14/r15 (and rbx, carrying the GS swap flag) are callee-saved per
//     the SysV ABI, so they survive the `call <inner>` between SAVE and RESTORE.
//   * SAVE clobbers rax/rdx (XSAVE mask) and sets r14 = original RSP (the GPR
//     base) and r15 = the aligned XSAVE slot. The handler is invoked with RSP at
//     the slot base, so the slot [r15, r15+1088) sits ABOVE RSP and is untouched
//     by the handler's stack usage.
//   * RESTORE clobbers rax/rdx, XRSTORs from r15, then restores RSP from r14.
//
// XCR0 mask 0x7 (x87 | SSE | AVX) matches context_switch.S. Requires
// CR4.OSXSAVE + XCR0 set (CpuEnableXsaveAvx, done at boot).

// 1152 = 1088 (FxsaveArea) + 64 slack so the `and $-64` alignment-down still
// leaves a full XSAVE area.
#define BROOK_FPU_SAVE_IRQ \
    "movq %%rsp, %%r14\n\t" \
    "subq $1152, %%rsp\n\t" \
    "andq $-64, %%rsp\n\t" \
    "movq %%rsp, %%r15\n\t" \
    "xorl %%eax, %%eax\n\t" \
    "movq %%rax, 512(%%r15)\n\t" \
    "movq %%rax, 520(%%r15)\n\t" \
    "movq %%rax, 528(%%r15)\n\t" \
    "movq %%rax, 536(%%r15)\n\t" \
    "movq %%rax, 544(%%r15)\n\t" \
    "movq %%rax, 552(%%r15)\n\t" \
    "movq %%rax, 560(%%r15)\n\t" \
    "movq %%rax, 568(%%r15)\n\t" \
    "movl $7, %%eax\n\t" \
    "xorl %%edx, %%edx\n\t" \
    "xsave64 (%%r15)\n\t"

#define BROOK_FPU_RESTORE_IRQ \
    "movl $7, %%eax\n\t" \
    "xorl %%edx, %%edx\n\t" \
    "xrstor64 (%%r15)\n\t" \
    "movq %%r14, %%rsp\n\t"
