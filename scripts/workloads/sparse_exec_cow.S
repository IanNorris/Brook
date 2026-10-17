/* Static Linux-ABI probe for sparse ELF loading and real fork/COW isolation.
 * Build with sparse_exec_cow.ld; no libc or runtime dependencies. */
.global _start
.section .text
_start:
    mov $57, %eax                 /* fork */
    syscall
    test %rax, %rax
    js fail_fork
    jz child
    mov %rax, %r12
    sub $16, %rsp
    mov %r12, %rdi
    mov %rsp, %rsi
    xor %edx, %edx
    xor %r10d, %r10d
    mov $61, %eax                 /* wait4 */
    syscall
    cmp %r12, %rax
    jne fail_wait
    cmpl $0, (%rsp)
    jne fail_wait
    cmpq $0x12345678, data_value(%rip)
    jne fail_cow
    cmpq $0, bss_value(%rip)
    jne fail_cow
    cmpb $0x42, text_value(%rip)
    jne fail_cow
    mov $0x800000, %edi           /* virtual gap between load segments */
    mov $4096, %esi
    mov $1, %edx
    mov $10, %eax                 /* mprotect: must reject the hole */
    syscall
    cmp $-12, %rax                /* -ENOMEM */
    jne fail_hole
    mov $1, %eax
    mov $1, %edi
    lea passed(%rip), %rsi
    mov $(passed_end-passed), %edx
    syscall
    xor %edi, %edi
    jmp exit
child:
    movq $0x76543210, data_value(%rip)
    movq $0x11111111, bss_value(%rip)
    /* Existing loader policy permits writes to Image pages. This also
     * exercises a full initialized page reused from staging storage. */
    movb $0x24, text_value(%rip)
    cmpq $0x76543210, data_value(%rip)
    jne fail_cow
    cmpq $0x11111111, bss_value(%rip)
    jne fail_cow
    cmpb $0x24, text_value(%rip)
    jne fail_cow
    xor %edi, %edi
    jmp exit
fail_fork:
    mov $71, %edi
    jmp exit
fail_wait:
    mov $72, %edi
    jmp exit
fail_hole:
    mov $73, %edi
    jmp exit
fail_cow:
    mov $74, %edi
exit:
    mov $60, %eax
    syscall
    ud2
.section .rodata
text_value: .byte 0x42
passed: .ascii "SPARSE_EXEC_COW PASS\n"
passed_end:
.section .data
data_value: .quad 0x12345678
.section .bss
.balign 8
bss_value: .skip 8
.section .note.GNU-stack,"",@progbits
