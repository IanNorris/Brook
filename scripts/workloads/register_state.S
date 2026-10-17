/* Native entry/return probe. Snapshot before any C or libc executes. */
.text
.macro LOAD_STATE
fxrstor64 register_seed_fx(%rip)
mov register_seed+0(%rip),%rax
mov register_seed+8(%rip),%rbx
mov register_seed+16(%rip),%rcx
mov register_seed+24(%rip),%rdx
mov register_seed+32(%rip),%rsi
mov register_seed+40(%rip),%rdi
mov register_seed+48(%rip),%rbp
mov register_seed+56(%rip),%r8
mov register_seed+64(%rip),%r9
mov register_seed+72(%rip),%r10
mov register_seed+80(%rip),%r11
mov register_seed+88(%rip),%r12
mov register_seed+96(%rip),%r13
mov register_seed+104(%rip),%r14
mov register_seed+112(%rip),%r15
pushq register_flags(%rip)
popfq
movq $0x35792468,-16(%rsp)
movq $0x35792468,-24(%rsp)
movq $0x35792468,-32(%rsp)
movq $0x35792468,-40(%rsp)
movq $0x35792468,-48(%rsp)
movq $0x35792468,-56(%rsp)
movq $0x35792468,-64(%rsp)
movq $0x35792468,-72(%rsp)
movq $0x35792468,-80(%rsp)
movq $0x35792468,-88(%rsp)
movq $0x35792468,-96(%rsp)
movq $0x35792468,-104(%rsp)
movq $0x35792468,-112(%rsp)
movq $0x35792468,-120(%rsp)
movq $0x35792468,-128(%rsp)
.endm
.macro SNAPSHOT
mov %rax,register_actual+0(%rip)
mov %rbx,register_actual+8(%rip)
mov %rcx,register_actual+16(%rip)
mov %rdx,register_actual+24(%rip)
mov %rsi,register_actual+32(%rip)
mov %rdi,register_actual+40(%rip)
mov %rbp,register_actual+48(%rip)
mov %r8,register_actual+56(%rip)
mov %r9,register_actual+64(%rip)
mov %r10,register_actual+72(%rip)
mov %r11,register_actual+80(%rip)
mov %r12,register_actual+88(%rip)
mov %r13,register_actual+96(%rip)
mov %r14,register_actual+104(%rip)
mov %r15,register_actual+112(%rip)
mov %rsp,register_actual+120(%rip)
pushfq
popq register_actual+128(%rip)
fxsave64 register_actual_fx(%rip)
mov -16(%rsp),%rax
mov %rax,register_actual_redzone+0(%rip)
mov -24(%rsp),%rax
mov %rax,register_actual_redzone+8(%rip)
mov -32(%rsp),%rax
mov %rax,register_actual_redzone+16(%rip)
mov -40(%rsp),%rax
mov %rax,register_actual_redzone+24(%rip)
mov -48(%rsp),%rax
mov %rax,register_actual_redzone+32(%rip)
mov -56(%rsp),%rax
mov %rax,register_actual_redzone+40(%rip)
mov -64(%rsp),%rax
mov %rax,register_actual_redzone+48(%rip)
mov -72(%rsp),%rax
mov %rax,register_actual_redzone+56(%rip)
mov -80(%rsp),%rax
mov %rax,register_actual_redzone+64(%rip)
mov -88(%rsp),%rax
mov %rax,register_actual_redzone+72(%rip)
mov -96(%rsp),%rax
mov %rax,register_actual_redzone+80(%rip)
mov -104(%rsp),%rax
mov %rax,register_actual_redzone+88(%rip)
mov -112(%rsp),%rax
mov %rax,register_actual_redzone+96(%rip)
mov -120(%rsp),%rax
mov %rax,register_actual_redzone+104(%rip)
mov -128(%rsp),%rax
mov %rax,register_actual_redzone+112(%rip)
cld
.endm
.global register_probe_syscall
.type register_probe_syscall,@function
register_probe_syscall:
push %rbx
push %rbp
push %r12
push %r13
push %r14
push %r15
mov %rsp,register_expected_rsp(%rip)
LOAD_STATE
syscall
.global register_syscall_resume
register_syscall_resume:
SNAPSHOT
pop %r15
pop %r14
pop %r13
pop %r12
pop %rbp
pop %rbx
ret
.size register_probe_syscall,.-register_probe_syscall
.global register_probe_fault
.type register_probe_fault,@function
register_probe_fault:
push %rbx
push %rbp
push %r12
push %r13
push %r14
push %r15
mov %rsp,register_expected_rsp(%rip)
LOAD_STATE
movb $0x5a,(%rdi)
SNAPSHOT
pop %r15
pop %r14
pop %r13
pop %r12
pop %rbp
pop %rbx
ret
.size register_probe_fault,.-register_probe_fault
.global register_probe_signal
.type register_probe_signal,@function
register_probe_signal:
push %rbx
push %rbp
push %r12
push %r13
push %r14
push %r15
mov %rsp,register_expected_rsp(%rip)
LOAD_STATE
ud2
SNAPSHOT
pop %r15
pop %r14
pop %r13
pop %r12
pop %rbp
pop %rbx
ret
.size register_probe_signal,.-register_probe_signal
.global register_probe_timer
.type register_probe_timer,@function
register_probe_timer:
push %rbx
push %rbp
push %r12
push %r13
push %r14
push %r15
mov %rsp,register_expected_rsp(%rip)
LOAD_STATE
1: loop 1b
SNAPSHOT
pop %r15
pop %r14
pop %r13
pop %r12
pop %rbp
pop %rbx
ret
.size register_probe_timer,.-register_probe_timer
.section .note.GNU-stack,"",@progbits
