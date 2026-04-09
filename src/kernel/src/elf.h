#pragma once

#include <stdint.h>

// Freestanding ELF64 structure definitions for the Brook module loader.
// Only the fields and constants needed by a relocatable-ELF loader are
// included — no libc dependency.

// ---- ELF identification ----

static constexpr uint8_t  ELFMAG0       = 0x7F;
static constexpr uint8_t  ELFMAG1       = 'E';
static constexpr uint8_t  ELFMAG2       = 'L';
static constexpr uint8_t  ELFMAG3       = 'F';
static constexpr uint8_t  ELFCLASS64    = 2;    // 64-bit
static constexpr uint8_t  ELFDATA2LSB   = 1;    // little-endian
static constexpr uint8_t  ET_REL        = 1;    // relocatable object
static constexpr uint16_t EM_X86_64     = 62;

// ---- ELF header ----

struct Elf64_Ehdr {
    uint8_t  e_ident[16]; // Magic, class, data, version, OS/ABI, padding
    uint16_t e_type;      // ET_REL / ET_EXEC / ET_DYN
    uint16_t e_machine;   // EM_X86_64
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;     // program header offset
    uint64_t e_shoff;     // section header offset
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;  // index of .shstrtab section
};

// ---- Section header ----

static constexpr uint32_t SHT_NULL     = 0;
static constexpr uint32_t SHT_PROGBITS = 1;  // .text, .rodata, .data
static constexpr uint32_t SHT_SYMTAB   = 2;
static constexpr uint32_t SHT_STRTAB   = 3;
static constexpr uint32_t SHT_RELA     = 4;  // relocations with addend
static constexpr uint32_t SHT_NOBITS   = 8;  // .bss (no file content)

static constexpr uint32_t SHF_ALLOC   = 0x2;  // section occupies memory
static constexpr uint32_t SHF_EXECINSTR = 0x4; // executable

struct Elf64_Shdr {
    uint32_t sh_name;      // offset into .shstrtab
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;      // virtual address (0 in ET_REL)
    uint64_t sh_offset;    // offset in file
    uint64_t sh_size;
    uint32_t sh_link;      // for SYMTAB: strtab index; for RELA: symtab index
    uint32_t sh_info;      // for RELA: section index to relocate
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

// ---- Symbol table entry ----

static constexpr uint8_t  STB_LOCAL    = 0;
static constexpr uint8_t  STB_GLOBAL   = 1;
static constexpr uint8_t  STB_WEAK     = 2;
static constexpr uint16_t SHN_UNDEF    = 0;     // undefined (external) symbol
static constexpr uint16_t SHN_ABS      = 0xFFF1; // absolute symbol (no section)
static constexpr uint16_t SHN_COMMON   = 0xFFF2;

inline uint8_t  ELF64_ST_BIND(uint8_t info) { return info >> 4; }
inline uint8_t  ELF64_ST_TYPE(uint8_t info) { return info & 0xF; }

struct Elf64_Sym {
    uint32_t st_name;   // offset into string table
    uint8_t  st_info;   // binding + type
    uint8_t  st_other;
    uint16_t st_shndx;  // section index, or SHN_UNDEF / SHN_ABS
    uint64_t st_value;  // offset within section (ET_REL) or VA (ET_EXEC)
    uint64_t st_size;
};

// ---- Relocation with addend ----

inline uint32_t ELF64_R_SYM(uint64_t info)  { return static_cast<uint32_t>(info >> 32); }
inline uint32_t ELF64_R_TYPE(uint64_t info) { return static_cast<uint32_t>(info); }

struct Elf64_Rela {
    uint64_t r_offset; // offset within the section to patch
    uint64_t r_info;   // symbol index + relocation type
    int64_t  r_addend;
};

// ---- x86-64 relocation types ----

static constexpr uint32_t R_X86_64_NONE    = 0;
static constexpr uint32_t R_X86_64_64      = 1;  // S + A            (64-bit absolute)
static constexpr uint32_t R_X86_64_PC32    = 2;  // S + A - P        (32-bit PC-relative)
static constexpr uint32_t R_X86_64_GOT32   = 3;  // G + A            (GOT entry, not used in kernel)
static constexpr uint32_t R_X86_64_PLT32   = 4;  // L + A - P        (treated same as PC32 for kernel)
static constexpr uint32_t R_X86_64_32      = 10; // S + A            (32-bit zero-extended)
static constexpr uint32_t R_X86_64_32S     = 11; // S + A            (32-bit sign-extended)
static constexpr uint32_t R_X86_64_PC64    = 24; // S + A - P        (64-bit PC-relative)
