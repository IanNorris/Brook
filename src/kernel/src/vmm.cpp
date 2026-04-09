#include "vmm.h"
#include "pmm.h"
#include "serial.h"

namespace brook {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr uint64_t PAGE_SIZE  = 4096;
// Physical address occupies bits [51:12] in a PTE.
// Bits [11:0] are flags, bits [62:52] are used for PID, bit 63 is NX.
static constexpr uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL;

// ---------------------------------------------------------------------------
// VMALLOC bump allocator state
// ---------------------------------------------------------------------------

static uint64_t g_vmallocNext = VMALLOC_BASE;

// ---------------------------------------------------------------------------
// Allocation registration table — 1024 static slots.
// Tracks every VmmAllocPages call for diagnostics and ownership queries.
// ---------------------------------------------------------------------------

static constexpr uint32_t VMM_MAX_ALLOCS = 1024;
static VmmAllocation g_vmmAllocs[VMM_MAX_ALLOCS] = {};

static VmmAllocation* FindAllocSlot(uint64_t virtBase)
{
    // Exact match on virtBase.
    for (uint32_t i = 0; i < VMM_MAX_ALLOCS; i++)
        if (g_vmmAllocs[i].virtBase == virtBase) return &g_vmmAllocs[i];
    return nullptr;
}

static VmmAllocation* FindFreeSlot()
{
    for (uint32_t i = 0; i < VMM_MAX_ALLOCS; i++)
        if (g_vmmAllocs[i].virtBase == 0) return &g_vmmAllocs[i];
    return nullptr;
}

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

static inline void ZeroPage(uint64_t physAddr)
{
    uint64_t* p = reinterpret_cast<uint64_t*>(physAddr);
    for (int i = 0; i < 512; i++) p[i] = 0;
}

static inline uint64_t Pml4Index(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline uint64_t PdptIndex(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline uint64_t PdIndex(uint64_t v)   { return (v >> 21) & 0x1FF; }
static inline uint64_t PtIndex(uint64_t v)   { return (v >> 12) & 0x1FF; }

// Allocate a page table page — tagged PageTable so it shows up clearly in PMM dumps.
static uint64_t AllocTablePage()
{
    return PmmAllocPage(MemTag::PageTable, KernelPid);
}

static uint64_t* GetOrAllocEntry(uint64_t* parent, uint64_t idx)
{
    if (!(parent[idx] & VMM_PRESENT))
    {
        uint64_t childPhys = AllocTablePage();
        if (childPhys == 0) return nullptr;
        ZeroPage(childPhys);
        parent[idx] = childPhys | VMM_PRESENT | VMM_WRITABLE;
    }
    uint64_t childPhys = parent[idx] & PHYS_MASK;
    return reinterpret_cast<uint64_t*>(childPhys);
}

static uint64_t* WalkToPtr(uint64_t virtAddr, bool create)
{
    uint64_t cr3   = ReadCR3() & PHYS_MASK;
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

    // Guard: PS bit set means this is a 2MB page — don't descend further.
    if (pd[PdIndex(virtAddr)] & (1ULL << 7)) return nullptr;

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

bool VmmMapPage(uint64_t virtAddr, uint64_t physAddr, uint64_t flags,
                MemTag tag, uint16_t pid)
{
    // Enforce null pointer guard: reject any mapping in the first 1GB.
    if (virtAddr < VIRTUAL_NULL_GUARD)
    {
        SerialPrintf("VMM: rejected mapping at 0x%p (below null guard)\n",
                     reinterpret_cast<void*>(virtAddr));
        return false;
    }

    uint64_t* pte = WalkToPtr(virtAddr, /*create=*/true);
    if (!pte) return false;

    // Encode tag in bits [9-11] and PID in bits [52-62].
    *pte = (physAddr & PHYS_MASK)
         | VMM_PRESENT
         | (flags & ~(VMM_PRESENT | PTE_TAG_MASK | PTE_PID_MASK | VMM_NO_EXEC))
         | (((uint64_t)(uint8_t)tag & 0x7) << PTE_TAG_SHIFT)
         | (((uint64_t)pid & 0x7FF) << PTE_PID_SHIFT)
         | (flags & VMM_NO_EXEC);
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

uint64_t VmmAllocPages(uint64_t pageCount, uint64_t flags, MemTag tag, uint16_t pid)
{
    if (pageCount == 0) return 0;

    if (g_vmallocNext + pageCount * PAGE_SIZE > VMALLOC_BASE + VMALLOC_SIZE)
        return 0;

    uint64_t virtBase = g_vmallocNext;
    g_vmallocNext += pageCount * PAGE_SIZE;

    for (uint64_t i = 0; i < pageCount; i++)
    {
        uint64_t phys = PmmAllocPage(tag, pid);
        if (phys == 0)
        {
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

        if (!VmmMapPage(virtBase + i * PAGE_SIZE, phys, flags, tag, pid))
        {
            PmmFreePage(phys);
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

    // Register the allocation.
    VmmAllocation* slot = FindFreeSlot();
    if (slot)
    {
        slot->virtBase  = virtBase;
        slot->pageCount = pageCount;
        slot->tag       = tag;
        slot->pid       = pid;
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

    // Clear registration.
    VmmAllocation* alloc = FindAllocSlot(virtAddr);
    if (alloc) alloc->virtBase = 0;
}

uint64_t VmmVirtToPhys(uint64_t virtAddr)
{
    uint64_t* pte = WalkToPtr(virtAddr, false);
    if (!pte || !(*pte & VMM_PRESENT)) return 0;
    return (*pte & PHYS_MASK) | (virtAddr & (PAGE_SIZE - 1));
}

const VmmAllocation* VmmGetAllocation(uint64_t virtAddr)
{
    // Find the allocation record whose range contains virtAddr.
    for (uint32_t i = 0; i < VMM_MAX_ALLOCS; i++)
    {
        const VmmAllocation& a = g_vmmAllocs[i];
        if (a.virtBase == 0) continue;
        if (virtAddr >= a.virtBase && virtAddr < a.virtBase + a.pageCount * PAGE_SIZE)
            return &a;
    }
    return nullptr;
}

MemTag VmmGetPageTag(uint64_t virtAddr)
{
    uint64_t* pte = WalkToPtr(virtAddr, false);
    if (!pte || !(*pte & VMM_PRESENT)) return MemTag::Free;
    return static_cast<MemTag>((*pte & PTE_TAG_MASK) >> PTE_TAG_SHIFT);
}

uint16_t VmmGetPagePid(uint64_t virtAddr)
{
    uint64_t* pte = WalkToPtr(virtAddr, false);
    if (!pte || !(*pte & VMM_PRESENT)) return KernelPid;
    return static_cast<uint16_t>((*pte & PTE_PID_MASK) >> PTE_PID_SHIFT);
}

void VmmKillPid(uint16_t pid)
{
    if (pid == KernelPid) return;

    // Walk the registration table and free all allocations for this PID.
    for (uint32_t i = 0; i < VMM_MAX_ALLOCS; i++)
    {
        VmmAllocation& a = g_vmmAllocs[i];
        if (a.virtBase != 0 && a.pid == pid)
        {
            VmmFreePages(a.virtBase, a.pageCount);
            // VmmFreePages clears virtBase; loop continues from fresh state.
        }
    }

    // Free any physical pages that were tracked at the PMM level.
    PmmKillPid(pid);
}

} // namespace brook
