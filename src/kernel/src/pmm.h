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
// Ownership tracking uses a PageDescriptor per page — dynamically allocated
// after PmmEnableTracking() (requires kernel heap). Pages owned by a PID are
// kept in a doubly-linked list per PID, enabling O(pages_owned) process kill
// rather than O(total_pages) scanning.

// Sentinel value for PageDescriptor linked-list terminators.
static constexpr uint32_t PMM_NULL_PAGE = 0xFFFFFFFFu;

// Maximum number of PIDs tracked simultaneously.
static constexpr uint32_t PMM_MAX_PIDS  = 1024;

// Per-page ownership record.  12 bytes, doubly-linked within its PID's list.
struct PageDescriptor
{
    uint32_t next;   // next page index in PID's list (PMM_NULL_PAGE = tail)
    uint32_t prev;   // prev page index in PID's list (PMM_NULL_PAGE = head)
    uint16_t pid;    // owning PID
    uint8_t  tag;    // MemTag value
    uint8_t  _pad;
};

// Head/tail/count for one PID's page list.
struct PidList
{
    uint32_t head;        // first page index (PMM_NULL_PAGE = empty)
    uint32_t tail;        // last page index
    uint32_t pageCount;   // pages currently owned
    uint32_t _pad;
};

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
extern "C" uint64_t PmmAllocPage(MemTag tag = MemTag::KernelData, uint16_t pid = KernelPid);

// Allocate 'count' contiguous 4KB page frames with the given tag/pid.
// Returns the physical base address on success, 0 if no contiguous run found.
extern "C" uint64_t PmmAllocPages(uint64_t count,
                       MemTag tag = MemTag::KernelData,
                       uint16_t pid = KernelPid);

// Free a previously allocated page frame. No-op if physAddr is 0.
void PmmFreePage(uint64_t physAddr);

// Ownership: set/get tag and PID for a page.
// SetOwner is safe to call before PmmEnableTracking (no-op until then).
void     PmmSetOwner(uint64_t physAddr, MemTag tag, uint16_t pid = KernelPid);
MemTag   PmmGetTag(uint64_t physAddr);
uint16_t PmmGetPid(uint64_t physAddr);

// Free all physical pages owned by a PID and return them to the free pool.
// Walks the PID's page list (O(pages owned)), clears bitmap bits, then resets
// the PID list. PmmKillPid(KernelPid) is a no-op to protect kernel pages.
void PmmKillPid(uint16_t pid);

// Enumerate pages owned by a PID. Calls callback(physAddr, tag, ctx) for each.
// Callback returns false to stop early. Safe to call before PmmEnableTracking
// (no-op if tracking not yet enabled).
void PmmEnumeratePid(uint16_t pid,
                     bool (*callback)(uint64_t physAddr, MemTag tag, void* ctx),
                     void* ctx);

// Print a per-PID page count summary to serial (for debugging).
void PmmDumpPidStats();

// Statistics — useful for diagnostics and tests.
uint64_t PmmGetFreePageCount();
uint64_t PmmGetTotalPageCount();

} // namespace brook
