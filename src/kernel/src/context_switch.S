// context_switch.S — Low-level context switch for Brook OS scheduler.
//
// void context_switch(SavedContext* old, SavedContext* new,
//                     FxsaveArea* oldFx, FxsaveArea* newFx);
//
// Saves callee-saved registers + SSE state into old/oldFx,
// restores from new/newFx.
// If old->cr3 != new->cr3, writes CR3 to switch page tables.
// Sets IA32_FS_BASE MSR from new->fsBase for TLS.
//
// SavedContext layout (must match process.h):
//   offset  0: r15
//   offset  8: r14
//   offset 16: r13
//   offset 24: r12
//   offset 32: rbx
//   offset 40: rbp
//   offset 48: rsp
//   offset 56: rip
//   offset 64: rflags
//   offset 72: cr3
//   offset 80: fsBase

.text
.global context_switch
.type context_switch, @function

context_switch:
    // RDI = old SavedContext*, RSI = new SavedContext*
    // RDX = old FxsaveArea*,  RCX = new FxsaveArea*

    // --- Save SSE/FPU state first (before any XMM clobber) ---
    fxsave (%rdx)

    // --- Save callee-saved GPRs ---
    movq %r15,  0(%rdi)
    movq %r14,  8(%rdi)
    movq %r13, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %rbx, 32(%rdi)
    movq %rbp, 40(%rdi)

    // Save stack pointer
    movq %rsp, 48(%rdi)

    // Save resume address
    leaq .Lresume(%rip), %rax
    movq %rax, 56(%rdi)

    // Save RFLAGS
    pushfq
    popq %rax
    movq %rax, 64(%rdi)

    // Save CR3
    movq %cr3, %rax
    movq %rax, 72(%rdi)

    // Save RCX (new FxsaveArea*) — we need it after stack switch.
    // Stash in R8 which is caller-saved and we don't need to preserve.
    movq %rcx, %r8

    // --- Restore new context ---

    // Switch CR3 if different
    movq 72(%rsi), %rax
    movq %cr3, %rcx
    cmpq %rax, %rcx
    je .Lsame_cr3
    movq %rax, %cr3
.Lsame_cr3:

    // Set FS base (IA32_FS_BASE = 0xC0000100)
    movq 80(%rsi), %rax
    testq %rax, %rax
    jz .Lskip_fsbase
    movl $0xC0000100, %ecx
    movq %rax, %rdx
    shrq $32, %rdx
    wrmsr
.Lskip_fsbase:

    // Restore RFLAGS
    movq 64(%rsi), %rax
    pushq %rax
    popfq

    // Restore callee-saved GPRs
    movq  0(%rsi), %r15
    movq  8(%rsi), %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %rbx
    movq 40(%rsi), %rbp

    // Restore stack pointer
    movq 48(%rsi), %rsp

    // Restore SSE/FPU state (R8 = new FxsaveArea*)
    fxrstor (%r8)

    // Jump to saved RIP
    jmpq *56(%rsi)

.Lresume:
    // Switched back to this context. C++ call frame is intact.
    ret

.size context_switch, . - context_switch
