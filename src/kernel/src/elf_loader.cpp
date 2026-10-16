// elf_loader.cpp -- Load ET_EXEC / ET_DYN ELF binaries for user-mode execution.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).
// Handles PT_LOAD segments, PT_TLS, program break setup.
// Supports dynamic linking via PT_INTERP (see LoadInterpreter in process.cpp).

#include "elf.h"
#include "process.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/address_space.h"
#include "serial.h"
#include "string.h"

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

// Validate every source range before allocating pages. A bounded file alone
// does not bound an ELF's BSS or holes between segments.
struct ElfLayout
{
    const Elf64_Ehdr* header;
    const Elf64_Phdr* segments;
    uint64_t first, end;
};

static bool ValidateElf(const uint8_t* data, uint64_t size, ElfLayout* out)
{
    if (!data || size < sizeof(Elf64_Ehdr)) return false;
    const auto* h = reinterpret_cast<const Elf64_Ehdr*>(data);
    if (h->e_ident[0] != ELFMAG0 || h->e_ident[1] != ELFMAG1 ||
        h->e_ident[2] != ELFMAG2 || h->e_ident[3] != ELFMAG3 ||
        h->e_ident[4] != ELFCLASS64 || h->e_ident[5] != ELFDATA2LSB ||
        h->e_ident[6] != 1 || h->e_version != 1 || h->e_machine != EM_X86_64 ||
        (h->e_type != ET_EXEC && h->e_type != ET_DYN) ||
        h->e_ehsize != sizeof(Elf64_Ehdr) || h->e_phentsize != sizeof(Elf64_Phdr) ||
        !h->e_phnum || !h->e_phoff) return false;
    const uint64_t phBytes = uint64_t{h->e_phnum} * sizeof(Elf64_Phdr);
    if (h->e_phoff > size || phBytes > size - h->e_phoff) return false;
    const auto* ph = reinterpret_cast<const Elf64_Phdr*>(data + h->e_phoff);
    uint64_t first = ~uint64_t{0}, end = 0;
    bool interpSeen = false, tlsSeen = false, entryMapped = false;
    for (uint16_t i = 0; i < h->e_phnum; ++i)
    {
        const auto& p = ph[i];
        // PT_NULL has no defined contents; other file-backed headers must
        // describe bytes actually present in this exact-size image.
        if (p.p_type == PT_NULL) continue;
        if ((p.p_filesz || p.p_type == PT_LOAD || p.p_type == PT_TLS) &&
            (p.p_offset > size || p.p_filesz > size - p.p_offset)) return false;
        if (p.p_memsz > ~uint64_t{0} - p.p_vaddr) return false;
        if (p.p_type == PT_LOAD || p.p_type == PT_TLS)
        {
            if (p.p_filesz > p.p_memsz) return false;
            if (p.p_align > 1 && (p.p_align & (p.p_align - 1))) return false;
        }
        if (p.p_type == PT_LOAD)
        {
            if (p.p_align > 1 && (p.p_vaddr % p.p_align != p.p_offset % p.p_align)) return false;
            if (!p.p_memsz) continue;
            if (p.p_vaddr < first) first = p.p_vaddr;
            const uint64_t segmentEnd = p.p_vaddr + p.p_memsz;
            if (segmentEnd > end) end = segmentEnd;
            if ((p.p_flags & PF_X) && h->e_entry >= p.p_vaddr && h->e_entry < segmentEnd)
                entryMapped = true;
        }
        else if (p.p_type == PT_INTERP)
        {
            if (interpSeen || p.p_filesz < 2 || p.p_filesz >= sizeof(ElfBinary{}.interpPath) ||
                data[p.p_offset + p.p_filesz - 1] != 0 || !data[p.p_offset]) return false;
            for (uint64_t n = 0; n + 1 < p.p_filesz; ++n)
                if (!data[p.p_offset + n]) return false;
            interpSeen = true;
        }
        else if (p.p_type == PT_TLS)
        {
            // ElfBinary stores alignment in uint16_t; refuse truncation.
            if (tlsSeen || p.p_align > 0xffff || p.p_memsz > 512ULL * 1024 * 1024) return false;
            tlsSeen = true;
            bool templateMapped = !p.p_filesz;
            for (uint16_t j = 0; j < h->e_phnum && !templateMapped; ++j)
            {
                const auto& load = ph[j];
                if (load.p_type != PT_LOAD || p.p_vaddr < load.p_vaddr) continue;
                const uint64_t delta = p.p_vaddr - load.p_vaddr;
                if (delta <= load.p_filesz && p.p_filesz <= load.p_filesz - delta &&
                    load.p_offset <= p.p_offset && delta == p.p_offset - load.p_offset)
                    templateMapped = true;
            }
            if (!templateMapped) return false;
        }
    }
    if (first >= end || !entryMapped || end > ~uint64_t{0} - 4095) return false;
    first = AlignDown(first, 4096);
    end = AlignUp(end, 4096);
    if (end - first > 512ULL * 1024 * 1024) return false;
    *out = {h, ph, first, end};
    return true;
}

