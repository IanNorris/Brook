#pragma once

#include <stdint.h>
#include "address.h"
#include "mem_tag.h"

namespace brook {
//
// Address space layout:
//   0x0000_0000_0000_0000..0x0000_0000_3FFF_FFFF  NULL GUARD — always unmapped (1GB)
//   0x0000_0000_4000_0000..0x0000_7FFF_FFFF_FFFF  User space (future, starts at 1GB)
//   0x0000_0000_0000_0000..0x0000_00FF_FFFF_FFFF  Bootloader identity map (transition)
//   0xFFFF_C000_0000_0000..0xFFFF_C07F_FFFF_FFFF  VMALLOC region (PML4[384])
//   0xFFFF_C080_0000_0000..0xFFFF_C0FF_0000_0000  Kernel heap (PML4[385])
//   0xFFFF_FFFF_8000_0000..0xFFFF_FFFF_FFFF_FFFF  Kernel image (PML4[511])
//
// Null guard: VmmMapPage refuses any mapping below VIRTUAL_NULL_GUARD.

static constexpr uint64_t VIRTUAL_NULL_GUARD = 64 * 1024; // 64KB

// Page flag bits (match x86-64 PTE format).
static constexpr uint64_t VMM_PRESENT   = (1ULL << 0);
static constexpr uint64_t VMM_WRITABLE  = (1ULL << 1);
static constexpr uint64_t VMM_USER      = (1ULL << 2);
static constexpr uint64_t VMM_NO_EXEC   = (1ULL << 63); // requires EFER.NXE
static constexpr uint64_t VMM_FORCE_MAP = (1ULL << 62); // bypass null guard (SMP trampoline)

// PTE available-bit encoding for ownership tracking.
static constexpr uint64_t PTE_TAG_SHIFT  = 9;
static constexpr uint64_t PTE_TAG_MASK   = (0x7ULL   << PTE_TAG_SHIFT);
static constexpr uint64_t PTE_COW_BIT    = (1ULL << 52); // COW: page was writable, now RO-shared
static constexpr uint64_t PTE_PID_SHIFT  = 53;
static constexpr uint64_t PTE_PID_MASK   = (0x3FFULL << PTE_PID_SHIFT); // 10 bits = max 1024 PIDs

// Base of the VMALLOC virtual address region.
static constexpr uint64_t VMALLOC_BASE  = 0xFFFFC00000000000ULL;
static constexpr uint64_t VMALLOC_SIZE  = 32ULL * 1024 * 1024 * 1024; // 32GB

// Direct physical map: all physical RAM mapped here by the bootloader (2MB pages).
static constexpr uint64_t DIRECT_MAP_BASE = 0xFFFF800000000000ULL;

// Convert between physical and direct-map virtual addresses.
inline VirtualAddress  PhysToVirt(PhysicalAddress phys) { return VirtualAddress(DIRECT_MAP_BASE + phys.raw()); }
inline PhysicalAddress VirtToPhys(VirtualAddress virt)  { return PhysicalAddress(virt.raw() - DIRECT_MAP_BASE); }

// Module-space region: within the kernel's ±2GB window for R_X86_64_32S relocations.
static constexpr uint64_t MODULE_BASE  = 0xFFFFFFFF90000000ULL;
static constexpr uint64_t MODULE_SIZE  = 256ULL * 1024 * 1024; // 256 MB

// Record of a single VmmAllocPages call — for diagnostics and ownership tracking.
struct VmmAllocation
{
    uint64_t virtBase;    // virtual base address (0 = unused slot)
    uint64_t pageCount;
    MemTag   tag;
    uint16_t pid;
};

// Initialise the VMM. Must be called after PmmInit().
void VmmInit();

// ---------------------------------------------------------------------------
// Page table operations — all take an explicit PageTable parameter.
// Pass KernelPageTable for kernel-space operations.
// ---------------------------------------------------------------------------

// Map a single 4KB page.
// Returns false on null-guard violation or page table allocation failure.
bool VmmMapPage(PageTable pt, VirtualAddress virtAddr, PhysicalAddress physAddr,
                uint64_t flags,
                MemTag tag = MemTag::KernelData, uint16_t pid = KernelPid);

// Unmap a single 4KB page. Issues INVLPG. No-op if not mapped.
void VmmUnmapPage(PageTable pt, VirtualAddress virtAddr);

// Translate virtual → physical by walking the given page table.
// Returns a null PhysicalAddress if unmapped.
PhysicalAddress VmmVirtToPhys(PageTable pt, VirtualAddress virtAddr);

// Return a pointer to the leaf PTE for a virtual address.
// Returns nullptr if the page table walk fails at any level.
uint64_t* VmmGetPte(PageTable pt, VirtualAddress virtAddr);

// Extract the MemTag encoded in a mapped page's PTE available bits.
MemTag VmmGetPageTag(PageTable pt, VirtualAddress virtAddr);

// Extract the PID encoded in a mapped page's PTE available bits.
uint16_t VmmGetPagePid(PageTable pt, VirtualAddress virtAddr);

// ---------------------------------------------------------------------------
// VMALLOC / module allocators (always kernel page table)
// ---------------------------------------------------------------------------

// Allocate contiguous virtual pages from the VMALLOC region.
// Returns the virtual base address, or 0 on failure.
VirtualAddress VmmAllocPages(uint64_t pageCount,
                             uint64_t flags = VMM_WRITABLE,
                             MemTag tag     = MemTag::KernelData,
                             uint16_t pid   = KernelPid);

// Free pages previously allocated with VmmAllocPages.
void VmmFreePages(VirtualAddress virtAddr, uint64_t pageCount);

// Allocate a kernel stack with guard pages at both ends.
// Returns the base of the usable stack region (guard pages are unmapped).
// The virtual layout is: [guard page] [usable pages] [guard page].
// Pass the returned address and pageCount to VmmFreeKernelStack to free.
VirtualAddress VmmAllocKernelStack(uint64_t pageCount,
                                   MemTag tag     = MemTag::KernelData,
                                   uint16_t pid   = KernelPid);

// Free a kernel stack allocated with VmmAllocKernelStack.
// virtAddr is the usable base (as returned by VmmAllocKernelStack).
void VmmFreeKernelStack(VirtualAddress virtAddr, uint64_t pageCount);

// Allocate pages in the kernel module region (within ±2GB of kernel image).
VirtualAddress VmmAllocModulePages(uint64_t pageCount,
                                   uint64_t flags = VMM_WRITABLE,
                                   MemTag tag     = MemTag::Device,
                                   uint16_t pid   = KernelPid);

// Look up the allocation record for a VMALLOC address.
const VmmAllocation* VmmGetAllocation(VirtualAddress virtAddr);

// Free all virtual allocations owned by a PID, then call PmmKillPid.
void VmmKillPid(uint16_t pid);

// Count active VMALLOC allocations, optionally filtered by PID.
uint32_t VmmCountAllocations(uint16_t filterPid = 0xFFFF);

// ---------------------------------------------------------------------------
// Per-process page tables
// ---------------------------------------------------------------------------

// Create a new user-mode page table with shared kernel upper-half.
PageTable VmmCreateUserPageTable();

// Free a per-process PML4 and all intermediate page-table pages in the
// user-half (PML4[0..255]).  Does NOT free leaf data pages.
void VmmDestroyUserPageTable(PageTable pt);

// Get the kernel's page table.
PageTable VmmKernelCR3();

// Switch the active page table.  Writes CR3 and flushes the TLB.
void VmmSwitchPageTable(PageTable pt);

} // namespace brook
