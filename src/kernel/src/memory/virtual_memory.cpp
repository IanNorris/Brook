#include "virtual_memory.h"
#include "physical_memory.h"
#include "serial.h"
#include "spinlock.h"

namespace brook {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr uint64_t PAGE_SIZE  = 4096;
// Physical address occupies bits [51:12] in a PTE.
// Bits [11:0] are flags, bits [62:53] are used for PID, bit 52 is COW, bit 63 is NX.
static constexpr uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL;

// ---------------------------------------------------------------------------
// VMALLOC bump allocator state
// ---------------------------------------------------------------------------

static uint64_t g_vmallocNext = VMALLOC_BASE;

// SMP lock protecting VMALLOC/module bump allocators and allocation registry.
static SpinLock g_vmmLock;

// Separate lock for kernel page table walks (create=true) to prevent
// concurrent GetOrAllocEntry races on shared intermediate levels.
static SpinLock g_kernelPtLock;

// SMP lock protecting USER page table walks/modifications. Threads in the
// same thread group share a page table; concurrent mmap's that fall in the
// same 2 MB region can race on leaf PT page allocation, causing one
// thread's PTE write to be silently lost. A single global lock here is
// sub-optimal for SMP scaling but is correct; per-tgid locking is a
// future optimisation. (BRO-003 root cause.)
static SpinLock g_userPtLock;

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
    volatile uint64_t* p = reinterpret_cast<volatile uint64_t*>(PhysToVirt(physAddr).raw());
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

        // Verify the page was actually zeroed (diagnostic for stale data bug).
        volatile uint64_t* check = reinterpret_cast<volatile uint64_t*>(PhysToVirt(childPhys).raw());
        for (int i = 0; i < 512; i++)
        {
            if (check[i] != 0)
            {
                SerialPrintf("VMM: BUG: ZeroPage failed! page=0x%lx [%d]=0x%lx\n",
                             childPhys.raw(), i, static_cast<uint64_t>(check[i]));
                // Re-zero with volatile writes
                for (int j = 0; j < 512; j++) check[j] = 0;
                break;
            }
        }

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

    // We map up to 16GB of physical RAM (16 PDPT entries × 1GB each).
    // Each PDPT entry points to a PD with 512 × 2MB page entries.
    // NOTE: this MUST be >= the configured QEMU -m size, otherwise PMM can
    // hand out page-table pages above the direct map and any subsequent
    // WalkToPtr will fault writing to an unmapped kernel address.
    // (GIMP triggered this with the previous 4GB cap on an 8GB VM.)
    static constexpr uint64_t DMAP_GB = 16;
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
    if (!(flags & VMM_FORCE_MAP) && virtAddr.raw() < VIRTUAL_NULL_GUARD)
    {
        SerialPrintf("VMM: rejected mapping at 0x%p (below null guard)\n",
                     reinterpret_cast<void*>(virtAddr.raw()));
        return false;
    }
    // Strip the force flag before writing the PTE
    flags &= ~VMM_FORCE_MAP;

    // Protect page table walks from concurrent modification. Both kernel
    // and user paths can race when create=true allocates intermediate
    // levels (PML4 → PDPT → PD → PT pages).
    bool isKernel = !pt;
    SpinLock& ptLock = isKernel ? g_kernelPtLock : g_userPtLock;
    uint64_t lf = SpinLockAcquire(&ptLock);

    uint64_t* pte = WalkToPtr(pt, virtAddr, /*create=*/true, flags);
    if (!pte)
    {
        SpinLockRelease(&ptLock, lf);
        return false;
    }

    *pte = (physAddr.raw() & PHYS_MASK)
         | VMM_PRESENT
         | (flags & ~(VMM_PRESENT | PTE_TAG_MASK | PTE_COW_BIT | PTE_PID_MASK | VMM_NO_EXEC))
         | (((uint64_t)(uint8_t)tag & 0x7) << PTE_TAG_SHIFT)
         | (((uint64_t)pid & 0x3FF) << PTE_PID_SHIFT)
         | (flags & VMM_NO_EXEC);
    Invlpg(virtAddr);