static bool RelocatedElfFits(const ElfLayout& image, uint64_t slide, uint64_t limit)
{
    if (image.end > limit || slide > limit - image.end) return false;
    for (uint16_t i = 0; i < image.header->e_phnum; ++i)
    {
        const auto& p = image.segments[i];
        if (p.p_type != PT_LOAD && p.p_type != PT_TLS && p.p_type != PT_PHDR) continue;
        if (p.p_vaddr > limit || slide > limit - p.p_vaddr ||
            p.p_memsz > limit - p.p_vaddr - slide) return false;
    }
    return image.first + slide >= 4096;
}

// Physical pages need not be contiguous. Translate once per destination page,
// and never silently skip an unmapped byte of an otherwise successful image.
static bool CopyElfBytes(PageTable pt, uint64_t dest, const uint8_t* source, uint64_t size)
{
    while (size)
    {
        const PhysicalAddress phys = VmmVirtToPhys(pt, VirtualAddress(dest));
        if (!phys) return false;
        uint64_t chunk = 4096 - (dest & 4095);
        if (chunk > size) chunk = size;
        memcpy(reinterpret_cast<void*>(PhysToVirt(phys).raw()), source, chunk);
        dest += chunk; source += chunk; size -= chunk;
    }
    return true;
}

static bool PrepareMainImage(const uint8_t* data, uint64_t size, ElfLayout* image,
                             uint64_t* slide)
{
    if (!ValidateElf(data, size, image)) return false;
    static constexpr uint64_t PIE_LOAD_BASE = 0x200000ULL;
    *slide = image->header->e_type == ET_DYN && image->first < PIE_LOAD_BASE ?
        PIE_LOAD_BASE - image->first : 0;
    // Leave room for brk and the guard preceding the caller's mmap arena.
    static constexpr uint64_t BRK_MMAP_GUARD = 16 * 1024 * 1024;
    return RelocatedElfFits(*image, *slide, USER_MMAP_END - PROGRAM_BREAK_SIZE - BRK_MMAP_GUARD);
}

// execve can reject malformed input before killing siblings or destroying the
// old address space. The actual loader uses this identical preflight.
bool ElfValidateMainImage(const uint8_t* data, uint64_t size)
{
    ElfLayout image;
    uint64_t slide;
    return PrepareMainImage(data, size, &image, &slide);
}

