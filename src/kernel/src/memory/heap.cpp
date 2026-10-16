#include "heap.h"
#include "virtual_memory.h"
#include "physical_memory.h"
#include "serial.h"
#include "mem_tag.h"
#include "spinlock.h"
#include "string.h"
#ifndef BROOK_HOST_TEST
#include "../unix_lifetime_trace.h"
#include "../ksym_addrs.h"   // KsymFindByAddr for kfree-callstack symbolization
#endif

namespace brook {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr uint32_t HEADER_MAGIC  = 0xB10CBEEF;
static constexpr uint32_t FOOTER_MAGIC  = 0xB10CBEE2;
static constexpr uint64_t ALIGN         = 64;
static constexpr uint64_t INITIAL_PAGES = 256;         // 1MB initial heap
static constexpr uint64_t EXPAND_PAGES  = 256;         // expand by 1MB at a time

// Dedicated virtual region for the heap (PML4[385] — separate from VMALLOC).
static constexpr uint64_t HEAP_VIRT_BASE = 0xFFFFC08000000000ULL;
static constexpr uint64_t HEAP_VIRT_MAX  = 0xFFFFC0FF00000000ULL; // 508GB max

// Poison fill patterns for debugging
static constexpr uint8_t  POISON_ALLOC  = 0xCD;  // uninitialized alloc
static constexpr uint32_t POISON_FREE4  = 0xDFDFDFDF;  // freed memory
static constexpr uint16_t POISON_MARK16 = 0xDFDF;      // high 16 bits of every poison word

// Runtime diagnostic toggle. Normal workloads avoid alloc/free poison fills;
// enable explicitly with "heap poison on" when investigating memory faults.
static volatile bool g_heapPoisonEnabled = false;

// BRO-179 forensic heap poison. When ARMED (post-boot, once the per-CPU GS base
// is valid), a freed block is filled with a pattern that keeps the 0xDFDF marker
// in the high 16 bits of every word — so existing 0xDFDF detectors/greps and the
// write-after-free check still work — and encodes, in the low 16 bits, the PID
// that performed the free and a heap-free sequence, alternating word by word.
// The #DF/#PF register dumps that print a corrupted Process struct then show
// e.g. CUR_PID=0xDFDF0005 KSTACK=0xDFDF1A2C — the freeing PID is readable by eye,
// and HeapDecodePoison() resolves the seq to the kfree callstack. The observed
// SIG1 corruption is heap free-poison written over a LIVE Process struct, so the
// poison that surfaces is the heap's: this names whose kfree did it.
static volatile bool g_heapPoisonForensic = false;

static constexpr uint32_t HEAP_FREELOG_SIZE = 1u << 14;  // 16384 records
static constexpr uint8_t  HEAP_STACK_DEPTH  = 6;
struct HeapFreeRec {
    uint64_t blockVa; uint32_t seq; uint16_t pid; uint8_t depth;
    uint64_t stack[HEAP_STACK_DEPTH];
};
static HeapFreeRec g_heapFreeLog[HEAP_FREELOG_SIZE];
static uint32_t    g_heapFreeSeq = 0;

// RBP frame-chain walk (mirrors profiler.cpp / PMM CaptureKernelStack). Bounded,
// no locks/alloc. Returns captured depth.
static inline uint8_t HeapCaptureStack(uint64_t startRbp, uint64_t* out, uint8_t max)
{
    constexpr uint64_t KBASE = 0xffffffff80000000ULL;
    constexpr uint64_t KEND  = 0xffffffffffffffffULL;
    uint64_t rbp = startRbp; uint8_t depth = 0;
    while (depth < max)
    {
        if (rbp < KBASE || rbp >= KEND - 16 || (rbp & 7) != 0) break;
        const uint64_t* f = reinterpret_cast<const uint64_t*>(rbp);
        uint64_t ret = f[1];
        if (ret < KBASE || ret >= KEND) break;
        out[depth++] = ret;
        uint64_t next = f[0];
        if (next <= rbp) break;
        rbp = next;
    }
    return depth;
}

// Read the current CPU's running PID from the per-CPU block (gs:40). Only valid
// once the GS base is established; callers gate on g_heapPoisonForensic.
static inline uint16_t HeapCurrentPid()
{
    uint64_t pid = 0xFFFF;
    __asm__ volatile("movq %%gs:40, %0" : "=r"(pid));
    return static_cast<uint16_t>(pid);
}

// ---------------------------------------------------------------------------
// Block layout
//
//  ┌─────────────────────┐  ← BlockHeader (64 bytes)
//  │ magic               │
//  │ size (whole block)  │
//  │ free                │
//  ├─────────────────────┤  ← user pointer
//  │                     │
//  │ user data           │
//  │                     │
//  ├─────────────────────┤  ← (header.size - sizeof Footer) bytes from header
//  │ size (whole block)  │  ← BlockFooter (8 bytes)
//  │ magic               │
//  └─────────────────────┘
// ---------------------------------------------------------------------------

struct BlockHeader
{
    uint32_t magic;
    uint32_t size;   // total block size including header + footer
    uint32_t free;   // 1 = free, 0 = allocated
    uint32_t _pad;
    BlockHeader* freePrev; // valid only while indexed as free
    BlockHeader* freeNext;
    uint8_t  _alignPad[32];
};

struct BlockFooter
{
    uint32_t size;
    uint32_t magic;
    uint8_t  _alignPad[56];
};

static_assert(sizeof(BlockHeader) == 64 && sizeof(BlockFooter) == 64, "heap boundary-tag ABI");

static constexpr uint64_t HEADER_SIZE   = sizeof(BlockHeader);
static constexpr uint64_t FOOTER_SIZE   = sizeof(BlockFooter);
static constexpr uint64_t OVERHEAD      = HEADER_SIZE + FOOTER_SIZE;
static constexpr uint64_t MIN_BLOCK     = OVERHEAD + ALIGN; // smallest useful block

// ---------------------------------------------------------------------------
// Heap state
// ---------------------------------------------------------------------------

static uint8_t* g_heapStart   = nullptr;
static uint8_t* g_heapEnd     = nullptr;  // exclusive
static uint64_t g_freeBytes   = 0;
static BlockHeader* g_freeBins[32] = {};
static uint32_t g_freeBinMask = 0;

// SMP lock protecting all heap metadata.
// IrqSpinLock: prevents preemption deadlock when timer interrupt
// fires while a CPU holds the lock.
static IrqSpinLock g_heapLock;

// Runtime-off fast path uses the existing IRQ lock unchanged. Generation drops
// acquisitions that straddle enable/reset, so an old waiter cannot pollute a
// fresh measurement interval. All counters are protected by g_heapLock.
static bool g_heapTimingEnabled = false;
static uint64_t g_heapTimingGeneration = 0;
static HeapTimingStats g_heapTiming;

static uint64_t HeapTimingTsc()
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

class HeapTimedLock {
    uint64_t flags = 0, acquired = 0, wait = 0;
    bool locked = true;
public:
    bool measured = false;
    uint64_t headers = 0;
    explicit HeapTimedLock(unsigned operation)
    {
        if (!__atomic_load_n(&g_heapTimingEnabled, __ATOMIC_RELAXED)) {
            flags = IrqSpinLockAcquire(&g_heapLock);
            return;
        }
#if !defined(BROOK_HOST_TEST) || defined(BROOK_STANDALONE_TEST)
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
#endif
        const uint64_t generation = __atomic_load_n(&g_heapTimingGeneration, __ATOMIC_RELAXED);
        const uint64_t start = HeapTimingTsc();
        // Already IF=0; retain the original caller flags for the final release.
        (void)IrqSpinLockAcquire(&g_heapLock);
        acquired = HeapTimingTsc();
        measured = __atomic_load_n(&g_heapTimingEnabled, __ATOMIC_RELAXED) &&
                   generation == __atomic_load_n(&g_heapTimingGeneration, __ATOMIC_RELAXED);
        if (measured) {
            wait = acquired - start;
            if (operation == 0) ++g_heapTiming.allocations;
            else if (operation == 1) ++g_heapTiming.frees;
            else ++g_heapTiming.reallocations;
        }
    }
    void Unlock()
    {
        if (!locked) return;
        if (measured) {
            const uint64_t hold = HeapTimingTsc() - acquired;
            g_heapTiming.headers += headers;
            if (headers > g_heapTiming.maxHeaders) g_heapTiming.maxHeaders = headers;
            g_heapTiming.waitCycles += wait;
            if (wait > g_heapTiming.maxWaitCycles) g_heapTiming.maxWaitCycles = wait;
            g_heapTiming.holdCycles += hold;
            if (hold > g_heapTiming.maxHoldCycles) g_heapTiming.maxHoldCycles = hold;
        }
        locked = false;
        IrqSpinLockRelease(&g_heapLock, flags);
    }
    ~HeapTimedLock() { Unlock(); }
};

class HeapExpansionTiming {
    bool measured;
    uint64_t start;
public:
    explicit HeapExpansionTiming(bool enabled) : measured(enabled), start(enabled ? HeapTimingTsc() : 0)
    { if (measured) ++g_heapTiming.expansions; }
    ~HeapExpansionTiming() {
        if (!measured) return;
        const uint64_t cycles = HeapTimingTsc() - start;
        g_heapTiming.expansionCycles += cycles;
        if (cycles > g_heapTiming.maxExpansionCycles) g_heapTiming.maxExpansionCycles = cycles;
    }
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Boundary tags have uint32 sizes, and every block is ALIGN-aligned.
static constexpr uint64_t MAX_BLOCK_SIZE = (0xFFFFFFFFULL & ~(ALIGN - 1));
static bool RequestBlockSize(uint64_t requested, uint32_t* needed)
{
    // Check before adding either alignment slack or metadata.
    if (requested == 0 || requested > MAX_BLOCK_SIZE - OVERHEAD) return false;
    const uint64_t rounded = (requested + ALIGN - 1) & ~(ALIGN - 1);
    const uint64_t total = rounded + OVERHEAD;
    *needed = static_cast<uint32_t>(total < MIN_BLOCK ? MIN_BLOCK : total);
    return true;
}

static inline BlockHeader* ToHeader(void* userPtr)
{
    return reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uint8_t*>(userPtr) - HEADER_SIZE);
}

static inline BlockFooter* GetFooter(BlockHeader* h)
{
    return reinterpret_cast<BlockFooter*>(
        reinterpret_cast<uint8_t*>(h) + h->size - FOOTER_SIZE);
}

static inline void* UserPtr(BlockHeader* h)
{
    return reinterpret_cast<uint8_t*>(h) + HEADER_SIZE;
}

static inline BlockHeader* NextBlock(BlockHeader* h)
{
    return reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uint8_t*>(h) + h->size);
}

