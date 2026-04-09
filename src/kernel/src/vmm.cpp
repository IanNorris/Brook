#include "vmm.h"
#include "pmm.h"
#include "serial.h"

namespace brook {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr uint64_t PAGE_SIZE     = 4096;
static constexpr uint64_t PHYS_MASK    = ~(PAGE_SIZE - 1);  // mask off flag bits

// ---------------------------------------------------------------------------
// VMALLOC bump allocator state
// ---------------------------------------------------------------------------

static uint64_t g_vmallocNext = VMALLOC_BASE;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline uint64_t ReadCR3()
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void Invlpg(uint64_t virtAddr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virtAddr) : "memory");
}

// Zero a physical page. We access it via the identity map (phys == virt).
static inline void ZeroPage(uint64_t physAddr)
{
    uint64_t* p = reinterpret_cast<uint64_t*>(physAddr);
    for (int i = 0; i < 512; i++) p[i] = 0;
}

// Decode PML4/PDPT/PD/PT indices from a virtual address.
static inline uint64_t Pml4Index(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline uint64_t PdptIndex(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline uint64_t PdIndex(uint64_t v)   { return (v >> 21) & 0x1FF; }
static inline uint64_t PtIndex(uint64_t v)   { return (v >> 12) & 0x1FF; }

// Return a pointer to a child table entry, allocating the child page if absent.
// 'parent' is the virtual address of the parent table (= physical addr, identity map).
// 'idx' is the entry index in the parent.
// Returns nullptr if the PMM is out of memory.
// NOTE: this must NOT be called for an entry that already holds a huge page (PS=1).
static uint64_t* GetOrAllocEntry(uint64_t* parent, uint64_t idx)
{
    if (!(parent[idx] & VMM_PRESENT))
    {
        uint64_t childPhys = PmmAllocPage();
        if (childPhys == 0) return nullptr;
        ZeroPage(childPhys);
        // Mark writable so lower levels can be written regardless of flags.
        parent[idx] = childPhys | VMM_PRESENT | VMM_WRITABLE;
    }
    // Physical address of the child table (strip flag bits).
    uint64_t childPhys = parent[idx] & PHYS_MASK;
    // Identity map: virtual address == physical address.
    return reinterpret_cast<uint64_t*>(childPhys);
}

// Walk (and optionally create) the page table path to the PTE for virtAddr.
// Returns a pointer to the PTE (which may be 0/not-present).
// Returns nullptr if an intermediate table could not be allocated.
// Set create=false to do a read-only walk (returns nullptr if any level missing).
static uint64_t* WalkToPtr(uint64_t virtAddr, bool create)
{
    uint64_t cr3  = ReadCR3() & PHYS_MASK;
    uint64_t* pml4 = reinterpret_cast<uint64_t*>(cr3);

    uint64_t* pdpt = create
        ? GetOrAllocEntry(pml4, Pml4Index(virtAddr))
        : (pml4[Pml4Index(virtAddr)] & VMM_PRESENT)
            ? reinterpret_cast<uint64_t*>(pml4[Pml4Index(virtAddr)] & PHYS_MASK)
            : nullptr;
    if (!pdpt) return nullptr;

    uint64_t* pd = create
        ? GetOrAllocEntry(pdpt, PdptIndex(virtAddr))
        : (pdpt[PdptIndex(virtAddr)] & VMM_PRESENT)
            ? reinterpret_cast<uint64_t*>(pdpt[PdptIndex(virtAddr)] & PHYS_MASK)
            : nullptr;
    if (!pd) return nullptr;

    // Guard: if this PD entry has the PS bit set, it's a 2MB page — don't
    // walk further. Callers mapping new 4KB pages should not hit a 2MB entry.
    if (pd[PdIndex(virtAddr)] & (1ULL << 7)) return nullptr; // PS bit

    uint64_t* pt = create
        ? GetOrAllocEntry(pd, PdIndex(virtAddr))
        : (pd[PdIndex(virtAddr)] & VMM_PRESENT)
            ? reinterpret_cast<uint64_t*>(pd[PdIndex(virtAddr)] & PHYS_MASK)
            : nullptr;
    if (!pt) return nullptr;

    return &pt[PtIndex(virtAddr)];
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void VmmInit()
{
    g_vmallocNext = VMALLOC_BASE;
    SerialPuts("VMM: initialized\n");
}

bool VmmMapPage(uint64_t virtAddr, uint64_t physAddr, uint64_t flags)
{
    uint64_t* pte = WalkToPtr(virtAddr, /*create=*/true);
    if (!pte) return false;

    uint64_t entry = (physAddr & PHYS_MASK) | VMM_PRESENT | (flags & ~VMM_PRESENT);
    *pte = entry;
    Invlpg(virtAddr);
    return true;
}

void VmmUnmapPage(uint64_t virtAddr)
{
    uint64_t* pte = WalkToPtr(virtAddr, /*create=*/false);
    if (pte && (*pte & VMM_PRESENT))
    {
        *pte = 0;
        Invlpg(virtAddr);
    }
}

uint64_t VmmAllocPages(uint64_t pageCount, uint64_t flags)
{
    if (pageCount == 0) return 0;

    // Check we won't overflow the VMALLOC region.
    if (g_vmallocNext + pageCount * PAGE_SIZE > VMALLOC_BASE + VMALLOC_SIZE)
        return 0;

    uint64_t virtBase = g_vmallocNext;
    g_vmallocNext += pageCount * PAGE_SIZE;

    for (uint64_t i = 0; i < pageCount; i++)
    {
        uint64_t phys = PmmAllocPage();
        if (phys == 0)
        {
            // Out of physical memory — unmap what we already set up and fail.
            for (uint64_t j = 0; j < i; j++)
            {
                uint64_t v = virtBase + j * PAGE_SIZE;
                uint64_t* pte = WalkToPtr(v, false);
                if (pte && (*pte & VMM_PRESENT))
                {
                    PmmFreePage(*pte & PHYS_MASK);
                    *pte = 0;
                    Invlpg(v);
                }
            }
            g_vmallocNext = virtBase; // reclaim virtual space (bump alloc)
            return 0;
        }

        if (!VmmMapPage(virtBase + i * PAGE_SIZE, phys, flags))
        {
            PmmFreePage(phys);
            // Unwind previous mappings.
            for (uint64_t j = 0; j < i; j++)
            {
                uint64_t v = virtBase + j * PAGE_SIZE;
                uint64_t* pte = WalkToPtr(v, false);
                if (pte && (*pte & VMM_PRESENT))
                {
                    PmmFreePage(*pte & PHYS_MASK);
                    *pte = 0;
                    Invlpg(v);
                }
            }
            g_vmallocNext = virtBase;
            return 0;
        }
    }

    return virtBase;
}

void VmmFreePages(uint64_t virtAddr, uint64_t pageCount)
{
    for (uint64_t i = 0; i < pageCount; i++)
    {
        uint64_t v = virtAddr + i * PAGE_SIZE;
        uint64_t* pte = WalkToPtr(v, false);
        if (pte && (*pte & VMM_PRESENT))
        {
            PmmFreePage(*pte & PHYS_MASK);
            *pte = 0;
            Invlpg(v);
        }
    }
}

uint64_t VmmVirtToPhys(uint64_t virtAddr)
{
    uint64_t* pte = WalkToPtr(virtAddr, false);
    if (!pte || !(*pte & VMM_PRESENT)) return 0;
    return (*pte & PHYS_MASK) | (virtAddr & (PAGE_SIZE - 1));
}

} // namespace brook