bool ElfLoad(const uint8_t* data, uint64_t size, ElfBinary* out,
             AddressSpaceState* space, uint16_t pid)
{
    PageTable pt = space->pageTable;
    ElfLayout image;
    uint64_t slide;
    if (!out || !PrepareMainImage(data, size, &image, &slide))
    {
        SerialPuts("ELF: invalid header, segment, or image bounds\n");
        return false;
    }
    const auto* ehdr = image.header;
    const auto* phdrs = image.segments;
    const uint64_t loadBase = image.first + slide;
    const uint64_t loadEnd = image.end + slide;
    // The AS is unpublished: construction owns it exclusively and may prepare
    // metadata without the runtime mutation mutex. Match the loader's existing
    // contiguous RWX allocation, including holes; segment hardening is separate.
    MappingRangePlan ranges;
    MappingRange mapped{loadBase,loadEnd,0,0,nullptr,7,7,MappingKind::Image,
                        MappingSharing::Private,MappingLeafOwnership::Managed};
    if (!ranges.Prepare(1) || !ranges.Insert(space->ranges,mapped)) return false;

    uint64_t loadSize = loadEnd - loadBase;
    uint64_t loadPages = loadSize / 4096;

    DbgPrintf("ELF: loading at 0x%lx-0x%lx (%lu pages)\n",
                 loadBase, loadEnd, loadPages);

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
            PmmFreePage(phys);
            return false;
        }

        auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        memset(p, 0, 4096);
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
            // Bounds-check against the in-memory ELF buffer.  Returning
            // false here is better than faulting the kernel when the file
            // is truncated or the caller passed a buffer smaller than the
            // on-disk image (e.g. a Go binary exceeding MAX_ELF_SIZE).
            if (phdr.p_offset > size ||
                phdr.p_filesz > size - phdr.p_offset)
            {
                SerialPrintf("ELF: PT_LOAD %u out of range "
                             "(offset=%lu filesz=%lu size=%lu)\n",
                             i, phdr.p_offset, phdr.p_filesz, size);
                return false;
            }

            if (!CopyElfBytes(pt, phdr.p_vaddr + slide, data + phdr.p_offset, phdr.p_filesz))
                return false;

            // BSS (p_memsz > p_filesz) is already zeroed from page init above

            DbgPrintf("ELF:   PT_LOAD 0x%lx-0x%lx (%lu bytes, %lu BSS)\n",
                         phdr.p_vaddr + slide, phdr.p_vaddr + slide + phdr.p_memsz,
                         phdr.p_filesz, phdr.p_memsz - phdr.p_filesz);
        }
        else if (phdr.p_type == PT_TLS)
        {
            if (phdr.p_offset > size ||
                phdr.p_filesz > size - phdr.p_offset)
            {
                SerialPrintf("ELF: PT_TLS out of range\n");
                return false;
            }
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
            if (phdr.p_offset > size ||
                phdr.p_filesz > size - phdr.p_offset)
            {
                SerialPuts("ELF: PT_INTERP out of range\n");
                return false;
            }

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

                DbgPrintf("ELF:   PT_INTERP '%s'\n", out->interpPath);
            }
        }
    }

    // Find the actual file-to-memory mapping of the complete phdr table;
    // file offsets are not generally offsets from the lowest virtual page.
    uint64_t phdrInMemory = 0;
    const uint64_t phdrBytes = uint64_t{ehdr->e_phnum} * sizeof(Elf64_Phdr);
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        const auto& p = phdrs[i];
        if (p.p_type != PT_LOAD || ehdr->e_phoff < p.p_offset) continue;
        const uint64_t delta = ehdr->e_phoff - p.p_offset;
        if (delta <= p.p_filesz && phdrBytes <= p.p_filesz - delta)
        {
            phdrInMemory = p.p_vaddr + slide + delta;
            break;
        }
    }

    // Populate output
    out->baseAddress     = loadBase;
    out->allocatedSize   = loadSize;
    out->entryPoint      = ehdr->e_entry + slide;
    out->programBreakLow = loadEnd;
    // PrepareMainImage reserves this range and the caller places mmap above
    // it when the image is too large for the legacy 256 MiB mmap base.
    out->programBreakHigh = loadEnd + PROGRAM_BREAK_SIZE;
    out->phdrVaddr       = phdrInMemory;
    out->phdrNum         = ehdr->e_phnum;
    out->phdrEntSize     = ehdr->e_phentsize;

    DbgPrintf("ELF: loaded OK, entry=0x%lx, break=0x%lx-0x%lx\n",
                 out->entryPoint, out->programBreakLow, out->programBreakHigh);

    // NOTE: All pages are currently mapped writable. Proper segment permissions
    // (read-only .text/.rodata) require COW support for pages shared between
    // a read-only and a read-write segment at page boundaries. Deferred until
    // Brook has full mmap MAP_PRIVATE COW semantics.

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
                   uint64_t base, AddressSpaceState* space, uint16_t pid)
{
    PageTable pt = space->pageTable;
    ElfLayout image;
    if (!ValidateElf(data, size, &image) || (base & 4095) || base < image.first)
    {
        SerialPuts("ELF interp: invalid header, segment, or image bounds\n");
        return 0;
    }
    const auto* ehdr = image.header;
    const auto* phdrs = image.segments;
    const uint64_t slide = base - image.first;
    if (!RelocatedElfFits(image, slide, 0x0000800000000000ULL)) return 0;
    // Reject collisions with existing mappings before allocating anything.
    // Adjacent interpreter segments may share pages allocated below.
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        const auto& p = phdrs[i];
        if (p.p_type != PT_LOAD || !p.p_memsz) continue;
        const uint64_t end = AlignUp(p.p_vaddr + slide + p.p_memsz, 4096);
        for (uint64_t va = AlignDown(p.p_vaddr + slide, 4096); va < end; va += 4096)
            if (VmmVirtToPhys(pt, VirtualAddress(va))) return 0;
    }

    // Allocate and load each PT_LOAD segment.
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        if (phdrs[i].p_type != PT_LOAD || !phdrs[i].p_memsz) continue;
        const auto& phdr = phdrs[i];

        uint64_t segStart  = AlignDown(phdr.p_vaddr + slide, 4096);
        uint64_t segEnd    = AlignUp(phdr.p_vaddr + slide + phdr.p_memsz, 4096);
        uint64_t segPages  = (segEnd - segStart) / 4096;
        MappingRangePlan ranges;
        MappingRange mapped{segStart,segEnd,0,0,nullptr,7,7,MappingKind::Image,
                            MappingSharing::Private,MappingLeafOwnership::Managed};
        // PT_LOAD segments can share a boundary page in this unpublished image.
        if (!ranges.Prepare(3) || !ranges.Replace(space->ranges,segStart,segEnd,&mapped)) return 0;

        uint64_t flags = VMM_WRITABLE | VMM_USER;

        for (uint64_t p = 0; p < segPages; ++p)
        {
            VirtualAddress va(segStart + p * 4096);
            // Skip if already mapped (segments can overlap pages).
            if (VmmVirtToPhys(pt, va)) continue;

            PhysicalAddress phys = PmmAllocPage(MemTag::User, pid);
            if (!phys) return 0;
            if (!VmmMapPage(pt, va, phys, flags, MemTag::User, pid))
            {
                PmmFreePage(phys);
                return 0;
            }

            // Zero via direct map
            auto* kp = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
            memset(kp, 0, 4096);
        }

        if (!CopyElfBytes(pt, phdr.p_vaddr + slide, data + phdr.p_offset, phdr.p_filesz))
            return 0;

        DbgPrintf("ELF interp:   PT_LOAD 0x%lx-0x%lx (%lu bytes)\n",
                     phdr.p_vaddr + slide, phdr.p_vaddr + slide + phdr.p_memsz,
                     phdr.p_filesz);
    }

    uint64_t entry = ehdr->e_entry + slide;
    DbgPrintf("ELF interp: loaded at base=0x%lx entry=0x%lx\n", base, entry);

    // Memory fence: ensure all PTE writes are visible to other CPUs before
    // the scheduler moves this process to an AP core.
    __asm__ volatile("mfence" ::: "memory");

    return entry;
}

} // namespace brook
