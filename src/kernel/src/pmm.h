#pragma once

#include <stdint.h>
#include "boot_protocol/boot_protocol.h"

namespace brook {

// Physical Memory Manager — bitmap allocator for 4KB page frames.
//
// Convention: bitmap bit = 0 means free, 1 means used/reserved.
// Supports up to 64GB of physical RAM (2MB bitmap in BSS).
//
// Must be called once before any allocation, after the kernel is running
// with its own page tables and the boot protocol is accessible.
void PmmInit(const BootProtocol* proto);

// Allocate a single 4KB page frame.
// Returns the physical address on success, 0 on out-of-memory.
uint64_t PmmAllocPage();

// Allocate 'count' contiguous 4KB page frames.
// Returns the physical base address on success, 0 if no contiguous run found.
uint64_t PmmAllocPages(uint64_t count);

// Free a previously allocated page frame. No-op if physAddr is 0.
// Silently ignores double-free (safe but not detected).
void PmmFreePage(uint64_t physAddr);

// Statistics — useful for diagnostics and tests.
uint64_t PmmGetFreePageCount();
uint64_t PmmGetTotalPageCount();

} // namespace brook
