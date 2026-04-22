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

// Walk all heap blocks and verify integrity (magic values, sizes).
// Returns true if the heap is consistent, false if corruption is found.
// Logs details of the first corrupt block to serial.
bool HeapCheckIntegrity();

// Enable/disable poison fill on alloc/free. Enabled by default.
// Disabling improves performance for allocation-heavy workloads.
void HeapSetPoison(bool enable);

// Dump heap statistics to serial: total blocks, free/used counts, sizes.
void HeapDumpStats();

// Snapshot of heap state, filled by HeapGetStats.  Takes the heap lock
// internally; safe to call from any thread.  Used by diagnostic channels.
struct HeapStats {
    uint64_t regionStart;
    uint64_t regionEnd;
    uint64_t heapSizeBytes;
    uint32_t totalBlocks;
    uint32_t usedBlocks;
    uint32_t freeBlocks;
    uint64_t usedBytes;
    uint64_t freeBytes;
    uint32_t largestFreeBlock;
    bool     poisonEnabled;
};
void HeapGetStats(HeapStats* out);

} // namespace brook
