// context_switch.S — Low-level context switch for Brook OS scheduler.
//
// void context_switch(SavedContext* old, SavedContext* new);
//
// Saves callee-saved registers into `old`, restores from `new`.
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
    // --- Save current (old) context ---
    // RDI = old SavedContext*, RSI = new SavedContext*

    // Save callee-saved GPRs
    movq %r15,  0(%rdi)
    movq %r14,  8(%rdi)
    movq %r13, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %rbx, 32(%rdi)
    movq %rbp, 40(%rdi)

    // Save stack pointer
    movq %rsp, 48(%rdi)

    // Save return address as RIP (caller's return address is on the stack)
    // We use a label so the old context resumes right after the switch.
    leaq .Lresume(%rip), %rax
    movq %rax, 56(%rdi)

    // Save RFLAGS
    pushfq
    popq %rax
    movq %rax, 64(%rdi)

    // Save CR3
    movq %cr3, %rax
    movq %rax, 72(%rdi)

    // --- FXSAVE old SSE/FPU state ---
    // old FxsaveArea is at offset sizeof(SavedContext) = 88 bytes in Process,
    // but we receive SavedContext*, not Process*. The caller passes
    // &proc->savedCtx, and fxsave is the next field. So fxsaveArea is at
    // savedCtx + sizeof(SavedContext) = +88.
    // Actually, let's compute it: SavedContext has 11 uint64_t = 88 bytes.
    // But FxsaveArea must be 16-byte aligned. The struct is alignas(16).
    // In the Process struct, savedCtx is followed by fxsave.
    // offset of fxsave from savedCtx = sizeof(SavedContext) padded to 16.
    // SavedContext = 88 bytes, padded to 96 for alignment.
    // So fxsave is at savedCtx + 96.
    // Wait — we should just pass the fxsave pointer separately or compute
    // from Process*. For simplicity, we'll save/restore fxsave from C++ side.
    // The asm only handles GPRs + CR3 + fsBase. FXSAVE/FXRSTOR is done by
    // the scheduler C++ code bracketing this call.

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
    // RAX already has low 32 bits (wrmsr uses EAX:EDX)
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

    // Jump to saved RIP
    jmpq *56(%rsi)

.Lresume:
    // We arrive here when this context is switched BACK to.
    // The C++ caller's stack frame is intact, so we just return.
    ret

.size context_switch, . - context_switch
