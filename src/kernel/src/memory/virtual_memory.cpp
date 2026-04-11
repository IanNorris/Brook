#include "virtual_memory.h"
#include "physical_memory.h"
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

// The kernel's PML4 physical address, captured at init time.
static PhysicalAddress g_kernelCR3{};

// Module-space bump allocator
static uint64_t g_moduleNext = MODULE_BASE;

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

static inline void Invlpg(VirtualAddress virtAddr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virtAddr.raw()) : "memory");
}

static inline void ZeroPage(PhysicalAddress physAddr)
{
    uint64_t* p = reinterpret_cast<uint64_t*>(PhysToVirt(physAddr).raw());
    for (int i = 0; i < 512; i++) p[i] = 0;
}

// Zero a page via identity mapping. Only safe during early init before the
// identity map is modified by ELF loading.
static inline void ZeroPageIdentity(uint64_t physAddr)
{
    uint64_t* p = reinterpret_cast<uint64_t*>(physAddr);
    for (int i = 0; i < 512; i++) p[i] = 0;
}

static inline uint64_t Pml4Index(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline uint64_t PdptIndex(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline uint64_t PdIndex(uint64_t v)   { return (v >> 21) & 0x1FF; }
static inline uint64_t PtIndex(uint64_t v)   { return (v >> 12) & 0x1FF; }

// Allocate a page table page — tagged PageTable so it shows up clearly in PMM dumps.
static PhysicalAddress AllocTablePage()
{
    return PmmAllocPage(MemTag::PageTable, KernelPid);
}

static uint64_t* GetOrAllocEntry(uint64_t* parent, uint64_t idx, uint64_t extraFlags = 0)
{
    if (!(parent[idx] & VMM_PRESENT))
    {
        PhysicalAddress childPhys = AllocTablePage();
        if (!childPhys) return nullptr;
        ZeroPage(childPhys);
        parent[idx] = childPhys.raw() | VMM_PRESENT | VMM_WRITABLE | (extraFlags & VMM_USER);
    }
    else if ((extraFlags & VMM_USER) && !(parent[idx] & VMM_USER))
    {
        parent[idx] |= VMM_USER;
    }
    uint64_t childPhys = parent[idx] & PHYS_MASK;
    return reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(childPhys)).raw());
}

// Resolve PageTable: KernelPageTable (null pml4) → kernel CR3.
static inline PhysicalAddress ResolvePml4(PageTable pt)
{
    return pt ? pt.pml4 : g_kernelCR3;
}

