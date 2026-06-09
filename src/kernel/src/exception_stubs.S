// exception_stubs.S — Assembly ISR stubs for exceptions that need full GPR access.
//
// Full-register exception stubs save all general-purpose registers into a
// FullExceptionFrame struct, call the C handler HandleExceptionFull(), then
// restore GPRs (which the handler may have modified for signal delivery) and
// IRETQ. Vectors without a CPU error code push a synthetic zero error code so
// the C-side frame layout stays identical.
//
// FullExceptionFrame layout (must match idt.cpp):
//   [RSP+0x00] r15
//   [RSP+0x08] r14
//   [RSP+0x10] r13
//   [RSP+0x18] r12
//   [RSP+0x20] r11
//   [RSP+0x28] r10
//   [RSP+0x30] r9
//   [RSP+0x38] r8
//   [RSP+0x40] rbp
//   [RSP+0x48] rdi
//   [RSP+0x50] rsi
//   [RSP+0x58] rdx
//   [RSP+0x60] rcx
//   [RSP+0x68] rbx
//   [RSP+0x70] rax
//   [RSP+0x78] errorCode   (pushed by CPU)
//   [RSP+0x80] rip         (pushed by CPU)
//   [RSP+0x88] cs          (pushed by CPU)
//   [RSP+0x90] rflags      (pushed by CPU)
//   [RSP+0x98] rsp         (pushed by CPU, only for ring 3→0 transition)
//   [RSP+0xA0] ss          (pushed by CPU, only for ring 3→0 transition)

.text

// BRO-178 paranoid SWAPGS (see gs_paranoid.h for the full rationale): decide
// swapgs by the ACTUAL GS base via rdmsr, not the saved CS RPL, so an interrupt
// landing in a ring-0 window with user GS still live swaps correctly. Carries
// the "did we swap" decision in RBX (callee-saved, preserved across the C call;
// used after GPRs are pushed and before they are popped). \@ gives each macro
// expansion unique local labels.
.macro GS_PARANOID_ENTRY
    movl $0xC0000101, %ecx       // IA32_GS_BASE
    rdmsr                         // edx:eax = base
    testl $0x80000000, %edx       // kernel base is canonical-high (EDX bit 31 set)
    jnz .Lpgs_kernel\@            // already kernel GS: no swap
    swapgs
    movl $1, %ebx                 // flag = swapped
    jmp .Lpgs_done\@
.Lpgs_kernel\@:
    xorl %ebx, %ebx               // flag = no swap
.Lpgs_done\@:
.endm

.macro GS_PARANOID_EXIT
    testl %ebx, %ebx
    jz .Lpgs_noswap\@
    swapgs
.Lpgs_noswap\@:
.endm

// Macro for an exception without a CPU-pushed error code.
.macro EXCEPTION_STUB_NOERR vector
.global ExceptionStub\vector
.type ExceptionStub\vector, @function
ExceptionStub\vector:
    // Synthesize an error_code slot so FullExceptionFrame matches the error
    // code exceptions: errorCode, rip, cs, rflags, rsp, ss.
    pushq $0

    // Push all GPRs to form FullExceptionFrame
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

        // BRO-178: paranoid swapgs by actual GS base (see gs_paranoid.h)
    GS_PARANOID_ENTRY

    movq %rsp, %rdi
    movq $\vector, %rsi

    movq %rsp, %rbp
    andq $-16, %rsp
    subq $8, %rsp
    push %rbp

    call HandleExceptionFull

    pop %rsp
    // BRO-178: paranoid swapgs restore (see gs_paranoid.h)
    GS_PARANOID_EXIT

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    // Remove synthetic error code
    addq $8, %rsp

    iretq
.endm

// Macro for an exception with error code.
// The CPU pushes: [SS, RSP, RFLAGS, CS, RIP, ErrorCode]
// We push all GPRs, call the C handler, and restore.
.macro EXCEPTION_STUB_ERR vector
.global ExceptionStub\vector
.type ExceptionStub\vector, @function
ExceptionStub\vector:
    // CPU has already pushed: error_code, rip, cs, rflags, rsp, ss
    // Push all GPRs to form FullExceptionFrame
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

        // BRO-178: paranoid swapgs by actual GS base (see gs_paranoid.h)
    GS_PARANOID_ENTRY

    // RDI = pointer to FullExceptionFrame (our pushed regs + CPU frame)
    movq %rsp, %rdi
    // RSI = vector number
    movq $\vector, %rsi

    // Align stack to 16 bytes for C calling convention.
    // We need RSP at the moment of `call` to be 0-mod-16 so the callee
    // sees the SysV-mandated 8-mod-16. After `and $-16, %rsp` RSP is
    // 0-mod-16; `push %rbp` would leave it at 8-mod-16, and the call's
    // own 8-byte push would give the callee 0-mod-16 — every downstream
    // function then gets a misaligned stack and any movaps to a local
    // ends in #GP. Add an 8-byte pad so the saved-rsp push lands on
    // 0-mod-16.
    movq %rsp, %rbp
    andq $-16, %rsp
    subq $8, %rsp      // pad so RSP is 0-mod-16 at the `call`
    push %rbp          // save original RSP (popped on return)

    call HandleExceptionFull

    pop %rsp           // restore original RSP (FullExceptionFrame pointer)
    // BRO-178: paranoid swapgs restore (see gs_paranoid.h)
    GS_PARANOID_EXIT

    // Restore all GPRs (handler may have modified them for signal delivery)
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    // Remove error code
    addq $8, %rsp

    iretq
.endm

// Generate stubs for exceptions that need full register capture / QR panic.
EXCEPTION_STUB_NOERR 6
EXCEPTION_STUB_ERR 13
EXCEPTION_STUB_ERR 14
