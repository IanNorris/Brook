// elf_loader.cpp -- Load ET_EXEC / ET_DYN ELF binaries for user-mode execution.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).
// Handles PT_LOAD segments, PT_TLS, program break setup.
// Supports dynamic linking via PT_INTERP (see LoadInterpreter in process.cpp).

#include "elf.h"
#include "process.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
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
             PageTable pt, uint16_t pid)
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

    // For PIE (ET_DYN) binaries with segments starting near 0, we need to
    // relocate to a sensible user-space address.
    uint64_t slide = 0;
    static constexpr uint64_t PIE_LOAD_BASE = 0x200000ULL; // 2 MB
    if (ehdr->e_type == ET_DYN && loadBase < PIE_LOAD_BASE)
    {
        slide = PIE_LOAD_BASE - loadBase;
        loadBase += slide;
        loadEnd  += slide;
    }

    uint64_t loadSize = loadEnd - loadBase;
    uint64_t loadPages = loadSize / 4096;

    DbgPrintf("ELF: loading at 0x%lx-0x%lx (%lu pages)\n",
                 loadBase, loadEnd, loadPages);

    // Helper: translate a user virtual address to a kernel-writable pointer
    // via the direct physical map, so we can write to user pages without
    // switching CR3.
    auto userToKernel = [&](uint64_t userVaddr) -> uint8_t* {
        PhysicalAddress phys = VmmVirtToPhys(pt, VirtualAddress(userVaddr));
        if (!phys) return nullptr;
        return reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
    };

    // Allocate pages for the binary segments only.
    // Program break pages are allocated lazily by sys_brk.
    for (uint64_t page = 0; page < loadPages; ++page)
    {
        VirtualAddress vaddr(loadBase + page * 4096);
        PhysicalAddress phys = PmmAllocPage(MemTag::User, pid);
        if (!phys)
        {
            SerialPrintf("ELF: out of memory at page %lu\n", page);
            return false;
        }

        uint64_t flags = VMM_WRITABLE | VMM_USER;

        if (!VmmMapPage(pt, vaddr, phys, flags, MemTag::User, pid))
        {
            SerialPrintf("ELF: failed to map vaddr 0x%lx\n", vaddr.raw());
            return false;
        }

        auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        for (uint64_t b = 0; b < 4096; ++b) p[b] = 0;
    }

    // Second pass: load PT_LOAD segments, extract PT_TLS and PT_INTERP.
    out->tlsInitData  = nullptr;
    out->tlsInitSize  = 0;
    out->tlsTotalSize = 0;
    out->tlsAlign     = 0;
    out->interpPath[0] = '\0';

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        const auto& phdr = phdrs[i];

        if (phdr.p_type == PT_LOAD)
        {
            // Copy file data via direct map (apply slide for PIE)
            const auto* src = data + phdr.p_offset;
            for (uint64_t b = 0; b < phdr.p_filesz; ++b)
            {
                uint8_t* dst = userToKernel(phdr.p_vaddr + slide + b);
                if (dst) *dst = src[b];
            }

            // BSS (p_memsz > p_filesz) is already zeroed from page init above

            DbgPrintf("ELF:   PT_LOAD 0x%lx-0x%lx (%lu bytes, %lu BSS)\n",
                         phdr.p_vaddr + slide, phdr.p_vaddr + slide + phdr.p_memsz,
                         phdr.p_filesz, phdr.p_memsz - phdr.p_filesz);
        }
        else if (phdr.p_type == PT_TLS)
        {
            // TLS template: record user vaddr and sizes (with slide).
            out->tlsInitData  = reinterpret_cast<uint8_t*>(phdr.p_vaddr + slide);
            out->tlsInitSize  = phdr.p_filesz;
            out->tlsTotalSize = phdr.p_memsz;
            out->tlsAlign     = static_cast<uint16_t>(phdr.p_align);

            DbgPrintf("ELF:   PT_TLS init=%lu total=%lu align=%u\n",
                         phdr.p_filesz, phdr.p_memsz, out->tlsAlign);
        }
        else if (phdr.p_type == PT_INTERP)
        {
            // Extract interpreter path from the ELF.
            uint64_t pathLen = phdr.p_filesz;
            if (pathLen > 0 && pathLen < sizeof(out->interpPath))
            {
                for (uint64_t b = 0; b < pathLen; ++b)
                    out->interpPath[b] = static_cast<char>(data[phdr.p_offset + b]);
                // Ensure null-termination (p_filesz includes the null byte usually)
                out->interpPath[pathLen] = '\0';
                // Strip trailing null if present in the stored length
                while (pathLen > 0 && out->interpPath[pathLen - 1] == '\0')
                    --pathLen;

                SerialPrintf("ELF:   PT_INTERP '%s'\n", out->interpPath);
            }
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
            phdrInMemory = phdrs[i].p_vaddr + slide;
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
    out->entryPoint      = ehdr->e_entry + slide;
    out->programBreakLow = loadEnd;
    out->programBreakHigh = loadEnd + PROGRAM_BREAK_SIZE;
    out->phdrVaddr       = phdrInMemory;
    out->phdrNum         = ehdr->e_phnum;
    out->phdrEntSize     = ehdr->e_phentsize;

    DbgPrintf("ELF: loaded OK, entry=0x%lx, break=0x%lx-0x%lx\n",
                 out->entryPoint, out->programBreakLow, out->programBreakHigh);

    // Memory fence: ensure all PTE writes are globally visible.
    __asm__ volatile("mfence" ::: "memory");

    return true;
}