static inline BlockHeader* PrevBlock(BlockHeader* h)
{
    // Read the footer of the previous block (immediately before this header).
    BlockFooter* prevFoot = reinterpret_cast<BlockFooter*>(
        reinterpret_cast<uint8_t*>(h) - FOOTER_SIZE);
    return reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uint8_t*>(h) - prevFoot->size);
}

static inline bool IsValidHeader(BlockHeader* h)
{
    return h != nullptr && h->magic == HEADER_MAGIC;
}

// Floor-log2 classes. Only the requested class may contain undersized
// blocks; every block in a larger class is large enough. Links live outside
// the poisoned payload. All index operations require g_heapLock.
static unsigned FreeBin(uint32_t size)
{
    return 31u - static_cast<unsigned>(__builtin_clz(size));
}

static void FreeInsert(BlockHeader* h)
{
    const unsigned bin = FreeBin(h->size);
    h->freePrev = nullptr;
    h->freeNext = g_freeBins[bin];
    if (h->freeNext) h->freeNext->freePrev = h;
    g_freeBins[bin] = h;
    g_freeBinMask |= uint32_t{1} << bin;
}

static void FreeRemove(BlockHeader* h)
{
    const unsigned bin = FreeBin(h->size);
    if (h->freePrev) h->freePrev->freeNext = h->freeNext;
    else g_freeBins[bin] = h->freeNext;
    if (h->freeNext) h->freeNext->freePrev = h->freePrev;
    if (!g_freeBins[bin]) g_freeBinMask &= ~(uint32_t{1} << bin);
    h->freePrev = h->freeNext = nullptr;
}

