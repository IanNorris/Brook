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
// Alignment: all allocations are 64-byte aligned.
// Expansion: the heap grows by calling VmmAllocPages() when exhausted.

// Initialise the heap. Must be called after VmmInit().
extern "C" void HeapInit();

// Allocate at least 'size' bytes. Returns a 64-byte-aligned pointer, or
// nullptr on failure. Requesting 0 bytes returns nullptr.
// uint32 boundary tags limit an aligned total block to 0xffffffc0 bytes;
// with the current 128-byte header/footer, the largest request is 0xffffff40.
// Larger requests fail before mapping pages; the heap itself may span more.
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

// Enable/disable poison fill on alloc/free. Disabled by default.
// Enabling reseeds existing free regions; live allocations are left intact.
// The shell and debug channel accept "heap poison on|off" at runtime.
void HeapSetPoison(bool enable);

// Opt-in allocator timing; no heap walk or allocation in these controls.
// on preserves counters; reset clears them without changing enabled state.
struct HeapTimingStats {
    bool enabled;
    uint64_t allocations, frees, reallocations, expansions;
    uint64_t headers, maxHeaders, expansionHeaders;
    uint64_t waitCycles, maxWaitCycles, holdCycles, maxHoldCycles;
    uint64_t expansionCycles, maxExpansionCycles;
};
void HeapTimingEnable(bool enabled);
void HeapTimingReset();
void HeapTimingSnapshot(HeapTimingStats* out);
// Writes a bounded NUL-terminated report; cycles are raw TSC units.
void HeapTimingFormat(char* out, uint64_t capacity);

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

// BRO-179 forensic: arm PID/seq-encoded heap poison (call once post-SMP, when
// the per-CPU GS base is valid). Until armed, freed blocks use the plain
// 0xDFDFDFDF poison.
void HeapArmForensicPoison();

// BRO-179 forensic: decode a heap-poison qword seen at a crash site into the
// freeing PID + heap-free-seq and dump the kfree callstack that wrote it.
// Returns true if the qword carried the 0xDFDF marker.
bool HeapDecodePoison(uint64_t qword);

} // namespace brook
