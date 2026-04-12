#include "physical_memory.h"
#include "serial.h"
#include "spinlock.h"

// Forward-declared to avoid circular headers.
namespace brook {
    extern "C" void* kmalloc(uint64_t);
    VirtualAddress VmmAllocPages(uint64_t pageCount, uint64_t flags,
                                 MemTag tag, uint16_t pid);
}

// Linker-defined symbol — end of the kernel image (virtual address).
// Declared outside any namespace so the linker resolves it correctly.
extern "C" uint8_t __kernel_end_sym[] __asm__("__kernel_end");

namespace brook {

// ---------------------------------------------------------------------------
// Bitmap storage — 4MB in BSS (zeroed by ELF loader), covers 128GB physical.
// Bit = 0 means free, bit = 1 means used/reserved.
// ---------------------------------------------------------------------------

static constexpr uint64_t PAGE_SIZE       = 4096;
static constexpr uint64_t MAX_PHYS_GB     = 128;
static constexpr uint64_t MAX_PHYS_PAGES  = (MAX_PHYS_GB * 1024ULL * 1024 * 1024) / PAGE_SIZE;
static constexpr uint64_t BITMAP_WORDS    = MAX_PHYS_PAGES / 64; // 524288 words = 4MB

static uint64_t g_bitmap[BITMAP_WORDS]; // in BSS, starts zeroed
static uint64_t g_totalPages = 0;       // highest tracked page index
static uint64_t g_freePages  = 0;
static uint64_t g_nextHint   = 0;       // search hint for fast sequential alloc

// SMP lock protecting the bitmap, free count, and page descriptors.
static SpinLock g_pmmLock;

// ---------------------------------------------------------------------------
// Ownership tracking — dynamically allocated after PmmEnableTracking().
// Null until then; all tag/pid operations are no-ops before that.
// ---------------------------------------------------------------------------

static PageDescriptor* g_pageDescs = nullptr;  // [g_totalPages], via kmalloc

// Per-PID doubly-linked page lists.  Static (16KB), indexed by PID.
static PidList g_pidLists[PMM_MAX_PIDS] = {};

// ---------------------------------------------------------------------------
// Internal list helpers (all require g_pageDescs != nullptr)
// ---------------------------------------------------------------------------

static inline PageDescriptor& Desc(uint32_t idx) { return g_pageDescs[idx]; }

// Remove a page from whatever PID list it currently belongs to.
// Does NOT reset the descriptor's tag/pid — caller must do that.
static void ListRemove(uint32_t idx)
{
    PageDescriptor& d = Desc(idx);
    uint16_t pid = d.pid;

    if (d.prev != PMM_NULL_PAGE)
        Desc(d.prev).next = d.next;
    else
        g_pidLists[pid].head = d.next;  // idx was the head

    if (d.next != PMM_NULL_PAGE)
        Desc(d.next).prev = d.prev;
    else
        g_pidLists[pid].tail = d.prev;  // idx was the tail

    if (g_pidLists[pid].pageCount > 0)
        g_pidLists[pid].pageCount--;

    d.next = d.prev = PMM_NULL_PAGE;
}

// Append a page to the tail of a PID's list and set its tag.
// The page must NOT currently be in any list (next/prev == PMM_NULL_PAGE).
static void ListAppend(uint32_t idx, uint16_t pid, MemTag tag)
{
    PageDescriptor& d = Desc(idx);
    d.pid  = pid;
    d.tag  = static_cast<uint8_t>(tag);
    d.next = PMM_NULL_PAGE;
    d.prev = g_pidLists[pid].tail;

    if (g_pidLists[pid].tail != PMM_NULL_PAGE)
        Desc(g_pidLists[pid].tail).next = idx;
    else
        g_pidLists[pid].head = idx;  // list was empty

    g_pidLists[pid].tail = idx;
    g_pidLists[pid].pageCount++;
}

static inline void TrackAlloc(uint32_t pageIdx, MemTag tag, uint16_t pid)
{
    // Free pages are not in any list; just append to the new owner's list.
    if (!g_pageDescs) return;
    ListAppend(pageIdx, pid, tag);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline bool IsUsed(uint64_t idx)
{
    return (g_bitmap[idx / 64] >> (idx % 64)) & 1ULL;
}

static inline void SetUsed(uint64_t idx)
{
    g_bitmap[idx / 64] |= (1ULL << (idx % 64));
}

static inline void SetFree(uint64_t idx)
{
    g_bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}

// Mark [physBase, physBase + pages*PAGE_SIZE) as used.
// Decrements g_freePages for each page that was previously free.
static void MarkRangeUsed(uint64_t physBase, uint64_t pages)
{
    if (pages == 0) return;
    uint64_t first = physBase / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++)
    {
        uint64_t idx = first + i;
        if (idx >= MAX_PHYS_PAGES) break;
        if (!IsUsed(idx))
        {
            SetUsed(idx);
            g_freePages--;
        }
    }
}

// Mark [physBase, physBase + pages*PAGE_SIZE) as free.
// Increments g_freePages for each page that was previously used.
static void MarkRangeFree(uint64_t physBase, uint64_t pages)
{
    if (pages == 0) return;
    uint64_t first = physBase / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++)
    {
        uint64_t idx = first + i;
        if (idx >= MAX_PHYS_PAGES) break;
        if (IsUsed(idx))
        {
            SetFree(idx);
            g_freePages++;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void PmmInit(const BootProtocol* proto)
{
    // Step 1: Mark every page as used (all-ones). We do this explicitly
    // because BSS starts as zero (= all free), which is unsafe as a default.
    for (uint64_t w = 0; w < BITMAP_WORDS; w++)
        g_bitmap[w] = ~0ULL;

    g_freePages  = 0;
    g_totalPages = 0;
    g_nextHint   = 0;

    // Step 2: Determine total tracked page count from the HIGHEST conventional
    // memory address. Skip MMIO entries — they live at high physical addresses
    // (PCIe BARs etc.) and would otherwise inflate totalPages to the cap.
    for (uint32_t i = 0; i < proto->memoryMapCount; i++)
    {
        const MemoryDescriptor& d = proto->memoryMap[i];
        if (d.type == MemoryType::Mmio)    continue;
        if (d.type == MemoryType::Reserved) continue;
        uint64_t endPage = (d.physicalStart / PAGE_SIZE) + d.pageCount;
        if (endPage > g_totalPages) g_totalPages = endPage;
    }
    if (g_totalPages > MAX_PHYS_PAGES) g_totalPages = MAX_PHYS_PAGES;

    // Step 3: Free all regions that the memory map says are usable.
    for (uint32_t i = 0; i < proto->memoryMapCount; i++)
    {
        const MemoryDescriptor& d = proto->memoryMap[i];
        if (d.type == MemoryType::Free)
            MarkRangeFree(d.physicalStart, d.pageCount);
    }

    // Step 4: Re-mark regions that must stay reserved.

    // Low 1MB — ISA legacy, real-mode IVT, BIOS data, memory-mapped devices.
    MarkRangeUsed(0, 256);

    // Kernel image (includes BSS where this bitmap lives).
    static constexpr uint64_t KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ULL;
    uint64_t kernVirtEnd  = reinterpret_cast<uint64_t>(&__kernel_end_sym);
    uint64_t kernPhysSize = kernVirtEnd - KERNEL_VIRT_BASE;
    uint64_t kernPages    = (kernPhysSize + PAGE_SIZE - 1) / PAGE_SIZE;
    MarkRangeUsed(proto->kernelPhysBase, kernPages);

    // Framebuffer — linear buffer, typically MMIO-mapped but must not be paged out.
    uint64_t fbBytes = static_cast<uint64_t>(proto->framebuffer.stride) * proto->framebuffer.height;
    uint64_t fbPages = (fbBytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (fbPages > 0)
        MarkRangeUsed(proto->framebuffer.physicalBase, fbPages);

    // BootData / BootloaderCode regions — already marked used (not freed in step 3)
    // but explicitly confirm them here to be safe.
    for (uint32_t i = 0; i < proto->memoryMapCount; i++)
    {
        const MemoryDescriptor& d = proto->memoryMap[i];
        if (d.type == MemoryType::BootData || d.type == MemoryType::BootloaderCode)
            MarkRangeUsed(d.physicalStart, d.pageCount);
    }

    SerialPrintf("PMM: %u MB free of %u MB total physical\n",
                 static_cast<uint32_t>((g_freePages * PAGE_SIZE) / (1024 * 1024)),
                 static_cast<uint32_t>((g_totalPages * PAGE_SIZE) / (1024 * 1024)));
}

PhysicalAddress PmmAllocPage(MemTag tag, uint16_t pid)
{
    uint64_t flags = SpinLockAcquire(&g_pmmLock);

    // Search from hint forward, then wrap around once.
    uint64_t startWord = g_nextHint / 64;
    uint64_t endWord   = (g_totalPages + 63) / 64;

    for (int pass = 0; pass < 2; pass++)
    {
        uint64_t wStart = (pass == 0) ? startWord : 0;
        uint64_t wEnd   = (pass == 0) ? endWord   : startWord;

        for (uint64_t w = wStart; w < wEnd; w++)
        {
            if (g_bitmap[w] == ~0ULL) continue; // all used

            // Find first free bit in this word.
            int bit = __builtin_ctzll(~g_bitmap[w]);
            uint64_t idx = w * 64 + static_cast<uint64_t>(bit);
            if (idx >= g_totalPages) { SpinLockRelease(&g_pmmLock, flags); return PhysicalAddress{}; }

            SetUsed(idx);
            g_freePages--;
            g_nextHint = idx + 1;
            TrackAlloc(static_cast<uint32_t>(idx), tag, pid);
            SpinLockRelease(&g_pmmLock, flags);
            return PhysicalAddress(idx * PAGE_SIZE);
        }
    }
    SpinLockRelease(&g_pmmLock, flags);
    return PhysicalAddress{}; // out of memory
}

PhysicalAddress PmmAllocPages(uint64_t count, MemTag tag, uint16_t pid)
{
    if (count == 0) return PhysicalAddress{};
    if (count == 1) return PmmAllocPage(tag, pid);

    uint64_t flags = SpinLockAcquire(&g_pmmLock);

    // Linear scan for a contiguous run of 'count' free pages.
    uint64_t runStart = 0;
    uint64_t runLen   = 0;

    for (uint64_t idx = 0; idx < g_totalPages; idx++)
    {
        if (!IsUsed(idx))
        {
            if (runLen == 0) runStart = idx;
            runLen++;
            if (runLen == count)
            {
                for (uint64_t i = 0; i < count; i++)
                {
                    SetUsed(runStart + i);
                    g_freePages--;
                    TrackAlloc(static_cast<uint32_t>(runStart + i), tag, pid);
                }
                SpinLockRelease(&g_pmmLock, flags);
                return PhysicalAddress(runStart * PAGE_SIZE);
            }
        }
        else
        {
            runLen = 0;
        }
    }
    SpinLockRelease(&g_pmmLock, flags);
    return PhysicalAddress{}; // no contiguous run found
}

void PmmFreePage(PhysicalAddress physAddr)
{
    if (!physAddr) return;
    if ((physAddr.raw() & (PAGE_SIZE - 1)) != 0) return;

    uint64_t idx = physAddr.raw() / PAGE_SIZE;
    if (idx >= g_totalPages) return;

    uint64_t flags = SpinLockAcquire(&g_pmmLock);

    if (!IsUsed(idx)) { SpinLockRelease(&g_pmmLock, flags); return; }

    SetFree(idx);
    g_freePages++;
    if (idx < g_nextHint) g_nextHint = idx;

    if (g_pageDescs)
    {
        ListRemove(static_cast<uint32_t>(idx));
        PageDescriptor& d = Desc(static_cast<uint32_t>(idx));
        d.pid = 0;
        d.tag = static_cast<uint8_t>(MemTag::Free);
    }

    SpinLockRelease(&g_pmmLock, flags);
}

void PmmSetOwner(PhysicalAddress physAddr, MemTag tag, uint16_t pid)
{
    if ((physAddr.raw() & (PAGE_SIZE - 1)) != 0) return;
    if (!g_pageDescs) return;
    uint64_t idx64 = physAddr.raw() / PAGE_SIZE;
    if (idx64 >= g_totalPages) return;
    uint32_t idx = static_cast<uint32_t>(idx64);
    if (Desc(idx).pid != 0 || Desc(idx).tag != static_cast<uint8_t>(MemTag::Free))
        ListRemove(idx);
    ListAppend(idx, pid, tag);
}

MemTag PmmGetTag(PhysicalAddress physAddr)
{
    if (!g_pageDescs) return MemTag::KernelData;
    uint64_t idx = physAddr.raw() / PAGE_SIZE;
    if (idx >= g_totalPages) return MemTag::System;
    return static_cast<MemTag>(Desc(static_cast<uint32_t>(idx)).tag);
}

uint16_t PmmGetPid(PhysicalAddress physAddr)
{
    if (!g_pageDescs) return KernelPid;
    uint64_t idx = physAddr.raw() / PAGE_SIZE;
    if (idx >= g_totalPages) return KernelPid;
    return Desc(static_cast<uint32_t>(idx)).pid;
}

void PmmEnableTracking()
{
    // Allocate descriptor array via VmmAllocPages — too large for a single
    // kmalloc call (g_totalPages * 12B can be ~768KB). As a permanent
    // system-lifetime allocation, bypassing the heap is appropriate.
    static constexpr uint64_t PAGE_SIZE_LOCAL = 4096;
    static constexpr uint64_t VMM_WRITABLE_LOCAL = (1ULL << 1);
    uint64_t descBytes = g_totalPages * sizeof(PageDescriptor);
    uint64_t descPages = (descBytes + PAGE_SIZE_LOCAL - 1) / PAGE_SIZE_LOCAL;
    VirtualAddress descVirt = VmmAllocPages(descPages, VMM_WRITABLE_LOCAL,
                                            MemTag::KernelData, KernelPid);
    g_pageDescs = reinterpret_cast<PageDescriptor*>(descVirt.raw());

    if (!descVirt)
    {
        g_pageDescs = nullptr;
        SerialPuts("PMM: WARNING: tracking allocation failed — ownership disabled\n");
        return;
    }

    // Initialise all lists to empty.
    for (uint32_t i = 0; i < PMM_MAX_PIDS; i++)
        g_pidLists[i] = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0, 0 };

    // Initialise all descriptors to a known state before building lists.
    for (uint32_t i = 0; i < static_cast<uint32_t>(g_totalPages); i++)
    {
        g_pageDescs[i] = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0,
                           static_cast<uint8_t>(MemTag::Free), 0 };
    }

    // Backfill: add used pages to KernelPid's list; free pages are left
    // out of all lists (the bitmap IS the free pool — no list needed).
    uint32_t usedCount = 0, freeCount = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(g_totalPages); i++)
    {
        if (IsUsed(i))
        {
            ListAppend(i, KernelPid, MemTag::KernelData);
            usedCount++;
        }
        else
        {
            // Free page: descriptor stays initialised to Free/0/PMM_NULL_PAGE.
            freeCount++;
        }
    }

    SerialPrintf("PMM: tracking enabled — %u pages (%u used, %u free), "
                 "descriptors: %u KB\n",
                 static_cast<uint32_t>(g_totalPages),
                 usedCount, freeCount,
                 static_cast<uint32_t>(g_totalPages * sizeof(PageDescriptor) / 1024));
}

void PmmKillPid(uint16_t pid)
{
    if (!g_pageDescs) return;
    if (pid == KernelPid) return;  // never kill kernel pages

    uint64_t flags = SpinLockAcquire(&g_pmmLock);

    uint32_t idx = g_pidLists[pid].head;
    uint32_t count = 0;

    while (idx != PMM_NULL_PAGE)
    {
        uint32_t next = Desc(idx).next;

        // Clear bitmap bit and update stats.
        if (IsUsed(idx))
        {
            SetFree(idx);
            g_freePages++;
            if (idx < g_nextHint) g_nextHint = idx;
        }

        // Reset descriptor to free state (not in any list).
        Desc(idx) = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0,
                      static_cast<uint8_t>(MemTag::Free), 0 };
        count++;

        idx = next;
    }

