#include "heap.h"
#include "virtual_memory.h"
#include "physical_memory.h"
#include "serial.h"
#include "mem_tag.h"
#include "spinlock.h"

namespace brook {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr uint32_t HEADER_MAGIC  = 0xB10CBEEF;
static constexpr uint32_t FOOTER_MAGIC  = 0xB10CBEE2;
static constexpr uint64_t ALIGN         = 16;
static constexpr uint64_t INITIAL_PAGES = 256;         // 1MB initial heap
static constexpr uint64_t EXPAND_PAGES  = 256;         // expand by 1MB at a time

// Dedicated virtual region for the heap (PML4[385] — separate from VMALLOC).
static constexpr uint64_t HEAP_VIRT_BASE = 0xFFFFC08000000000ULL;
static constexpr uint64_t HEAP_VIRT_MAX  = 0xFFFFC0FF00000000ULL; // 508GB max

// Poison fill patterns for debugging
static constexpr uint8_t  POISON_ALLOC  = 0xCD;  // uninitialized alloc
static constexpr uint32_t POISON_FREE4  = 0xDFDFDFDF;  // freed memory

// Global toggle — can be disabled at runtime for performance-sensitive paths.
static volatile bool g_heapPoisonEnabled = true;

// ---------------------------------------------------------------------------
// Block layout
//
//  ┌─────────────────────┐  ← BlockHeader (24 bytes)
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
};

struct BlockFooter
{
    uint32_t size;
    uint32_t magic;
    uint64_t _pad;  // pad to 16 bytes so OVERHEAD = 32 (multiple of ALIGN)
};

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

// SMP lock protecting all heap metadata.
static SpinLock g_heapLock;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

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

static void WriteBlock(uint8_t* base, uint32_t size, uint32_t free)
{
    auto* h   = reinterpret_cast<BlockHeader*>(base);
    h->magic  = HEADER_MAGIC;
    h->size   = size;
    h->free   = free;
    h->_pad   = 0;
    auto* f   = GetFooter(h);
    f->size   = size;
    f->magic  = FOOTER_MAGIC;

    // Poison free blocks so the write-after-free check in kmalloc works
    // for all free blocks (initial, split remainders, and kfree'd).
    if (free && g_heapPoisonEnabled && size > OVERHEAD)
    {
        uint32_t* p32 = reinterpret_cast<uint32_t*>(base + HEADER_SIZE);
        uint64_t count32 = (size - OVERHEAD) / 4;
        for (uint64_t i = 0; i < count32; i++)
            p32[i] = POISON_FREE4;
    }
}

// Map physical pages into the heap's dedicated virtual region.
// Returns true on success.
static bool HeapMapPages(uint64_t virtStart, uint64_t pageCount)
{
    for (uint64_t i = 0; i < pageCount; i++)
    {
        uint64_t virt = virtStart + i * 4096;
        PhysicalAddress phys = PmmAllocPage(MemTag::Heap, KernelPid);
        if (!phys) return false;
        if (!VmmMapPage(KernelPageTable, VirtualAddress(virt), phys, VMM_WRITABLE, MemTag::Heap, KernelPid))
        {
            PmmFreePage(phys);
            return false;
        }
    }
    return true;
}