static BlockHeader* FreeFind(uint32_t needed, HeapTimedLock& timing)
{
    const unsigned bin = FreeBin(needed);
    for (BlockHeader* h = g_freeBins[bin]; h; h = h->freeNext) {
        if (timing.measured) ++timing.headers;
        if (h->size >= needed) return h;
    }
    const uint32_t larger = bin == 31 ? 0 : g_freeBinMask & (~uint32_t{0} << (bin + 1));
    if (!larger) return nullptr;
    if (timing.measured) ++timing.headers;
    return g_freeBins[__builtin_ctz(larger)];
}

static void WriteBlock(uint8_t* base, uint32_t size, uint32_t free)
{
#ifndef BROOK_HOST_TEST
    UnixLifeHeap(base, size, free ? UnixLifeEvent::HeapWriteFree : UnixLifeEvent::HeapAllocate,
                 reinterpret_cast<uint64_t>(__builtin_return_address(0)));
#endif
    auto* h   = reinterpret_cast<BlockHeader*>(base);
    h->magic  = HEADER_MAGIC;
    h->size   = size;
    h->free   = free;
    h->_pad   = 0;
    // Free-block reseeding (HeapSetPoison) must preserve existing index links.
    if (!free) h->freePrev = h->freeNext = nullptr;
    auto* f   = GetFooter(h);
    f->size   = size;
    f->magic  = FOOTER_MAGIC;

    // Poison free blocks so the write-after-free check in kmalloc works
    // for all free blocks (initial, split remainders, and kfree'd).
    if (free && g_heapPoisonEnabled && size > OVERHEAD)
    {
        uint32_t* p32 = reinterpret_cast<uint32_t*>(base + HEADER_SIZE);
        uint64_t count32 = (size - OVERHEAD) / 4;
        if (g_heapPoisonForensic)
        {
            // BRO-179: PID/seq-encoded poison. Record the free's PID + callstack
            // into the ring, then fill with the encoded pattern (marker 0xDFDF in
            // the high half of every word; low half alternates pid / seq).
            uint16_t pid = HeapCurrentPid();
            uint32_t seq = ++g_heapFreeSeq;
            HeapFreeRec& r = g_heapFreeLog[seq & (HEAP_FREELOG_SIZE - 1)];
            r.blockVa = reinterpret_cast<uint64_t>(base);
            r.seq = seq;
            r.pid = pid;
            r.depth = HeapCaptureStack(
                reinterpret_cast<uint64_t>(__builtin_frame_address(0)),
                r.stack, HEAP_STACK_DEPTH);
            const uint32_t wpid = (static_cast<uint32_t>(POISON_MARK16) << 16) | pid;
            const uint32_t wseq = (static_cast<uint32_t>(POISON_MARK16) << 16) | (seq & 0xFFFFu);
            for (uint64_t i = 0; i < count32; i++)
                p32[i] = (i & 1) ? wseq : wpid;
        }
        else
        {
            for (uint64_t i = 0; i < count32; i++)
                p32[i] = POISON_FREE4;
        }
    }
}

