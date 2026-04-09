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

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(4096);
    __kernel_end = .;
}
