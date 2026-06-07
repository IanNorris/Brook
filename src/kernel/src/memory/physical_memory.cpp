#include "physical_memory.h"
#include "serial.h"
#include "spinlock.h"
#include "portio.h"

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
// IrqSpinLock: memory allocation can be called from any context, and a timer
// interrupt preempting a lock holder would deadlock if the new thread tries
// the same lock on the same CPU.
static IrqSpinLock g_pmmLock;

// ---------------------------------------------------------------------------
// BRO-176 DIAGNOSTIC: low-perturbation free-log.
// The double-free is a Heisenbug — heavy per-free page-table-walk audits slow
// the kernel enough to mask the race. This is the cheap alternative: every
// USER-page ref/unref events write a single ring record (phys, op, pid, count)
// with NO page-table walk and minimal locking (g_pmmLock already held). The
// crash handler (idt.cpp) translates the faulting pointer -> phys and dumps that
// frame's full ref/unref trail, naming the exact unmatched op (the COW undercount).
// TEMPORARY — strip with the rest of the BRO-176 instrumentation.
// ---------------------------------------------------------------------------
// op codes for FreeRec
enum : uint8_t {
    REFOP_ALLOC = 0,   // fresh allocation, refcount := 1
    REFOP_REF   = 1,   // PmmRefPage, refcount++
    REFOP_DEC   = 2,   // PmmUnrefPage/Free/Kill decremented (still shared)
    REFOP_FREE  = 3,   // actually freed (refcount hit 0)
};
struct FreeRec { uint64_t phys; uint32_t seq; uint16_t ownerPid; uint8_t op; uint8_t count; const char* site; };
static constexpr uint32_t FREELOG_SIZE = 1u << 17; // 131072 records (ref+unref doubles volume)
static FreeRec  g_freeLog[FREELOG_SIZE];
static uint32_t g_freeLogSeq = 0;
static bool     g_freeLogOn  = false;

// Caller MUST hold g_pmmLock.
static inline void RefLogRecord(uint64_t phys, uint16_t ownerPid, uint8_t op,
                                uint8_t count, const char* site)
{
    if (!g_freeLogOn) return;
    uint32_t i = g_freeLogSeq & (FREELOG_SIZE - 1);
    g_freeLog[i] = { phys & ~0xFFFULL, ++g_freeLogSeq, ownerPid, op, count, site };
}
// Back-compat shim for the existing free sites (op=FREE, count=0).
static inline void FreeLogRecord(uint64_t phys, uint16_t ownerPid, const char* site)
{
    RefLogRecord(phys, ownerPid, REFOP_FREE, 0, site);
}

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
// Idempotent: a no-op if the page is not currently on its PID's list, so it
// is safe to call repeatedly (e.g. when a shared page is unref'd by several
// owners during teardown — see PmmUnrefPage/PmmKillPid, BRO-161).
static void ListRemove(uint32_t idx)
{
    PageDescriptor& d = Desc(idx);
    uint16_t pid = d.pid;

    // Not linked: no prev/next and not the list head -> already removed.
    if (d.prev == PMM_NULL_PAGE && d.next == PMM_NULL_PAGE &&
        g_pidLists[pid].head != idx)
        return;

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

// BRO-176: lock-free reflog dump (defined later); callers below already hold
// g_pmmLock, so they must use this variant — PmmDumpFreeLog re-takes the lock.
static int PmmDumpFreeLogLocked(uint64_t phys);

static inline void TrackAlloc(uint32_t pageIdx, MemTag tag, uint16_t pid)
{
    // Free pages are not in any list; just append to the new owner's list.
    if (!g_pageDescs) return;
    ListAppend(pageIdx, pid, tag);
    Desc(pageIdx).refCount = 1;  // exclusive ownership on fresh allocation
    // BRO-176 stale-mapping detector (ALLOC side). A frame returned by the
    // allocator must have NO existing USER PTE mapping it — it was on the free
    // list. If mapCount is already nonzero here, some process still maps this
    // physical frame even though we are about to hand it to a NEW owner: a PTE
    // outlived its frame's free (the stale-mapping bug). The free-time check
    // MISSES this case because the stale mapping was uncounted in the freeing
    // process's generation and the doomed process's later teardown MapDec brings
    // the count back to 0. Catch it HERE, at the instant of the colliding
    // allocation, before we reset — naming both the new owner and the frame.
    uint16_t preMap = __atomic_load_n(&Desc(pageIdx).mapCount, __ATOMIC_RELAXED);
    if (preMap != 0)
    {
        SerialPrintf("BRO176-ALLOCMAPPED: phys=0x%lx handed to pid=%u but mapCount=%u "
                     "— still mapped by a stale PTE (freed-while-mapped, missed at free)!\n",
                     static_cast<uint64_t>(pageIdx) * PAGE_SIZE, (unsigned)pid,
                     (unsigned)preMap);
        PmmDumpFreeLogLocked(static_cast<uint64_t>(pageIdx) * PAGE_SIZE);
    }
    // A freshly (re)allocated frame has no mappers yet. Reset the map accounting
    // so a recycled page starts clean regardless of how it was freed.
    __atomic_store_n(&Desc(pageIdx).mapCount, 0, __ATOMIC_RELAXED);
    if (tag == MemTag::User)
        RefLogRecord(static_cast<uint64_t>(pageIdx) * PAGE_SIZE, pid, REFOP_ALLOC, 1, "alloc");
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
    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);

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
            if (idx >= g_totalPages) { IrqSpinLockRelease(&g_pmmLock, pmmFlags); return PhysicalAddress{}; }

            SetUsed(idx);
            g_freePages--;
            g_nextHint = idx + 1;
            TrackAlloc(static_cast<uint32_t>(idx), tag, pid);
            IrqSpinLockRelease(&g_pmmLock, pmmFlags);

            return PhysicalAddress(idx * PAGE_SIZE);
        }
    }
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
    return PhysicalAddress{}; // out of memory
}

