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
//   0xFFFF_C000_0000_0000..0xFFFF_DFFF_FFFF_FFFF  VMALLOC region (PML4[384])
//   0xFFFF_FFFF_8000_0000..0xFFFF_FFFF_FFFF_FFFF  Kernel image (PML4[511])
//
// Null guard: VmmMapPage refuses any mapping below VIRTUAL_NULL_GUARD.
// This ensures no kernel code can inadvertently create a mapping that would
// make a null dereference succeed. User page tables will never include this
// range, so user-mode null dereferences always fault.

// Virtual addresses below this are permanently reserved (null pointer guard).
static constexpr uint64_t VIRTUAL_NULL_GUARD = 1ULL << 30; // 1GB

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
