OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(__vdso_clock_gettime)
PHDRS {
    code PT_LOAD FLAGS(5) FILEHDR PHDRS;
    dynamic PT_DYNAMIC FLAGS(4);
    stack PT_GNU_STACK FLAGS(6);
}
SECTIONS {
    . = SIZEOF_HEADERS;
    .hash : { *(.hash) } :code
    .gnu.hash : { *(.gnu.hash) } :code
    .dynsym : { *(.dynsym) } :code
    .dynstr : { *(.dynstr) } :code
    .gnu.version : { *(.gnu.version) } :code
    .gnu.version_d : { *(.gnu.version_d) } :code
    .dynamic : { *(.dynamic) } :code :dynamic
    .text : { *(.text*) } :code
    .rodata : { *(.rodata*) } :code
    .data : { *(.data*) *(.got*) *(.bss*) } :code
    .rela.dyn : { *(.rela*) } :code
    .rel.dyn : { *(.rel.*) } :code
    .plt : { *(.plt*) } :code
    ASSERT(SIZEOF(.data) == 0, "vDSO must not have writable data or GOT")
    ASSERT(SIZEOF(.rela.dyn) + SIZEOF(.rel.dyn) + SIZEOF(.plt) == 0,
           "vDSO must not need relocations or PLT")
    ASSERT(. <= 4096, "vDSO code exceeds one page")
    . = 4096;
    HIDDEN(_brook_pvclock = .);
    /* One PT_LOAD is required by glibc's pre-mapped vDSO base discovery.
       The kernel enforces stricter R/NX permissions on this second page. */
    .vvar (NOLOAD) : { . += 4096; } :code
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
