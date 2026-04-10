#pragma once

#include <stdint.h>
#include "mem_tag.h"

namespace brook {

// Virtual Memory Manager — maps physical pages into virtual address space.
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
// This ensures no kernel code can inadvertently create a mapping that would
// make a null dereference succeed. User page tables will never include this
// range, so user-mode null dereferences always fault.

// Virtual addresses below this are permanently reserved (null pointer guard).
// 64KB is enough to catch null dereferences while allowing ELF user binaries
// loaded at the standard 0x400000 (4MB) base address.
static constexpr uint64_t VIRTUAL_NULL_GUARD = 64 * 1024; // 64KB

// Page flag bits (match x86-64 PTE format).
static constexpr uint64_t VMM_PRESENT   = (1ULL << 0);
static constexpr uint64_t VMM_WRITABLE  = (1ULL << 1);
static constexpr uint64_t VMM_USER      = (1ULL << 2);
static constexpr uint64_t VMM_NO_EXEC   = (1ULL << 63); // requires EFER.NXE

// PTE available-bit encoding for ownership tracking.
// Bits [9-11]  = MemTag (3 bits, values 0-7, matches MemTag enum exactly).
// Bits [52-62] = PID    (11 bits, max PID 2047).
static constexpr uint64_t PTE_TAG_SHIFT  = 9;
static constexpr uint64_t PTE_TAG_MASK   = (0x7ULL   << PTE_TAG_SHIFT);
static constexpr uint64_t PTE_PID_SHIFT  = 52;
static constexpr uint64_t PTE_PID_MASK   = (0x7FFULL << PTE_PID_SHIFT);

// Base of the VMALLOC virtual address region.
static constexpr uint64_t VMALLOC_BASE  = 0xFFFFC00000000000ULL;
static constexpr uint64_t VMALLOC_SIZE  = 32ULL * 1024 * 1024 * 1024; // 32GB

// Direct physical map: all physical RAM mapped here by the bootloader (2MB pages).
// Used by the VMM to access page-table pages safely, even after the low identity
// map (PML4[0]) is modified by user-space ELF loading.
static constexpr uint64_t DIRECT_MAP_BASE = 0xFFFF800000000000ULL;

// Convert a physical address to its kernel-accessible virtual address via
// the direct physical map.  Must only be used for physical RAM addresses.
inline uint64_t PhysToVirt(uint64_t phys) { return DIRECT_MAP_BASE + phys; }

// Convert a direct-map virtual address back to its physical address.
inline uint64_t VirtToPhys(uint64_t virt) { return virt - DIRECT_MAP_BASE; }

// Module-space region: within the kernel's ±2GB window for R_X86_64_32S relocations.
// The kernel image occupies 0xFFFFFFFF80000000..~0xFFFFFFFF805xxxxx.
// Module code is placed starting at 0xFFFFFFFF90000000 (256 MB).
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

// Map a single 4KB page: virtual virtAddr → physical physAddr.
// virtAddr must be >= VIRTUAL_NULL_GUARD (null guard enforced).
// flags: combination of VMM_WRITABLE, VMM_USER, VMM_NO_EXEC.
// VMM_PRESENT is always set automatically.
// tag/pid are encoded in PTE available bits [9-11] and [52-62].
// Returns false on null-guard violation or page table allocation failure.
bool VmmMapPage(uint64_t virtAddr, uint64_t physAddr, uint64_t flags,
                MemTag tag = MemTag::KernelData, uint16_t pid = KernelPid);

// Unmap a single 4KB page. Issues INVLPG. No-op if not mapped.
void VmmUnmapPage(uint64_t virtAddr);

// Allocate 'pageCount' contiguous virtual pages from the VMALLOC region,
// backing each with a fresh physical page from the PMM.
// tag/pid are recorded in the VMM allocation table and stamped on each
// backing physical page via PmmSetOwner.
// Returns the virtual base address, or 0 on failure.
extern "C" uint64_t VmmAllocPages(uint64_t pageCount,
                       uint64_t flags = VMM_WRITABLE,
                       MemTag tag     = MemTag::KernelData,
                       uint16_t pid   = KernelPid);

// Free pages previously allocated with VmmAllocPages.
extern "C" void VmmFreePages(uint64_t virtAddr, uint64_t pageCount);

// Translate virtual → physical by walking the live page tables.
// Returns 0 if the address is not mapped.
extern "C" uint64_t VmmVirtToPhys(uint64_t virtAddr);

// Allocate pages in the kernel module region (within ±2GB of kernel image).
// Used for loading driver modules that are compiled with -mcmodel=kernel.
uint64_t VmmAllocModulePages(uint64_t pageCount,
                             uint64_t flags = VMM_WRITABLE,
                             MemTag tag     = MemTag::Device,
                             uint16_t pid   = KernelPid);

// Look up the allocation record for a VMALLOC address. Returns nullptr if not found.
const VmmAllocation* VmmGetAllocation(uint64_t virtAddr);

// Extract the MemTag encoded in a mapped page's PTE available bits.
// Returns MemTag::Free if the address is not mapped.
MemTag VmmGetPageTag(uint64_t virtAddr);

// Extract the PID encoded in a mapped page's PTE available bits.
// Returns KernelPid if the address is not mapped.
uint16_t VmmGetPagePid(uint64_t virtAddr);

// Free all virtual allocations owned by a PID, then call PmmKillPid.
// No-op if tracking is not enabled or pid == KernelPid.
void VmmKillPid(uint16_t pid);

} // namespace brook
