#pragma once

#include <stdint.h>

namespace brook {

// Virtual Memory Manager — maps physical pages into virtual address space.
//
// Address space layout:
//   0x0000_0000_0000_0000..0x0000_7FFF_FFFF_FFFF  User space (future)
//   0x0000_0000_0000_0000..0x0000_00FF_FFFF_FFFF  Identity map (phys==virt, PML4[0])
//   0xFFFF_C000_0000_0000..0xFFFF_DFFF_FFFF_FFFF  VMALLOC region (PML4[384])
//   0xFFFF_FFFF_8000_0000..0xFFFF_FFFF_FFFF_FFFF  Kernel image (PML4[511])
//
// Key property: while the identity map is active, any physical address P
// (within RAM) is accessible at virtual address P. This lets VmmMapPage
// manipulate page table pages using their physical addresses directly.

// Page flag bits (match x86-64 PTE format).
static constexpr uint64_t VMM_PRESENT   = (1ULL << 0);
static constexpr uint64_t VMM_WRITABLE  = (1ULL << 1);
static constexpr uint64_t VMM_USER      = (1ULL << 2);
static constexpr uint64_t VMM_NO_EXEC   = (1ULL << 63); // requires EFER.NXE

// Base of the VMALLOC virtual address region.
static constexpr uint64_t VMALLOC_BASE  = 0xFFFFC00000000000ULL;
static constexpr uint64_t VMALLOC_SIZE  = 32ULL * 1024 * 1024 * 1024; // 32GB

// Initialise the VMM. Must be called after PmmInit().
void VmmInit();

// Map a single 4KB page: virtual virtAddr → physical physAddr.
// flags: combination of VMM_WRITABLE, VMM_USER, VMM_NO_EXEC.
// VMM_PRESENT is always set automatically.
// Returns false if a page table page could not be allocated.
bool VmmMapPage(uint64_t virtAddr, uint64_t physAddr, uint64_t flags);

// Unmap a single 4KB page. Issues INVLPG. No-op if not mapped.
void VmmUnmapPage(uint64_t virtAddr);

// Allocate 'pageCount' contiguous virtual pages from the VMALLOC region,
// backing each with a fresh physical page from the PMM.
// Returns the virtual base address, or 0 on failure.
uint64_t VmmAllocPages(uint64_t pageCount, uint64_t flags = VMM_WRITABLE);

// Free pages previously allocated with VmmAllocPages.
// Unmaps each virtual page and returns the backing physical pages to the PMM.
void VmmFreePages(uint64_t virtAddr, uint64_t pageCount);

// Translate virtual → physical by walking the live page tables.
// Returns 0 if the address is not mapped (or maps to physical 0).
uint64_t VmmVirtToPhys(uint64_t virtAddr);

} // namespace brook
