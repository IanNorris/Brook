#include "paging.h"
#include "console.h"
#include "memory.h"

namespace brook
{
namespace bootloader
{

static constexpr UINT64 PAGE_PRESENT  = 0x1ULL;
static constexpr UINT64 PAGE_WRITABLE = 0x2ULL;
static constexpr UINT64 PAGE_2MB      = 0x80ULL;  // PS bit: 2MB page in PD
static constexpr UINT64 PAGE_SIZE_4K  = 0x1000ULL;
static constexpr UINT64 PAGE_SIZE_1GB = 0x40000000ULL;

// Scan the UEFI memory map to find the highest physical address in use.
// Uses AllocatePool/FreePool so it can be called before the page table
// allocation without disturbing any pre-existing allocations.
// Returns the address rounded UP to the next 1GB boundary (convenient for
// sizing PDPT entries) and capped at MaxIdentityMapGB GB.
static UINT64 ScanHighestPhysAddress(EFI_BOOT_SERVICES* bootServices)
{
    // Phase 1: query required buffer size.
    UINTN  mapSize    = 0;
    UINTN  mapKey     = 0;
    UINTN  descSize   = 0;
    UINT32 descVer    = 0;
    EFI_STATUS status = bootServices->GetMemoryMap(&mapSize, nullptr, &mapKey, &descSize, &descVer);
    if (status != EFI_BUFFER_TOO_SMALL && EFI_ERROR(status))
        return 4 * PAGE_SIZE_1GB; // safe fallback: 4GB

    mapSize += 8 * descSize; // small headroom

    void* buf = nullptr;
    status = bootServices->AllocatePool(EfiLoaderData, mapSize, &buf);
    if (EFI_ERROR(status) || buf == nullptr)
        return 4 * PAGE_SIZE_1GB;

    status = bootServices->GetMemoryMap(&mapSize,
        reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(buf),
        &mapKey, &descSize, &descVer);

    UINT64 highest = 0;
    if (!EFI_ERROR(status))
    {
        UINTN count = mapSize / descSize;
        for (UINTN i = 0; i < count; i++)
        {
            auto* d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(
                reinterpret_cast<UINT8*>(buf) + i * descSize);
            UINT64 end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE_4K;
            if (end > highest) highest = end;
        }
    }

    bootServices->FreePool(buf);

    if (highest == 0) return 4 * PAGE_SIZE_1GB;

    // Round up to next 1GB boundary.
    highest = (highest + PAGE_SIZE_1GB - 1) & ~(PAGE_SIZE_1GB - 1);

    // Cap at our maximum.
    UINT64 cap = MaxIdentityMapGB * PAGE_SIZE_1GB;
    return (highest < cap) ? highest : cap;
}

PageTableAllocation AllocatePageTables(EFI_BOOT_SERVICES* bootServices, UINT64 kernelPhysPages)
{
    // Determine how many 1GB PD tables the identity map needs.
    UINT64 highestPhys     = ScanHighestPhysAddress(bootServices);
    UINT64 identityPdCount = highestPhys / PAGE_SIZE_1GB;
    if (identityPdCount < 4) identityPdCount = 4; // always cover at least 4GB

    // Determine how many 4KB PTs the kernel mapping needs.
    // Each PT covers 512 pages = 2MB.
    UINT64 kernelPtCount = (kernelPhysPages + 511) / 512;
    if (kernelPtCount == 0) kernelPtCount = 1;
    if (kernelPtCount > MaxKernelPTs) kernelPtCount = MaxKernelPTs;

    // Layout (all contiguous):
    //  [0]           pml4         — 1 page
    //  [1]           pdptLow      — 1 page
    //  [2..2+N-1]    pdLow[N]     — identityPdCount pages
    //  [2+N]         pdptHigh     — 1 page
    //  [3+N]         pdKernel     — 1 page
    //  [4+N..4+N+M-1] ptKernel[M] — kernelPtCount pages
    UINT64 totalPages = 1 + 1 + identityPdCount + 1 + 1 + kernelPtCount;

    EFI_PHYSICAL_ADDRESS base = AllocatePages(bootServices, EfiLoaderData,
        totalPages * PAGE_SIZE_4K);

    bootServices->SetMem(reinterpret_cast<void*>(base),
        totalPages * PAGE_SIZE_4K, 0);

    UINT64 offset = 0;
    PageTableAllocation alloc;

    alloc.pml4    = reinterpret_cast<UINT64*>(base + (offset++) * PAGE_SIZE_4K);
    alloc.pdptLow = reinterpret_cast<UINT64*>(base + (offset++) * PAGE_SIZE_4K);

    for (UINT64 i = 0; i < identityPdCount; i++)
        alloc.pdLow[i] = reinterpret_cast<UINT64*>(base + (offset++) * PAGE_SIZE_4K);
    alloc.identityPdCount = identityPdCount;

    alloc.pdptHigh = reinterpret_cast<UINT64*>(base + (offset++) * PAGE_SIZE_4K);
    alloc.pdKernel = reinterpret_cast<UINT64*>(base + (offset++) * PAGE_SIZE_4K);

    for (UINT64 i = 0; i < kernelPtCount; i++)
        alloc.ptKernel[i] = reinterpret_cast<UINT64*>(base + (offset++) * PAGE_SIZE_4K);
    alloc.kernelPtCount = kernelPtCount;

    return alloc;
}

void BuildPageTables(const PageTableAllocation& pt, UINT64 kernelPhysBase, UINT64 kernelPhysPages)
{
    // ----------------------------------------------------------------
    // Identity map: PML4[0] → PDPT_LOW → pdLow[i] (one per 1GB, 2MB pages)
    // Covers all physical RAM found during boot.
    // ----------------------------------------------------------------
    pt.pml4[0] = reinterpret_cast<UINT64>(pt.pdptLow) | PAGE_PRESENT | PAGE_WRITABLE;

    for (UINT64 i = 0; i < pt.identityPdCount; i++)
    {
        pt.pdptLow[i] = reinterpret_cast<UINT64>(pt.pdLow[i]) | PAGE_PRESENT | PAGE_WRITABLE;

        for (UINT64 j = 0; j < 512; j++)
        {
            UINT64 physAddr = (i << 30) | (j << 21);
            pt.pdLow[i][j]  = physAddr | PAGE_PRESENT | PAGE_WRITABLE | PAGE_2MB;
        }
    }

    // ----------------------------------------------------------------
    // Kernel high mapping using 4KB pages:
    //   PML4[511]     → PDPT_HIGH
    //   PDPT_HIGH[510]→ PD_KERNEL
    //   PD_KERNEL[i]  → ptKernel[i]   (NOT PS bit — this is a PT pointer)
    //   ptKernel[i][j]→ 4KB physical page
    //
    // Virtual 0xFFFFFFFF80000000 decomposes as:
    //   PML4[511] PDPT[510] PD[0] PT[0] offset[0]
    // ----------------------------------------------------------------
    pt.pml4[511]     = reinterpret_cast<UINT64>(pt.pdptHigh) | PAGE_PRESENT | PAGE_WRITABLE;
    pt.pdptHigh[510] = reinterpret_cast<UINT64>(pt.pdKernel) | PAGE_PRESENT | PAGE_WRITABLE;

    for (UINT64 ptIdx = 0; ptIdx < pt.kernelPtCount; ptIdx++)
    {
        // PD entry points to the PT (no PS bit → 4KB granularity).
        pt.pdKernel[ptIdx] = reinterpret_cast<UINT64>(pt.ptKernel[ptIdx])
                             | PAGE_PRESENT | PAGE_WRITABLE;

        for (UINT64 j = 0; j < 512; j++)
        {
            UINT64 pageIdx = ptIdx * 512 + j;
            if (pageIdx >= kernelPhysPages) break;
            UINT64 physAddr         = kernelPhysBase + pageIdx * PAGE_SIZE_4K;
            pt.ptKernel[ptIdx][j]   = physAddr | PAGE_PRESENT | PAGE_WRITABLE;
        }
    }
}

void LoadCR3(const PageTableAllocation& pt)
{
    UINT64 pml4Phys = reinterpret_cast<UINT64>(pt.pml4);
    asm volatile("mov %0, %%cr3" : : "r"(pml4Phys) : "memory");
}

} // namespace bootloader
} // namespace brook