static uint64_t* WalkToPtr(PageTable pageTable, VirtualAddress virtAddr,
                            bool create, uint64_t flags = 0)
{
    uint64_t* pml4 = reinterpret_cast<uint64_t*>(PhysToVirt(ResolvePml4(pageTable)).raw());
    uint64_t va = virtAddr.raw();

    uint64_t* pdpt = create
        ? GetOrAllocEntry(pml4, Pml4Index(va), flags)
        : (pml4[Pml4Index(va)] & VMM_PRESENT)
            ? reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(pml4[Pml4Index(va)] & PHYS_MASK)).raw())
            : nullptr;
    if (!pdpt) return nullptr;

    uint64_t* pd = create
        ? GetOrAllocEntry(pdpt, PdptIndex(va), flags)
        : (pdpt[PdptIndex(va)] & VMM_PRESENT)
            ? reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(pdpt[PdptIndex(va)] & PHYS_MASK)).raw())
            : nullptr;
    if (!pd) return nullptr;

    // If PD entry has PS bit set, it's a 2MB page. Split it into 4KB pages
    // so we can map individual pages within this range.
    if (pd[PdIndex(va)] & (1ULL << 7))
    {
        if (!create) return nullptr;

        uint64_t oldEntry = pd[PdIndex(va)];
        uint64_t hugeBase = oldEntry & PHYS_MASK;
        uint64_t oldFlags = oldEntry & ~PHYS_MASK & ~(1ULL << 7);

        PhysicalAddress ptPhys = AllocTablePage();
        if (!ptPhys) return nullptr;
        ZeroPage(ptPhys);

        uint64_t* pt = reinterpret_cast<uint64_t*>(PhysToVirt(ptPhys).raw());
        for (uint64_t i = 0; i < 512; ++i)
            pt[i] = (hugeBase + i * 4096) | (oldFlags & ~(1ULL << 7)) | VMM_PRESENT;

        pd[PdIndex(va)] = ptPhys.raw() | VMM_PRESENT | VMM_WRITABLE
                           | (oldFlags & VMM_USER)
                           | (flags & VMM_USER);

        // Flush all TLB entries in this 2MB range
        uint64_t base2M = (va >> 21) << 21;
        for (uint64_t i = 0; i < 512; ++i)
            Invlpg(VirtualAddress(base2M + i * 4096));

        return &pt[PtIndex(va)];
    }

    uint64_t* pt = create
        ? GetOrAllocEntry(pd, PdIndex(va), flags)
        : (pd[PdIndex(va)] & VMM_PRESENT)
            ? reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(pd[PdIndex(va)] & PHYS_MASK)).raw())
            : nullptr;
    if (!pt) return nullptr;

    return &pt[PtIndex(va)];
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void VmmInit()
{
    g_vmallocNext = VMALLOC_BASE;
    g_kernelCR3 = PhysicalAddress(ReadCR3() & PHYS_MASK);

    uint64_t cr3 = ReadCR3() & PHYS_MASK;
    uint64_t* pml4 = reinterpret_cast<uint64_t*>(cr3); // identity-mapped

    PhysicalAddress pdptPhys = PmmAllocPage(MemTag::PageTable, KernelPid);
    if (!pdptPhys) { SerialPuts("VMM: FATAL cannot alloc dmap PDPT\n"); return; }
    ZeroPageIdentity(pdptPhys.raw());

    // We map up to 4GB of physical RAM (4 PDPT entries × 1GB each).
    // Each PDPT entry points to a PD with 512 × 2MB page entries.
    static constexpr uint64_t DMAP_GB = 4;
    static constexpr uint64_t PAGE_2MB = 0x80ULL; // PS bit

    uint64_t* pdptPtr = reinterpret_cast<uint64_t*>(pdptPhys.raw()); // identity-mapped
    for (uint64_t g = 0; g < DMAP_GB; g++)
    {
        PhysicalAddress pdPhys = PmmAllocPage(MemTag::PageTable, KernelPid);
        if (!pdPhys) { SerialPuts("VMM: FATAL cannot alloc dmap PD\n"); return; }
        ZeroPageIdentity(pdPhys.raw());

        uint64_t* pdPtr = reinterpret_cast<uint64_t*>(pdPhys.raw()); // identity-mapped
        for (uint64_t j = 0; j < 512; j++)
        {
            uint64_t physAddr = (g << 30) | (j << 21);
            pdPtr[j] = physAddr | VMM_PRESENT | VMM_WRITABLE | PAGE_2MB;
        }

        pdptPtr[g] = pdPhys.raw() | VMM_PRESENT | VMM_WRITABLE;
    }

    pml4[256] = pdptPhys.raw() | VMM_PRESENT | VMM_WRITABLE;

    // Flush TLB for the entire direct map range
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    SerialPuts("VMM: direct physical map active at 0xFFFF800000000000\n");
    SerialPuts("VMM: initialized\n");
}

bool VmmMapPage(PageTable pt, VirtualAddress virtAddr, PhysicalAddress physAddr,
                uint64_t flags, MemTag tag, uint16_t pid)
{
    if (virtAddr.raw() < VIRTUAL_NULL_GUARD)
    {
        SerialPrintf("VMM: rejected mapping at 0x%p (below null guard)\n",
                     reinterpret_cast<void*>(virtAddr.raw()));
        return false;
    }

    uint64_t* pte = WalkToPtr(pt, virtAddr, /*create=*/true, flags);
    if (!pte) return false;

    *pte = (physAddr.raw() & PHYS_MASK)
         | VMM_PRESENT
         | (flags & ~(VMM_PRESENT | PTE_TAG_MASK | PTE_PID_MASK | VMM_NO_EXEC))
         | (((uint64_t)(uint8_t)tag & 0x7) << PTE_TAG_SHIFT)
         | (((uint64_t)pid & 0x7FF) << PTE_PID_SHIFT)
         | (flags & VMM_NO_EXEC);
    Invlpg(virtAddr);
    return true;
}

void VmmUnmapPage(PageTable pt, VirtualAddress virtAddr)
{
    uint64_t* pte = WalkToPtr(pt, virtAddr, /*create=*/false);
    if (pte && (*pte & VMM_PRESENT))
    {
        *pte = 0;
        Invlpg(virtAddr);
    }
}

