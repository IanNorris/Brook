#pragma once

#include <stdint.h>

namespace brook {

// Kernel heap — dynamic allocation backed by the VMM.
//
// Boundary-tag allocator (Knuth/dlmalloc style):
//   [BlockHeader][user data][BlockFooter]
//
// Header and footer store the block size, allowing O(1) coalescing in both
// directions. Magic values detect common corruption patterns.
//
// Alignment: all allocations are 16-byte aligned.
// Expansion: the heap grows by calling VmmAllocPages() when exhausted.

// Initialise the heap. Must be called after VmmInit().
extern "C" void HeapInit();

// Allocate at least 'size' bytes. Returns a 16-byte-aligned pointer, or
// nullptr on failure. Requesting 0 bytes returns nullptr.
extern "C" void* kmalloc(uint64_t size);

// Free a pointer returned by kmalloc. No-op on nullptr.
// Writes a trap value over the header if HEAP_POISON is defined.
extern "C" void kfree(void* ptr);

// Resize an allocation. Equivalent to alloc+copy+free if the block cannot
// be extended in-place. Returns nullptr (and leaves ptr untouched) on failure.
extern "C" void* krealloc(void* ptr, uint64_t newSize);

// Diagnostic: return number of free bytes in the current heap region.
uint64_t HeapFreeBytes();

} // namespace brook