    SpinLockRelease(&ptLock, lf);
    return true;
}

void VmmUnmapPage(PageTable pt, VirtualAddress virtAddr)
{
    bool isKernel = !pt;
    SpinLock& ptLock = isKernel ? g_kernelPtLock : g_userPtLock;
    uint64_t lf = SpinLockAcquire(&ptLock);

    uint64_t* pte = WalkToPtr(pt, virtAddr, /*create=*/false);
    if (pte && (*pte & VMM_PRESENT))
    {
        *pte = 0;
        Invlpg(virtAddr);
    }

    SpinLockRelease(&ptLock, lf);
}

VirtualAddress VmmAllocPages(uint64_t pageCount, uint64_t flags, MemTag tag, uint16_t pid)
{
    if (pageCount == 0) return VirtualAddress{};

    uint64_t lf = SpinLockAcquire(&g_vmmLock);

    if (g_vmallocNext + pageCount * PAGE_SIZE > VMALLOC_BASE + VMALLOC_SIZE)
    {
        SpinLockRelease(&g_vmmLock, lf);
        return VirtualAddress{};
    }

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
            SpinLockRelease(&g_vmmLock, lf);
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
            SpinLockRelease(&g_vmmLock, lf);
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

    SpinLockRelease(&g_vmmLock, lf);
    return VirtualAddress(virtBase);
}

VirtualAddress VmmAllocKernelStack(uint64_t pageCount, MemTag tag, uint16_t pid)
{
    if (pageCount == 0) return VirtualAddress{};

    // Reserve pageCount + 2 virtual pages: [guard] [usable * pageCount] [guard]
    uint64_t totalPages = pageCount + 2;

    uint64_t lf = SpinLockAcquire(&g_vmmLock);

    if (g_vmallocNext + totalPages * PAGE_SIZE > VMALLOC_BASE + VMALLOC_SIZE)
    {
        SpinLockRelease(&g_vmmLock, lf);
        return VirtualAddress{};
    }

    uint64_t virtBase = g_vmallocNext;
    g_vmallocNext += totalPages * PAGE_SIZE;

    // Map only the middle pages — guard pages stay unmapped.
    uint64_t usableBase = virtBase + PAGE_SIZE;  // skip bottom guard
    for (uint64_t i = 0; i < pageCount; i++)
    {
        PhysicalAddress phys = PmmAllocPage(tag, pid);
        if (!phys)
        {
            for (uint64_t j = 0; j < i; j++)
            {
                VirtualAddress v(usableBase + j * PAGE_SIZE);
                uint64_t* pte = WalkToPtr(KernelPageTable, v, false);
                if (pte && (*pte & VMM_PRESENT))
                {
                    PmmFreePage(PhysicalAddress(*pte & PHYS_MASK));
                    *pte = 0;
                    Invlpg(v);
                }
            }
            g_vmallocNext = virtBase;
            SpinLockRelease(&g_vmmLock, lf);
            return VirtualAddress{};
        }

        if (!VmmMapPage(KernelPageTable, VirtualAddress(usableBase + i * PAGE_SIZE), phys, VMM_WRITABLE, tag, pid))
        {
            PmmFreePage(phys);
            for (uint64_t j = 0; j < i; j++)
            {
                VirtualAddress v(usableBase + j * PAGE_SIZE);
                uint64_t* pte = WalkToPtr(KernelPageTable, v, false);
                if (pte && (*pte & VMM_PRESENT))
                {
                    PmmFreePage(PhysicalAddress(*pte & PHYS_MASK));
                    *pte = 0;
                    Invlpg(v);
                }
            }
            g_vmallocNext = virtBase;
            SpinLockRelease(&g_vmmLock, lf);
            return VirtualAddress{};
        }
    }

    VmmAllocation* slot = FindFreeSlot();
    if (slot)
    {
        slot->virtBase  = usableBase;
        slot->pageCount = pageCount;
        slot->tag       = tag;
        slot->pid       = pid;
    }

    SpinLockRelease(&g_vmmLock, lf);
    return VirtualAddress(usableBase);
}