VirtualAddress VmmAllocPages(uint64_t pageCount, uint64_t flags, MemTag tag, uint16_t pid)
{
    if (pageCount == 0) return VirtualAddress{};

    if (g_vmallocNext + pageCount * PAGE_SIZE > VMALLOC_BASE + VMALLOC_SIZE)
        return VirtualAddress{};

    uint64_t virtBase = g_vmallocNext;
    g_vmallocNext += pageCount * PAGE_SIZE;

    for (uint64_t i = 0; i < pageCount; i++)
    {
        PhysicalAddress phys = PmmAllocPage(tag, pid);
        if (!phys)
        {
            for (uint64_t j = 0; j < i; j++)
            {
                VirtualAddress v(virtBase + j * PAGE_SIZE);
                uint64_t* pte = WalkToPtr(KernelPageTable, v, false);
                if (pte && (*pte & VMM_PRESENT))
                {
                    PmmFreePage(PhysicalAddress(*pte & PHYS_MASK));
                    *pte = 0;
                    Invlpg(v);
                }
            }
            g_vmallocNext = virtBase;
            return VirtualAddress{};
        }

        if (!VmmMapPage(KernelPageTable, VirtualAddress(virtBase + i * PAGE_SIZE), phys, flags, tag, pid))
        {
            PmmFreePage(phys);
            for (uint64_t j = 0; j < i; j++)
            {
                VirtualAddress v(virtBase + j * PAGE_SIZE);
                uint64_t* pte = WalkToPtr(KernelPageTable, v, false);
                if (pte && (*pte & VMM_PRESENT))
                {
                    PmmFreePage(PhysicalAddress(*pte & PHYS_MASK));
                    *pte = 0;
                    Invlpg(v);
                }
            }
            g_vmallocNext = virtBase;
            return VirtualAddress{};
        }
    }

    VmmAllocation* slot = FindFreeSlot();
    if (slot)
    {
        slot->virtBase  = virtBase;
        slot->pageCount = pageCount;
        slot->tag       = tag;
        slot->pid       = pid;
    }

    return VirtualAddress(virtBase);
}

VirtualAddress VmmAllocModulePages(uint64_t pageCount, uint64_t flags, MemTag tag, uint16_t pid)
{
    if (pageCount == 0) return VirtualAddress{};
    if (g_moduleNext + pageCount * PAGE_SIZE > MODULE_BASE + MODULE_SIZE)
        return VirtualAddress{};

    uint64_t virtBase = g_moduleNext;
    g_moduleNext += pageCount * PAGE_SIZE;

    for (uint64_t i = 0; i < pageCount; i++)
    {
        PhysicalAddress phys = PmmAllocPage(tag, pid);
        if (!phys)
        {
            for (uint64_t j = 0; j < i; j++)
            {
                VirtualAddress v(virtBase + j * PAGE_SIZE);
                uint64_t* pte = WalkToPtr(KernelPageTable, v, false);
                if (pte && (*pte & VMM_PRESENT))
                {
                    PmmFreePage(PhysicalAddress(*pte & PHYS_MASK));
                    *pte = 0;
                    Invlpg(v);
                }
            }
            g_moduleNext = virtBase;
            return VirtualAddress{};
        }

        if (!VmmMapPage(KernelPageTable, VirtualAddress(virtBase + i * PAGE_SIZE), phys, flags, tag, pid))
        {
            PmmFreePage(phys);
            for (uint64_t j = 0; j < i; j++)
            {
                VirtualAddress v(virtBase + j * PAGE_SIZE);
                uint64_t* pte = WalkToPtr(KernelPageTable, v, false);
                if (pte && (*pte & VMM_PRESENT))
                {
                    PmmFreePage(PhysicalAddress(*pte & PHYS_MASK));
                    *pte = 0;
                    Invlpg(v);
                }
            }
            g_moduleNext = virtBase;
            return VirtualAddress{};
        }
    }

    VmmAllocation* slot = FindFreeSlot();
    if (slot)
    {
        slot->virtBase  = virtBase;
        slot->pageCount = pageCount;
        slot->tag       = tag;
        slot->pid       = pid;
    }

    return VirtualAddress(virtBase);
}

