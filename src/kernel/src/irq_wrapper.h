#pragma once

// IRQ_NAKED_HANDLER(name, inner_fn)
//
// Generates a naked ISR wrapper that:
//   1. Does SWAPGS if interrupted from ring 3 (user mode)
//   2. Saves all 15 GPRs
//   3. Calls inner_fn() — a plain void(void) C function
//   4. Restores all GPRs
//   5. Does SWAPGS back if returning to ring 3
//   6. iretq
//
// This is required for ANY interrupt handler that accesses gs-relative
// per-CPU data (spinlocks, ProcessCurrent, cpuIndex, etc).  The old
// __attribute__((interrupt)) stubs did NOT perform SWAPGS, so when they
// fired from user mode, gs:N reads went to user memory instead of the
// kernel's per-CPU KernelCpuEnv — causing #PF or silent corruption.
//
// The inner function must NOT be __attribute__((interrupt)).
// It must handle EOI itself (call ApicSendEoi()).
//
// Usage:
//   static void MyIrqInner(void) { ... ApicSendEoi(); }
//   IRQ_NAKED_HANDLER(MyIrq, MyIrqInner)
//   // Then install &MyIrq as the IDT handler.

#define IRQ_NAKED_HANDLER(name, inner_fn) \
    __attribute__((naked)) \
    static void name(void) \
    { \
        __asm__ volatile( \
            /* SWAPGS if interrupted from ring 3 */ \
            "testq $3, 8(%%rsp)\n\t" \
            "jz 1f\n\t" \
            "swapgs\n\t" \
            "1:\n\t" \
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
            "cld\n\t" \
            "call %P0\n\t" \
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
            /* SWAPGS back if returning to ring 3 */ \
            "testq $3, 8(%%rsp)\n\t" \
            "jz 2f\n\t" \
            "swapgs\n\t" \
            "2:\n\t" \
            "iretq\n\t" \
            : \
            : "i"(inner_fn) \
            : "memory" \
        ); \
    }