// ---------------------------------------------------------------------------
// ElfLoadAt -- Load an ELF (the dynamic linker / interpreter) at a fixed
// base address.  For ET_DYN (PIE) objects, all p_vaddr values are relative
// to zero and we relocate them by adding `base`.
// Returns the entry point (absolute) or 0 on failure.
// ---------------------------------------------------------------------------

uint64_t ElfLoadAt(const uint8_t* data, uint64_t size,
                   uint64_t base, PageTable pt, uint16_t pid)
{
    if (size < sizeof(Elf64_Ehdr)) return 0;

    auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);

    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3)
        return 0;
    if (ehdr->e_ident[4] != ELFCLASS64) return 0;
    if (ehdr->e_machine != EM_X86_64)   return 0;

    auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(data + ehdr->e_phoff);

    // Find the virtual address extent of all PT_LOAD segments.
    uint64_t lowestVaddr = ~0ULL;
    uint64_t highestEnd  = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint64_t segStart = phdrs[i].p_vaddr;
        uint64_t segEnd   = segStart + phdrs[i].p_memsz;
        if (segStart < lowestVaddr) lowestVaddr = segStart;
        if (segEnd > highestEnd)    highestEnd = segEnd;
    }

    if (lowestVaddr >= highestEnd) return 0;

    // For PIE/ET_DYN, p_vaddr starts near 0 — we add `base` to relocate.
    uint64_t slide = base - AlignDown(lowestVaddr, 4096);

    // Allocate and load each PT_LOAD segment.
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        if (phdrs[i].p_type != PT_LOAD) continue;
        const auto& phdr = phdrs[i];

        uint64_t segStart  = AlignDown(phdr.p_vaddr + slide, 4096);
        uint64_t segEnd    = AlignUp(phdr.p_vaddr + slide + phdr.p_memsz, 4096);
        uint64_t segPages  = (segEnd - segStart) / 4096;

        uint64_t flags = VMM_WRITABLE | VMM_USER;

        for (uint64_t p = 0; p < segPages; ++p)
        {
            VirtualAddress va(segStart + p * 4096);
            // Skip if already mapped (segments can overlap pages).
            if (VmmVirtToPhys(pt, va)) continue;

            PhysicalAddress phys = PmmAllocPage(MemTag::User, pid);
            if (!phys) return 0;
            if (!VmmMapPage(pt, va, phys, flags, MemTag::User, pid)) return 0;

            // Zero via direct map
            auto* kp = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
            for (uint64_t b = 0; b < 4096; ++b) kp[b] = 0;
        }

        // Copy file data via direct map
        const auto* src = data + phdr.p_offset;
        for (uint64_t b = 0; b < phdr.p_filesz; ++b)
        {
            uint64_t userVA = phdr.p_vaddr + slide + b;
            PhysicalAddress phys = VmmVirtToPhys(pt, VirtualAddress(userVA));
            if (!phys) continue;
            auto* dst = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
            *dst = src[b];
        }

        SerialPrintf("ELF interp:   PT_LOAD 0x%lx-0x%lx (%lu bytes)\n",
                     phdr.p_vaddr + slide, phdr.p_vaddr + slide + phdr.p_memsz,
                     phdr.p_filesz);
    }

    uint64_t entry = ehdr->e_entry + slide;
    SerialPrintf("ELF interp: loaded at base=0x%lx entry=0x%lx\n", base, entry);

    // Memory fence: ensure all PTE writes are visible to other CPUs before
    // the scheduler moves this process to an AP core.
    __asm__ volatile("mfence" ::: "memory");

    return entry;
}

} // namespace brook