void VmmFreeKernelStack(VirtualAddress virtAddr, uint64_t pageCount)
{
    // Free the mapped pages (guard pages have no physical backing).
    VmmFreePages(virtAddr, pageCount);
    // The guard page virtual addresses are simply leaked from the vmalloc region.
    // This is acceptable — kernel stacks are long-lived and few in number.
}

VirtualAddress VmmAllocModulePages(uint64_t pageCount, uint64_t flags, MemTag tag, uint16_t pid)
{
    if (pageCount == 0) return VirtualAddress{};

    uint64_t lf = SpinLockAcquire(&g_vmmLock);

    if (g_moduleNext + pageCount * PAGE_SIZE > MODULE_BASE + MODULE_SIZE)
    {
        SpinLockRelease(&g_vmmLock, lf);
        return VirtualAddress{};
    }

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
            SpinLockRelease(&g_vmmLock, lf);
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
            SpinLockRelease(&g_vmmLock, lf);
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

    SpinLockRelease(&g_vmmLock, lf);
    return VirtualAddress(virtBase);
}

void VmmFreePages(VirtualAddress virtAddr, uint64_t pageCount)
{
    uint64_t lf = SpinLockAcquire(&g_vmmLock);

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

    SpinLockRelease(&g_vmmLock, lf);
}

PhysicalAddress VmmVirtToPhys(PageTable pt, VirtualAddress virtAddr)
{
    uint64_t* pte = WalkToPtr(pt, virtAddr, false);
    if (!pte || !(*pte & VMM_PRESENT)) return PhysicalAddress{};
    return PhysicalAddress((*pte & PHYS_MASK) | (virtAddr.raw() & (PAGE_SIZE - 1)));
}

uint64_t* VmmGetPte(PageTable pt, VirtualAddress virtAddr)
{
    uint64_t* pte = WalkToPtr(pt, virtAddr, false);
    if (!pte || !(*pte & VMM_PRESENT)) return nullptr;
    return pte;
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

bool VmmKernelMarkReadOnly(VirtualAddress virtAddr, uint64_t size)
{
    uint64_t va = virtAddr.raw();
    if ((va & (PAGE_SIZE - 1)) || (size & (PAGE_SIZE - 1)) || size == 0)
    {
        SerialPrintf("VMM: VmmKernelMarkReadOnly: unaligned va=0x%lx size=0x%lx\n",
                     va, size);
        return false;
    }

    uint64_t lf = SpinLockAcquire(&g_kernelPtLock);
    bool ok = true;
    for (uint64_t off = 0; off < size; off += PAGE_SIZE)
    {
        VirtualAddress page(va + off);
        uint64_t* pte = WalkToPtr(KernelPageTable, page, /*create=*/false);
        if (!pte || !(*pte & VMM_PRESENT))
        {
            SerialPrintf("VMM: VmmKernelMarkReadOnly: page 0x%lx not present\n",
                         page.raw());
            ok = false;
            continue;
        }
        *pte &= ~VMM_WRITABLE;
        Invlpg(page);
    }
    SpinLockRelease(&g_kernelPtLock, lf);
    return ok;
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
    else
    {
        // Leaf level (PT): unref data pages (handles COW shared pages)
        for (uint64_t i = 0; i < 512; i++)
        {
            if (!(table[i] & VMM_PRESENT)) continue;
            PhysicalAddress pagePhys(table[i] & PHYS_MASK);
            PmmUnrefPage(pagePhys);
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
