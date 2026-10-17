ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(7);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x400000;
    .text : { *(.text) *(.rodata) BYTE(0); . = ALIGN(4096); } :text
    . = 0x1000000;
    .data : { *(.data) . = ALIGN(4096); } :data
    .bss (NOLOAD) : { *(.bss) . = ALIGN(4096); } :data
    /DISCARD/ : { *(.note*) *(.eh_frame*) }
}
