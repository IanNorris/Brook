/* Exercise the TEST/LEA/CMOVE sequence at the saved ld.so bootstrap fault.
 * No syscall or memory dereference occurs inside either critical window.
 * The high addresses are arithmetic inputs, never pointers to access. */
.text
.global loader_cmov_probe
.type loader_cmov_probe,@function
loader_cmov_probe:
    push %r14
    push %r15
    mov %rdi,%r15
    mov %rsi,%rdx
    movabs $0x7f0000000000,%r14
    movabs $0x7f0000000d18,%r8
1:
    xor %r10d,%r10d
    mov %r8,%rax
    xor %edi,%edi
    test %dil,%dil
    lea (%r14,%rax),%rcx
#ifdef BROOK_CMOV_NEGATIVE_CONTROL
    or $1,%r9d
#endif
    cmove %rax,%rcx
    cmp %r8,%rcx
    jne 3f
    mov $1,%r10d
    mov $0xd18,%eax
    mov $0x20,%edi
    test %dil,%dil
    lea (%r14,%rax),%rcx
    cmove %rax,%rcx
    cmp %r8,%rcx
    jne 3f
    sub $1,%rdx
    jnz 1b
    xor %eax,%eax
2:
    pop %r15
    pop %r14
    ret
3:
    mov %rax,0(%r15)
    mov %rcx,8(%r15)
    mov %r14,16(%r15)
    pushfq
    popq 24(%r15)
    mov %r10,32(%r15)
    mov %rdx,40(%r15)
    mov $1,%eax
    jmp 2b
.size loader_cmov_probe,.-loader_cmov_probe
.section .note.GNU-stack,"",@progbits
