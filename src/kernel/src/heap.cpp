#include "heap.h"
#include "vmm.h"
#include "serial.h"

namespace brook {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr uint32_t HEADER_MAGIC  = 0xB10CBEEF;
static constexpr uint32_t FOOTER_MAGIC  = 0xB10CBEE2;
static constexpr uint64_t ALIGN         = 16;
static constexpr uint64_t INITIAL_PAGES = 64;          // 256KB initial heap
static constexpr uint64_t EXPAND_PAGES  = 64;          // expand by 256KB at a time

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
}

// Expand the heap by EXPAND_PAGES via VMM. Returns false on failure.
static bool ExpandHeap()
{
    uint64_t newVirt = VmmAllocPages(EXPAND_PAGES);
    if (newVirt == 0) return false;

    uint8_t* newRegion = reinterpret_cast<uint8_t*>(newVirt);

    // If the new region is physically contiguous with the current heap end
    // we can merge them. Otherwise treat it as a separate free block and
    // leave a sentinel to prevent walking past the boundary.
    if (newRegion == g_heapEnd)
    {
        // Walk from start to find the last valid block.
        BlockHeader* cur  = reinterpret_cast<BlockHeader*>(g_heapStart);
        BlockHeader* prev = nullptr;
        while (reinterpret_cast<uint8_t*>(cur) < g_heapEnd - HEADER_SIZE
               && IsValidHeader(cur))
        {
            prev = cur;
            cur  = NextBlock(cur);
        }

        if (prev && prev->free)
        {
            // Absorb the new pages into this free block.
            uint32_t extra = static_cast<uint32_t>(EXPAND_PAGES * 4096);
            WriteBlock(reinterpret_cast<uint8_t*>(prev), prev->size + extra, 1);
            g_heapEnd  += extra;
            g_freeBytes += extra;
        }
        else
        {
            // Last block is in use: add a new free block.
            uint32_t blockSize = static_cast<uint32_t>(EXPAND_PAGES * 4096);
            WriteBlock(g_heapEnd, blockSize, 1);
            g_heapEnd   += blockSize;
            g_freeBytes += blockSize - OVERHEAD;
        }
    }
    else
    {
        // Non-contiguous: start a fresh free block in the new region.
        uint32_t blockSize = static_cast<uint32_t>(EXPAND_PAGES * 4096);
        WriteBlock(newRegion, blockSize, 1);
        // Update g_heapEnd to the new region (we walk from g_heapStart,
        // so non-contiguous regions are just not reachable by the walker).
        // For simplicity, only extend if contiguous. Otherwise log a warning.
        SerialPuts("Heap: warning: non-contiguous expansion (fragmented vmalloc)\n");
        // Mark it free for use by treating it as if it's contiguous.
        g_heapEnd   = newRegion + blockSize;
        g_heapStart = (g_heapStart == nullptr) ? newRegion : g_heapStart;
        g_freeBytes += blockSize - OVERHEAD;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void HeapInit()
{
    uint64_t virtBase = VmmAllocPages(INITIAL_PAGES);
    if (virtBase == 0)
    {
        SerialPuts("Heap: FATAL: initial allocation failed\n");
        return;
    }

    g_heapStart = reinterpret_cast<uint8_t*>(virtBase);
    g_heapEnd   = g_heapStart + INITIAL_PAGES * 4096;

    // Write a single free block filling the entire region.
    uint32_t blockSize = static_cast<uint32_t>(INITIAL_PAGES * 4096);
    WriteBlock(g_heapStart, blockSize, 1);
    g_freeBytes = blockSize - OVERHEAD;

    SerialPrintf("Heap: %u KB initial region at 0x%p\n",
                 static_cast<uint32_t>((INITIAL_PAGES * 4096) / 1024),
                 reinterpret_cast<void*>(virtBase));
}

void* kmalloc(uint64_t size)
{
    if (size == 0 || g_heapStart == nullptr) return nullptr;

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
                // Split only if the remainder is large enough to be useful.
                if (cur->size >= needed + static_cast<uint32_t>(MIN_BLOCK))
                {
                    uint32_t remSize = cur->size - needed;
                    WriteBlock(reinterpret_cast<uint8_t*>(cur) + needed, remSize, 1);
                    g_freeBytes += remSize - OVERHEAD;
                }

                WriteBlock(reinterpret_cast<uint8_t*>(cur), needed, 0);
                g_freeBytes -= needed;
                return UserPtr(cur);
            }

            cur = NextBlock(cur);
        }

        // First pass exhausted — try to expand.
        if (pass == 0 && !ExpandHeap()) return nullptr;
    }

    return nullptr;
}

void kfree(void* ptr)
{
    if (ptr == nullptr) return;

    BlockHeader* h = ToHeader(ptr);
    if (!IsValidHeader(h)) return; // corrupt or double-free
    if (h->free) return;           // already free — silent (tests expect this)

    h->free = 1;
    g_freeBytes += h->size;

    // Coalesce with next block if free.
    BlockHeader* next = NextBlock(h);
    if (reinterpret_cast<uint8_t*>(next) < g_heapEnd && IsValidHeader(next) && next->free)
    {
        g_freeBytes -= OVERHEAD; // the header+footer we're absorbing
        WriteBlock(reinterpret_cast<uint8_t*>(h), h->size + next->size, 1);
    }

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
}

void* krealloc(void* ptr, uint64_t newSize)
{
    if (ptr == nullptr) return kmalloc(newSize);
    if (newSize == 0) { kfree(ptr); return nullptr; }

    BlockHeader* h = ToHeader(ptr);
    if (!IsValidHeader(h)) return nullptr;

    uint64_t aligned = (newSize + ALIGN - 1) & ~(ALIGN - 1);
    uint32_t needed  = static_cast<uint32_t>(aligned + OVERHEAD);
    if (needed < static_cast<uint32_t>(MIN_BLOCK))
        needed = static_cast<uint32_t>(MIN_BLOCK);

    // Block is already large enough.
    if (h->size >= needed) return ptr;

    // Allocate new block, copy, free old.
    void* newPtr = kmalloc(newSize);
    if (!newPtr) return nullptr;

    uint64_t copyBytes = h->size - OVERHEAD;
    if (copyBytes > newSize) copyBytes = newSize;

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

} // namespace brook
