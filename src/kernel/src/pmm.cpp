#include "pmm.h"
#include "serial.h"

// Linker-defined symbol — end of the kernel image (virtual address).
// Declared outside any namespace so the linker resolves it correctly.
extern "C" uint8_t __kernel_end_sym[] __asm__("__kernel_end");

namespace brook {

// ---------------------------------------------------------------------------
// Bitmap storage — 2MB in BSS (zeroed by ELF loader), covers 64GB physical.
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

uint64_t PmmAllocPage()
{
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
            if (idx >= g_totalPages) return 0;

            SetUsed(idx);
            g_freePages--;
            g_nextHint = idx + 1;
            return idx * PAGE_SIZE;
        }
    }
    return 0; // out of memory
}

uint64_t PmmAllocPages(uint64_t count)
{
    if (count == 0) return 0;
    if (count == 1) return PmmAllocPage();

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
                // Mark the run as used and return.
                for (uint64_t i = 0; i < count; i++)
                {
                    SetUsed(runStart + i);
                    g_freePages--;
                }
                return runStart * PAGE_SIZE;
            }
        }
        else
        {
            runLen = 0;
        }
    }
    return 0; // no contiguous run found
}

void PmmFreePage(uint64_t physAddr)
{
    if (physAddr == 0) return;
    if ((physAddr & (PAGE_SIZE - 1)) != 0) return; // not page-aligned, ignore

    uint64_t idx = physAddr / PAGE_SIZE;
    if (idx >= g_totalPages) return;
    if (!IsUsed(idx)) return; // double-free, silently ignore for now

    SetFree(idx);
    g_freePages++;
    if (idx < g_nextHint) g_nextHint = idx; // update hint for faster next alloc
}

uint64_t PmmGetFreePageCount()  { return g_freePages;  }
uint64_t PmmGetTotalPageCount() { return g_totalPages; }

} // namespace brook
