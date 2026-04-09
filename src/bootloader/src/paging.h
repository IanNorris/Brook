#pragma once

#include <Uefi.h>

namespace brook
{
namespace bootloader
{

// Virtual base address where the kernel is mapped.
static constexpr UINT64 KernelVirtualBase = 0xFFFFFFFF80000000ULL;

// Pre-allocated, zeroed page table structures.
// All 8 pages are allocated as a contiguous block before ExitBootServices.
struct PageTableAllocation
{
    UINT64* pml4;       // Page Map Level 4 (1 page)
    UINT64* pdptLow;    // PDPT for low identity map (1 page)
    UINT64* pdLow[4];   // PDs for identity mapping 0-4GB (4 pages)
    UINT64* pdptHigh;   // PDPT for high kernel address space (1 page)
    UINT64* pdKernel;   // PD for kernel virtual address (1 page)
};

// Allocate and zero 8 contiguous 4KB pages for page table structures.
// Must be called BEFORE ExitBootServices (uses UEFI AllocatePages + SetMem).
PageTableAllocation AllocatePageTables(EFI_BOOT_SERVICES* bootServices);

// Fill in page table entries:
//   - Identity map 0-4GB with 2MB pages
//   - Map kernelPhysBase at KernelVirtualBase (2MB page)
// Call AFTER ExitBootServices. No UEFI services used.
void BuildPageTables(const PageTableAllocation& pt, UINT64 kernelPhysBase);

// Load the PML4 physical address into CR3, activating our page tables.
// Call immediately after BuildPageTables, before jumping to kernel.
void LoadCR3(const PageTableAllocation& pt);

} // namespace bootloader
} // namespace brook
