// ============================================================================
// AP Trampoline — real mode (16-bit) → protected (32-bit) → long mode (64-bit)
//
// This code is copied to a low-memory page (< 1MB) at runtime.
// The BSP patches the data block at known offsets before sending SIPIs.
//
// Register convention through the transitions:
//   EBX = physical base address of this page (computed in 16-bit, used in 32-bit)
// ============================================================================

.code16
.section .text
.globl _ap_trampoline_start
.globl _ap_trampoline_end
.globl _ap_data_block

_ap_trampoline_start:

ap_entry_16:
    cli
    cld

    // Compute page physical base: CS * 16
    xor     %eax, %eax
    mov     %cs, %ax
    mov     %ax, %ds                   // DS = CS for data access
    shl     $4, %eax                   // EAX = linear base address
    mov     %eax, %ebx                 // EBX = page_base (kept through 32-bit)

    // Patch the temporary GDT descriptor's base address
    // ap_gdt_desc.base = page_base + (ap_gdt_null - _ap_trampoline_start)
    add     $(ap_gdt_null - _ap_trampoline_start), %eax
    mov     %eax, (ap_gdt_desc_base - _ap_trampoline_start)

    // Load temporary 32-bit GDT
    lgdt    (ap_gdt_desc - _ap_trampoline_start)

    // Enable protected mode (PE bit in CR0)
    mov     %cr0, %eax
    or      $1, %eax
    mov     %eax, %cr0

    // Compute absolute address of ap_entry_32 for far jump
    lea     (ap_entry_32 - _ap_trampoline_start), %eax
    add     %ebx, %eax
    mov     %eax, (ap_jmp32_target - _ap_trampoline_start)

    .byte   0x66, 0xEA                 // 32-bit far jump
ap_jmp32_target:
    .long   0                          // patched above
    .word   0x08                       // GDT code32 selector

// ============================================================================
.code32
.align 4
ap_entry_32:
    // EBX = page_base (preserved from 16-bit code)

    // Load 32-bit data segment (flat, base=0)
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %ss
    mov     %ax, %es

    // Now in 32-bit flat mode. All addresses must be page_base + offset.
    // EBX holds page_base; use it to access the data block.

    // Temp stack (32-bit, in low memory — patched by BSP)
    mov     (ap_data_tmp_stack - _ap_trampoline_start)(%ebx), %esp

    // Load PML4 into CR3
    mov     (ap_data_pml4 - _ap_trampoline_start)(%ebx), %eax
    mov     %eax, %cr3

    // Enable PAE (CR4 bit 5)
    mov     %cr4, %eax
    or      $(1 << 5), %eax
    mov     %eax, %cr4

    // Enable long mode + NX via IA32_EFER MSR
    mov     $0xC0000080, %ecx
    rdmsr
    or      $(1 << 8), %eax           // LME
    or      $(1 << 11), %eax          // NXE
    wrmsr

    // Enable paging (CR0 bit 31)
    mov     %cr0, %eax
    or      $(1 << 31), %eax
    mov     %eax, %cr0

    // Far jump to 64-bit code using the temp GDT's code64 selector (0x18).
    // The temp GDT is already loaded (from 16-bit), and has a 64-bit code
    // descriptor at offset 0x18.
    lea     (ap_entry_64 - _ap_trampoline_start)(%ebx), %eax
    mov     %eax, (ap_jmp64_target - _ap_trampoline_start)(%ebx)

    .byte   0xEA                       // far jump
ap_jmp64_target:
    .long   0                          // patched above
    .word   0x18                       // temp GDT code64 selector

// ============================================================================
.code64
.align 8
ap_entry_64:
    // Now in 64-bit long mode with temp GDT and paging.

    // Set up data segments from temp GDT
    mov     $0x10, %ax                 // temp GDT data selector
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    xor     %ax, %ax
    mov     %ax, %fs
    mov     %ax, %gs

    // EBX still holds page_base (zero-extended to RBX in 64-bit mode)
    movl    %ebx, %ebx                 // zero-extend EBX → RBX

    // Now load the real kernel GDT (64-bit lgdt reads 10-byte descriptor)
    lgdt    (ap_data_gdt64 - _ap_trampoline_start)(%rbx)

    // Reload CS with kernel code selector via far return
    pushq   $0x08                      // kernel code selector
    lea     1f(%rip), %rax
    pushq   %rax
    lretq
1:
    // Reload data segments with kernel selectors
    mov     $0x10, %ax                 // kernel data selector
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    xor     %ax, %ax
    mov     %ax, %fs
    mov     %ax, %gs

    // Load the kernel IDT
    lidt    (ap_data_idt - _ap_trampoline_start)(%rbx)

    // Switch to the real kernel stack (64-bit, high memory)
    mov     (ap_data_stack64 - _ap_trampoline_start)(%rbx), %rsp
    mov     %rsp, %rbp

    // Jump to the C++ AP entry function (address in high kernel space)
    mov     (ap_data_entry_fn - _ap_trampoline_start)(%rbx), %rax
    jmp     *%rax

    hlt

// ============================================================================
// Temporary 32-bit GDT (only for 16→32 transition)
// ============================================================================
.align 16
ap_gdt_null:
    .quad   0x0000000000000000         // null
ap_gdt_code32:
    .quad   0x00CF9A000000FFFF         // 0x08: 32-bit code, base=0, limit=4GB
ap_gdt_data32:
    .quad   0x00CF92000000FFFF         // 0x10: 32-bit data, base=0, limit=4GB
ap_gdt_code64:
    .quad   0x00209A0000000000         // 0x18: 64-bit code (L=1, D=0)

ap_gdt_desc:
    .word   (4 * 8) - 1               // limit = 4 entries * 8 bytes - 1
ap_gdt_desc_base:
    .long   0                          // base — patched at runtime

// ============================================================================
// Data block — patched by the BSP before sending SIPIs.
// The C++ code uses offsetof-style calculations to find these fields.
// ============================================================================
.align 8
_ap_data_block:

ap_data_pml4:
    .long   0                          // [+0]  CR3 value (PML4 physical address, 32-bit)

ap_data_tmp_stack:
    .long   0                          // [+4]  32-bit temp stack pointer (physical)

.align 8
ap_data_stack64:
    .quad   0                          // [+8]  64-bit kernel stack top

ap_data_entry_fn:
    .quad   0                          // [+16] 64-bit C++ entry function

// Kernel GDT pointer (limit:2 + base:8 = 10 bytes, used by lgdt)
.align 2
ap_data_gdt64:
    .word   0                          // [+24] GDT limit
    .quad   0                          // [+26] GDT base

// Kernel IDT pointer
.align 2
ap_data_idt:
    .word   0                          // [+34] IDT limit
    .quad   0                          // [+36] IDT base

_ap_trampoline_end:

