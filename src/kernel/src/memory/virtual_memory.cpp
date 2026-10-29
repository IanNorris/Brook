#include "virtual_memory.h"
#include "physical_memory.h"
#include "serial.h"
#include "spinlock.h"
#include "vmalloc_arena.h"

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

static VmallocArena<VMALLOC_SIZE / PAGE_SIZE> g_vmallocArena;

// SMP lock protecting VMALLOC/module bump allocators and allocation registry.
// IrqSpinLock: prevents preemption deadlock when timer interrupt
// fires while a CPU holds the lock.
static IrqSpinLock g_vmmLock;

// Separate lock for kernel page table walks (create=true) to prevent
// concurrent GetOrAllocEntry races on shared intermediate levels.
static IrqSpinLock g_kernelPtLock;

// SMP lock protecting USER page table walks/modifications. Threads in the
// same thread group share a page table; concurrent mmap's that fall in the
// same 2 MB region can race on leaf PT page allocation, causing one
// thread's PTE write to be silently lost. A single global lock here is
// sub-optimal for SMP scaling but is correct; per-tgid locking is a
// future optimisation. (BRO-003 root cause.)
static IrqSpinLock g_userPtLock;

// The kernel's PML4 physical address, captured at init time.
static PhysicalAddress g_kernelCR3{};
#ifdef BROOK_HOST_TEST
void VmmHostSetKernelRoot(PageTable root) { g_kernelCR3 = root.pml4; }
#endif

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
#if defined(BROOK_HOST_TEST) && !defined(BROOK_STANDALONE_TEST)
    cr3 = 0;
#else
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
#endif
    return cr3;
}

static inline void Invlpg(VirtualAddress virtAddr)
{
#if defined(BROOK_HOST_TEST) && !defined(BROOK_STANDALONE_TEST)
    (void)virtAddr;
#else
    __asm__ volatile("invlpg (%0)" : : "r"(virtAddr.raw()) : "memory");
#endif
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
                            bool create, uint64_t flags = 0, bool* occupiedLarge = nullptr)
{
    uint64_t* pml4 = reinterpret_cast<uint64_t*>(PhysToVirt(ResolvePml4(pageTable)).raw());
    uint64_t va = virtAddr.raw();

    uint64_t* pdpt = create
        ? GetOrAllocEntry(pml4, Pml4Index(va), flags)
        : (pml4[Pml4Index(va)] & VMM_PRESENT)
            ? reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(pml4[Pml4Index(va)] & PHYS_MASK)).raw())
            : nullptr;
    if (!pdpt) return nullptr;
    // A present 1GB leaf is data, never a pointer to a page directory. This
    // walker does not split 1GB leaves; try-install reports the occupied range.
    if ((pdpt[PdptIndex(va)] & (VMM_PRESENT | (1ULL << 7))) ==
        (VMM_PRESENT | (1ULL << 7))) {
        if (occupiedLarge) *occupiedLarge = true;
        return nullptr;
    }

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
        if (occupiedLarge) { *occupiedLarge = true; return nullptr; }
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
    IrqSpinLock& ptLock = isKernel ? g_kernelPtLock : g_userPtLock;
    uint64_t ptFlags = IrqSpinLockAcquire(&ptLock);

    uint64_t* pte = WalkToPtr(pt, virtAddr, /*create=*/true, flags);
    if (!pte)
    {
        IrqSpinLockRelease(&ptLock, ptFlags);
        return false;
    }

    // BRO-176 map accounting: if we are REPLACING a present USER PTE, drop the
    // old frame's mapcount first (this VA no longer maps it).
    uint64_t oldPte = *pte;
    if (VmmLeafBacked(oldPte) && (oldPte & VMM_USER))
        PmmMapDec(PhysicalAddress(oldPte & PHYS_MASK));

    *pte = (physAddr.raw() & PHYS_MASK)
         | VMM_PRESENT
         | (flags & ~(VMM_PRESENT | PTE_TAG_MASK | PTE_COW_BIT | PTE_PID_MASK | VMM_NO_EXEC | PTE_HIDDEN_BIT))
         | (((uint64_t)(uint8_t)tag & 0x7) << PTE_TAG_SHIFT)
         | (((uint64_t)pid & 0x3FF) << PTE_PID_SHIFT)
         | (flags & VMM_NO_EXEC);
    Invlpg(virtAddr);

    // A new present USER PTE now maps this frame — count it.
    if (flags & VMM_USER)
        PmmMapInc(physAddr);

    IrqSpinLockRelease(&ptLock, ptFlags);
    return true;
}

VmmMapResult VmmTryMapPage(PageTable pt, VirtualAddress virtAddr,
                           PhysicalAddress physAddr, uint64_t flags,
                           MemTag tag, uint16_t pid, bool accessible)
{
    if ((flags & (PTE_COW_BIT | VMM_WRITABLE)) == (PTE_COW_BIT | VMM_WRITABLE))
        return VmmMapResult::InvalidAddress;
    if (!(flags & VMM_FORCE_MAP) && virtAddr.raw() < VIRTUAL_NULL_GUARD)
        return VmmMapResult::InvalidAddress;
    flags &= ~VMM_FORCE_MAP;
    IrqSpinLock& lock = !pt ? g_kernelPtLock : g_userPtLock;
    uint64_t saved = IrqSpinLockAcquire(&lock);
    bool large = false;
    // An occupied result must not even promote ancestor USER permissions.
    // Probe without creation before preparing any missing intermediate tables.
    uint64_t* pte = WalkToPtr(pt, virtAddr, false, 0, &large);
    if (large || (pte && VmmLeafBacked(*pte))) {
        IrqSpinLockRelease(&lock, saved);
        return VmmMapResult::Occupied;
    }
    // Build missing tables without promoting existing ancestor permissions.
    // On allocation failure, empty tables may remain owned by this page table.
    if (!pte) pte = WalkToPtr(pt, virtAddr, true, 0);
    VmmMapResult result;
    if (!pte) result = VmmMapResult::OutOfMemory;
    else {
        *pte = (physAddr.raw() & PHYS_MASK) | (accessible ? VMM_PRESENT : PTE_HIDDEN_BIT)
             | (flags & ~(VMM_PRESENT | PTE_TAG_MASK | PTE_PID_MASK | VMM_NO_EXEC | PTE_HIDDEN_BIT))
             | (((uint64_t)(uint8_t)tag & 0x7) << PTE_TAG_SHIFT)
             | (((uint64_t)pid & 0x3FF) << PTE_PID_SHIFT)
             | (flags & VMM_NO_EXEC);
        if (flags & VMM_USER) {
            PmmMapInc(physAddr);
            // All three intermediate tables now exist. Promote only after a
            // successful leaf install, never on Occupied or allocation failure.
            auto* entry = reinterpret_cast<uint64_t*>(PhysToVirt(ResolvePml4(pt)).raw());
            const uint64_t indices[] = {Pml4Index(virtAddr.raw()), PdptIndex(virtAddr.raw()), PdIndex(virtAddr.raw())};
            for (uint64_t index : indices) {
                entry[index] |= VMM_USER;
                entry = reinterpret_cast<uint64_t*>(PhysToVirt(VmmLeafPhysical(entry[index])).raw());
            }
        }
        Invlpg(virtAddr);
        result = VmmMapResult::Mapped;
    }
    IrqSpinLockRelease(&lock, saved);
    return result;
}

PhysicalAddress VmmLeafPhysical(uint64_t leaf) { return PhysicalAddress(leaf & PHYS_MASK); }
uint64_t VmmReadLeaf(PageTable pt, VirtualAddress address) {
    IrqSpinLock& lock = !pt ? g_kernelPtLock : g_userPtLock;
    uint64_t irq = IrqSpinLockAcquire(&lock);
    bool large = false;
    auto* entry = WalkToPtr(pt,address,false,0,&large);
    uint64_t leaf = !large && entry ? *entry : 0;
    IrqSpinLockRelease(&lock,irq); return leaf;
}

bool VmmClearAccessedIfMatches(PageTable pt,VirtualAddress address,PhysicalAddress expected) {
    if(!pt || (address.raw()&4095) || !expected)return false;
    uint64_t irq=IrqSpinLockAcquire(&g_userPtLock);
    bool large=false;auto* entry=WalkToPtr(pt,address,false,0,&large);bool changed=false;
    if(entry && !large) {
        uint64_t old=__atomic_load_n(entry,__ATOMIC_RELAXED);
        while((old&(VMM_PRESENT|VMM_USER|VMM_ACCESSED))==(VMM_PRESENT|VMM_USER|VMM_ACCESSED) &&
              !(old&PTE_COW_BIT) && VmmLeafPhysical(old)==expected) {
            const uint64_t aged=old&~VMM_ACCESSED;
            if(__atomic_compare_exchange_n(entry,&old,aged,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) {
                changed=true;break;
            }
        }
    }
    IrqSpinLockRelease(&g_userPtLock,irq);return changed;
}

using LeafUpdate = uint64_t (*)(VirtualAddress,uint64_t,void*);
static bool WalkLeafRange(uint64_t* table, unsigned level, uint64_t base,
                           uint64_t begin, uint64_t end, bool detach,
                           VmmRetireLeaf retire, void* context, uint64_t& count,
                           LeafUpdate update = nullptr)
{
    const unsigned shift = 12 + 9 * (level - 1);
    const uint64_t size = uint64_t{1} << shift;
    uint64_t first = begin > base ? (begin-base) >> shift : 0;
    uint64_t last = (end-1-base) >> shift;
    if (last > 511) last = 511;
    for (uint64_t i = first; i <= last; ++i) {
        uint64_t leaf = table[i];
        if (level == 1 ? !VmmLeafBacked(leaf) : !(leaf & VMM_PRESENT)) continue;
        uint64_t address = base + i*size;
        if (level > 1) {
            if (leaf & (1ULL << 7)) return false;
            auto* child = reinterpret_cast<uint64_t*>(PhysToVirt(VmmLeafPhysical(leaf)).raw());
            const uint64_t low = begin > address ? begin : address;
            const uint64_t high = end < address+size ? end : address+size;
            if (!WalkLeafRange(child,level-1,address,low,high,detach,retire,context,count,update)) return false;
        } else {
            ++count;
            if (detach) {
                retire(VirtualAddress(address),leaf,context);
                if (leaf & VMM_USER) PmmMapDec(VmmLeafPhysical(leaf));
                table[i] = 0; Invlpg(VirtualAddress(address));
            } else if (update) {
                const uint64_t updated=update(VirtualAddress(address),leaf,context);
                if (updated!=leaf) { table[i]=updated;Invlpg(VirtualAddress(address)); }
            }
        }
    }
    return true;
}
static bool ValidLeafRange(PageTable pt, uint64_t begin, uint64_t end) {
    return pt && begin < end && end <= 0x0000800000000000ULL && !(begin & 4095) && !(end & 4095);
}
bool VmmPrepareRange(PageTable pt, uint64_t begin, uint64_t end) {
    if (!ValidLeafRange(pt,begin,end)) return false;
    uint64_t irq = IrqSpinLockAcquire(&g_userPtLock);
    bool result = true;
    for (uint64_t address = begin; address < end;) {
        bool large = false;
        auto* leaf = WalkToPtr(pt,VirtualAddress(address),false,0,&large);
        if (large || (!leaf && !WalkToPtr(pt,VirtualAddress(address),true,0))) { result = false; break; }
        address = (address | uint64_t{0x1fffff}) + 1;
    }
    IrqSpinLockRelease(&g_userPtLock,irq); return result;
}
struct MoveLeaves { PageTable table;uint64_t source,destination;bool prepare,occupied,move,failed; };
static uint64_t MoveLeaf(VirtualAddress address,uint64_t leaf,void* opaque) {
    auto& context=*static_cast<MoveLeaves*>(opaque);
    if (context.failed) return leaf;
    const VirtualAddress destination(context.destination+address.raw()-context.source);
    bool large=false;auto* target=WalkToPtr(context.table,destination,false,0,&large);
    if (!large && !target && context.prepare) target=WalkToPtr(context.table,destination,true,0);
    if (large || !target || (!context.occupied && VmmLeafBacked(*target))) { context.failed=true;return leaf; }
    if (!context.move) return leaf;
    *target=leaf;
    if (leaf & VMM_USER) {
        auto* table=reinterpret_cast<uint64_t*>(PhysToVirt(context.table.pml4).raw());
        const uint64_t indices[]={Pml4Index(destination.raw()),PdptIndex(destination.raw()),PdIndex(destination.raw())};
        for (uint64_t index:indices) { table[index]|=VMM_USER;table=reinterpret_cast<uint64_t*>(PhysToVirt(VmmLeafPhysical(table[index])).raw()); }
    }
    Invlpg(destination);return 0; // transfer the PTE-owned reference, no ref/map-count change
}
static bool MoveLeafRange(PageTable pt,uint64_t begin,uint64_t end,uint64_t destination,
                           bool prepare,bool occupied,bool move) {
    if (!ValidLeafRange(pt,begin,end) || !ValidLeafRange(pt,destination,destination+(end-begin)) ||
        (destination<end && begin<destination+(end-begin))) return false;
    uint64_t irq=IrqSpinLockAcquire(&g_userPtLock);
    auto* table=reinterpret_cast<uint64_t*>(PhysToVirt(pt.pml4).raw());
    uint64_t count=0;
    MoveLeaves context{pt,begin,destination,prepare,occupied,false,false};
    bool result=WalkLeafRange(table,4,0,begin,end,false,nullptr,nullptr,count);
    if (result && !occupied) {
        count=0;
        result=WalkLeafRange(table,4,0,destination,destination+(end-begin),false,nullptr,nullptr,count) && !count;
    }
    if (result) {
        count=0;
        result=WalkLeafRange(table,4,0,begin,end,false,nullptr,&context,count,MoveLeaf) && !context.failed;
    }
    if (result && move) {
        context.move=true;count=0;
        if (!WalkLeafRange(table,4,0,begin,end,false,nullptr,&context,count,MoveLeaf) || context.failed) __builtin_trap();
    }
    IrqSpinLockRelease(&g_userPtLock,irq);return result;
}
bool VmmPrepareMove(PageTable pt,uint64_t begin,uint64_t end,uint64_t destination) {
    return MoveLeafRange(pt,begin,end,destination,true,true,false);
}
bool VmmCanMoveRange(PageTable pt,uint64_t begin,uint64_t end,uint64_t destination,bool occupiedDestination) {
    return MoveLeafRange(pt,begin,end,destination,false,occupiedDestination,false);
}
bool VmmMoveRange(PageTable pt,uint64_t begin,uint64_t end,uint64_t destination) {
    return MoveLeafRange(pt,begin,end,destination,false,false,true);
}
struct MeasureWalk {
    uint64_t cursor, budget;
    bool accountRam;
    VmmRangeStats* stats;
};
static bool MeasureTable(uint64_t* table, unsigned level, uint64_t base,
                          uint64_t begin, uint64_t end, MeasureWalk& walk)
{
    const uint64_t span = uint64_t{1} << (12 + (level-1)*9);
    const uint64_t first = (begin-base)/span;
    const uint64_t last = (end-1-base)/span;
    for (uint64_t i=first;i<=last;++i) {
        if (!walk.budget) return true;
        --walk.budget; ++walk.stats->tableEntriesVisited;
        const uint64_t address = base + i*span;
        const uint64_t low = begin > address ? begin : address;
        const uint64_t high = end < address+span ? end : address+span;
        const uint64_t entry = table[i];
        if (!VmmLeafBacked(entry)) { walk.cursor=high;continue; }
        if (level>1) {
            if (!(entry & VMM_PRESENT) || (entry & (1ULL<<7))) return false;
            auto* child = reinterpret_cast<uint64_t*>(PhysToVirt(VmmLeafPhysical(entry)).raw());
            if (!MeasureTable(child,level-1,address,low,high,walk)) return false;
        } else {
            if (entry & VMM_USER) {
                auto& stats = *walk.stats;
                ++stats.backedPages;
                if (!(entry & VMM_PRESENT)) ++stats.hiddenPages;
                if (entry & PTE_COW_BIT) ++stats.cowPages;
                if (entry & VMM_WRITABLE) ++stats.writablePages;
                if (walk.accountRam) {
                    const uint32_t mappings=PmmGetMapCount(VmmLeafPhysical(entry));
                    if (!mappings) ++stats.unknownMapCountPages;
                    else {
                        if (mappings>1) ++stats.multiplyMappedPages;
                        stats.proportionalUnits += (4096ULL<<16)/mappings;
                    }
                }
            }
            walk.cursor=high;
        }
    }
    return true;
}
bool VmmMeasureRange(PageTable pt, uint64_t begin, uint64_t end,
                     bool accountRam, VmmRangeStats* stats)
{
    if (!stats) return false;
    *stats={};
    if (!ValidLeafRange(pt,begin,end)) return false;
    VmmRangeStats measured{};
    uint64_t cursor=begin;
    while(cursor<end) {
        // Bound real traversal work, rather than iterating every 2MiB of a
        // potentially multi-TiB untouched reservation.
        MeasureWalk walk{cursor,4096,accountRam,&measured};
        const uint64_t irq=IrqSpinLockAcquire(&g_userPtLock);
        auto* table=reinterpret_cast<uint64_t*>(PhysToVirt(pt.pml4).raw());
        const bool valid=MeasureTable(table,4,0,cursor,end,walk);
        IrqSpinLockRelease(&g_userPtLock,irq);
        if (!valid || walk.cursor<=cursor) return false;
        ++measured.lockBatches;cursor=walk.cursor;
    }
    *stats=measured;
    return true;
}

bool VmmCountRangeLeaves(PageTable pt, uint64_t begin, uint64_t end, uint64_t* count)
{
    if (!count || !ValidLeafRange(pt,begin,end)) return false;
    *count = 0;
    uint64_t irq = IrqSpinLockAcquire(&g_userPtLock);
    auto* table = reinterpret_cast<uint64_t*>(PhysToVirt(pt.pml4).raw());
    bool result = WalkLeafRange(table,4,0,begin,end,false,nullptr,nullptr,*count);
    IrqSpinLockRelease(&g_userPtLock,irq);
    return result;
}
struct CollectAddresses { uint64_t* addresses; uint64_t count; };
static uint64_t CollectAddress(VirtualAddress address, uint64_t leaf, void* opaque) {
    auto& collected = *static_cast<CollectAddresses*>(opaque);
    collected.addresses[collected.count++] = address.raw();
    return leaf;
}
bool VmmCollectRangeAddresses(PageTable pt, uint64_t begin, uint64_t end,
                              uint64_t* addresses, uint64_t capacity, uint64_t* count) {
    if (!count || (capacity && !addresses) || !ValidLeafRange(pt,begin,end)) return false;
    *count = 0;
    uint64_t irq = IrqSpinLockAcquire(&g_userPtLock);
    auto* table = reinterpret_cast<uint64_t*>(PhysToVirt(pt.pml4).raw());
    uint64_t required = 0;
    bool result = WalkLeafRange(table,4,0,begin,end,false,nullptr,nullptr,required) && required <= capacity;
    if (result && required) {
        CollectAddresses collected{addresses,0};
        result = WalkLeafRange(table,4,0,begin,end,false,nullptr,&collected,*count,CollectAddress);
    }
    IrqSpinLockRelease(&g_userPtLock,irq);
    return result;
}
bool VmmDetachRange(PageTable pt, uint64_t begin, uint64_t end, uint64_t capacity,
                    VmmRetireLeaf retire, void* context, uint64_t* count)
{
    if (!retire || !count || !ValidLeafRange(pt,begin,end)) return false;
    *count = 0;
    uint64_t irq = IrqSpinLockAcquire(&g_userPtLock);
    auto* table = reinterpret_cast<uint64_t*>(PhysToVirt(pt.pml4).raw());
    uint64_t required = 0;
    bool result = WalkLeafRange(table,4,0,begin,end,false,nullptr,nullptr,required) && required <= capacity;
    if (result) {
        if (!WalkLeafRange(table,4,0,begin,end,true,retire,context,*count)) {
            SerialPuts("VMM: range walk changed under page-table lock\n");
            __builtin_trap();
        }
    }
    IrqSpinLockRelease(&g_userPtLock,irq);
    return result;
}
struct ProtectLeaves { uint32_t protection; VmmPrivateLeafQuery query; void* context; };
static uint64_t ProtectLeaf(VirtualAddress address, uint64_t leaf, void* opaque) {
    auto& update = *static_cast<ProtectLeaves*>(opaque);
    if (!update.protection) return (leaf & ~VMM_PRESENT) | PTE_HIDDEN_BIT;
    uint64_t result = (leaf & ~(PTE_HIDDEN_BIT | VMM_WRITABLE | VMM_NO_EXEC | PTE_COW_BIT)) | VMM_PRESENT;
    if (!(update.protection & 4)) result |= VMM_NO_EXEC;
    if (update.protection & 2) {
        bool privatePage = update.query && update.query(address,leaf,update.context);
        if (privatePage && ((leaf & PTE_COW_BIT) || PmmGetRefCount(VmmLeafPhysical(leaf)) > 1))
            result |= PTE_COW_BIT;
        else result = (result & ~PTE_COW_BIT) | VMM_WRITABLE;
    }
    return result;
}
bool VmmProtectRange(PageTable pt, uint64_t begin, uint64_t end, uint32_t protection,
                     VmmPrivateLeafQuery query, void* context) {
    if ((protection & ~7u) || !ValidLeafRange(pt,begin,end)) return false;
    uint64_t irq = IrqSpinLockAcquire(&g_userPtLock);
    auto* table = reinterpret_cast<uint64_t*>(PhysToVirt(pt.pml4).raw());
    uint64_t count = 0; ProtectLeaves update{protection,query,context};
    bool result = WalkLeafRange(table,4,0,begin,end,false,nullptr,nullptr,count);
    if (result) {
        count = 0;
        if (!WalkLeafRange(table,4,0,begin,end,false,nullptr,&update,count,ProtectLeaf)) __builtin_trap();
    }
    IrqSpinLockRelease(&g_userPtLock,irq); return result;
}

uint64_t VmmDetachPage(PageTable pt, VirtualAddress virtAddr)
{
    IrqSpinLock& lock = !pt ? g_kernelPtLock : g_userPtLock;
    uint64_t saved = IrqSpinLockAcquire(&lock);
    uint64_t* pte = WalkToPtr(pt, virtAddr, false);
    uint64_t old = pte && VmmLeafBacked(*pte) ? *pte : 0;
    if (old) {
        if (old & VMM_USER) PmmMapDec(VmmLeafPhysical(old));
        *pte = 0;
        Invlpg(virtAddr);
    }
    IrqSpinLockRelease(&lock, saved);
    return old;
}

uint64_t VmmDetachPageIfMatches(PageTable pt, VirtualAddress virtAddr, PhysicalAddress expected)
{
    IrqSpinLock& lock = !pt ? g_kernelPtLock : g_userPtLock;
    uint64_t saved = IrqSpinLockAcquire(&lock);
    uint64_t* pte = WalkToPtr(pt, virtAddr, false);
    uint64_t old = pte && VmmLeafBacked(*pte) && VmmLeafPhysical(*pte) == expected ? *pte : 0;
    if (old) {
        if (old & VMM_USER) PmmMapDec(expected);
        *pte = 0;
        Invlpg(virtAddr);
    }
    IrqSpinLockRelease(&lock, saved);
    return old;
}

void VmmUnmapPage(PageTable pt, VirtualAddress virtAddr)
{
    (void)VmmDetachPage(pt, virtAddr);
}

// g_vmmLock covers reservation, construction, teardown and retirement. Physical
// failures use the same delayed VA retirement path as ordinary frees.
static VirtualAddress AllocVmalloc(uint64_t pageCount, uint64_t flags, MemTag tag,
                                   uint16_t pid, bool guards)
{
    uint64_t saved = IrqSpinLockAcquire(&g_vmmLock);
    uint64_t usablePage = 0;
    if (!g_vmallocArena.Reserve(pageCount, guards, usablePage)) {
        IrqSpinLockRelease(&g_vmmLock, saved);
        return VirtualAddress{};
    }
    const uint64_t base = VMALLOC_BASE + usablePage * PAGE_SIZE;
    uint64_t mapped = 0;
    for (; mapped < pageCount; ++mapped) {
        const PhysicalAddress phys = PmmAllocPage(tag, pid);
        if (!phys) break;
        if (!VmmMapPage(KernelPageTable, VirtualAddress(base + mapped * PAGE_SIZE),
                        phys, flags, tag, pid)) {
            PmmFreePage(phys);
            break;
        }
    }
    if (mapped != pageCount) {
        for (uint64_t i = 0; i < mapped; ++i) {
            const uint64_t leaf = VmmDetachPage(KernelPageTable, VirtualAddress(base + i * PAGE_SIZE));
            if (leaf) PmmFreePage(VmmLeafPhysical(leaf));
        }
        if (!g_vmallocArena.Retire(usablePage, pageCount)) __builtin_trap();
        IrqSpinLockRelease(&g_vmmLock, saved);
        return VirtualAddress{};
    }
    if (VmmAllocation* slot = FindFreeSlot()) {
        slot->virtBase = base; slot->pageCount = pageCount;
        slot->tag = tag; slot->pid = pid;
    }
    IrqSpinLockRelease(&g_vmmLock, saved);
    return VirtualAddress(base);
}

VirtualAddress VmmAllocPages(uint64_t pageCount, uint64_t flags, MemTag tag, uint16_t pid)
{
    return AllocVmalloc(pageCount, flags, tag, pid, false);
}

VirtualAddress VmmAllocKernelStack(uint64_t pageCount, MemTag tag, uint16_t pid)
{
    return AllocVmalloc(pageCount, VMM_WRITABLE, tag, pid, true);
}

void VmmFreeKernelStack(VirtualAddress virtAddr, uint64_t pageCount)
{
    // The arena recognizes the complete guarded reservation from the usable
    // range. The existing caller remains responsible for being off this stack.
    VmmFreePages(virtAddr, pageCount);
}

bool VmmBeginReclaim()
{
    const uint64_t saved = IrqSpinLockAcquire(&g_vmmLock);
    const bool result = g_vmallocArena.BeginReclaim();
    IrqSpinLockRelease(&g_vmmLock, saved);
    return result;
}

VmmArenaStatistics VmmArenaReadStatistics()
{
    const uint64_t saved = IrqSpinLockAcquire(&g_vmmLock);
    const auto stats = g_vmallocArena.Stats();
    IrqSpinLockRelease(&g_vmmLock, saved);
    return {stats.reserved, stats.retired, stats.peak, stats.totalReserved,
            stats.totalReclaimed, stats.failed};
}

void VmmCompleteReclaim()
{
    const uint64_t saved = IrqSpinLockAcquire(&g_vmmLock);
    if (!g_vmallocArena.CompleteReclaim()) __builtin_trap();
    IrqSpinLockRelease(&g_vmmLock, saved);
}

VirtualAddress VmmAllocModulePages(uint64_t pageCount, uint64_t flags, MemTag tag, uint16_t pid)
{
    if (pageCount == 0) return VirtualAddress{};

    uint64_t vmmFlags = IrqSpinLockAcquire(&g_vmmLock);

    if (g_moduleNext + pageCount * PAGE_SIZE > MODULE_BASE + MODULE_SIZE)
    {
        IrqSpinLockRelease(&g_vmmLock, vmmFlags);
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
            IrqSpinLockRelease(&g_vmmLock, vmmFlags);
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
            IrqSpinLockRelease(&g_vmmLock, vmmFlags);
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

    IrqSpinLockRelease(&g_vmmLock, vmmFlags);
    return VirtualAddress(virtBase);
}

void VmmFreePages(VirtualAddress virtAddr, uint64_t pageCount)
{
    const uint64_t address = virtAddr.raw();
    if (!pageCount || (address & (PAGE_SIZE - 1))) return;
    const bool vmalloc = address >= VMALLOC_BASE && address < VMALLOC_BASE + VMALLOC_SIZE;
    const bool module = address >= MODULE_BASE && address < MODULE_BASE + MODULE_SIZE;
    if (!vmalloc && !module) return;
    const uint64_t limit = vmalloc ? VMALLOC_BASE + VMALLOC_SIZE : MODULE_BASE + MODULE_SIZE;
    if (pageCount > (limit - address) / PAGE_SIZE) return;
    const uint64_t saved = IrqSpinLockAcquire(&g_vmmLock);
    const uint64_t page = vmalloc ? (address - VMALLOC_BASE) / PAGE_SIZE : 0;
    VmallocArena<VMALLOC_SIZE / PAGE_SIZE>::Range reservation{};
    if (vmalloc && !g_vmallocArena.Describe(page, pageCount, reservation)) {
        IrqSpinLockRelease(&g_vmmLock, saved);
        return;
    }
    for (uint64_t i = 0; i < pageCount; ++i) {
        // Clear the PTE under its lock BEFORE queuing the frame for the PMM
        // barrier. A barrier cannot protect a still-present mapping.
        const uint64_t leaf = VmmDetachPage(KernelPageTable, virtAddr + i * PAGE_SIZE);
        if (leaf) PmmFreePage(VmmLeafPhysical(leaf));
    }
    if (vmalloc && !g_vmallocArena.Retire(page, pageCount)) __builtin_trap();
    if (VmmAllocation* alloc = FindAllocSlot(address)) alloc->virtBase = 0;
    IrqSpinLockRelease(&g_vmmLock, saved);
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

// BRO-176 diag: dump a physical page's ref/unref history ring (physical_memory.cpp).
extern "C" int PmmDumpFreeLog(uint64_t phys);

VmmCowResolution VmmCowResolveWrite(PageTable pt, VirtualAddress faultAddr, uint16_t pid)
{
    PhysicalAddress retired;
    VirtualAddress pageAddr(faultAddr.raw() & ~(PAGE_SIZE - 1));

    // Hold g_userPtLock across the whole refcount-check + PTE update so this is
    // atomic against concurrent COW faults on the same page, against fork's
    // COW-share (VmmForkCommitCowShare), and against munmap/mprotect — all of
    // which take this lock. Under it, the only refcount INCREASE (fork) cannot
    // run, so a plain PmmGetRefCount read is a sound basis for the copy vs.
    // in-place decision; a concurrent DECREASE (process exit) only ever makes
    // in-place safer. The TLB shootdown is intentionally deferred to the caller
    // (see header): it busy-waits for remote IPI ACKs and we hold this lock
    // with interrupts disabled.
    uint64_t ptFlags = IrqSpinLockAcquire(&g_userPtLock);

    uint64_t* pte = WalkToPtr(pt, pageAddr, /*create=*/false);
    if (!pte || !(*pte & VMM_PRESENT) || !(*pte & PTE_COW_BIT))
    {
        IrqSpinLockRelease(&g_userPtLock, ptFlags);
        return {VmmCowResult::NotCow, {}};
    }

    PhysicalAddress oldPhys(*pte & PHYS_MASK);

    // BRO-176 red-handed detector: this PTE is Present + COW (checked above), so
    // oldPhys MUST still be referenced (refcount >= 1) — a COW-mapped frame is by
    // definition alive. If the refcount is 0 here, the frame was FREED while still
    // COW-mapped in this AS: the cross-process COW undercount. Catch it at the
    // instant of use, with the phys known, and dump its ref/unref trail naming the
    // unmatched op — rather than thousands of forks later when a child reads the
    // recycled page's poison (SIG1) or a reused page-table page faults reserved-bit
    // (the kernel #PF in VmmGetPte). One PmmGetRefCount call: low perturbation.
    uint32_t oldRc = PmmGetRefCount(oldPhys);
    if (oldRc == 0)
    {
        SerialPrintf("BRO176-COWUAF: COW source phys=0x%lx refcount=0 while "
                     "PTE present+COW va=0x%lx pid=%u — freed-while-mapped!\n",
                     oldPhys.raw(), pageAddr.raw(), (unsigned)pid);
        PmmDumpFreeLog(oldPhys.raw());
        // Fall through to the copy path (treat as shared) so we copy out of the
        // (poisoned) page and at least don't make a freed frame writable in place.
    }

    if (oldRc > 1 || oldRc == 0)
    {
        // Shared page: copy to a fresh page and drop our ref on the shared one.
        PhysicalAddress newPhys = PmmAllocPage(MemTag::User, pid);
        if (!newPhys)
        {
            IrqSpinLockRelease(&g_userPtLock, ptFlags);
            return {VmmCowResult::OutOfMemory, {}};
        }

        // BRO-179: pin the source frame across the copy. We hold g_userPtLock,
        // which serializes against concurrent COW faults, fork's COW-share, and
        // munmap/mprotect — but NOT against the teardown unref path
        // (FreeTableLevel->PmmUnrefPage in ProcessDestroy), which takes only
        // g_pmmLock. Without an explicit pin, a concurrent unref could drop
        // oldPhys to 0, free it, and let it be recycled (e.g. to the kernel heap,
        // which writes 0xDFDFDFDF kfree-poison) WHILE the memcpy below reads it —
        // the cross-domain frame reuse that made a user process read heap poison
        // (SIG1). PmmRefPageIfAlive bumps the refcount only if the frame is still
        // live, atomically under g_pmmLock, closing the TOCTOU between the
        // PTE-present check above and the copy.
        bool pinned = (oldRc != 0) && PmmRefPageIfAlive(oldPhys);

        auto* dst = reinterpret_cast<uint64_t*>(PhysToVirt(newPhys).raw());
        if (pinned)
        {
            // Source is pinned-alive: safe to copy its contents.
            auto* src = reinterpret_cast<const uint64_t*>(PhysToVirt(oldPhys).raw());
            for (uint64_t b = 0; b < PAGE_SIZE / sizeof(uint64_t); b++)
                dst[b] = src[b];
            PmmUnrefPage(oldPhys);  // drop the pin
        }
        else
        {
            // Source is already free (oldRc==0, or freed between the PTE check
            // and the pin): its bytes are untrustworthy — it may have been
            // recycled and poisoned. Do NOT propagate poison into the child;
            // give it a zeroed page. This is the freed-while-COW-mapped bug; the
            // oldRc==0 detector above logs it.
            ZeroPage(newPhys);
        }

        uint64_t newPte = (newPhys.raw() & PHYS_MASK)
                        | ((*pte) & ~(PHYS_MASK | PTE_COW_BIT))
                        | VMM_WRITABLE;
        newPte = (newPte & ~PTE_PID_MASK)
               | (((uint64_t)pid & 0x3FF) << PTE_PID_SHIFT);
        *pte = newPte;

        // BRO-176 map accounting: this PTE was repointed from the shared frame
        // (oldPhys) to a fresh private frame (newPhys). Drop the mapcount on the
        // old frame and bump it on the new one, in lockstep with the ref change.
        // A COW PTE is always a USER mapping, so oldPhys is a user frame.
        if (oldPhys)
            PmmMapDec(oldPhys);
        PmmMapInc(newPhys);

        // Drop THIS owner's original reference on the shared source (the COW PTE
        // we just repointed). Separate from the copy pin above. (When oldRc==0
        // there is no reference of ours to drop.)
        if (oldRc != 0)
            retired = oldPhys; // Transfer the PTE reference through shootdown.
    }
    else
    {
        // Sole owner: just make the existing page writable and clear COW.
        *pte = ((*pte) & ~PTE_COW_BIT) | VMM_WRITABLE;
    }

    IrqSpinLockRelease(&g_userPtLock, ptFlags);
    return {VmmCowResult::Resolved, retired};
}

void VmmForkCommitCowShare(uint64_t* parentLeaf, uint64_t* childLeaf,
                           uint64_t childPte, PhysicalAddress srcPhys)
{
    uint64_t ptFlags = IrqSpinLockAcquire(&g_userPtLock);
    // Downgrade parent to read-only+COW, install child's read-only+COW PTE, and
    // bump the shared refcount as one atomic step vs. the COW write-fault
    // handler, closing the window where the handler could see refcount==1 and
    // upgrade in place while fork is mid-share.
    *parentLeaf = (*parentLeaf & ~VMM_WRITABLE) | PTE_COW_BIT;
    *childLeaf  = childPte;
    PmmRefPage(srcPhys);
    IrqSpinLockRelease(&g_userPtLock, ptFlags);
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

    uint64_t ptFlags = IrqSpinLockAcquire(&g_kernelPtLock);
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
    IrqSpinLockRelease(&g_kernelPtLock, ptFlags);
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

static void FreeTableLevel(PhysicalAddress tablePhys, int level, bool preserveBorrowedDevices,
                           uint64_t base, VmmBorrowedLeafQuery query, void* context)
{
    uint64_t* table = reinterpret_cast<uint64_t*>(PhysToVirt(tablePhys).raw());
    if (level > 1)
    {
        for (uint64_t i = 0; i < 512; i++)
        {
            if (!(table[i] & VMM_PRESENT)) continue;
            if (table[i] & (1ULL << 7)) continue; // huge page, skip
            FreeTableLevel(PhysicalAddress(table[i] & PHYS_MASK), level - 1, preserveBorrowedDevices,
                           base + (i << (12 + 9 * (level - 1))), query, context);
        }
    }
    else
    {
        // Leaf level (PT): unref data pages (handles COW shared pages)
        for (uint64_t i = 0; i < 512; i++)
        {
            if (!VmmLeafBacked(table[i])) continue;
            PhysicalAddress pagePhys(table[i] & PHYS_MASK);
            // BRO-176 map accounting: this process's USER PTE for the frame is
            // being torn down — drop its mapcount in lockstep with the unref so
            // the freeing site sees mapCount==0 only when no PTE remains.
            if (table[i] & VMM_USER)
                PmmMapDec(pagePhys);
            const bool borrowed = query ? query(VirtualAddress(base + (i << 12)),table[i],context) :
                preserveBorrowedDevices && ((table[i] & PTE_TAG_MASK) >> PTE_TAG_SHIFT) ==
                    static_cast<uint64_t>(MemTag::Device);
            if (!borrowed) PmmUnrefPage(pagePhys);
        }
    }
    PmmFreePage(tablePhys);
}

void VmmDestroyUserPageTable(PageTable pt, bool preserveBorrowedDevices,
                             VmmBorrowedLeafQuery query, void* context)
{
    if (!pt) return;

    uint64_t* pml4 = reinterpret_cast<uint64_t*>(PhysToVirt(pt.pml4).raw());

    for (uint64_t i = 0; i < 256; i++)
    {
        if (!(pml4[i] & VMM_PRESENT)) continue;
        FreeTableLevel(PhysicalAddress(pml4[i] & PHYS_MASK), 3, preserveBorrowedDevices,
                       i << 39, query, context);
    }

    PmmFreePage(pt.pml4);
}

} // namespace brook