// Map physical pages into the heap's dedicated virtual region.
// Returns true on success.
static bool HeapMapPages(uint64_t virtStart, uint64_t pageCount)
{
    // The range remains unpublished: g_heapEnd and all boundary tags stay
    // unchanged until every page is mapped. Neither this CPU nor any allocator
    // client accesses these new addresses before success. Thus rollback needs
    // local invalidation, but no remote shootdown (there are no remote users).
    uint64_t mapped = 0;
    for (; mapped < pageCount; ++mapped)
    {
        const uint64_t virt = virtStart + mapped * 4096;
        PhysicalAddress phys = PmmAllocPage(MemTag::Heap, KernelPid);
        if (!phys) break;
        if (VmmTryMapPage(KernelPageTable, VirtualAddress(virt), phys,
                         VMM_WRITABLE, MemTag::Heap, KernelPid) != VmmMapResult::Mapped)
        {
            PmmFreePage(phys);
            break;
        }
    }
    if (mapped == pageCount) return true;
    while (mapped) {
        const uint64_t leaf = VmmDetachPage(KernelPageTable,
            VirtualAddress(virtStart + --mapped * 4096));
        if (leaf) PmmUnrefPage(VmmLeafPhysical(leaf));
    }
    // Empty intermediate tables are still owned by the kernel page table and
    // reusable by a subsequent attempt; all newly installed leaf refs are gone.
    return false;
}

