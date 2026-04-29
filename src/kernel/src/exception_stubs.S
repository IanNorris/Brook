// exception_stubs.S — Assembly ISR stubs for exceptions that need full GPR access.
//
// Vectors 13 (#GP) and 14 (#PF) push an error code. The stubs save all
// general-purpose registers into a FullExceptionFrame struct, call the C
// handler HandleExceptionFull(), then restore GPRs (which the handler may
// have modified for signal delivery) and IRETQ.
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

    // SWAPGS if coming from user mode (check CS on the interrupt frame)
    // CS is at [RSP + 0x88] = 15 GPRs * 8 + errorCode(8) + RIP(8) = 0x88
    testq $3, 0x88(%rsp)
    jz 1f
    swapgs
1:

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

    // SWAPGS back if returning to user mode
    testq $3, 0x88(%rsp)
    jz 2f
    swapgs
2:

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

// Generate stubs for #GP (13) and #PF (14)
EXCEPTION_STUB_ERR 13
EXCEPTION_STUB_ERR 14
