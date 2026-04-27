/* Brook kernel linker script - high virtual address */
ENTRY(KernelMain)

SECTIONS
{
    . = 0xFFFFFFFF80000000;

    .text : {
        *(.text .text.*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    }

    /* Kernel symbol export table — bounded by __start/__stop for KsymLookup. */
    .ksymtab : ALIGN(8) {
        __start_ksymtab = .;
        *(.ksymtab)
        __stop_ksymtab = .;
    }

    .data : ALIGN(4096) {
        *(.data .data.*)
    }

    /* Syscall dispatch table — its own page so we can mark it
       read-only after SyscallTableInit. Bounded by start/stop
       symbols so the runtime knows the exact page range. */
    .syscall_table : ALIGN(4096) {
        __syscall_table_start = .;
        *(.syscall_table)
        . = ALIGN(4096);
        __syscall_table_end = .;
    }

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(4096);
    __kernel_end = .;
}