void VmmFreePages(VirtualAddress virtAddr, uint64_t pageCount)
{
    for (uint64_t i = 0; i < pageCount; i++)
    {
        VirtualAddress v = virtAddr + i * PAGE_SIZE;
        uint64_t* pte = WalkToPtr(KernelPageTable, v, false);
        if (pte && (*pte & VMM_PRESENT))
        {
            PmmFreePage(PhysicalAddress(*pte & PHYS_MASK));
            *pte = 0;
            Invlpg(v);
        }
    }

    // Clear registration.
    VmmAllocation* alloc = FindAllocSlot(virtAddr.raw());
    if (alloc) alloc->virtBase = 0;
}

PhysicalAddress VmmVirtToPhys(PageTable pt, VirtualAddress virtAddr)
{
    uint64_t* pte = WalkToPtr(pt, virtAddr, false);
    if (!pte || !(*pte & VMM_PRESENT)) return PhysicalAddress{};
    return PhysicalAddress((*pte & PHYS_MASK) | (virtAddr.raw() & (PAGE_SIZE - 1)));
}

const VmmAllocation* VmmGetAllocation(VirtualAddress virtAddr)
{
    for (uint32_t i = 0; i < VMM_MAX_ALLOCS; i++)
    {
        const VmmAllocation& a = g_vmmAllocs[i];
        if (a.virtBase == 0) continue;
        if (virtAddr.raw() >= a.virtBase && virtAddr.raw() < a.virtBase + a.pageCount * PAGE_SIZE)
            return &a;
    }
    return nullptr;
}

MemTag VmmGetPageTag(PageTable pt, VirtualAddress virtAddr)
{
    uint64_t* pte = WalkToPtr(pt, virtAddr, false);
    if (!pte || !(*pte & VMM_PRESENT)) return MemTag::Free;
    return static_cast<MemTag>((*pte & PTE_TAG_MASK) >> PTE_TAG_SHIFT);
}

uint16_t VmmGetPagePid(PageTable pt, VirtualAddress virtAddr)
{
    uint64_t* pte = WalkToPtr(pt, virtAddr, false);
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
            VmmFreePages(VirtualAddress(a.virtBase), a.pageCount);
            // VmmFreePages clears virtBase; loop continues from fresh state.
        }
    }

    // Free any physical pages that were tracked at the PMM level.
    PmmKillPid(pid);
}

uint32_t VmmCountAllocations(uint16_t filterPid)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < VMM_MAX_ALLOCS; i++)
    {
        if (g_vmmAllocs[i].virtBase == 0) continue;
        if (filterPid == 0xFFFF || g_vmmAllocs[i].pid == filterPid)
            count++;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Per-process page tables
// ---------------------------------------------------------------------------

PageTable VmmKernelCR3()
{
    return PageTable(g_kernelCR3);
}

void VmmSwitchPageTable(PageTable pt)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pt.pml4.raw()) : "memory");
}

PageTable VmmCreateUserPageTable()
{
    PhysicalAddress pml4Phys = AllocTablePage();
    if (!pml4Phys) return PageTable{};
    ZeroPage(pml4Phys);

    uint64_t* newPml4  = reinterpret_cast<uint64_t*>(PhysToVirt(pml4Phys).raw());
    uint64_t* kernPml4 = reinterpret_cast<uint64_t*>(PhysToVirt(g_kernelCR3).raw());

    for (uint64_t i = 256; i < 512; i++)
        newPml4[i] = kernPml4[i];

    return PageTable(pml4Phys);
}

static void FreeTableLevel(PhysicalAddress tablePhys, int level)
{
    uint64_t* table = reinterpret_cast<uint64_t*>(PhysToVirt(tablePhys).raw());
    if (level > 1)
    {
        for (uint64_t i = 0; i < 512; i++)
        {
            if (!(table[i] & VMM_PRESENT)) continue;
            if (table[i] & (1ULL << 7)) continue; // huge page, skip
            FreeTableLevel(PhysicalAddress(table[i] & PHYS_MASK), level - 1);
        }
    }
    PmmFreePage(tablePhys);
}

void VmmDestroyUserPageTable(PageTable pt)
{
    if (!pt) return;

    uint64_t* pml4 = reinterpret_cast<uint64_t*>(PhysToVirt(pt.pml4).raw());

    for (uint64_t i = 0; i < 256; i++)
    {
        if (!(pml4[i] & VMM_PRESENT)) continue;
        FreeTableLevel(PhysicalAddress(pml4[i] & PHYS_MASK), 3);
    }

    PmmFreePage(pt.pml4);
}

} // namespace brook