// Expand the heap by at least minBytes (rounded up to page granularity).
// Always contiguous since we control the virtual address region.
static bool ExpandHeap(uint64_t minBytes = 0)
{
    uint64_t expandPages = EXPAND_PAGES;
    if (minBytes > expandPages * 4096)
        expandPages = (minBytes + 4095) / 4096;

    uint64_t expandVirt = reinterpret_cast<uint64_t>(g_heapEnd);
    if (expandVirt + expandPages * 4096 > HEAP_VIRT_MAX)
    {
        SerialPuts("Heap: expansion would exceed max virtual region\n");
        return false;
    }

    if (!HeapMapPages(expandVirt, expandPages))
    {
        SerialPuts("Heap: expansion page mapping failed\n");
        return false;
    }

    // Always contiguous — merge with the last block or add a new free block.
    BlockHeader* cur  = reinterpret_cast<BlockHeader*>(g_heapStart);
    BlockHeader* prev = nullptr;
    while (reinterpret_cast<uint8_t*>(cur) < g_heapEnd - HEADER_SIZE
           && IsValidHeader(cur))
    {
        prev = cur;
        cur  = NextBlock(cur);
    }

    uint32_t extra = static_cast<uint32_t>(expandPages * 4096);
    if (prev && prev->free)
    {
        WriteBlock(reinterpret_cast<uint8_t*>(prev), prev->size + extra, 1);
    }
    else
    {
        WriteBlock(g_heapEnd, extra, 1);
    }
    g_heapEnd   += extra;
    g_freeBytes += extra;

    DbgPrintf("Heap: expanded by %u KB (total %lu KB)\n",
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
    g_freeBytes = blockSize - OVERHEAD;

    SerialPrintf("Heap: %u KB initial region at 0x%p\n",
                 static_cast<uint32_t>((INITIAL_PAGES * 4096) / 1024),
                 reinterpret_cast<void*>(HEAP_VIRT_BASE));
}

void* kmalloc(uint64_t size)
{
    if (size == 0 || g_heapStart == nullptr) return nullptr;

    uint64_t lf = SpinLockAcquire(&g_heapLock);

    // Round size up to alignment, then account for overhead.
    uint64_t aligned = (size + ALIGN - 1) & ~(ALIGN - 1);
    uint32_t needed  = static_cast<uint32_t>(aligned + OVERHEAD);
    if (needed < MIN_BLOCK) needed = static_cast<uint32_t>(MIN_BLOCK);

    // First-fit search.
    for (int pass = 0; pass < 2; pass++)
    {
        BlockHeader* cur = reinterpret_cast<BlockHeader*>(g_heapStart);

        while (reinterpret_cast<uint8_t*>(cur) < g_heapEnd)
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
                        if (p32[i] != POISON_FREE4)
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

                uint32_t allocSize = cur->size; // consume entire free block
                if (cur->size >= needed + static_cast<uint32_t>(MIN_BLOCK))
                {
                    // Split: remainder is large enough to be its own block
                    uint32_t remSize = cur->size - needed;
                    WriteBlock(reinterpret_cast<uint8_t*>(cur) + needed, remSize, 1);
                    g_freeBytes += remSize - OVERHEAD;
                    allocSize = needed;
                }

                WriteBlock(reinterpret_cast<uint8_t*>(cur), allocSize, 0);
                g_freeBytes -= allocSize;
                void* result = UserPtr(cur);

                // Poison freshly allocated memory to catch use-of-uninitialized.
                if (g_heapPoisonEnabled)
                {
                    uint64_t userBytes = allocSize - OVERHEAD;
                    uint8_t* dst = reinterpret_cast<uint8_t*>(result);
                    for (uint64_t i = 0; i < userBytes; i++)
                        dst[i] = POISON_ALLOC;
                }

                SpinLockRelease(&g_heapLock, lf);
                return result;
            }

            cur = NextBlock(cur);
        }

        // First pass exhausted — try to expand with enough room.
        if (pass == 0 && !ExpandHeap(needed)) { SpinLockRelease(&g_heapLock, lf); return nullptr; }
    }

    SpinLockRelease(&g_heapLock, lf);
    return nullptr;
}

