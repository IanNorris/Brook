#pragma once

// ---------------------------------------------------------------------------
// BRO-178: paranoid SWAPGS — decide by the ACTUAL GS base, not the saved CS.
// ---------------------------------------------------------------------------
//
// Every interrupt/IPI/exception entry historically chose whether to SWAPGS by
// testing the saved CS RPL (`testq $3, CS; swapgs`). That assumes "interrupted
// ring 0 ⇒ kernel GS is already active", which is FALSE for any ring-0 instant
// where the user GS base is still live — the window between a return-path
// `swapgs` and its `iretq`/`sysret`, in either direction. An interrupt landing
// in that window saw a kernel CS, skipped swapgs, and ran the handler with the
// user GS base (0) → a #PF at gs:176 / CR2=0xB0 (the recurring BRO-166-class
// crash). `cli` discipline cannot close this for NMI / #MC / #DB, which ignore
// IF — so the CS-RPL test is unsound by construction, not merely fragile.
//
// The durable fix (paranoid entry): read IA32_GS_BASE and swap iff it is a
// user (non-canonical-high) base, regardless of CS. The kernel GS base is a
// canonical high-half KernelCpuEnv* (>= 0xFFFF800000000000, so EDX bit 31 set
// after rdmsr); the user GS base is 0 (or any low address). A matching
// paranoid exit swaps back iff entry swapped.
//
// The entry/exit "did we swap" decision is carried in RBX:
//   * RBX is callee-saved, so the C inner handler preserves it across its call,
//     and context_switch saves/restores it via SavedContext — so the flag
//     survives even the timer ISR's preempt-and-switch path.
//   * The macros run AFTER the handler has pushed all GPRs (so the interrupted
//     RBX is safely on the stack) and BEFORE it pops them (so the original RBX
//     is restored on return). rdmsr clobbers RAX/RCX/RDX, which are likewise
//     already saved at that point.
//
// The dangerous case is always ring0→ring0 (no stack switch), so there is no
// GS-bootstrap chicken-and-egg for the rdmsr scratch — we are already on a
// valid kernel stack.
//
// IA32_GS_BASE = 0xC0000101. Kernel-base discriminator: EDX bit 31 (0x80000000)
// set ⇔ canonical-high ⇔ kernel GS already active ⇔ no swap needed.

// --- Inline-asm form (apic.cpp naked handlers, irq_wrapper.h) --------------
// Numeric labels 71/72/73 are used with f/b refs so they are safe to reuse
// across multiple asm blocks in one translation unit.
#define GS_PARANOID_ENTRY_EBX \
    "movl $0xC0000101, %%ecx\n\t" \
    "rdmsr\n\t" \
    "testl $0x80000000, %%edx\n\t" \
    "jnz 71f\n\t"            /* kernel GS already active: no swap */ \
    "swapgs\n\t" \
    "movl $1, %%ebx\n\t"     /* flag = swapped */ \
    "jmp 72f\n\t" \
    "71:\n\t" \
    "xorl %%ebx, %%ebx\n\t"  /* flag = did not swap */ \
    "72:\n\t"

#define GS_PARANOID_EXIT_EBX \
    "testl %%ebx, %%ebx\n\t" \
    "jz 73f\n\t" \
    "swapgs\n\t" \
    "73:\n\t"
