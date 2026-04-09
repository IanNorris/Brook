#pragma once

#include <stdint.h>
#include "boot_protocol/boot_protocol.h"
#include "mem_tag.h"

namespace brook {

// Physical Memory Manager — bitmap allocator for 4KB page frames.
//
// Convention: bitmap bit = 0 means free, 1 means used/reserved.
// Supports up to 128GB of physical RAM (4MB bitmap in BSS).
//
// Ownership tracking (tag + PID) is available after PmmEnableTracking()
// is called — which requires the kernel heap to be initialised first.

// Initialise the PMM from the boot protocol memory map.
// Must be called before any allocation, after the kernel has its own page tables.
void PmmInit(const BootProtocol* proto);

// Enable per-page ownership tracking. Must be called AFTER HeapInit().
// Dynamically allocates tag and PID arrays (sized to g_totalPages) and
// backfills them: all currently-used pages are tagged KernelData/KernelPid.
void PmmEnableTracking();

// Allocate a single 4KB page frame.
// tag and pid default to KernelData/KernelPid for convenience.
// Returns the physical address on success, 0 on out-of-memory.
uint64_t PmmAllocPage(MemTag tag = MemTag::KernelData, uint16_t pid = KernelPid);

// Allocate 'count' contiguous 4KB page frames with the given tag/pid.
// Returns the physical base address on success, 0 if no contiguous run found.
uint64_t PmmAllocPages(uint64_t count,
                       MemTag tag = MemTag::KernelData,
                       uint16_t pid = KernelPid);

// Free a previously allocated page frame. No-op if physAddr is 0.
void PmmFreePage(uint64_t physAddr);

// Ownership: set/get tag and PID for a page.
// SetOwner is safe to call before PmmEnableTracking (no-op until then).
void     PmmSetOwner(uint64_t physAddr, MemTag tag, uint16_t pid = KernelPid);
MemTag   PmmGetTag(uint64_t physAddr);
uint16_t PmmGetPid(uint64_t physAddr);

// Statistics — useful for diagnostics and tests.
uint64_t PmmGetFreePageCount();
uint64_t PmmGetTotalPageCount();

} // namespace brook
