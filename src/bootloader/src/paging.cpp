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
static constexpr UINT64 PAGE_SIZE_2MB = 0x200000ULL;
static constexpr UINT64 PageTablePageCount = 8ULL;

PageTableAllocation AllocatePageTables(EFI_BOOT_SERVICES* bootServices)
{
    // Allocate all 8 page table pages as one contiguous block.
    // This keeps them all in a single UEFI allocation.
    EFI_PHYSICAL_ADDRESS base = AllocatePages(bootServices, EfiLoaderData,
        PageTablePageCount * PAGE_SIZE_4K);

    // Zero all pages - unset entries MUST be zero (not-present).
    bootServices->SetMem(reinterpret_cast<void*>(base),
        PageTablePageCount * PAGE_SIZE_4K, 0);

    PageTableAllocation alloc;
    alloc.pml4       = reinterpret_cast<UINT64*>(base + 0 * PAGE_SIZE_4K);
    alloc.pdptLow    = reinterpret_cast<UINT64*>(base + 1 * PAGE_SIZE_4K);
    alloc.pdLow[0]   = reinterpret_cast<UINT64*>(base + 2 * PAGE_SIZE_4K);
    alloc.pdLow[1]   = reinterpret_cast<UINT64*>(base + 3 * PAGE_SIZE_4K);
    alloc.pdLow[2]   = reinterpret_cast<UINT64*>(base + 4 * PAGE_SIZE_4K);
    alloc.pdLow[3]   = reinterpret_cast<UINT64*>(base + 5 * PAGE_SIZE_4K);
    alloc.pdptHigh   = reinterpret_cast<UINT64*>(base + 6 * PAGE_SIZE_4K);
    alloc.pdKernel   = reinterpret_cast<UINT64*>(base + 7 * PAGE_SIZE_4K);

    return alloc;
}

void BuildPageTables(const PageTableAllocation& pt, UINT64 kernelPhysBase)
{
    // ----------------------------------------------------------------
    // Identity map 0-4GB using 2MB pages.
    // This keeps bootloader code, stack, boot protocol, and framebuffer
    // accessible after CR3 is loaded.
    // ----------------------------------------------------------------

    // PML4[0] -> PDPT_LOW
    pt.pml4[0] = reinterpret_cast<UINT64>(pt.pdptLow) | PAGE_PRESENT | PAGE_WRITABLE;

    for (UINT64 i = 0; i < 4; i++)
    {
        // PDPT_LOW[i] -> pdLow[i]  (each covers 1GB)
        pt.pdptLow[i] = reinterpret_cast<UINT64>(pt.pdLow[i]) | PAGE_PRESENT | PAGE_WRITABLE;

        // Fill PD with 512 x 2MB pages covering [i*1GB, (i+1)*1GB)
        for (UINT64 j = 0; j < 512; j++)
        {
            UINT64 physAddr = (i << 30) | (j << 21);
            pt.pdLow[i][j] = physAddr | PAGE_PRESENT | PAGE_WRITABLE | PAGE_2MB;
        }
    }

    // ----------------------------------------------------------------
    // Map kernel at KernelVirtualBase (0xFFFFFFFF80000000)
    //   PML4[511] -> PDPT_HIGH
    //   PDPT_HIGH[510] -> PD_KERNEL
    //   PD_KERNEL[0]   -> 2MB page at kernelPhysBase
    // ----------------------------------------------------------------
    pt.pml4[511]     = reinterpret_cast<UINT64>(pt.pdptHigh) | PAGE_PRESENT | PAGE_WRITABLE;
    pt.pdptHigh[510] = reinterpret_cast<UINT64>(pt.pdKernel) | PAGE_PRESENT | PAGE_WRITABLE;

    // kernelPhysBase is already 2MB-aligned (0x400000).
    // Mask off low bits just to be safe.
    pt.pdKernel[0] = (kernelPhysBase & ~(PAGE_SIZE_2MB - 1)) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_2MB;
}

void LoadCR3(const PageTableAllocation& pt)
{
    UINT64 pml4Phys = reinterpret_cast<UINT64>(pt.pml4);
    asm volatile("mov %0, %%cr3" : : "r"(pml4Phys) : "memory");
}

} // namespace bootloader
} // namespace brook
