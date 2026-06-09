#pragma once

#include <stdint.h>
#include "address.h"
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
    uint8_t  refCount; // COW reference count (0=untracked, 1=exclusive, 2+=shared)
    uint16_t mapCount; // BRO-176: # of present USER PTEs mapping this frame.
                       // Maintained O(1) at PTE install/remove. Invariant: when a
                       // User page is freed (refCount→0) mapCount MUST be 0; a
                       // nonzero value means a PTE outlived its reference (the
                       // stale-mapping bug). See PmmMapInc/PmmMapDec.
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
// Returns the physical address on success, null PhysicalAddress on OOM.
PhysicalAddress PmmAllocPage(MemTag tag = MemTag::KernelData, uint16_t pid = KernelPid);

// Allocate 'count' contiguous 4KB page frames.
// Returns the physical base address on success, null PhysicalAddress if no run found.
PhysicalAddress PmmAllocPages(uint64_t count,
                              MemTag tag = MemTag::KernelData,
                              uint16_t pid = KernelPid);

// Free a previously allocated page frame. No-op if null.
void PmmFreePage(PhysicalAddress physAddr);


// Ownership: get tag and PID for a page.
MemTag   PmmGetTag(PhysicalAddress physAddr);
uint16_t PmmGetPid(PhysicalAddress physAddr);

// Free all physical pages owned by a PID and return them to the free pool.
// Walks the PID's page list (O(pages owned)), clears bitmap bits, then resets
// the PID list. PmmKillPid(KernelPid) is a no-op to protect kernel pages.
void PmmKillPid(uint16_t pid);

// Free all physical pages owned by a PID that have the given MemTag.
// Leaves other pages (e.g., kernel stack) untouched.
void PmmFreeByTag(uint16_t pid, MemTag tag);

// COW reference counting — increment/decrement refcount on a physical page.
// PmmRefPage increments the refcount (call when sharing a page in fork).
// PmmUnrefPage decrements and frees if it reaches zero.
// PmmGetRefCount returns the current refcount (0 if untracked).
void     PmmRefPage(PhysicalAddress physAddr);
void     PmmUnrefPage(PhysicalAddress physAddr);
// BRO-179: atomically pin a frame only if still live (used + refCount>0).
// Returns true (and increments) if alive, false if already free (never
// resurrects). Used to pin a COW source across the copy.
bool     PmmRefPageIfAlive(PhysicalAddress physAddr);
uint8_t  PmmGetRefCount(PhysicalAddress physAddr);

// BRO-176 stale-mapping detector: track the number of present USER PTEs that
// map a physical frame. Call PmmMapInc when installing a present USER PTE and
// PmmMapDec when clearing/replacing one. When the frame is freed (refCount→0)
// the PMM asserts mapCount==0; a nonzero value names a frame freed while still
// mapped — caught at the instant of the erroneous free, before poison is read.
void     PmmMapInc(PhysicalAddress physAddr);
void     PmmMapDec(PhysicalAddress physAddr);

// Enumerate pages owned by a PID. Calls callback(physAddr, tag, ctx) for each.
void PmmEnumeratePid(uint16_t pid,
                     bool (*callback)(PhysicalAddress physAddr, MemTag tag, void* ctx),
                     void* ctx);

// Print a per-PID page count summary to serial (for debugging).
void PmmDumpPidStats();

// Statistics — useful for diagnostics and tests.
uint64_t PmmGetFreePageCount();
uint64_t PmmGetTotalPageCount();

// BRO-179: start the quarantine drain kernel thread. Must be called once after
// the scheduler is up (so KernelThreadCreate works). Until it runs, freed frames
// queue in quarantine and are released via the safety-valve path; once running,
// it drains them through an all-CPU TLB barrier.
void PmmStartDrainThread();

// BRO-179 forensic: decode a poison qword (0xDFDF-marked) seen at a crash site
// into the original owner PID + free-seq and dump that frame's alloc/free
// callstack history. Returns true if the qword carried the poison marker.
extern "C" bool PmmDecodePoison(uint64_t qword);

} // namespace brook