    g_pidLists[pid] = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0, 0 };

    SpinLockRelease(&g_pmmLock, flags);

    SerialPrintf("PMM: PmmKillPid(%u): freed %u pages\n",
                 static_cast<uint32_t>(pid), count);
}

void PmmFreeByTag(uint16_t pid, MemTag tag)
{
    if (!g_pageDescs) return;
    if (pid == KernelPid) return;

    uint64_t flags = SpinLockAcquire(&g_pmmLock);

    uint32_t idx = g_pidLists[pid].head;
    uint32_t count = 0;

    while (idx != PMM_NULL_PAGE)
    {
        uint32_t next = Desc(idx).next;

        if (static_cast<MemTag>(Desc(idx).tag) == tag && IsUsed(idx))
        {
            // Remove from PID list
            uint32_t prev = Desc(idx).prev;
            if (prev != PMM_NULL_PAGE)
                Desc(prev).next = next;
            else
                g_pidLists[pid].head = next;
            if (next != PMM_NULL_PAGE)
                Desc(next).prev = prev;
            else
                g_pidLists[pid].tail = prev;
            g_pidLists[pid].pageCount--;

            // Free the page
            SetFree(idx);
            g_freePages++;
            if (idx < g_nextHint) g_nextHint = idx;
            Desc(idx) = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0,
                          static_cast<uint8_t>(MemTag::Free), 0 };
            count++;
        }

        idx = next;
    }

    SpinLockRelease(&g_pmmLock, flags);

    (void)count;
    DbgPrintf("PMM: PmmFreeByTag(%u, %u): freed %u pages\n",
              static_cast<uint32_t>(pid), static_cast<uint32_t>(tag), count);
}

void PmmEnumeratePid(uint16_t pid,
                     bool (*callback)(PhysicalAddress physAddr, MemTag tag, void* ctx),
                     void* ctx)
{
    if (!g_pageDescs) return;

    uint32_t idx = g_pidLists[pid].head;
    while (idx != PMM_NULL_PAGE)
    {
        uint32_t next = Desc(idx).next;
        if (!callback(PhysicalAddress(static_cast<uint64_t>(idx) * PAGE_SIZE),
                      static_cast<MemTag>(Desc(idx).tag), ctx))
            break;
        idx = next;
    }
}

void PmmDumpPidStats()
{
    if (!g_pageDescs)
    {
        SerialPuts("PMM: tracking not enabled\n");
        return;
    }
    SerialPuts("PMM: per-PID page counts:\n");
    for (uint32_t p = 0; p < PMM_MAX_PIDS; p++)
    {
        if (g_pidLists[p].pageCount > 0)
            SerialPrintf("  PID %u: %u pages\n", p, g_pidLists[p].pageCount);
    }
}

uint64_t PmmGetFreePageCount()  { return g_freePages;  }
uint64_t PmmGetTotalPageCount() { return g_totalPages; }

} // namespace brook
