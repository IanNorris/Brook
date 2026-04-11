// elf_loader.cpp -- Load ET_EXEC / ET_DYN ELF binaries for user-mode execution.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).
// Handles PT_LOAD segments, PT_TLS, program break setup.
// Static linking only (no dynamic linker / PT_INTERP).

#include "elf.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"

namespace brook {

// Align value up to alignment boundary.
static inline uint64_t AlignUp(uint64_t val, uint64_t align)
{
    return (val + align - 1) & ~(align - 1);
}

static inline uint64_t AlignDown(uint64_t val, uint64_t align)
{
    return val & ~(align - 1);
}

bool ElfLoad(const uint8_t* data, uint64_t size, ElfBinary* out,
             uint64_t pml4Phys, uint16_t pid)
{
    if (size < sizeof(Elf64_Ehdr))
    {
        SerialPuts("ELF: file too small\n");
        return false;
    }

    auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);

    // Validate ELF magic
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3)
    {
        SerialPuts("ELF: bad magic\n");
        return false;
    }

    if (ehdr->e_ident[4] != ELFCLASS64)
    {
        SerialPuts("ELF: not 64-bit\n");
        return false;
    }

    if (ehdr->e_machine != EM_X86_64)
    {
        SerialPuts("ELF: not x86-64\n");
        return false;
    }

    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
    {
        SerialPrintf("ELF: unsupported type %u (need ET_EXEC or ET_DYN)\n", ehdr->e_type);
        return false;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0)
    {
        SerialPuts("ELF: no program headers\n");
        return false;
    }

    // Parse program headers
    auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(data + ehdr->e_phoff);

    // First pass: find the virtual address range we need to allocate.
    uint64_t lowestVaddr  = ~0ULL;
    uint64_t highestEnd   = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t segStart = phdrs[i].p_vaddr;
        uint64_t segEnd   = segStart + phdrs[i].p_memsz;

        if (segStart < lowestVaddr) lowestVaddr = segStart;
        if (segEnd > highestEnd)    highestEnd = segEnd;
    }

    if (lowestVaddr >= highestEnd)
    {
        SerialPuts("ELF: no PT_LOAD segments\n");
        return false;
    }

    // Align to page boundaries
    uint64_t loadBase = AlignDown(lowestVaddr, 4096);
    uint64_t loadEnd  = AlignUp(highestEnd, 4096);
    uint64_t loadSize = loadEnd - loadBase;
    uint64_t loadPages = loadSize / 4096;

    SerialPrintf("ELF: loading at 0x%lx-0x%lx (%lu pages)\n",
                 loadBase, loadEnd, loadPages);

    // Helper: translate a user virtual address to a kernel-writable pointer
    // via the direct physical map, so we can write to user pages without
    // switching CR3.
    auto userToKernel = [&](uint64_t userVaddr) -> uint8_t* {
        uint64_t phys = VmmVirtToPhys(pml4Phys, userVaddr);
        if (!phys) return nullptr;
        return reinterpret_cast<uint8_t*>(PhysToVirt(phys));
    };

    // Allocate pages for the binary segments only.
    // Program break pages are allocated lazily by sys_brk.
    for (uint64_t page = 0; page < loadPages; ++page)
    {
        uint64_t vaddr = loadBase + page * 4096;
        uint64_t phys = PmmAllocPage(MemTag::User, pid);
        if (!phys)
        {
            SerialPrintf("ELF: out of memory at page %lu\n", page);
            return false;
        }

        uint64_t flags = VMM_WRITABLE | VMM_USER;

        if (!VmmMapPage(pml4Phys, vaddr, phys, flags, MemTag::User, pid))
        {
            SerialPrintf("ELF: failed to map vaddr 0x%lx\n", vaddr);
            return false;
        }

        // Zero the page via direct map (kernel can't access user vaddrs)
        auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys));
        for (uint64_t b = 0; b < 4096; ++b) p[b] = 0;
    }

    // Second pass: load PT_LOAD segments and extract PT_TLS.
    out->tlsInitData  = nullptr;
    out->tlsInitSize  = 0;
    out->tlsTotalSize = 0;
    out->tlsAlign     = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        const auto& phdr = phdrs[i];

        if (phdr.p_type == PT_LOAD)
        {
            // Copy file data via direct map
            const auto* src = data + phdr.p_offset;
            for (uint64_t b = 0; b < phdr.p_filesz; ++b)
            {
                uint8_t* dst = userToKernel(phdr.p_vaddr + b);
                if (dst) *dst = src[b];
            }

            // BSS (p_memsz > p_filesz) is already zeroed from page init above

            SerialPrintf("ELF:   PT_LOAD 0x%lx-0x%lx (%lu bytes, %lu BSS)\n",
                         phdr.p_vaddr, phdr.p_vaddr + phdr.p_memsz,
                         phdr.p_filesz, phdr.p_memsz - phdr.p_filesz);
        }
        else if (phdr.p_type == PT_TLS)
        {
            // TLS template: record user vaddr and sizes.
            // The actual data will be copied in ProcessCreate via direct map.
            out->tlsInitData  = reinterpret_cast<uint8_t*>(phdr.p_vaddr);
            out->tlsInitSize  = phdr.p_filesz;
            out->tlsTotalSize = phdr.p_memsz;
            out->tlsAlign     = static_cast<uint16_t>(phdr.p_align);

            SerialPrintf("ELF:   PT_TLS init=%lu total=%lu align=%u\n",
                         phdr.p_filesz, phdr.p_memsz, out->tlsAlign);
        }
    }

    // Copy program headers into loaded memory so AT_PHDR can point to them.
    // For ET_EXEC, the PT_PHDR segment (if present) already puts them in memory.
    // Otherwise, they're at ehdr->e_phoff which we've loaded if it falls in a PT_LOAD.
    uint64_t phdrInMemory = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        if (phdrs[i].p_type == PT_PHDR)
        {
            phdrInMemory = phdrs[i].p_vaddr;
            break;
        }
    }

    // If no PT_PHDR, check if ehdr + phoff falls within loaded range
    if (phdrInMemory == 0 && ehdr->e_phoff < loadSize)
    {
        phdrInMemory = loadBase + ehdr->e_phoff;
    }

    // Populate output
    out->baseAddress     = loadBase;
    out->allocatedSize   = loadSize;
    out->entryPoint      = ehdr->e_entry;
    out->programBreakLow = loadEnd;
    out->programBreakHigh = loadEnd + PROGRAM_BREAK_SIZE;
    out->phdrVaddr       = phdrInMemory;
    out->phdrNum         = ehdr->e_phnum;
    out->phdrEntSize     = ehdr->e_phentsize;

    SerialPrintf("ELF: loaded OK, entry=0x%lx, break=0x%lx-0x%lx\n",
                 out->entryPoint, out->programBreakLow, out->programBreakHigh);

    return true;
}

} // namespace brook
