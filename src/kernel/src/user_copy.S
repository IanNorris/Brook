// SysV: RDI destination, RSI source, RDX bytes. Return bytes remaining.
// Each labeled instruction has exactly one local exception return. No stack
// ownership is bypassed: the fault handler redirects only to that stub's RET.
.text
.p2align 4
.global BrookCopyFromUserRaw
.type BrookCopyFromUserRaw,@function
BrookCopyFromUserRaw:
    movq %rdx,%rcx
    cld
.global BrookCopyFromUserInstruction
BrookCopyFromUserInstruction:
    rep movsb
    xorl %eax,%eax
    ret
.global BrookCopyFromUserFixup
BrookCopyFromUserFixup:
    movq %rcx,%rax
    ret
.size BrookCopyFromUserRaw,.-BrookCopyFromUserRaw

.p2align 4
.global BrookCopyToUserRaw
.type BrookCopyToUserRaw,@function
BrookCopyToUserRaw:
    movq %rdx,%rcx
    cld
.global BrookCopyToUserInstruction
BrookCopyToUserInstruction:
    rep movsb
    xorl %eax,%eax
    ret
.global BrookCopyToUserFixup
BrookCopyToUserFixup:
    movq %rcx,%rax
    ret
.size BrookCopyToUserRaw,.-BrookCopyToUserRaw
.section .note.GNU-stack,"",@progbits