PhysicalAddress PmmAllocPages(uint64_t count, MemTag tag, uint16_t pid)
{
    if (count == 0) return PhysicalAddress{};
    if (count == 1) return PmmAllocPage(tag, pid);

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);

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
                IrqSpinLockRelease(&g_pmmLock, pmmFlags);
                return PhysicalAddress(runStart * PAGE_SIZE);
            }
        }
        else
        {
            runLen = 0;
        }
    }
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
    return PhysicalAddress{}; // no contiguous run found
}

// BRO-176: forward declarations for the stale-mapping leak check (defined after
// PmmUnrefPage). Used by the free sites below.
extern "C" int PmmDumpFreeLog(uint64_t phys);
static inline void MapLeakCheckLocked(uint32_t idx, const char* site);

void PmmFreePage(PhysicalAddress physAddr)
{
    if (!physAddr) return;
    if ((physAddr.raw() & (PAGE_SIZE - 1)) != 0) return;

    uint64_t idx = physAddr.raw() / PAGE_SIZE;
    if (idx >= g_totalPages) return;

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);

    if (!IsUsed(idx)) { IrqSpinLockRelease(&g_pmmLock, pmmFlags); return; }

    // If refcounted and shared, just decrement — don't free yet. Also drop the
    // page from this owner's PID list: once a page is shared, the page-table
    // walk (FreeTableLevel) is the single freeing authority and decrements once
    // per mapper at each process's exit. Leaving the page on the original
    // owner's list would let PmmKillPid later free it out from under a live
    // co-owner (BRO-161). ListRemove is idempotent, so repeated unrefs are safe.
    if (g_pageDescs && Desc(static_cast<uint32_t>(idx)).refCount > 1)
    {
        auto& dd = Desc(static_cast<uint32_t>(idx));
        dd.refCount--;
        if (dd.tag == static_cast<uint8_t>(MemTag::User))
            RefLogRecord(physAddr.raw(), dd.pid, REFOP_DEC, dd.refCount, "PmmFreePage");
        ListRemove(static_cast<uint32_t>(idx));
        IrqSpinLockRelease(&g_pmmLock, pmmFlags);
        return;
    }

    SetFree(idx);
    g_freePages++;
    if (idx < g_nextHint) g_nextHint = idx;

    if (g_pageDescs)
    {
        PageDescriptor& d = Desc(static_cast<uint32_t>(idx));
        MapLeakCheckLocked(static_cast<uint32_t>(idx), "PmmFreePage");
        if (d.tag == static_cast<uint8_t>(MemTag::User))
            FreeLogRecord(physAddr.raw(), d.pid, "PmmFreePage");
        ListRemove(static_cast<uint32_t>(idx));
        d.pid = 0;
        d.tag = static_cast<uint8_t>(MemTag::Free);
        d.refCount = 0;
        __atomic_store_n(&d.mapCount, 0, __ATOMIC_RELAXED);
    }

    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
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

