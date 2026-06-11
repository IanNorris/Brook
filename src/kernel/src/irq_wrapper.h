#pragma once

#include "gs_paranoid.h"
#include "fpu_irq.h"

// IRQ_NAKED_HANDLER(name, inner_fn)
//
// Generates a naked ISR wrapper that:
//   1. Saves all 15 GPRs
//   2. BRO-178 paranoid SWAPGS: swaps to kernel GS iff the ACTUAL GS base is a
//      user base (decided by rdmsr, not the saved CS RPL), carrying the
//      did-swap flag in RBX
//   3. Calls inner_fn() — a plain void(void) C function
//   4. Paranoid SWAPGS restore (swaps back iff entry swapped)
//   5. Restores all GPRs
//   6. iretq
//
// This is required for ANY interrupt handler that accesses gs-relative
// per-CPU data (spinlocks, ProcessCurrent, cpuIndex, etc).  The old
// __attribute__((interrupt)) stubs did NOT perform SWAPGS, so when they
// fired from user mode, gs:N reads went to user memory instead of the
// kernel's per-CPU KernelCpuEnv — causing #PF or silent corruption.
//
// The inner function must be extern "C" so its symbol name is usable
// in the inline asm call instruction.
// It must handle EOI itself (call ApicSendEoi()).
//
// Usage:
//   extern "C" void MyIrqInner(void) { ... ApicSendEoi(); }
//   IRQ_NAKED_HANDLER(MyIrq, MyIrqInner)
//   // Then install &MyIrq as the IDT handler.

#define IRQ_NAKED_HANDLER(name, inner_fn) \
    __attribute__((naked)) \
    static void name(void) \
    { \
        __asm__ volatile( \
            /* Save all GPRs */ \
            "push %%rax\n\t" \
            "push %%rbx\n\t" \
            "push %%rcx\n\t" \
            "push %%rdx\n\t" \
            "push %%rsi\n\t" \
            "push %%rdi\n\t" \
            "push %%rbp\n\t" \
            "push %%r8\n\t" \
            "push %%r9\n\t" \
            "push %%r10\n\t" \
            "push %%r11\n\t" \
            "push %%r12\n\t" \
            "push %%r13\n\t" \
            "push %%r14\n\t" \
            "push %%r15\n\t" \
            /* BRO-178 paranoid swapgs by actual GS base (ebx = did-swap flag) */ \
            GS_PARANOID_ENTRY_EBX \
            /* BRO-187: save interrupted x87/SSE/AVX state (r14=orig rsp, \
               r15=XSAVE slot) before the handler clobbers vector registers. */ \
            BROOK_FPU_SAVE_IRQ \
            "cld\n\t" \
            "call " #inner_fn "\n\t" \
            /* BRO-187: restore FPU state after the handler, before swapgs. */ \
            BROOK_FPU_RESTORE_IRQ \
            /* BRO-178 paranoid swapgs restore before popping GPRs */ \
            GS_PARANOID_EXIT_EBX \
            /* Restore GPRs */ \
            "pop %%r15\n\t" \
            "pop %%r14\n\t" \
            "pop %%r13\n\t" \
            "pop %%r12\n\t" \
            "pop %%r11\n\t" \
            "pop %%r10\n\t" \
            "pop %%r9\n\t" \
            "pop %%r8\n\t" \
            "pop %%rbp\n\t" \
            "pop %%rdi\n\t" \
            "pop %%rsi\n\t" \
            "pop %%rdx\n\t" \
            "pop %%rcx\n\t" \
            "pop %%rbx\n\t" \
            "pop %%rax\n\t" \
            "iretq\n\t" \
            : \
            : \
            : "memory" \
        ); \
    }