// Expand the heap by at least minBytes (rounded up to page granularity).
// Always contiguous since we control the virtual address region.
static bool ExpandHeap(uint64_t minBytes, bool measured)
{
    HeapExpansionTiming timing(measured);
    if (minBytes > MAX_BLOCK_SIZE) return false;
    uint64_t extra = EXPAND_PAGES * 4096;
    if (minBytes > extra) extra = (minBytes + 4095) & ~uint64_t{4095};
    // A page-rounded maximum-size request can require more than one block.
    // Ensure its second block has room for both boundary tags and payload.
    if (extra > MAX_BLOCK_SIZE && extra - MAX_BLOCK_SIZE < MIN_BLOCK)
        extra += 4096;
    const uint64_t expandPages = extra / 4096;
    const uint64_t expandVirt = reinterpret_cast<uint64_t>(g_heapEnd);
    if (expandVirt > HEAP_VIRT_MAX || extra > HEAP_VIRT_MAX - expandVirt)
    {
        SerialPuts("Heap: expansion would exceed max virtual region\n");
        return false;
    }

    if (!HeapMapPages(expandVirt, expandPages))
    {
        SerialPuts("Heap: expansion page mapping failed\n");
        return false;
    }

    // Boundary footer finds the tail in O(1), without scanning live blocks.
    BlockHeader* prev = g_heapEnd > g_heapStart
        ? PrevBlock(reinterpret_cast<BlockHeader*>(g_heapEnd)) : nullptr;
    if (measured && prev) ++g_heapTiming.expansionHeaders;

    const bool merged = prev && prev->free && extra <= MAX_BLOCK_SIZE - prev->size;
    if (merged)
    {
        FreeRemove(prev);
        WriteBlock(reinterpret_cast<uint8_t*>(prev),
                   static_cast<uint32_t>(prev->size + extra), 1);
        FreeInsert(prev);
        g_freeBytes += extra;
    }
    else if (extra <= MAX_BLOCK_SIZE)
    {
        WriteBlock(g_heapEnd, static_cast<uint32_t>(extra), 1);
        FreeInsert(reinterpret_cast<BlockHeader*>(g_heapEnd));
        g_freeBytes += extra - OVERHEAD;
    }
    else
    {
        WriteBlock(g_heapEnd, static_cast<uint32_t>(MAX_BLOCK_SIZE), 1);
        WriteBlock(g_heapEnd + MAX_BLOCK_SIZE,
                   static_cast<uint32_t>(extra - MAX_BLOCK_SIZE), 1);
        FreeInsert(reinterpret_cast<BlockHeader*>(g_heapEnd));
        FreeInsert(reinterpret_cast<BlockHeader*>(g_heapEnd + MAX_BLOCK_SIZE));
        g_freeBytes += extra - 2 * OVERHEAD;
    }
    g_heapEnd += extra;

    DbgPrintf("Heap: expanded by %lu KB (total %lu KB)\n",
                 extra / 1024,
                 (unsigned long)(g_heapEnd - g_heapStart) / 1024);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void HeapInit()
{
    if (!HeapMapPages(HEAP_VIRT_BASE, INITIAL_PAGES))
    {
        SerialPuts("Heap: FATAL: initial allocation failed\n");
        return;
    }

    g_heapStart = reinterpret_cast<uint8_t*>(HEAP_VIRT_BASE);
    g_heapEnd   = g_heapStart + INITIAL_PAGES * 4096;

    // Write a single free block filling the entire region.
    uint32_t blockSize = static_cast<uint32_t>(INITIAL_PAGES * 4096);
    WriteBlock(g_heapStart, blockSize, 1);
    for (auto& bin : g_freeBins) bin = nullptr;
    g_freeBinMask = 0;
    FreeInsert(reinterpret_cast<BlockHeader*>(g_heapStart));
    g_freeBytes = blockSize - OVERHEAD;

    SerialPrintf("Heap: %u KB initial region at 0x%p\n",
                 static_cast<uint32_t>((INITIAL_PAGES * 4096) / 1024),
                 reinterpret_cast<void*>(HEAP_VIRT_BASE));
}

void* kmalloc(uint64_t size)
{
    if (size == 0 || g_heapStart == nullptr) return nullptr;

    HeapTimedLock timing(0);

    uint32_t needed;
    if (!RequestBlockSize(size, &needed)) return nullptr;

    // Search free blocks only; retain one retry after expansion.
    for (int pass = 0; pass < 2; pass++)
    {
        BlockHeader* cur = FreeFind(needed, timing);

        if (cur)
        {
            if (!IsValidHeader(cur)) break;

            if (cur->free && cur->size >= needed)
            {
                // Verify free-poison is intact — detect write-after-free.
                if (g_heapPoisonEnabled)
                {
                    const uint32_t* p32 = reinterpret_cast<const uint32_t*>(
                        reinterpret_cast<uint8_t*>(cur) + HEADER_SIZE);
                    uint64_t check = (cur->size - OVERHEAD) / 4;
                    if (check > 16) check = 16; // spot-check first 64 bytes
                    for (uint64_t i = 0; i < check; i++)
                    {
                        // BRO-179: forensic poison varies the low 16 bits (pid/
                        // seq), so validate the 0xDFDF marker in the high half
                        // rather than an exact value — still catches any non-
                        // poison write-after-free.
                        if ((p32[i] >> 16) != POISON_MARK16)
                        {
                            SerialPrintf("HEAP: write-after-free detected! "
                                         "block at 0x%lx size=%u, offset %lu: "
                                         "expected 0x%x got 0x%x\n",
                                         reinterpret_cast<uint64_t>(cur),
                                         cur->size,
                                         (unsigned long)(i * 4), POISON_FREE4, p32[i]);
                            break;
                        }
                    }
                }

                FreeRemove(cur);
                uint32_t allocSize = cur->size; // consume entire free block
                // Remove the old free block's usable-byte contribution.
                g_freeBytes -= (cur->size - OVERHEAD);
                if (cur->size - needed >= MIN_BLOCK)
                {
                    // Split: remainder is large enough to be its own block
                    uint32_t remSize = cur->size - needed;
                    WriteBlock(reinterpret_cast<uint8_t*>(cur) + needed, remSize, 1);
                    FreeInsert(reinterpret_cast<BlockHeader*>(reinterpret_cast<uint8_t*>(cur) + needed));
                    g_freeBytes += remSize - OVERHEAD;
                    allocSize = needed;
                }

                WriteBlock(reinterpret_cast<uint8_t*>(cur), allocSize, 0);
                void* result = UserPtr(cur);

                // Poison freshly allocated memory to catch use-of-uninitialized.
                if (g_heapPoisonEnabled)
                {
                    uint64_t userBytes = allocSize - OVERHEAD;
                    uint8_t* dst = reinterpret_cast<uint8_t*>(result);
                    for (uint64_t i = 0; i < userBytes; i++)
                        dst[i] = POISON_ALLOC;
                }

                timing.Unlock();
                return result;
            }

        }

        // First pass exhausted — try to expand with enough room.
        if (pass == 0 && !ExpandHeap(needed, timing.measured)) { timing.Unlock(); return nullptr; }
    }

    timing.Unlock();
    return nullptr;
}

void kfree(void* ptr)
{
    if (ptr == nullptr) return;

    HeapTimedLock timing(1);

    BlockHeader* h = ToHeader(ptr);
#ifndef BROOK_HOST_TEST
    UnixLifeHeap(ptr, 0, UnixLifeEvent::HeapAttempt, reinterpret_cast<uint64_t>(__builtin_return_address(0)));
#endif
    if (!IsValidHeader(h)) { timing.Unlock(); return; }
    if (h->free) { timing.Unlock(); return; }

    g_freeBytes += h->size - OVERHEAD;

    // Coalesce with next block if free.
    BlockHeader* next = NextBlock(h);
    uint32_t totalSize = h->size;
    if (reinterpret_cast<uint8_t*>(next) < g_heapEnd && IsValidHeader(next) && next->free
        && next->size <= MAX_BLOCK_SIZE - totalSize)
    {
        // Merging recovers one header/footer pair as usable free payload.
        FreeRemove(next);
        g_freeBytes += OVERHEAD;
        totalSize += next->size;
    }

#ifndef BROOK_HOST_TEST
    // Record the actual coalesced range before mutation. The recorder never
    // acquires heap/FD/registry locks: only heap -> diagnostic lock exists.
    UnixLifeHeap(h, totalSize, UnixLifeEvent::HeapFree, reinterpret_cast<uint64_t>(__builtin_return_address(0)));
#endif
    // WriteBlock marks free=1 and poisons user data in one pass.
    WriteBlock(reinterpret_cast<uint8_t*>(h), totalSize, 1);

    // Coalesce with previous block if free (and we're not at the heap start).
    if (reinterpret_cast<uint8_t*>(h) > g_heapStart)
    {
        BlockHeader* prev = PrevBlock(h);
        if (IsValidHeader(prev) && prev->free && prev->size <= MAX_BLOCK_SIZE - h->size)
        {
            FreeRemove(prev);
            g_freeBytes += OVERHEAD;
#ifndef BROOK_HOST_TEST
            UnixLifeHeap(prev, prev->size + h->size, UnixLifeEvent::HeapFree,
                         reinterpret_cast<uint64_t>(__builtin_return_address(0)));
#endif
            WriteBlock(reinterpret_cast<uint8_t*>(prev), prev->size + h->size, 1);
            h = prev;
        }
    }
    FreeInsert(h);

    timing.Unlock();
}
void* krealloc(void* ptr, uint64_t newSize)
{
    if (ptr == nullptr) return kmalloc(newSize);
    if (newSize == 0) { kfree(ptr); return nullptr; }

    HeapTimedLock timing(2);

    BlockHeader* h = ToHeader(ptr);
    if (!IsValidHeader(h)) { timing.Unlock(); return nullptr; }

    uint32_t needed;
    if (!RequestBlockSize(newSize, &needed)) return nullptr;

    // Block is already large enough.
    if (h->size >= needed) { timing.Unlock(); return ptr; }

    // Snapshot the copy size while we still hold the lock and the block is valid.
    uint64_t copyBytes = h->size - OVERHEAD;
    if (copyBytes > newSize) copyBytes = newSize;

    timing.Unlock();

    // Allocate new block, copy, free old.
    void* newPtr = kmalloc(newSize);
    if (!newPtr) return nullptr;

    memcpy(newPtr, ptr, copyBytes);

    kfree(ptr);
    return newPtr;
}

uint64_t HeapFreeBytes()
{
    return g_freeBytes;
}

bool HeapCheckIntegrity()
{
    if (!g_heapStart) return true;

    uint64_t heapFlags = IrqSpinLockAcquire(&g_heapLock);

    uint32_t blockCount = 0;
    uint64_t freeBytes = 0;
    uint64_t freeCounts[32] = {};
    BlockHeader* cur = reinterpret_cast<BlockHeader*>(g_heapStart);

    while (reinterpret_cast<uint8_t*>(cur) < g_heapEnd)
    {
        if (!IsValidHeader(cur))
        {
            uint64_t off = reinterpret_cast<uint8_t*>(cur) - g_heapStart;
            SerialPrintf("HeapCheck: corrupt header at block #%u offset 0x%lx "
                         "(magic=0x%x)\n", blockCount, (unsigned long)off, cur->magic);
            IrqSpinLockRelease(&g_heapLock, heapFlags);
            return false;
        }

        if (cur->size < MIN_BLOCK || (cur->size & (ALIGN - 1)) || cur->size > static_cast<uint64_t>(g_heapEnd - reinterpret_cast<uint8_t*>(cur)))
        {
            uint64_t off = reinterpret_cast<uint8_t*>(cur) - g_heapStart;
            SerialPrintf("HeapCheck: invalid size %u at block #%u offset 0x%lx\n",
                         cur->size, blockCount, (unsigned long)off);
            IrqSpinLockRelease(&g_heapLock, heapFlags);
            return false;
        }

        BlockFooter* f = GetFooter(cur);
        if (f->magic != FOOTER_MAGIC || f->size != cur->size)
        {
            uint64_t off = reinterpret_cast<uint8_t*>(cur) - g_heapStart;
            SerialPrintf("HeapCheck: footer mismatch at block #%u offset 0x%lx "
                         "(ftr_magic=0x%x ftr_size=%u hdr_size=%u)\n",
                         blockCount, (unsigned long)off, f->magic, f->size, cur->size);
            IrqSpinLockRelease(&g_heapLock, heapFlags);
            return false;
        }

        if (cur->free) {
            freeBytes += cur->size - OVERHEAD;
            ++freeCounts[FreeBin(cur->size)];
            cur->_pad = 1; // diagnostic visitation mark, not allocator state
        } else if (cur->freePrev || cur->freeNext) {
            SerialPuts("HeapCheck: allocated block has free-index links\n");
            IrqSpinLockRelease(&g_heapLock, heapFlags);
            return false;
        }
        blockCount++;
        cur = NextBlock(cur);
    }

    // A bounded walk catches cycles/duplicates, missing nodes, wrong classes,
    // broken backlinks and stale bitmap entries without allocating scratch.
    for (unsigned bin = 0; bin < 32; ++bin) {
        uint64_t count = 0;
        BlockHeader* prev = nullptr;
        bool valid = bool(g_freeBinMask & (uint32_t{1} << bin)) == bool(g_freeBins[bin]);
        for (BlockHeader* node = g_freeBins[bin]; valid && node; node = node->freeNext) {
            const uint64_t address = reinterpret_cast<uint64_t>(node);
            if (++count > freeCounts[bin] || address < reinterpret_cast<uint64_t>(g_heapStart) ||
                address > reinterpret_cast<uint64_t>(g_heapEnd) - MIN_BLOCK ||
                (address & (ALIGN - 1)) || !IsValidHeader(node) || !node->free ||
                node->size < MIN_BLOCK || (node->size & (ALIGN - 1)) ||
                FreeBin(node->size) != bin || node->freePrev != prev || node->_pad != 1) {
                valid = false;
                break;
            }
            node->_pad = 0;
            prev = node;
        }
        if (!valid || count != freeCounts[bin]) {
            SerialPrintf("HeapCheck: free-index mismatch in bin %u\n", bin);
            IrqSpinLockRelease(&g_heapLock, heapFlags);
            return false;
        }
    }

    for (cur = reinterpret_cast<BlockHeader*>(g_heapStart);
         reinterpret_cast<uint8_t*>(cur) < g_heapEnd; cur = NextBlock(cur)) {
        if (cur->free && cur->_pad != 0) {
            SerialPuts("HeapCheck: physical free block missing from index\n");
            IrqSpinLockRelease(&g_heapLock, heapFlags);
            return false;
        }
    }

    if (freeBytes != g_freeBytes) {
        SerialPrintf("HeapCheck: free-byte accounting mismatch (counted=%lu recorded=%lu)\n",
                     freeBytes, g_freeBytes);
        IrqSpinLockRelease(&g_heapLock, heapFlags);
        return false;
    }
    IrqSpinLockRelease(&g_heapLock, heapFlags);
    return true;
}

void HeapTimingEnable(bool enabled)
{
    const uint64_t flags = IrqSpinLockAcquire(&g_heapLock);
    __atomic_add_fetch(&g_heapTimingGeneration, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_heapTimingEnabled, enabled, __ATOMIC_RELAXED);
    IrqSpinLockRelease(&g_heapLock, flags);
}

void HeapTimingReset()
{
    const uint64_t flags = IrqSpinLockAcquire(&g_heapLock);
    g_heapTiming = {};
    __atomic_add_fetch(&g_heapTimingGeneration, 1, __ATOMIC_RELAXED);
    IrqSpinLockRelease(&g_heapLock, flags);
}

void HeapTimingSnapshot(HeapTimingStats* out)
{
    if (!out) return;
    const uint64_t flags = IrqSpinLockAcquire(&g_heapLock);
    *out = g_heapTiming;
    out->enabled = __atomic_load_n(&g_heapTimingEnabled, __ATOMIC_RELAXED);
    IrqSpinLockRelease(&g_heapLock, flags);
}

void HeapTimingFormat(char* out, uint64_t capacity)
{
    if (!out || !capacity) return;
    HeapTimingStats stats;
    HeapTimingSnapshot(&stats);
    uint64_t used = 0;
    auto append = [&](const char* text) {
        while (*text) { if (used + 1 < capacity) out[used++] = *text; ++text; }
    };
    auto value = [&](const char* name, uint64_t number) {
        append(name);
        char digits[20]; unsigned count = 0;
        do { digits[count++] = static_cast<char>('0' + number % 10); number /= 10; } while (number);
        while (count) { char text[2] = {digits[--count], 0}; append(text); }
    };
    append(stats.enabled ? "heap timing: on (raw TSC cycles)\n" : "heap timing: off (raw TSC cycles)\n");
    value("alloc=", stats.allocations); value(" free=", stats.frees); value(" realloc=", stats.reallocations);
    value(" expand_attempts=", stats.expansions); append("\n");
    value("search_headers=", stats.headers); value(" max_search_headers=", stats.maxHeaders);
    value(" expand_headers=", stats.expansionHeaders); append("\n");
    value("irqoff_wait_cycles=", stats.waitCycles); value(" max=", stats.maxWaitCycles); append("\n");
    value("hold_cycles=", stats.holdCycles); value(" max=", stats.maxHoldCycles); append("\n");
    value("expand_cycles=", stats.expansionCycles); value(" max=", stats.maxExpansionCycles); append("\n");
    out[used] = 0;
}

void HeapSetPoison(bool enable)
{
    uint64_t flags = IrqSpinLockAcquire(&g_heapLock);
    if (enable && !g_heapPoisonEnabled) {
        // Free regions created while poisoning was off contain ordinary data.
        // Seed them before enabling allocation's poison check, without touching
        // live allocations. Otherwise toggling on reports false UAF failures.
        g_heapPoisonEnabled = true;
        auto* cur = reinterpret_cast<BlockHeader*>(g_heapStart);
        while (cur && reinterpret_cast<uint8_t*>(cur) < g_heapEnd) {
            if (!IsValidHeader(cur)) break;
            if (cur->free)
                WriteBlock(reinterpret_cast<uint8_t*>(cur), cur->size, 1);
            cur = NextBlock(cur);
        }
    } else {
        g_heapPoisonEnabled = enable;
    }
    IrqSpinLockRelease(&g_heapLock, flags);
}

void HeapGetStats(HeapStats* out)
{
    if (!out) return;
    *out = {};
    if (!g_heapStart) return;

    uint64_t heapFlags = IrqSpinLockAcquire(&g_heapLock);

    uint32_t totalBlocks = 0, freeBlocks = 0, usedBlocks = 0;
    uint64_t freeBytes = 0, usedBytes = 0;
    uint32_t largestFree = 0;

    BlockHeader* cur = reinterpret_cast<BlockHeader*>(g_heapStart);
    while (reinterpret_cast<uint8_t*>(cur) < g_heapEnd && IsValidHeader(cur))
    {
        totalBlocks++;
        if (cur->free)
        {
            freeBlocks++;
            freeBytes += cur->size - OVERHEAD;
            if (cur->size > largestFree) largestFree = cur->size;
        }
        else
        {
            usedBlocks++;
            usedBytes += cur->size - OVERHEAD;
        }
        cur = NextBlock(cur);
    }

    IrqSpinLockRelease(&g_heapLock, heapFlags);

    out->regionStart      = reinterpret_cast<uint64_t>(g_heapStart);
    out->regionEnd        = reinterpret_cast<uint64_t>(g_heapEnd);
    out->heapSizeBytes    = static_cast<uint64_t>(g_heapEnd - g_heapStart);
    out->totalBlocks      = totalBlocks;
    out->usedBlocks       = usedBlocks;
    out->freeBlocks       = freeBlocks;
    out->usedBytes        = usedBytes;
    out->freeBytes        = freeBytes;
    out->largestFreeBlock = largestFree;
    out->poisonEnabled    = g_heapPoisonEnabled;
}

void HeapDumpStats()
{
    if (!g_heapStart) { SerialPuts("Heap: not initialised\n"); return; }

    HeapStats s;
    HeapGetStats(&s);

    SerialPrintf("=== HEAP STATS ===\n");
    SerialPrintf("  Region: 0x%lx - 0x%lx (%lu KB)\n",
                 s.regionStart, s.regionEnd,
                 (unsigned long)(s.heapSizeBytes / 1024));
    SerialPrintf("  Blocks: %u total (%u used, %u free)\n",
                 s.totalBlocks, s.usedBlocks, s.freeBlocks);
    SerialPrintf("  Used:   %lu bytes\n", (unsigned long)s.usedBytes);
    SerialPrintf("  Free:   %lu bytes (largest block: %u)\n",
                 (unsigned long)s.freeBytes, s.largestFreeBlock);
    SerialPrintf("  Poison: %s\n", s.poisonEnabled ? "enabled" : "disabled");
    SerialPrintf("==================\n");
}

// BRO-179: arm the forensic (PID/seq-encoded) heap poison. Call once after the
// per-CPU GS base is valid (post-SMP), so HeapCurrentPid() is safe.
void HeapArmForensicPoison()
{
    g_heapPoisonForensic = true;
}

// BRO-179: decode a heap-poison qword seen at a crash site. If it carries the
// 0xDFDF marker, print the freeing PID + heap-free-seq and the kfree callstack
// that wrote it (symbolized). Returns true if the qword carried the marker.
// Lock-free read of the ring — safe in a panic/#DF dump (other CPUs halted).
bool HeapDecodePoison(uint64_t qword)
{
    uint32_t lo = static_cast<uint32_t>(qword & 0xFFFFFFFFu);
    uint32_t hi = static_cast<uint32_t>(qword >> 32);
    if ((lo >> 16) != POISON_MARK16 && (hi >> 16) != POISON_MARK16) return false;
    // Fill is [pid, seq, pid, seq, ...]; lower-addressed half is pid, higher seq.
    uint16_t pid = static_cast<uint16_t>(lo & 0xFFFFu);
    uint16_t seq16 = static_cast<uint16_t>(hi & 0xFFFFu);
    SerialPrintf("  BRO179-HEAPPOISON-DECODE qword=0x%lx -> freed by pid=%u seq(low16)=0x%x\n",
                 qword, (unsigned)pid, (unsigned)seq16);
    for (uint32_t i = 0; i < HEAP_FREELOG_SIZE; i++)
    {
        HeapFreeRec& r = g_heapFreeLog[i];
        if (r.seq == 0) continue;
        if ((r.seq & 0xFFFFu) != seq16 || r.pid != pid) continue;
        SerialPrintf("  BRO179-HEAPPOISON matches kfree seq=%u pid=%u blockVa=0x%lx — callstack:\n",
                     r.seq, (unsigned)r.pid, r.blockVa);
        for (uint8_t s = 0; s < r.depth; s++)
        {
            const char* nm = nullptr; uint64_t off = 0;
#ifndef BROOK_HOST_TEST
            if (KsymFindByAddr(r.stack[s], &nm, &off) && nm)
                SerialPrintf("      #%u 0x%lx  %s+0x%lx\n", s, r.stack[s], nm, off);
            else
                SerialPrintf("      #%u 0x%lx\n", s, r.stack[s]);
#else
            (void)nm; (void)off;
            SerialPrintf("      #%u 0x%lx\n", s, r.stack[s]);
#endif
        }
        return true;
    }
    SerialPrintf("  BRO179-HEAPPOISON: no live kfree record for that seq (ring wrapped)\n");
    return true;
}

} // namespace brook
