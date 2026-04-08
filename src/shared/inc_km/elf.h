#pragma once

#include <stdint.h>

// Minimal ELF64 type definitions for the Brook bootloader ELF loader.
// Only the structures needed to parse and load PT_LOAD segments are included.

// ELF identification indices
static constexpr uint8_t ElfMagic0    = 0x7F;
static constexpr uint8_t ElfMagic1    = 'E';
static constexpr uint8_t ElfMagic2    = 'L';
static constexpr uint8_t ElfMagic3    = 'F';
static constexpr uint8_t ElfClass64   = 2;     // 64-bit object
static constexpr uint8_t ElfDataLSB   = 1;     // Little-endian

// e_type
static constexpr uint16_t ElfTypeExec = 2;     // Executable file
static constexpr uint16_t ElfTypeDyn  = 3;     // Shared object (PIE)

// e_machine
static constexpr uint16_t ElfMachineX86_64 = 62;

// p_type
static constexpr uint32_t PtLoad  = 1;         // Loadable segment
static constexpr uint32_t PtDyn   = 2;         // Dynamic linking info

// p_flags
static constexpr uint32_t PfExec  = 0x1;
static constexpr uint32_t PfWrite = 0x2;
static constexpr uint32_t PfRead  = 0x4;

struct Elf64Header
{
    uint8_t  ident[16];   // Magic + class + data encoding + version + OS/ABI + padding
    uint16_t type;        // Object file type (ElfTypeExec / ElfTypeDyn)
    uint16_t machine;     // Target ISA (ElfMachineX86_64)
    uint32_t version;     // ELF version (always 1)
    uint64_t entry;       // Virtual entry point
    uint64_t phOffset;    // Offset of program header table
    uint64_t shOffset;    // Offset of section header table (unused for loading)
    uint32_t flags;       // Architecture-specific flags
    uint16_t ehSize;      // Size of this header (64 bytes for ELF64)
    uint16_t phEntSize;   // Size of one program header entry
    uint16_t phCount;     // Number of program header entries
    uint16_t shEntSize;   // Size of one section header entry
    uint16_t shCount;     // Number of section header entries
    uint16_t shStrIndex;  // Section name string table index
};

struct Elf64ProgramHeader
{
    uint32_t type;        // Segment type (PtLoad etc.)
    uint32_t flags;       // Segment-dependent flags
    uint64_t offset;      // Offset in file
    uint64_t vaddr;       // Virtual address in memory
    uint64_t paddr;       // Physical address (usually same as vaddr)
    uint64_t fileSize;    // Bytes in file image of segment
    uint64_t memSize;     // Bytes in memory image (>= fileSize; BSS adds to this)
    uint64_t align;       // Alignment (power of 2; 0/1 = none)
};