// ---------------------------------------------------------------------------
// COW reference counting
// ---------------------------------------------------------------------------

void PmmRefPage(PhysicalAddress physAddr)
{
    if (!g_pageDescs || !physAddr) return;
    uint64_t idx64 = physAddr.raw() / PAGE_SIZE;
    if (idx64 >= g_totalPages) return;
    uint32_t idx = static_cast<uint32_t>(idx64);

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);
    auto& d = Desc(idx);
    if (d.refCount == 0)
        d.refCount = 2;  // legacy page: count existing owner + new sharer
    else if (d.refCount < 255)
        d.refCount++;
    if (d.tag == static_cast<uint8_t>(MemTag::User))
        RefLogRecord(physAddr.raw(), d.pid, REFOP_REF, d.refCount, "PmmRefPage");
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
}

void PmmUnrefPage(PhysicalAddress physAddr)
{
    if (!g_pageDescs || !physAddr) return;
    uint64_t idx64 = physAddr.raw() / PAGE_SIZE;
    if (idx64 >= g_totalPages) return;
    uint32_t idx = static_cast<uint32_t>(idx64);

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);
    auto& d = Desc(idx);
    if (d.refCount > 1)
    {
        d.refCount--;
        if (d.tag == static_cast<uint8_t>(MemTag::User))
            RefLogRecord(physAddr.raw(), d.pid, REFOP_DEC, d.refCount, "PmmUnrefPage");
        // Drop the page from its owner's PID list — see PmmFreePage and BRO-161.
        // Once shared, freeing is driven solely by the page-table walk (one
        // unref per mapper); leaving it listed lets PmmKillPid free it under a
        // live co-owner. ListRemove is idempotent.
        ListRemove(idx);
        IrqSpinLockRelease(&g_pmmLock, pmmFlags);
        return; // still shared, don't free
    }
    // refCount is 0 or 1 — this was the last (or only) reference, actually free
    if (IsUsed(idx))
    {
        MapLeakCheckLocked(idx, "PmmUnrefPage");
        if (d.tag == static_cast<uint8_t>(MemTag::User))
            FreeLogRecord(physAddr.raw(), d.pid, "PmmUnrefPage");
        SetFree(idx);
        g_freePages++;
        if (idx < g_nextHint) g_nextHint = idx;
    }
    ListRemove(idx);
    d = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0,
          static_cast<uint8_t>(MemTag::Free), 0, 0 };
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
}

// BRO-176 stale-mapping detector ------------------------------------------------
// Called at an ACTUAL free (refCount reached 0) while holding g_pmmLock. If a
// User frame is freed while a present USER PTE still maps it (mapCount != 0), the
// mapping outlived its reference — the BRO-176 stale-mapping bug. Name it
// red-handed, at the instant of the erroneous free, with its full ref/unref
// trail, BEFORE the page is recycled and poison is ever read.
static inline void MapLeakCheckLocked(uint32_t idx, const char* site)
{
    PageDescriptor& d = Desc(idx);
    if (d.tag != static_cast<uint8_t>(MemTag::User)) return;
    uint16_t mc = __atomic_load_n(&d.mapCount, __ATOMIC_RELAXED);
    if (mc != 0)
    {
        SerialPrintf("BRO176-MAPLEAK: phys=0x%lx FREED via %s with mapCount=%u still "
                     "mapped (owner pid=%u refCount=%u) — PTE outlived its reference!\n",
                     static_cast<uint64_t>(idx) * PAGE_SIZE, site, (unsigned)mc,
                     (unsigned)d.pid, (unsigned)d.refCount);
        PmmDumpFreeLogLocked(static_cast<uint64_t>(idx) * PAGE_SIZE);
    }
}

// PmmMapInc/PmmMapDec — O(1) USER-PTE map accounting (see header). Updated under
// the VMM page-table lock (g_userPtLock), which is independent of g_pmmLock, so
// mapCount is touched atomically everywhere.
void PmmMapInc(PhysicalAddress physAddr)
{
    if (!g_pageDescs || !physAddr) return;
    uint64_t idx = physAddr.raw() / PAGE_SIZE;
    if (idx >= g_totalPages) return;
    __atomic_add_fetch(&Desc(static_cast<uint32_t>(idx)).mapCount, 1, __ATOMIC_RELAXED);
}

