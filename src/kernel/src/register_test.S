// Test-kernel-only CPL0 probes. No runtime IDT replacement or live device calls.
.text
.macro LOAD_STATE
fxrstor64 kr_seed_fx(%rip)
mov kr_seed+0(%rip),%rax
mov kr_seed+8(%rip),%rbx
mov kr_seed+16(%rip),%rcx
mov kr_seed+24(%rip),%rdx
mov kr_seed+32(%rip),%rsi
mov kr_seed+40(%rip),%rdi
mov kr_seed+48(%rip),%rbp
mov kr_seed+56(%rip),%r8
mov kr_seed+64(%rip),%r9
mov kr_seed+72(%rip),%r10
mov kr_seed+80(%rip),%r11
mov kr_seed+88(%rip),%r12
mov kr_seed+96(%rip),%r13
mov kr_seed+104(%rip),%r14
mov kr_seed+112(%rip),%r15
pushq $0xcd7
popfq
.endm
.macro SNAPSHOT
mov %rax,kr_actual+0(%rip)
mov %rbx,kr_actual+8(%rip)
mov %rcx,kr_actual+16(%rip)
mov %rdx,kr_actual+24(%rip)
mov %rsi,kr_actual+32(%rip)
mov %rdi,kr_actual+40(%rip)
mov %rbp,kr_actual+48(%rip)
mov %r8,kr_actual+56(%rip)
mov %r9,kr_actual+64(%rip)
mov %r10,kr_actual+72(%rip)
mov %r11,kr_actual+80(%rip)
mov %r12,kr_actual+88(%rip)
mov %r13,kr_actual+96(%rip)
mov %r14,kr_actual+104(%rip)
mov %r15,kr_actual+112(%rip)
mov %rsp,kr_actual+120(%rip)
pushfq
popq kr_actual+128(%rip)
fxsave64 kr_actual_fx(%rip)
cld
mov 0(%rsp),%rax
mov %rax,kr_actual_guard+0(%rip)
mov 8(%rsp),%rax
mov %rax,kr_actual_guard+8(%rip)
mov 16(%rsp),%rax
mov %rax,kr_actual_guard+16(%rip)
mov 24(%rsp),%rax
mov %rax,kr_actual_guard+24(%rip)
.endm
.global kr_probe_irq
.type kr_probe_irq,@function
kr_probe_irq:
push %rbx
push %rbp
push %r12
push %r13
push %r14
push %r15
sub $32,%rsp
movq $0x35792468,0(%rsp)
movq $0x35792468,8(%rsp)
movq $0x35792468,16(%rsp)
movq $0x35792468,24(%rsp)
mov %rsp,kr_expected_rsp(%rip)
LOAD_STATE
.global kr_irq_instruction
kr_irq_instruction:
pushq $0x10
pushq kr_expected_rsp(%rip)
pushfq
pushq $0x08
pushq $kr_irq_resume
jmp *kr_irq_target(%rip)
.global kr_irq_resume
kr_irq_resume:
SNAPSHOT
add $32,%rsp
pop %r15
pop %r14
pop %r13
pop %r12
pop %rbp
pop %rbx
ret
.size kr_probe_irq,.-kr_probe_irq
.global kr_probe_ipi
.type kr_probe_ipi,@function
kr_probe_ipi:
push %rbx
push %rbp
push %r12
push %r13
push %r14
push %r15
sub $32,%rsp
movq $0x35792468,0(%rsp)
movq $0x35792468,8(%rsp)
movq $0x35792468,16(%rsp)
movq $0x35792468,24(%rsp)
mov %rsp,kr_expected_rsp(%rip)
LOAD_STATE
.global kr_ipi_instruction
kr_ipi_instruction:
sti
1: loop 1b
pushfq
popq kr_ipi_return_flags(%rip)
cli
.global kr_ipi_resume
kr_ipi_resume:
SNAPSHOT
add $32,%rsp
pop %r15
pop %r14
pop %r13
pop %r12
pop %rbp
pop %rbx
ret
.size kr_probe_ipi,.-kr_probe_ipi
.global kr_probe_ud
.type kr_probe_ud,@function
kr_probe_ud:
push %rbx
push %rbp
push %r12
push %r13
push %r14
push %r15
sub $32,%rsp
movq $0x35792468,0(%rsp)
movq $0x35792468,8(%rsp)
movq $0x35792468,16(%rsp)
movq $0x35792468,24(%rsp)
mov %rsp,kr_expected_rsp(%rip)
LOAD_STATE
.global kr_ud_instruction
kr_ud_instruction:
ud2
.global kr_ud_resume
kr_ud_resume:
SNAPSHOT
add $32,%rsp
pop %r15
pop %r14
pop %r13
pop %r12
pop %rbp
pop %rbx
ret
.size kr_probe_ud,.-kr_probe_ud
.global kr_probe_gp
.type kr_probe_gp,@function
kr_probe_gp:
push %rbx
push %rbp
push %r12
push %r13
push %r14
push %r15
sub $32,%rsp
movq $0x35792468,0(%rsp)
movq $0x35792468,8(%rsp)
movq $0x35792468,16(%rsp)
movq $0x35792468,24(%rsp)
mov %rsp,kr_expected_rsp(%rip)
LOAD_STATE
.global kr_gp_instruction
kr_gp_instruction:
mov %ax,%ds
.global kr_gp_resume
kr_gp_resume:
SNAPSHOT
add $32,%rsp
pop %r15
pop %r14
pop %r13
pop %r12
pop %rbp
pop %rbx
ret
.size kr_probe_gp,.-kr_probe_gp
.global kr_probe_pf
.type kr_probe_pf,@function
kr_probe_pf:
push %rbx
push %rbp
push %r12
push %r13
push %r14
push %r15
sub $32,%rsp
movq $0x35792468,0(%rsp)
movq $0x35792468,8(%rsp)
movq $0x35792468,16(%rsp)
movq $0x35792468,24(%rsp)
mov %rsp,kr_expected_rsp(%rip)
LOAD_STATE
.global kr_pf_instruction
kr_pf_instruction:
movb $0x5a,(%rdi)
.global kr_pf_resume
kr_pf_resume:
SNAPSHOT
add $32,%rsp
pop %r15
pop %r14
pop %r13
pop %r12
pop %rbp
pop %rbx
ret
.size kr_probe_pf,.-kr_probe_pf
// Deliberately destroy ABI caller-saved registers and vector state.
.global kr_clobber
.type kr_clobber,@function
kr_clobber:
mov %rsp,kr_inner_rsp(%rip)
pushfq
popq kr_inner_flags(%rip)
incq kr_arrivals(%rip)
mov $0x76543210,%rax
mov $0x76543210,%rcx
mov $0x76543210,%rdx
mov $0x76543210,%rsi
mov $0x76543210,%rdi
mov $0x76543210,%r8
mov $0x76543210,%r9
mov $0x76543210,%r10
mov $0x76543210,%r11
pxor %xmm0,%xmm0
pxor %xmm1,%xmm1
pxor %xmm2,%xmm2
pxor %xmm3,%xmm3
pxor %xmm4,%xmm4
pxor %xmm5,%xmm5
pxor %xmm6,%xmm6
pxor %xmm7,%xmm7
pxor %xmm8,%xmm8
pxor %xmm9,%xmm9
pxor %xmm10,%xmm10
pxor %xmm11,%xmm11
pxor %xmm12,%xmm12
pxor %xmm13,%xmm13
pxor %xmm14,%xmm14
pxor %xmm15,%xmm15
fninit
ret
.size kr_clobber,.-kr_clobber
.section .note.GNU-stack,"",@progbits