void kfree(void* ptr)
{
    if (ptr == nullptr) return;

    uint64_t lf = SpinLockAcquire(&g_heapLock);

    BlockHeader* h = ToHeader(ptr);
    if (!IsValidHeader(h)) { SpinLockRelease(&g_heapLock, lf); return; }
    if (h->free) { SpinLockRelease(&g_heapLock, lf); return; }

    g_freeBytes += h->size;

    // Coalesce with next block if free.
    BlockHeader* next = NextBlock(h);
    uint32_t totalSize = h->size;
    if (reinterpret_cast<uint8_t*>(next) < g_heapEnd && IsValidHeader(next) && next->free)
    {
        g_freeBytes -= OVERHEAD;
        totalSize += next->size;
    }

    // WriteBlock marks free=1 and poisons user data in one pass.
    WriteBlock(reinterpret_cast<uint8_t*>(h), totalSize, 1);

    // Coalesce with previous block if free (and we're not at the heap start).
    if (reinterpret_cast<uint8_t*>(h) > g_heapStart)
    {
        BlockHeader* prev = PrevBlock(h);
        if (IsValidHeader(prev) && prev->free)
        {
            g_freeBytes -= OVERHEAD;
            WriteBlock(reinterpret_cast<uint8_t*>(prev), prev->size + h->size, 1);
        }
    }

    SpinLockRelease(&g_heapLock, lf);
}
void* krealloc(void* ptr, uint64_t newSize)
{
    if (ptr == nullptr) return kmalloc(newSize);
    if (newSize == 0) { kfree(ptr); return nullptr; }

    uint64_t lf = SpinLockAcquire(&g_heapLock);

    BlockHeader* h = ToHeader(ptr);
    if (!IsValidHeader(h)) { SpinLockRelease(&g_heapLock, lf); return nullptr; }

    uint64_t aligned = (newSize + ALIGN - 1) & ~(ALIGN - 1);
    uint32_t needed  = static_cast<uint32_t>(aligned + OVERHEAD);
    if (needed < static_cast<uint32_t>(MIN_BLOCK))
        needed = static_cast<uint32_t>(MIN_BLOCK);

    // Block is already large enough.
    if (h->size >= needed) { SpinLockRelease(&g_heapLock, lf); return ptr; }

    // Snapshot the copy size while we still hold the lock and the block is valid.
    uint64_t copyBytes = h->size - OVERHEAD;
    if (copyBytes > newSize) copyBytes = newSize;

    SpinLockRelease(&g_heapLock, lf);

    // Allocate new block, copy, free old.
    void* newPtr = kmalloc(newSize);
    if (!newPtr) return nullptr;

    const uint8_t* src = reinterpret_cast<const uint8_t*>(ptr);
    uint8_t*       dst = reinterpret_cast<uint8_t*>(newPtr);
    for (uint64_t i = 0; i < copyBytes; i++) dst[i] = src[i];

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

    uint32_t blockCount = 0;
    BlockHeader* cur = reinterpret_cast<BlockHeader*>(g_heapStart);

    while (reinterpret_cast<uint8_t*>(cur) < g_heapEnd)
    {
        if (!IsValidHeader(cur))
        {
            uint64_t off = reinterpret_cast<uint8_t*>(cur) - g_heapStart;
            SerialPrintf("HeapCheck: corrupt header at block #%u offset 0x%lx "
                         "(magic=0x%x)\n", blockCount, (unsigned long)off, cur->magic);
            return false;
        }

        if (cur->size < MIN_BLOCK || cur->size > static_cast<uint32_t>(g_heapEnd - g_heapStart))
        {
            uint64_t off = reinterpret_cast<uint8_t*>(cur) - g_heapStart;
            SerialPrintf("HeapCheck: invalid size %u at block #%u offset 0x%lx\n",
                         cur->size, blockCount, (unsigned long)off);
            return false;
        }

        BlockFooter* f = GetFooter(cur);
        if (f->magic != FOOTER_MAGIC || f->size != cur->size)
        {
            uint64_t off = reinterpret_cast<uint8_t*>(cur) - g_heapStart;
            SerialPrintf("HeapCheck: footer mismatch at block #%u offset 0x%lx "
                         "(ftr_magic=0x%x ftr_size=%u hdr_size=%u)\n",
                         blockCount, (unsigned long)off, f->magic, f->size, cur->size);
            return false;
        }

        blockCount++;
        cur = NextBlock(cur);
    }

    return true;
}

void HeapSetPoison(bool enable)
{
    g_heapPoisonEnabled = enable;
}

void HeapGetStats(HeapStats* out)
{
    if (!out) return;
    *out = {};
    if (!g_heapStart) return;

    uint64_t lf = SpinLockAcquire(&g_heapLock);

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

    SpinLockRelease(&g_heapLock, lf);

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

} // namespace brook