void PmmMapDec(PhysicalAddress physAddr)
{
    if (!g_pageDescs || !physAddr) return;
    uint64_t idx = physAddr.raw() / PAGE_SIZE;
    if (idx >= g_totalPages) return;
    PageDescriptor& d = Desc(static_cast<uint32_t>(idx));
    if (__atomic_load_n(&d.mapCount, __ATOMIC_RELAXED) == 0)
    {
        // Unmap without a matching map — the opposite-end accounting bug from a
        // leak (a PTE removed twice, or removed for a frame it never mapped).
        SerialPrintf("BRO176-MAPUNDERFLOW: phys=0x%lx mapDec at mapCount=0 "
                     "(pid=%u tag=%u)\n", physAddr.raw(), (unsigned)d.pid, (unsigned)d.tag);
        return;
    }
    __atomic_sub_fetch(&d.mapCount, 1, __ATOMIC_RELAXED);
}

uint8_t PmmGetRefCount(PhysicalAddress physAddr)
{
    if (!g_pageDescs || !physAddr) return 0;
    uint64_t idx64 = physAddr.raw() / PAGE_SIZE;
    if (idx64 >= g_totalPages) return 0;
    return Desc(static_cast<uint32_t>(idx64)).refCount;
}

// BRO-176 crash-time discriminator: report the PMM's current view of a frame so
// the user-#GP sweep can tell apart (a) frame still owned by a USER process
// (stale PTE / shared), (b) frame currently FREE in the bitmap (freed-while-
// mapped), or (c) frame owned by the KERNEL HEAP (tag=Heap) = a frame reachable
// by a user mapping AND the kernel heap at once (the 0xDF source). Lock-free
// read — intended for the fault path only.
extern "C" void PmmDescribe(uint64_t phys, uint32_t* used, uint32_t* refCount,
                            uint32_t* mapCount, uint32_t* tag, uint32_t* ownerPid)
{
    uint64_t idx64 = phys / PAGE_SIZE;
    if (!g_pageDescs || idx64 >= g_totalPages)
    {
        if (used) *used = 0xFF;
        return;
    }
    uint32_t idx = static_cast<uint32_t>(idx64);
    PageDescriptor& d = Desc(idx);
    if (used)     *used     = IsUsed(idx) ? 1u : 0u;
    if (refCount) *refCount = d.refCount;
    if (mapCount) *mapCount = __atomic_load_n(&d.mapCount, __ATOMIC_RELAXED);
    if (tag)      *tag      = d.tag;
    if (ownerPid) *ownerPid = d.pid;
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
                           static_cast<uint8_t>(MemTag::Free), 0, 0 };
    }

    // Backfill: add used pages to KernelPid's list; free pages are left
    // out of all lists (the bitmap IS the free pool — no list needed).
    uint32_t usedCount = 0, freeCount = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(g_totalPages); i++)
    {
        if (IsUsed(i))
        {
            ListAppend(i, KernelPid, MemTag::KernelData);
            Desc(i).refCount = 1;  // exclusive owner
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

    g_freeLogOn = true;  // BRO-176 diag: arm the low-perturbation free-log
}

// BRO-176 diagnostic: print every recorded free of `phys` (most recent first).
// Quiet on miss (so it can be called per-leaf during a whole-page-table sweep).
// Returns the number of matching records found. Takes g_pmmLock.
//
// Uses RAW serial port polling rather than SerialPrintf: this is called from the
// #GP/#PF crash handler AFTER ExcForceSerialLock sets g_panicInProgress, which
// silences SerialPrintf/SerialPuts. Raw port writes bypass that so the owner/site
// (the whole point of the free-log) actually reaches the serial log.
static inline void FlRawChar(char c)
{
    if (c == '\n') {
        while ((inb(0x3FD) & 0x20) == 0) {}
        outb(0x3F8, '\r');
    }
    while ((inb(0x3FD) & 0x20) == 0) {}
    outb(0x3F8, static_cast<uint8_t>(c));
}
static inline void FlRawStr(const char* s) { if (s) while (*s) FlRawChar(*s++); }
static inline void FlRawHex(uint64_t v)
{
    FlRawChar('0'); FlRawChar('x');
    for (int sh = 60; sh >= 0; sh -= 4) {
        int n = (int)((v >> sh) & 0xF);
        FlRawChar((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}
static inline void FlRawDec(uint64_t v)
{
    char b[20]; int i = 0;
    if (!v) b[i++] = '0';
    while (v) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) FlRawChar(b[--i]);
}

// Lock-free reflog dump — caller MUST hold g_pmmLock. Used by the leak/alloc
// checks which run inside the PMM critical section (re-taking g_pmmLock here
// would self-deadlock the non-recursive ticket lock).
static int PmmDumpFreeLogLocked(uint64_t phys)
{
    static const char* opName[4] = { "ALLOC", "REF  ", "DEC  ", "FREE " };
    uint64_t target = phys & ~0xFFFULL;
    int found = 0;
    uint32_t total = g_freeLogSeq;
    uint32_t scan = (total < FREELOG_SIZE) ? total : FREELOG_SIZE;
    static constexpr int MAXSHOW = 24;
    uint32_t hits[MAXSHOW]; int nh = 0;
    for (uint32_t n = 1; n <= scan && nh < MAXSHOW; ++n)
    {
        uint32_t i = (g_freeLogSeq - n) & (FREELOG_SIZE - 1);
        FreeRec& r = g_freeLog[i];
        if (r.phys != target || !r.site) continue;
        hits[nh++] = i;
    }
    for (int k = nh - 1; k >= 0; --k)
    {
        FreeRec& r = g_freeLog[hits[k]];
        FlRawStr("  BRO176-REFLOG phys="); FlRawHex(r.phys);
        FlRawStr(" seq="); FlRawDec(r.seq);
        FlRawStr(" "); FlRawStr(r.op < 4 ? opName[r.op] : "?");
        FlRawStr(" ->count="); FlRawDec(r.count);
        FlRawStr(" pid="); FlRawDec(r.ownerPid);
        FlRawStr(" "); FlRawStr(r.site);
        FlRawChar('\n');
        ++found;
    }
    return found;
}

extern "C" int PmmDumpFreeLog(uint64_t phys)
{
    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);
    int found = PmmDumpFreeLogLocked(phys);
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
    return found;
}

void PmmKillPid(uint16_t pid)
{
    if (!g_pageDescs) return;
    if (pid == KernelPid) return;  // never kill kernel pages

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);

    uint32_t idx = g_pidLists[pid].head;
    [[maybe_unused]] uint32_t count = 0;
    [[maybe_unused]] uint32_t shared = 0;

    while (idx != PMM_NULL_PAGE)
    {
        uint32_t next = Desc(idx).next;

        if (Desc(idx).refCount > 1)
        {
            // COW shared page — decrement refcount, remove from this PID's list
            Desc(idx).refCount--;
            if (Desc(idx).tag == static_cast<uint8_t>(MemTag::User))
                RefLogRecord(static_cast<uint64_t>(idx) * PAGE_SIZE, pid, REFOP_DEC,
                             Desc(idx).refCount, "PmmKillPid");
            ListRemove(idx);  // update neighbours so list stays consistent
            shared++;
        }
        else
        {
            // Exclusive page — actually free it
            if (IsUsed(idx))
            {
                MapLeakCheckLocked(idx, "PmmKillPid");
                if (Desc(idx).tag == static_cast<uint8_t>(MemTag::User))
                    FreeLogRecord(static_cast<uint64_t>(idx) * PAGE_SIZE, pid, "PmmKillPid");
                SetFree(idx);
                g_freePages++;
                if (idx < g_nextHint) g_nextHint = idx;
            }
            Desc(idx) = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0,
                          static_cast<uint8_t>(MemTag::Free), 0, 0 };
        }
        count++;

        idx = next;
    }

    g_pidLists[pid] = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0, 0 };

    IrqSpinLockRelease(&g_pmmLock, pmmFlags);

    DbgPrintf("PMM: PmmKillPid(%u): processed %u pages (%u shared, refcount decremented)\n",
                 static_cast<uint32_t>(pid), count, shared);
}

void PmmFreeByTag(uint16_t pid, MemTag tag)
{
    if (!g_pageDescs) return;
    if (pid == KernelPid) return;

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);

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
                          static_cast<uint8_t>(MemTag::Free), 0, 0 };
            count++;
        }

        idx = next;
    }

    IrqSpinLockRelease(&g_pmmLock, pmmFlags);

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
