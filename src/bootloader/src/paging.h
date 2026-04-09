#pragma once

#include <Uefi.h>

namespace brook
{
namespace bootloader
{

// Virtual base address where the kernel is mapped.
static constexpr UINT64 KernelVirtualBase = 0xFFFFFFFF80000000ULL;

// Maximum physical memory to identity-map (in GB).
// One PD table (4KB) is needed per GB, so 128GB costs 512KB of page table space.
static constexpr UINT64 MaxIdentityMapGB = 128ULL;

// Maximum number of 4KB page tables for the kernel virtual mapping.
// Each PT covers 512 * 4KB = 2MB.  16 PTs cover 32MB — plenty.
static constexpr UINT64 MaxKernelPTs = 16ULL;

// Pre-allocated, zeroed page table structures.
//
// Sizing is determined at boot before ExitBootServices:
//   identityPdCount — how many 1GB PD tables are actually populated
//   kernelPtCount   — how many 4KB PTs cover the kernel image
//
// All pages are in one contiguous UEFI allocation so the layout is:
//   [pml4][pdptLow][pdLow×identityPdCount][pdptHigh][pdKernel][ptKernel×kernelPtCount]
struct PageTableAllocation
{
    UINT64* pml4;                       // PML4 (1 page)
    UINT64* pdptLow;                    // PDPT for low identity map (1 page)
    UINT64* pdLow[MaxIdentityMapGB];    // PDs for identity map, one per 1GB
    UINT64  identityPdCount;            // number of pdLow[] entries populated

    UINT64* pdptHigh;                   // PDPT for high virtual address space (1 page)
    UINT64* pdKernel;                   // PD for kernel range — entries point to ptKernel[]
    UINT64* ptKernel[MaxKernelPTs];     // PTs for 4KB kernel page mapping
    UINT64  kernelPtCount;              // number of ptKernel[] entries populated
};

// Allocate and zero all required page table pages as a single contiguous block.
// Internally scans the UEFI memory map to find the highest physical address so
// the identity map can cover all available RAM.
// Must be called BEFORE ExitBootServices.
PageTableAllocation AllocatePageTables(EFI_BOOT_SERVICES* bootServices, UINT64 kernelPhysPages);

// Fill in all page table entries (no UEFI calls — safe after ExitBootServices):
//   - Identity map 0..highestPhysAddr with 2MB pages (covers all RAM)
//   - Map kernel at KernelVirtualBase using 4KB pages
void BuildPageTables(const PageTableAllocation& pt, UINT64 kernelPhysBase, UINT64 kernelPhysPages);

// Load the PML4 physical address into CR3, activating our page tables.
void LoadCR3(const PageTableAllocation& pt);

} // namespace bootloader
} // namespace brook
