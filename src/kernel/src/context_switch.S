// context_switch.S — Low-level context switch for Brook OS scheduler.
//
// void context_switch(SavedContext* old, SavedContext* new,
//                     FxsaveArea* oldFx, FxsaveArea* newFx,
//                     volatile int32_t* oldRunningOnCpu);
//
// Saves callee-saved registers + x87/SSE/AVX state into old/oldFx,
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
    // R8  = &oldProc->runningOnCpu; published as -1 only after old context is saved

    // --- Save x87/SSE/AVX state first (before any vector clobber) ---
    movq %rdx, %r9
    movl $0x7, %eax              // XCR0 mask: x87 | SSE | AVX
    xorl %edx, %edx
    xsave64 (%r9)

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

    // Save FS base (IA32_FS_BASE = 0xC0000100) — must happen before
    // we clobber RCX/RDX. Keep newFx in R10; R8 still points at
    // oldProc->runningOnCpu.
    movq %rcx, %r10             // stash new FxsaveArea*
    movl $0xC0000100, %ecx
    rdmsr
    movl %eax, %ecx
    shlq $32, %rdx
    orq  %rcx, %rdx
    movq %rdx, 80(%rdi)         // old->fsBase

    // The old process is no longer running only after its register context is
    // fully saved. Publishing this earlier lets another CPU schedule it from a
    // stale SavedContext while this CPU is still inside context_switch.
    movl $-1, (%r8)

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

    // Restore RFLAGS — but keep IF cleared so a timer interrupt cannot
    // fire in the middle of the register restore.  Interrupts are properly
    // re-enabled later by iretq (preempted process) or sti (new process
    // via ProcessTrampoline).
    movq 64(%rsi), %rax
    andq $~0x200, %rax          // clear IF bit
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

    // Restore x87/SSE/AVX state (R10 = new FxsaveArea*)
    movl $0x7, %eax              // XCR0 mask: x87 | SSE | AVX
    xorl %edx, %edx
    xrstor64 (%r10)

    // Validate saved RIP is a canonical kernel address before jumping.
    movq 56(%rsi), %rax
    movq %rax, %rcx
    shrq $47, %rcx
    cmpq $0x1FFFF, %rcx
    je .Lrip_ok
    // RIP is corrupted — halt.
    cli
    ud2
.Lrip_ok:
    // Jump to saved RIP
    jmpq *56(%rsi)

.Lresume:
    // Switched back to this context. C++ call frame is intact.
    ret

.size context_switch, . - context_switch
