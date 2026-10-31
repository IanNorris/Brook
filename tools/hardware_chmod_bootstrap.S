/* Tiny x86-64 Linux ABI recovery bootstrap: chmod argv[1] to 0700.
 * Intended for a trusted maintenance shell and RAM-only staging; this has no
 * path restrictions and must not be exposed as an untrusted service. */
.global _start
_start:
    cmpq $2, (%rsp)
    jne fail
    mov 16(%rsp), %rdi
    mov $448, %rsi
    mov $90, %rax
    syscall
    test %rax, %rax
    js fail
    xor %rdi, %rdi
    jmp done
fail:
    mov $1, %rdi
done:
    mov $60, %rax
    syscall
