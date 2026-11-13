#include "virtual_memory.h"
#include "physical_memory.h"
#include "pte_owner_sidecars.h"
#include "serial.h"
#include "spinlock.h"
#include "vmalloc_arena.h"
#include "early_framebuffer_mapping.h"
#include "boot_protocol/boot_protocol.h"
#include "cpu.h"
#include "acpi.h"
#include "dell_i2c_id_policy.h"
#include "intel_vtd_probe_policy.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_ggtt_mmio.h"
#include "intel_bdw_display_probe.h"
#include "intel_bdw_forcewake_ack.h"
#include "intel_bdw_forcewake_adapter.h"
#include "intel_bdw_scratch_mmio.h"
#include "intel_bdw_bcs_mmio.h"
#include "intel_bdw_rcs_reset_mmio.h"
#include "intel_bdw_render_gt_mmio.h"
#include "intel_bdw_render_backend.h"
#include "intel_bdw_csb.h"
#include "intel_bdw_pat.h"
#include "intel_bdw_live_backend.h"
#include "intel_bdw_publish.h"
#include "intel_bdw_scanout.h"
#if defined(BROOK_GPU_SUBMISSION_TIMING)
#include "intel_bdw_submission_timing.h"
#endif
#include "intel_bdw_wait_yield.h"
#include "scheduler.h"
#include "smp.h"

namespace brook {
template<class F> static bool BdwTimedTransaction(BdwRenderRunResult& result,unsigned stage,F f){
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const auto start=BdwTimingNow();
#endif
    const bool ok=f();
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const auto end=BdwTimingNow();if(end>start)result.transactionTicks[stage]=end-start;
#else
    (void)result;(void)stage;
#endif
    return ok;
}

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
// Registry keyed by user L1 frame; protected by g_userPtLock. Entries remain
// alive across empty slots and are retired with the containing L1 table.
static PteOwnerSidecars g_userOwnerSidecars;
#if defined(BROOK_HOST_TEST) && !defined(BROOK_STANDALONE_TEST)
static uint64_t g_hostOwnerExperimentRoot=0;
static bool HostOwnerExperiment(PageTable pt) {
    return pt && pt.pml4.raw()==__atomic_load_n(&g_hostOwnerExperimentRoot,__ATOMIC_ACQUIRE);
}
#else
static bool HostOwnerExperiment(PageTable) {return false;}
#endif

// The kernel's PML4 physical address, captured at init time.
static PhysicalAddress g_kernelCR3{};
// User PML4s share their upper half with the kernel. An explicit kernel CR3
// and a shared upper-half leaf must use the same lock as KernelPageTable.
// Use the PML4 index rather than a canonical-address comparison: the walker
// indexes bit 47 even for noncanonical values.
static IrqSpinLock& PageTableLock(PageTable pt,VirtualAddress address) {
    return !pt || pt.pml4==g_kernelCR3 || ((address.raw()>>47)&1)
        ? g_kernelPtLock : g_userPtLock;
}
#ifdef BROOK_HOST_TEST
void VmmHostSetKernelRoot(PageTable root) { g_kernelCR3 = root.pml4; }
bool VmmHostUsesKernelPtLock(PageTable root,VirtualAddress address) {return &PageTableLock(root,address)==&g_kernelPtLock;}
#ifndef BROOK_STANDALONE_TEST
bool VmmHostOwnerExperimentActive(PageTable root) {return HostOwnerExperiment(root);}
#endif
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

// Called while the appropriate page-table lock is held. Keeping the leaf
// pointer in this interface lets an extended owner be associated with its L1
// frame and slot; raw-only callers must not reconstruct extended owners.
static uint64_t OwnerTable(const uint64_t* entry) {
    return VirtToPhys(VirtualAddress(reinterpret_cast<uint64_t>(entry)&~uint64_t(4095))).raw();
}
static unsigned OwnerSlot(const uint64_t* entry) {return unsigned((reinterpret_cast<uint64_t>(entry)&4095)/8);}
#if defined(BROOK_HOST_TEST) && !defined(BROOK_STANDALONE_TEST)
bool VmmHostOwnerSlotPresent(PageTable pt,VirtualAddress address) {
    if(!HostOwnerExperiment(pt) || address.raw()>=0x0000800000000000ULL)return false;
    const auto irq=IrqSpinLockAcquire(&g_userPtLock);
    const auto* entry=WalkToPtr(pt,address,false);uint16_t owner=0;
    const bool present=entry && g_userOwnerSidecars.Read(OwnerTable(entry),OwnerSlot(entry),owner);
    IrqSpinLockRelease(&g_userPtLock,irq);return present;
}
#endif
static VmmOwnedLeaf CaptureOwnedLeaf(const uint64_t* entry,uint64_t raw,bool extended=false) {
    auto owner=static_cast<uint16_t>((raw&PTE_PID_MASK)>>PTE_PID_SHIFT);
    if(extended && VmmLeafBacked(raw) && owner==PteOwnerSidecars::Escape &&
       !g_userOwnerSidecars.Read(OwnerTable(entry),OwnerSlot(entry),owner))__builtin_trap();
    return {raw,owner};
}
// Caller holds g_userPtLock. Empty sidecars survive failed publication and
// are reclaimed with their leaf table, so rollback never allocates.
static bool PrepareOwnerSidecarLocked(uint64_t* entry) {
    const auto table=OwnerTable(entry);
    if(g_userOwnerSidecars.Find(table))return true;
    const auto physical=PmmAllocPage(MemTag::PageTable,KernelPid);if(!physical)return false;
    auto* page=reinterpret_cast<PteOwnerSidecars::Page*>(PhysToVirt(physical).raw());
    const bool installed=g_userOwnerSidecars.Install(table,page);
    if(!installed)PmmFreePage(physical);
    return installed;
}
static void ClearExtendedOwner(uint64_t* entry,VmmOwnedLeaf owned,bool extended) {
    if(extended && owned.owner>=PteOwnerSidecars::Escape)
        g_userOwnerSidecars.Clear(OwnerTable(entry),OwnerSlot(entry));
}
static uint64_t WithInlineOwner(uint64_t raw,uint16_t owner) {
    if(owner>=PTE_PID_CAPACITY)__builtin_trap();
    return (raw&~PTE_PID_MASK)|(uint64_t(owner)<<PTE_PID_SHIFT);
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

static bool g_framebufferWc=false;
static EarlyFramebufferRange g_framebufferWcRange;
bool VmmFramebufferWriteCombining() { return __atomic_load_n(&g_framebufferWc,__ATOMIC_ACQUIRE); }
bool VmmFramebufferCacheFlags(uint64_t physical,uint64_t bytes,uint64_t& flags) {
    return FramebufferAliasCacheFlags(g_framebufferWcRange,VmmFramebufferWriteCombining(),physical,bytes,flags);
}
uint64_t VmmFramebufferMapping(uint64_t physical,uint64_t bytes) {
    constexpr uint64_t limit=16ull<<30;
    return bytes && physical<limit && bytes<=limit-physical ? DIRECT_MAP_BASE+physical : 0;
}

void VmmInitFramebufferMemory(const BootProtocol* boot)
{
    if(!boot) return;
    const auto& fb=boot->framebuffer;
    EarlyFramebufferRange range;
    if(!EarlyFramebufferBounds(fb.physicalBase,fb.width,fb.height,fb.stride,range)) {
        SerialPuts("FBMEM: unchanged (unsupported framebuffer bounds)\n");return;
    }
    // The GOP framebuffer is an MMIO aperture, never live allocator/kernel RAM.
    // A missing UEFI descriptor for a PCI hole is allowed; described RAM is not.
    for(uint32_t i=0;i<boot->memoryMapCount;++i) {
        const auto& d=boot->memoryMap[i];
        if(d.pageCount>(~uint64_t{0}-d.physicalStart)/4096) {
            SerialPuts("FBMEM: unchanged (invalid memory map)\n");return;
        }
        const uint64_t end=d.physicalStart+d.pageCount*4096;
        if(d.physicalStart>=range.end || end<=range.begin) continue;
        if(d.type!=MemoryType::Mmio && d.type!=MemoryType::Reserved && d.type!=MemoryType::Framebuffer) {
            SerialPuts("FBMEM: unchanged (framebuffer overlaps described RAM)\n");return;
        }
    }
    if(!CpuWriteCombiningReady() || !CpuPrepareFramebufferWc(range.begin,range.end)) {
        SerialPuts("FBMEM: unchanged (Intel PAT/MTRR WC unavailable)\n");return;
    }
    struct Memory {
        uint64_t flags=0,cr0=0,cr4=0;
        uint64_t* table(uint64_t p) { return reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(p)).raw()); }
        uint64_t allocate() { return AllocTablePage().raw(); }
        void release(uint64_t p) { PmmFreePage(PhysicalAddress(p)); }
        void flushTlb() {
            if(cr4&(1ull<<7)) __asm__ volatile("mov %0,%%cr4"::"r"(cr4&~(1ull<<7)):"memory");
            uint64_t cr3;__asm__ volatile("mov %%cr3,%0; mov %0,%%cr3":"=r"(cr3)::"memory");
            if(cr4&(1ull<<7)) __asm__ volatile("mov %0,%%cr4"::"r"(cr4):"memory");
        }
        void beginTransition() {
            __asm__ volatile("pushfq; pop %0; cli":"=r"(flags)::"memory");
            __asm__ volatile("mov %%cr0,%0":"=r"(cr0));
            __asm__ volatile("mov %%cr4,%0":"=r"(cr4));
            const uint64_t disabled=(cr0|(1ull<<30))&~(1ull<<29);
            __asm__ volatile("mov %0,%%cr0; wbinvd"::"r"(disabled):"memory");
            flushTlb();
        }
        void endTransition() {
            __asm__ volatile("wbinvd":::"memory");flushTlb();
            __asm__ volatile("mov %0,%%cr0"::"r"(cr0):"memory");
            if(flags&(1ull<<9)) __asm__ volatile("sti":::"memory");
        }
    } memory;
    static EarlyFramebufferPlan plan;
    auto* root=memory.table(g_kernelCR3.raw());
    if(!MapEarlyFramebufferWc(memory,root,range,plan)) {
        SerialPrintf("FBMEM: unchanged (%s)\n",plan.failure);return;
    }
    g_framebufferWcRange=range;
    __atomic_store_n(&g_framebufferWc,true,__ATOMIC_RELEASE);
    SerialPrintf("FBMEM: effective=WC phys=0x%lx..0x%lx split-tables=%u aliases=identity+HHDM\n",
                 range.begin,range.end,plan.count);
}

bool VmmMapPage(PageTable pt, VirtualAddress virtAddr, PhysicalAddress physAddr,
                uint64_t flags, MemTag tag, uint16_t pid)
{
    if(HostOwnerExperiment(pt))return false;
    // Never silently alias a mapping owner if a caller exceeds the PTE format.
    if (pid >= PTE_PID_CAPACITY) return false;
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
    IrqSpinLock& ptLock = PageTableLock(pt,virtAddr);
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

    *pte = WithInlineOwner((physAddr.raw() & PHYS_MASK)
         | VMM_PRESENT
         | (flags & ~(VMM_PRESENT | PTE_TAG_MASK | PTE_COW_BIT | PTE_PID_MASK | VMM_NO_EXEC | PTE_HIDDEN_BIT))
         | (((uint64_t)(uint8_t)tag & 0x7) << PTE_TAG_SHIFT)
         | (flags & VMM_NO_EXEC),pid);
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
    const bool extended=HostOwnerExperiment(pt);
    if(extended) {
        if(pid>=5120 || !(flags&VMM_USER) || virtAddr.raw()>=0x0000800000000000ULL || (virtAddr.raw()&4095))
            return VmmMapResult::InvalidAddress;
    } else if (pid >= PTE_PID_CAPACITY) return VmmMapResult::InvalidAddress;
    if ((flags & (PTE_COW_BIT | VMM_WRITABLE)) == (PTE_COW_BIT | VMM_WRITABLE))
        return VmmMapResult::InvalidAddress;
    if (!(flags & VMM_FORCE_MAP) && virtAddr.raw() < VIRTUAL_NULL_GUARD)
        return VmmMapResult::InvalidAddress;
    flags &= ~VMM_FORCE_MAP;
    IrqSpinLock& lock = PageTableLock(pt,virtAddr);
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
    else if(extended && pid>=PteOwnerSidecars::Escape && !PrepareOwnerSidecarLocked(pte))result=VmmMapResult::OutOfMemory;
    else {
        if(extended && pid>=PteOwnerSidecars::Escape && !g_userOwnerSidecars.Set(OwnerTable(pte),OwnerSlot(pte),pid))__builtin_trap();
        const auto inlineOwner=extended && pid>=PteOwnerSidecars::Escape?PteOwnerSidecars::Escape:pid;
        *pte = WithInlineOwner((physAddr.raw() & PHYS_MASK) | (accessible ? VMM_PRESENT : PTE_HIDDEN_BIT)
             | (flags & ~(VMM_PRESENT | PTE_TAG_MASK | PTE_PID_MASK | VMM_NO_EXEC | PTE_HIDDEN_BIT))
             | (((uint64_t)(uint8_t)tag & 0x7) << PTE_TAG_SHIFT)
             | (flags & VMM_NO_EXEC),inlineOwner);
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
VmmOwnedLeaf VmmReadOwnedLeaf(PageTable pt, VirtualAddress address) {
    if(HostOwnerExperiment(pt) && address.raw()>=0x0000800000000000ULL)return {};
    IrqSpinLock& lock = PageTableLock(pt,address);
    uint64_t irq = IrqSpinLockAcquire(&lock);
    bool large = false;
    auto* entry = WalkToPtr(pt,address,false,0,&large);
    auto leaf = !large && entry ? CaptureOwnedLeaf(entry,*entry,HostOwnerExperiment(pt) && address.raw()<0x0000800000000000ULL) : VmmOwnedLeaf{};
    IrqSpinLockRelease(&lock,irq); return leaf;
}
uint64_t VmmReadLeaf(PageTable pt, VirtualAddress address) {return VmmReadOwnedLeaf(pt,address).raw;}

struct AuditedAliasTables {
    bool readEntry(uint64_t page,uint16_t index,uint64_t& value) {
        BootResourceClassification c{};
        constexpr uint32_t allowed=(1u<<uint32_t(MemoryType::Free))|(1u<<uint32_t(MemoryType::BootData));
        if(index>=512 || !AcpiClassifyBootRange(page,4096,c) || c.unreported || !c.types || (c.types&~allowed))return false;
        const auto* entries=reinterpret_cast<const uint64_t*>(PhysToVirt(PhysicalAddress(page)).raw());
        value=__atomic_load_n(entries+index,__ATOMIC_ACQUIRE);return true;
    }
};
bool VmmInspectKernelAliases(uint64_t physical,KernelAliasMapping& identity,KernelAliasMapping& direct) {
    identity={};direct={};
    if(!physical || (physical&4095) || physical>=(16ull<<30))return false;
    AuditedAliasTables tables;
    const uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);
    const bool ok=InspectKernelAlias(tables,g_kernelCR3.raw(),physical,identity) &&
        InspectKernelAlias(tables,g_kernelCR3.raw(),DIRECT_MAP_BASE+physical,direct);
    IrqSpinLockRelease(&g_kernelPtLock,irq);
    if(!ok){identity={};direct={};}return ok;
}
bool VmmInspectKernelAddress(uint64_t address,KernelAliasMapping& mapping) {
    mapping={};AuditedAliasTables tables;
    const uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);
    const bool ok=InspectKernelAlias(tables,g_kernelCR3.raw(),address,mapping);
    IrqSpinLockRelease(&g_kernelPtLock,irq);if(!ok)mapping={};return ok;
}
bool VmmDellI2cIdCriticalRead(uint32_t& value) {
    value=0;
    struct Io {
        AuditedAliasTables tables;
        bool evidence(DellI2cCriticalEvidence& e) {
            KernelAliasMapping identity{},direct{},current{};
            const uint64_t root=g_kernelCR3.raw(),active=ReadCR3()&PHYS_MASK;
            // The debug service is a kernel thread. Refuse other roots rather
            // than assume a process's copied kernel-half entry stays shared.
            e.aliasesValid=active==root && InspectKernelAlias(tables,root,DellI2c0Page,identity) &&
                InspectKernelAlias(tables,root,DIRECT_MAP_BASE+DellI2c0Page,direct) &&
                InspectKernelAlias(tables,active,DIRECT_MAP_BASE+DellI2c0Page,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==DellI2c0Page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==DellI2c0Page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==DellI2c0Page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(DellI2c0Page,DellI2c0PrivatePage,cpu);
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool readComponent(uint32_t& result) {
            // One fixed aligned volatile load through the existing direct map.
            __asm__ volatile("lfence":::"memory");
            result=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+DellI2c0Page+DellI2cComponentOffset);return true;
        }
    } io;
    const uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);
    const bool ok=DellI2cIdCritical(io,value);
    IrqSpinLockRelease(&g_kernelPtLock,irq);return ok;
}

bool VmmDellGpuVtdCriticalRead(DellGpuVtdStatus& status) {
    struct Io {
        AuditedAliasTables tables;
        bool evidence(DellI2cCriticalEvidence& e) {
            KernelAliasMapping identity{},direct{},current{};
            const uint64_t root=g_kernelCR3.raw(),active=ReadCR3()&PHYS_MASK;
            e.aliasesValid=active==root && InspectKernelAlias(tables,root,DellGpuVtdPage,identity) &&
                InspectKernelAlias(tables,root,DIRECT_MAP_BASE+DellGpuVtdPage,direct) &&
                InspectKernelAlias(tables,active,DIRECT_MAP_BASE+DellGpuVtdPage,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==DellGpuVtdPage;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==DellGpuVtdPage;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==DellGpuVtdPage;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(DellGpuVtdPage,DellGpuVtdPage,cpu);
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool readVersion(uint32_t& v) {
            __asm__ volatile("lfence":::"memory");
            v=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+DellGpuVtdPage);return true;
        }
        bool readStatus(uint32_t& v) {
            __asm__ volatile("lfence":::"memory");
            v=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+DellGpuVtdPage+0x1c);return true;
        }
        bool readCapability(uint64_t& v) {
            __asm__ volatile("lfence":::"memory");
            v=*reinterpret_cast<const volatile uint64_t*>(DIRECT_MAP_BASE+DellGpuVtdPage+0x08);return true;
        }
        bool readProtectedMemory(uint32_t& v) {
            __asm__ volatile("lfence":::"memory");
            v=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+DellGpuVtdPage+0x64);return true;
        }
    } io;
    const uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);
    const bool ok=DellGpuVtdCritical(io,status);
    IrqSpinLockRelease(&g_kernelPtLock,irq);return ok;
}

bool VmmDellDisplayCriticalRead(BdwDisplaySnapshot& result) {
    struct Io {
        AuditedAliasTables tables;
        bool evidence(uint64_t page,DellI2cCriticalEvidence& e) {
            KernelAliasMapping identity{},direct{},current{};
            const uint64_t root=g_kernelCR3.raw(),active=ReadCR3()&PHYS_MASK;
            e.aliasesValid=active==root && InspectKernelAlias(tables,root,page,identity) &&
                InspectKernelAlias(tables,root,DIRECT_MAP_BASE+page,direct) &&
                InspectKernelAlias(tables,active,DIRECT_MAP_BASE+page,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu);
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool read32(uint64_t address,uint32_t& value) {
            __asm__ volatile("lfence":::"memory");
            value=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+address);return true;
        }
    } io;
    const uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);
    const bool ok=BdwDisplayCriticalRead(io,result);
    IrqSpinLockRelease(&g_kernelPtLock,irq);return ok;
}

namespace {
struct DellGopGgttIo {
    AuditedAliasTables tables;
    bool evidence(uint64_t page,DellI2cCriticalEvidence& e) {
        KernelAliasMapping identity{},direct{},current{};
        const uint64_t root=g_kernelCR3.raw(),active=ReadCR3()&PHYS_MASK;
        e.aliasesValid=active==root && InspectKernelAlias(tables,root,page,identity) &&
            InspectKernelAlias(tables,root,DIRECT_MAP_BASE+page,direct) &&
            InspectKernelAlias(tables,active,DIRECT_MAP_BASE+page,current);
        e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
        e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
        e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
        CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu);
        e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
        return true;
    }
    bool read(uint64_t address,uint64_t& value) {
        __asm__ volatile("lfence":::"memory");
        value=*reinterpret_cast<const volatile uint64_t*>(DIRECT_MAP_BASE+address);return true;
    }
};
}
bool VmmDellGopPtesCriticalRead(const BdwGopPtePlan& plan,BdwGopPtes& result) {
    DellGopGgttIo io;
    const uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);
    const bool ok=BdwGopPtesCritical(plan,io,result);
    IrqSpinLockRelease(&g_kernelPtLock,irq);return ok;
}

bool VmmDellGopBackingPage(uint32_t first,uint32_t count,uint64_t& physicalBase) {
    DellGopGgttIo io;
    const uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);
    const bool ok=BdwGopBackingPageCritical(first,count,io,physicalBase);
    IrqSpinLockRelease(&g_kernelPtLock,irq);return ok;
}

bool VmmDellGgttSurveyCriticalRead(uint32_t first,BdwGgttSurvey& result) {
    struct Io {
        AuditedAliasTables tables;uint32_t first;
        bool evidence(uint64_t page,DellI2cCriticalEvidence& e) {
            KernelAliasMapping identity{},direct{},current{};
            const uint64_t root=g_kernelCR3.raw(),active=ReadCR3()&PHYS_MASK;
            e.aliasesValid=active==root && InspectKernelAlias(tables,root,page,identity) &&
                InspectKernelAlias(tables,root,DIRECT_MAP_BASE+page,direct) &&
                InspectKernelAlias(tables,active,DIRECT_MAP_BASE+page,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu);
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool read(uint64_t address,uint64_t& value) {
            const uint64_t begin=0xf6800000ull+uint64_t(first)*8;
            if(address<begin || ((address-begin)&7) || (address-begin)/8>=64)return false;
            __asm__ volatile("lfence":::"memory");
            value=*reinterpret_cast<const volatile uint64_t*>(DIRECT_MAP_BASE+address);
            __asm__ volatile("lfence":::"memory");return true;
        }
    } io{{},first};
    const uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);
    const bool ok=BdwGgttSurveyCritical(first,io,result);
    IrqSpinLockRelease(&g_kernelPtLock,irq);return ok;
}

bool VmmDellReservedGgttCriticalRead(uint32_t first,BdwGgttReservedPtes& result) {
    struct Io {
        AuditedAliasTables tables;
        // This diagnostic authorizes reads only. It cannot validate a lease
        // or grant device ownership to a binding transaction.
        struct ReadOnlyOwner {
            bool readable(){return true;}
            bool owned(){return false;}
            bool quiescent(){return false;}
            bool invalidateGpuTranslations(){return false;}
            bool validateLease(const BdwGgttLease&){return false;}
            bool validateTakeover(const BdwGgttLease&,uint64_t){return false;}
        } owner;
        BdwGgttMmio<ReadOnlyOwner> mmio;
        uint32_t first;
        explicit Io(uint32_t firstIndex):mmio(owner,
            reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6800000ull+uint64_t(firstIndex/512)*4096),
            reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6101008ull),firstIndex,8),first(firstIndex){}
        bool evidence(uint64_t page,DellI2cCriticalEvidence& e) {
            KernelAliasMapping identity{},direct{},current{};
            const uint64_t root=g_kernelCR3.raw(),active=ReadCR3()&PHYS_MASK;
            e.aliasesValid=active==root && InspectKernelAlias(tables,root,page,identity) &&
                InspectKernelAlias(tables,root,DIRECT_MAP_BASE+page,direct) &&
                InspectKernelAlias(tables,active,DIRECT_MAP_BASE+page,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu);
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool read(uint64_t address,uint64_t& value) {
            const uint64_t begin=0xf6800000ull+uint64_t(first)*8;
            if(address<begin || ((address-begin)&7) || (address-begin)/8>=8)return false;
            return mmio.read(first+uint32_t((address-begin)/8),value);
        }
    } io(first);
    const uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);
    const bool ok=BdwReservedGgttPtesCritical(first,io,result);
    IrqSpinLockRelease(&g_kernelPtLock,irq);return ok;
}

bool VmmDellForcewakeAckCriticalRead(uint32_t& result) {
    struct Io {
        AuditedAliasTables tables;
        bool evidence(DellI2cCriticalEvidence& e) {
            constexpr uint64_t page=DellForcewakeAckPage;
            KernelAliasMapping identity{},direct{},current{};
            const uint64_t root=g_kernelCR3.raw(),active=ReadCR3()&PHYS_MASK;
            e.aliasesValid=active==root && InspectKernelAlias(tables,root,page,identity) &&
                InspectKernelAlias(tables,root,DIRECT_MAP_BASE+page,direct) &&
                InspectKernelAlias(tables,active,DIRECT_MAP_BASE+page,current);
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu) && cpu.cpu==0;
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool read(uint32_t& value) {
            __asm__ volatile("lfence":::"memory");
            value=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+DellForcewakeAckPage+0x44);return true;
        }
    } io;
    const uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);
    const bool ok=BdwForcewakeAckCritical(io,result);
    IrqSpinLockRelease(&g_kernelPtLock,irq);return ok;
}

// Serialize all users for this boot. A completed clear/ACK permits another
// bounded observation; an uncertain release permanently blocks further use.
// The page-table lock prevents mapping changes throughout each operation.
static void VmmDellForcewakeCriticalImpl(uint32_t khz,BdwForcewakeAdapterResult& result,bool otherEngines,bool patRequested,bool tlbRequested,bool resetRequested,bool scopeCheck,
                                        uint32_t scratchIndex,uint64_t scratchDma,BdwScratchTakeoverResult* scratchResult,uint8_t* scratchCpu,BdwBcsInitResult* initResult,bool clearScratch,BdwPatTestResult* patResult,uint64_t copyToken,BdwBcsRunResult* copyResult,BdwRcsResetTestResult* rcsReset=nullptr,BdwRenderGtInitResult* gtInit=nullptr,BdwRenderRunResult* renderResult=nullptr,uint32_t (*snapshot)[20*1024]=nullptr,bool scanoutCopy=false) {
    static BdwForcewakeClaim claim;result={};
    if(!claim.acquire())return;
    struct Io {
        AuditedAliasTables tables;bool tlbRequested,resetRequested,scratchRequested,initRequested,patChangeRequested,copyRequested,rcsResetRequested,gtInitRequested,renderRequested;
        uint64_t* irq=nullptr;bool mappingsValid=true;
        bool owned(){return mappingsValid;}
        bool evidence(uint64_t page,DellI2cCriticalEvidence& e){
            KernelAliasMapping identity{},direct{},current{};
            const uint64_t root=g_kernelCR3.raw(),active=ReadCR3()&PHYS_MASK;
            e.aliasesValid=active==root && InspectKernelAlias(tables,root,page,identity) &&
                InspectKernelAlias(tables,root,DIRECT_MAP_BASE+page,direct) &&
                InspectKernelAlias(tables,active,DIRECT_MAP_BASE+page,current);
            if(page==BdwForcewakePages[0] && !current.writable)e.aliasesValid=false;
            if(tlbRequested && page==BdwPrivatePatPage && !current.writable)e.aliasesValid=false;
            if(patChangeRequested && (page==BdwPrivatePatPage || page==BdwResetPage || page==BdwForcewakePages[3]) && !current.writable)e.aliasesValid=false;
            if(copyRequested && (page==BdwPrivatePatPage || page==BdwResetPage || page==BdwForcewakePages[3] ||
               page==0xf6880000ull || page==0xf6101000ull) && !current.writable)e.aliasesValid=false;
            if(resetRequested && (page==BdwResetPage || page==BdwForcewakePages[3]) && !current.writable)e.aliasesValid=false;
            if(scratchRequested && (page==BdwPrivatePatPage || page==0xf6880000ull || page==0xf6101000ull ||
               page==BdwResetPage || page==BdwForcewakePages[3]) && !current.writable)e.aliasesValid=false;
            if(initRequested && (page==BdwResetPage || page==BdwForcewakePages[3]) && !current.writable)e.aliasesValid=false;
            if(rcsResetRequested && (page==BdwResetPage || page==BdwOtherEnginePages[0]) && !current.writable)e.aliasesValid=false;
            if(gtInitRequested && (page==BdwOtherEnginePages[0] || page==BdwPrivatePatPage || page==BdwResetPage || page==BdwDellBar0+0xb000) && !current.writable)e.aliasesValid=false;
            if(renderRequested && (page==BdwOtherEnginePages[0] || page==BdwPrivatePatPage || page==BdwResetPage || page==BdwDellBar0+0xb000 || page==0xf6880000ull || page==0xf6101000ull) && !current.writable)e.aliasesValid=false;
            e.identityPresent=identity.present;e.identityMatches=identity.physical==page;e.identityPat=identity.patIndex;
            e.directPresent=direct.present;e.directMatches=direct.physical==page;e.directPat=direct.patIndex;
            e.currentDirectPresent=current.present;e.currentDirectMatches=current.physical==page;e.currentDirectPat=current.patIndex;
            CpuMemoryTypeObservation cpu{};e.cpuValid=CpuObserveMemoryTypes(page,page,cpu) && cpu.cpu==0;
            e.cacheDisabled=cpu.cacheDisabled;e.notWriteThrough=cpu.notWriteThrough;e.patSlot0=cpu.patSlot0;e.mtrrType=cpu.page0.type;
            return true;
        }
        bool read32(uint64_t address,uint32_t& value){
            if(!mappingsValid)return false;
            __asm__ volatile("lfence":::"memory");
            value=*reinterpret_cast<const volatile uint32_t*>(DIRECT_MAP_BASE+address);
            __asm__ volatile("lfence":::"memory");return true;
        }
        bool write32(uint64_t address,uint32_t value){
            if(!mappingsValid)return false;
            *reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+address)=value;
            __asm__ volatile("mfence":::"memory");return true;
        }
        bool tsc(uint64_t& value){uint32_t lo,hi,aux;
            __asm__ volatile("rdtscp; lfence":"=a"(lo),"=d"(hi),"=c"(aux)::"memory");
            value=(uint64_t(hi)<<32)|lo;return true;}
        void relax(){__asm__ volatile("pause");}
        bool MayYield(){return mappingsValid && irq && (*irq&0x200) && SmpCurrentCpuIndex()==0;}
        void Unlock(){IrqSpinLockRelease(&g_kernelPtLock,*irq);}
        void Wait(){SchedulerSleepMs(1);}
        void Lock(){*irq=IrqSpinLockAcquire(&g_kernelPtLock);}
        bool Audit(){
            if(SmpCurrentCpuIndex()!=0)return false;
            for(auto page:BdwForcewakePages){DellI2cCriticalEvidence e{};if(!evidence(page,e) || !BdwGttEvidenceValid(e))return false;}
            for(auto page:BdwOtherEnginePages){DellI2cCriticalEvidence e{};if(!evidence(page,e) || !BdwGttEvidenceValid(e))return false;}
            constexpr uint64_t extra[]={BdwDellBar0,BdwPrivatePatPage,BdwResetPage,BdwDellBar0+0xb000,0xf6880000ull,0xf6101000ull};
            for(auto page:extra){DellI2cCriticalEvidence e{};if(!evidence(page,e) || !BdwGttEvidenceValid(e))return false;}
            return true;
        }
        void Poison(){mappingsValid=false;}
    } io{{},tlbRequested,resetRequested,scratchResult!=nullptr,initResult!=nullptr,patResult!=nullptr,copyResult!=nullptr,rcsReset!=nullptr,gtInit!=nullptr,renderResult!=nullptr};
    uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);io.irq=&irq;
    // Read-only exercise of the callback boundary. The callback remains under
    // the same page-table lock and forcewake claim as the initial samples.
    auto work=[&](Io& held,const BdwForcewakeResult& initial){
        uint32_t ack=0,lo=0,hi=0;
        if(!(held.read32(BdwDellBar0+0x130044,ack) && ack!=0xffffffffu && (ack&1) &&
            held.read32(BdwDellBar0+0x40e0,lo) && held.read32(BdwDellBar0+0x40e4,hi) &&
            lo==initial.patLo && hi==initial.patHi))return false;
        struct GtOwner {
                Io& io;uint32_t khz;uint32_t selector=0,fuse=0;bool sampled=false;
                bool owned(){uint32_t a=0;return io.owned() && io.read32(BdwDellBar0+0x130044,a) && a!=0xffffffffu && (a&1);}
                bool allEnginesQuiescent(){return owned();} // sampled immediately before callback
                // Exact display state and PCI identity are checked by the
                // caller; like the existing copy path, firmware/SMM must not
                // concurrently touch the GT while Brook holds the claim.
                bool displayAndFirmwareSafe(){return owned();}
                bool mcrSteeringReady(){return owned();}
                bool readMcrTopology(uint32_t& s,uint32_t& f){
                    if(!owned() || !io.read32(BdwDellBar0+0xfdc,s) || !io.read32(BdwDellBar0+0x9120,f) || !owned())return false;
                    selector=s;fuse=f;sampled=true;return true;
                }
                bool readMcr(uint32_t r,uint32_t& v){return r==0xb100 && owned() && io.read32(BdwDellBar0+r,v) && owned();}
                bool writeMcrBroadcast(uint32_t r,uint32_t v){return r==0xb100 && owned() && io.write32(BdwDellBar0+r,v) && owned();}
                bool delayAtLeastOneMicrosecond(){
                    uint64_t start=0,now=0;if(!owned() || !khz || !io.tsc(start))return false;
                    const uint64_t ticks=(uint64_t(khz)+999)/1000;
                    for(unsigned i=0;i<1000000;++i){if(!owned() || !io.tsc(now) || now<start)return false;
                        if(now-start>=ticks)return true;io.relax();}return false;
                }
        };
        if(renderResult) {
            if(!snapshot || (scratchIndex!=65544 && scratchIndex!=65573) || !scratchDma || !copyToken)return false;
            const bool rebased=scratchIndex==65573;
            constexpr uint64_t pages[]={BdwDellBar0,BdwOtherEnginePages[0],BdwPrivatePatPage,BdwResetPage,
                BdwDellBar0+0xb000,0xf6880000ull,0xf6101000ull};
            for(auto page:pages){DellI2cCriticalEvidence e{};if(!held.evidence(page,e) || !BdwGttEvidenceValid(e))return false;}
            GtOwner gt{held,khz};
            struct RenderOwner {
                Io& io;GtOwner& gt;BdwRenderRunResult& result;uint32_t khz,line;uint64_t origin;uint32_t budget=4000000;
                bool patActive=false,gtReady=false,fault=false;
                bool firstCompletionPoll=true,firstRetirementPoll=true;
                bool owned(){return gt.owned();}
                bool quiescent(){
                    struct Reader {RenderOwner& o;bool owned(){return o.owned();}
                        bool read(uint32_t r,uint32_t& v){return o.io.read32(BdwDellBar0+r,v);}} reader{*this};
                    return owned() && BdwObserveEngine(reader,0x2000).submissionQuiescent;
                }
                bool allEnginesQuiescent(){return quiescent();} // others held idle under claim
                bool monotonicMs(uint64_t& ms){uint64_t t=0;if(!io.tsc(t) || t<origin)return false;ms=(t-origin)/khz;return true;}
                bool inTime(){uint64_t ms=0;return budget && --budget && monotonicMs(ms) && ms<2000;}
                bool pollContinue(){
                    if(!owned() || !inTime())return false;
                    if(result.pollYieldRequested){
                        if(!BdwYieldGpuWait(io)){result.pollYieldFailed=true;fault=true;return false;}
                        ++result.pollYields;
                    }
                    return owned() && inTime();
                }
                bool immediatePollAllowed(){return owned() && inTime();}
#if defined(BROOK_GPU_SUBMISSION_TIMING)
                void sampleRenderFrequency(){
                    if(!result.sampleFrequency || result.frequencySamples==result.FrequencySampleLimit)return;
                    uint64_t now=0;
                    if(!io.tsc(now))return;
                    const auto count=result.frequencySamples;
                    if(count && now-result.frequency[count-1].ticks<uint64_t{khz}*5)return;
                    // The forcewake claim already audits the 0xa000 MMIO page.
                    uint32_t status=~uint32_t{0};
                    if(!io.read32(BdwDellBar0+0xa01c,status))return; // GEN6_RPSTAT1
                    result.frequency[count]={now,status};
                    ++result.frequencySamples;
                    if(!count){
                        io.read32(BdwDellBar0+0xa008,result.frequencyRequest); // GEN6_RPNSWREQ
                        io.read32(BdwDellBar0+0xa024,result.rpsControl); // GEN6_RP_CONTROL
                        io.read32(BdwDellBar0+0xa090,result.rc6Control); // GEN6_RC_CONTROL
                    }
                }
#endif
                bool completionContinue(){
                    if(!BdwFirstGpuPoll(*this,firstCompletionPoll))return false;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
                    sampleRenderFrequency();
#endif
                    return true;
                }
                bool retirementContinue(){return BdwFirstGpuPoll(*this,firstRetirementPoll);}
                void relax(){io.relax();}
                bool renderStateReady(){return owned() && gtReady;}
                bool restoreRenderState(){
                    BdwRenderGtMmio<GtOwner> mmio(gt,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[0]),
                        reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage),reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
                    const auto r=BdwInitializeRenderGt(mmio);gtReady=r.status==BdwRenderGtInitStatus::Applied;
                    if(r.status==BdwRenderGtInitStatus::Uncertain)fault=true;
                    return gtReady;
                }
                bool gpuPatLease(){return patActive && owned();}
                bool preparePrivatePat(uint64_t descriptor){return gpuPatLease() && (descriptor&0x119)==0x119;}
                bool readPrivatePat(uint32_t& l,uint32_t& h){return owned() && io.read32(BdwDellBar0+0x40e0,l) && io.read32(BdwDellBar0+0x40e4,h);}
                bool safePatTransition(uint32_t l,uint32_t h,uint32_t nl,uint32_t nh){
                    return owned() && l==0x03030303u && h==0x03030303u && nl==0x03030307u && nh==0x0303030bu;
                }
                bool drainAndInvalidate(){
                    uint32_t l=0,h=0;if(!readPrivatePat(l,h) || l==0xffffffffu || h==0xffffffffu)return false;
                    BdwGen8TlbMmio<RenderOwner> mmio(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage));
                    BdwGen8TlbInvalidate invalidate;const bool ok=invalidate.Run(mmio);
                    if(invalidate.Uncertain())fault=true;return ok;
                }
                bool invalidateGpuTranslations(){return drainAndInvalidate();}
                bool resetRcs(BdwBcsResetIntent intent=BdwBcsResetIntent::IdleTakeover){
                    BdwRcsResetMmio<RenderOwner> reset(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[0]),
                        reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
                    const auto r=reset.reset(intent);gtReady=false;
                    if(r==BdwBcsResetResult::Uncertain)fault=true;
                    return r==BdwBcsResetResult::DomainAcknowledged;
                }
                bool admitPages(BdwOwnedPage* p,unsigned count){
                    if(!owned() || !p || count!=37)return false;
                    for(unsigned i=0;i<count;++i){const auto dma=p[i].dma;
                        if(!dma || (dma&4095) || dma>=(uint64_t{16}<<30) || p[i].cpu!=reinterpret_cast<uint8_t*>(DIRECT_MAP_BASE+dma))return false;
                        for(unsigned j=0;j<i;++j)if(dma==p[j].dma)return false;
                    }return true;
                }
                // PMM ownership and VT-d checks are performed by the caller
                // before taking the page-table lock; recheck direct-map
                // geometry here while the audited device lease is held.
                bool admitExtraTables(BdwOwnedPage* p,unsigned count){
                    if(!owned() || !p || !count || count>128)return false;
                    for(unsigned i=0;i<count;++i){const auto dma=p[i].dma;
                        if(!dma || (dma&4095) || dma>=(uint64_t{16}<<30) || p[i].cpu!=reinterpret_cast<uint8_t*>(DIRECT_MAP_BASE+dma))return false;
                    }return true;
                }
                bool flushPage(const BdwOwnedPage& p){
                    if(!owned() || !p.cpu || !line || line>4096 || 4096%line)return false;
                    __asm__ volatile("mfence":::"memory");
                    for(uint32_t off=0;off<4096;off+=line)__asm__ volatile("clflush (%0)"::"r"(p.cpu+off):"memory");
                    __asm__ volatile("mfence":::"memory");return owned();
                }
                bool syncForCpu(const BdwOwnedPage& p){return flushPage(p);}
                bool syncForDevice(const BdwOwnedPage& p){return flushPage(p);}
                bool publish(BdwOwnedPage* p,unsigned count){if(!owned() || !p || count!=37)return false;
                    for(unsigned i=0;i<count;++i)if(!flushPage(p[i]))return false;return true;}
                bool publishExtraTables(BdwOwnedPage* p,unsigned count){if(!owned() || !p || !count || count>128)return false;
                    for(unsigned i=0;i<count;++i)if(!flushPage(p[i]))return false;return true;}
                bool validateLease(const BdwGgttLease&){return owned();}
                bool validateTakeover(const BdwGgttLease&,uint64_t){return owned();}
                bool acknowledgeCsb(unsigned slot){uint32_t posted=0;return slot<6 && owned() &&
                    io.write32(BdwDellBar0+0x23a0,0x07000000u|(slot<<8)) && io.read32(BdwDellBar0+0x23a0,posted) &&
                    posted!=0xffffffffu && ((posted>>8)&7u)==slot && owned();}
            } owner{held,gt,*renderResult,khz,0,0};
            uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
            owner.line=((b>>8)&255)*8;
            if(!(d&(1u<<19)) || owner.line<16 || owner.line>4096 || (owner.line&(owner.line-1)) ||
               4096%owner.line || !held.tsc(owner.origin))return false;
            BdwRenderDeviceBackend<RenderOwner> backend(owner,reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6880000ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6101008ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[0]),scratchIndex,copyToken,scratchDma,rebased?2:1);
            struct Transaction {
                RenderOwner& o;BdwRenderDeviceBackend<RenderOwner>& backend;BdwRenderRunResult& result;
                uint64_t scratch;uint32_t (&snapshot)[20*1024];bool rebased;BdwPatLease lease;
                BdwPatMmio<RenderOwner> pat;
                Transaction(RenderOwner& owner,BdwRenderDeviceBackend<RenderOwner>& b,BdwRenderRunResult& r,uint64_t s,uint32_t (&snap)[20*1024],bool second)
                    :o(owner),backend(b),result(r),scratch(s),snapshot(snap),rebased(second),pat(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage)){}
                bool initializeGt(){return BdwTimedTransaction(result,0,[&](){return o.restoreRenderState();});}
                bool establishPat(){return BdwTimedTransaction(result,1,[&](){const bool ok=lease.Establish(pat,0x119);o.patActive=ok;
                    if(lease.State()==BdwPatLeaseState::Quarantined)o.fault=true;return ok;});}
#if defined(BROOK_BDW_RPS_TRIAL)
                bool restoreFrequency=false;
                uint32_t savedFrequency=0;
                void requestTrialFrequency(){
                    constexpr uint32_t requestReg=0xa008; // GEN6_RPNSWREQ
                    constexpr uint32_t trialBin=10; // 500 MHz, in 50 MHz bins
                    constexpr uint32_t ratioMask=0x7f000000u;
                    if(!result.tryFrequency || !o.owned())return;
                    DellI2cCriticalEvidence evidence{};
                    if(!o.io.evidence(BdwRenderFrequencyCapsPage,evidence) || !BdwGttEvidenceValid(evidence))return;
                    if(!o.io.read32(BdwDellBar0+BdwRenderFrequencyCaps,result.trialFrequencyCaps))return;
                    const uint32_t caps=result.trialFrequencyCaps;
                    const uint32_t minBin=(caps>>16)&0xff;
                    const uint32_t maxBin=caps&0xff;
                    uint32_t control=0;
                    if(caps==0xffffffffu || !minBin || minBin>trialBin || maxBin<trialBin ||
                       !o.io.read32(BdwDellBar0+0xa024,control) || control==0xffffffffu || !(control&(1u<<7)) ||
                       !o.io.read32(BdwDellBar0+requestReg,savedFrequency) || savedFrequency==0xffffffffu ||
                       ((savedFrequency&ratioMask)>>24)<minBin || ((savedFrequency&ratioMask)>>24)>=trialBin)return;
                    const auto desired=(savedFrequency&~ratioMask)|(trialBin<<24);
                    // Restore the prior request even if the posting read does
                    // not confirm the trial write.
                    restoreFrequency=true;
                    result.trialFrequencyOriginal=savedFrequency;
                    o.io.write32(BdwDellBar0+requestReg,desired);
                    o.io.read32(BdwDellBar0+requestReg,result.trialFrequencyPosted);
                }
                bool restoreTrialFrequency(){
                    if(!restoreFrequency)return true;
                    uint32_t posted=0;
                    return o.owned() && o.io.write32(BdwDellBar0+0xa008,savedFrequency) &&
                        o.io.read32(BdwDellBar0+0xa008,posted) && posted==savedFrequency;
                }
#endif
                bool run(){return BdwTimedTransaction(result,2,[&](){
#if defined(BROOK_BDW_RPS_TRIAL)
                    requestTrialFrequency();
#endif
                    const bool ok=rebased?BdwRunRebasedRender(backend,scratch,result):BdwRunRetainedRender(backend,scratch,result);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
                    const auto retired=BdwTimingNow();uint64_t issued=0,breadcrumb=0;backend.timingTicks(issued,breadcrumb);
                    if(ok && issued && breadcrumb>issued && retired>breadcrumb){
                        result.issueTicks=issued;
                        result.breadcrumbTicks=breadcrumb;
                        result.contextRetiredTicks=retired;
                    }
#endif
                    result.engineSampled=o.owned() && o.io.read32(BdwDellBar0+0x2034,result.head) &&
                        o.io.read32(BdwDellBar0+0x2030,result.tail) && o.io.read32(BdwDellBar0+0x209c,result.mode) &&
                        o.io.read32(BdwDellBar0+0x203c,result.control) && o.io.read32(BdwDellBar0+0x2074,result.acthd) &&
                        o.io.read32(BdwDellBar0+0x2064,result.ipeir) && o.io.read32(BdwDellBar0+0x2068,result.ipehr) &&
                        o.io.read32(BdwDellBar0+0x2234,result.execlist);
                    return ok;});}
                bool saveContext(){return BdwTimedTransaction(result,3,[&](){return rebased?BdwCopyRebasedRenderContext(backend,snapshot):BdwCopyRetainedRenderContext(backend,snapshot);});}
                bool resetAfter(){return BdwTimedTransaction(result,4,[&](){
                    if(!o.resetRcs(BdwBcsResetIntent::StopOwnedWork))return false;
                    if(result.ggttClearRequested && result.contextComplete){
                        result.ggttCleared=BdwRetireRetainedRenderAfterReset(backend);
                        if(!result.ggttCleared){o.fault=true;return false;}
                    }
                    return true;
                });}
                bool restorePat(){return BdwTimedTransaction(result,5,[&](){const bool ok=lease.Release(pat);o.patActive=false;return ok;});}
                bool uncertain(){return o.fault;}
            } transaction(owner,backend,*renderResult,scratchDma,*snapshot,rebased);
            renderResult->transaction=BdwRunOwnedRenderTransaction(transaction);
#if defined(BROOK_BDW_RPS_TRIAL)
            if(!transaction.restoreTrialFrequency())renderResult->transaction.uncertain=true;
#endif
            renderResult->ggttWriteAttempts=backend.ggttWriteAttempts();renderResult->portWriteAttempts=backend.portWriteAttempts();
            const auto& r=renderResult->transaction;
            return r.contextSaved && r.resetComplete && r.patRestored && !r.uncertain;
        }
        if(gtInit) {
            // Include all extra pages in the same locked UC-alias audit as
            // the forcewake/engine samples. No selector write is required.
            constexpr uint64_t pages[]={BdwDellBar0,BdwOtherEnginePages[0],BdwPrivatePatPage,BdwResetPage,BdwDellBar0+0xb000};
            for(auto page:pages){DellI2cCriticalEvidence e{};if(!held.evidence(page,e) || !BdwGttEvidenceValid(e))return false;}
            GtOwner owner{held,khz};
            BdwRenderGtMmio<GtOwner> mmio(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[0]),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage),reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
            *gtInit=BdwInitializeRenderGt(mmio);
            gtInit->selector=owner.selector;gtInit->fuse2=owner.fuse;gtInit->topologySampled=owner.sampled;
            return gtInit->status==BdwRenderGtInitStatus::Applied;
        }
        if(rcsReset) {
            DellI2cCriticalEvidence e{};
            if(!held.evidence(BdwResetPage,e) || !BdwGttEvidenceValid(e))return false;
            struct RcsOwner {
                Io& io;uint32_t khz;uint64_t origin;
                bool owned(){uint32_t a=0;return io.owned() && io.read32(BdwDellBar0+0x130044,a) && a!=0xffffffffu && (a&1);}
                bool quiescent(){return owned();} // all engines sampled immediately before callback
                bool monotonicMs(uint64_t& ms){uint64_t ticks=0;if(!io.tsc(ticks) || ticks<origin)return false;ms=(ticks-origin)/khz;return true;}
                void relax(){io.relax();}
            } owner{held,khz,0};
            if(!held.tsc(owner.origin))return false;
            rcsReset->attempted=true;
            BdwRcsResetMmio<RcsOwner> reset(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwOtherEnginePages[0]),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
            const auto outcome=reset.reset();
            rcsReset->complete=outcome==BdwBcsResetResult::DomainAcknowledged;
            rcsReset->uncertain=outcome==BdwBcsResetResult::Uncertain;
            return rcsReset->complete;
        }
        if(patResult) {
            struct PatOwner {
                Io& io;uint32_t khz;uint64_t origin;
                bool owned(){uint32_t ack=0;return io.owned() && io.read32(BdwDellBar0+0x130044,ack) && ack!=0xffffffffu && (ack&1);}
                bool allEnginesQuiescent(){return owned();} // sampled immediately before callback
                bool quiescent(){return owned();}
                bool monotonicMs(uint64_t& ms){uint64_t ticks=0;if(!io.tsc(ticks) || ticks<origin)return false;ms=(ticks-origin)/khz;return true;}
                void relax(){io.relax();}
                bool safePatTransition(uint32_t lo,uint32_t hi,uint32_t nextLo,uint32_t nextHi) {
                    return owned() && lo==0x03030303u && hi==0x03030303u && nextLo==0x03030307u && nextHi==0x0303030bu;
                }
                bool drainAndInvalidate() {
                    uint32_t lo=0,hi=0;
                    if(!owned() || !io.read32(BdwDellBar0+0x40e0,lo) || !io.read32(BdwDellBar0+0x40e4,hi) ||
                       lo==0xffffffffu || hi==0xffffffffu)return false;
                    BdwGen8TlbMmio<PatOwner> mmio(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage));
                    BdwGen8TlbInvalidate invalidate;return invalidate.Run(mmio);
                }
            } owner{held,khz,0};
            if(!held.tsc(owner.origin))return false;
            patResult->beforeLo=lo;patResult->beforeHi=hi;patResult->attempted=true;
            DellI2cCriticalEvidence resetEvidence{};
            if(!held.evidence(BdwResetPage,resetEvidence) || !BdwGttEvidenceValid(resetEvidence))return false;
            BdwBcsResetMmio<PatOwner> reset(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
            patResult->resetComplete=reset.reset()==BdwBcsResetResult::DomainAcknowledged;
            if(!patResult->resetComplete)return false;
            BdwPatMmio<PatOwner> mmio(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage));
            BdwPatLease lease;
            patResult->established=lease.Establish(mmio,0x119);
            if(!patResult->established)return false;
            const bool sampled=mmio.readPrivatePat(patResult->activeLo,patResult->activeHi);
            patResult->restored=lease.Release(mmio);
            const bool after=mmio.readPrivatePat(patResult->afterLo,patResult->afterHi);
            return sampled && after && patResult->restored && patResult->afterLo==lo && patResult->afterHi==hi;
        }
        if(!scratchResult && !initResult && !copyResult)return true;
        // The audited MMIO page contains the diagnostic's fixed slots and
        // the separate scanout reservation. Their PTE writes stay disjoint.
        if(!scratchDma || (scanoutCopy?(!copyResult || scratchIndex<65536 || scratchIndex>65536+512-8):scratchIndex!=65536))
            return false;
        const uint64_t scratchPages[]={0xf6880000ull,(scratchResult || copyResult)?0xf6101000ull:BdwResetPage,BdwResetPage};
        for(uint64_t page:scratchPages) {
            DellI2cCriticalEvidence e{};if(!held.evidence(page,e) || !BdwGttEvidenceValid(e))return false;
        }
        if(copyResult) {
            if(!copyToken)return false;
            struct CopyOwner {
                Io& io;uint32_t khz,line;uint64_t origin;uint32_t budget=4000000;
                bool patActive=false,scanout=false;
                bool owned(){uint32_t a=0;return io.owned() && io.read32(BdwDellBar0+0x130044,a) && a!=0xffffffffu && (a&1);}
                bool allEnginesQuiescent(){return owned();} // sampled immediately before callback
                bool monotonicMs(uint64_t& ms){uint64_t ticks=0;if(!io.tsc(ticks) || ticks<origin)return false;ms=(ticks-origin)/khz;return true;}
                bool inTime(){uint64_t ms=0;return budget && --budget && monotonicMs(ms) && ms<2000;}
                bool completionContinue(){return owned() && inTime();}
                bool retirementContinue(){return owned() && inTime();}
                void relax(){io.relax();}
                bool gpuPatLease(){return patActive && owned();}
                bool preparePrivatePat(uint64_t descriptor){return gpuPatLease() && (descriptor&0x119)==0x119;}
                bool readPrivatePat(uint32_t& lo,uint32_t& hi){return owned() && io.read32(BdwDellBar0+0x40e0,lo) && io.read32(BdwDellBar0+0x40e4,hi);}
                bool safePatTransition(uint32_t lo,uint32_t hi,uint32_t nlo,uint32_t nhi){
                    return owned() && lo==0x03030303u && hi==0x03030303u &&
                        nlo==(scanout?0x00030307u:0x03030307u) && nhi==0x0303030bu;
                }
                bool drainAndInvalidate(){
                    uint32_t lo=0,hi=0;
                    if(!readPrivatePat(lo,hi) || lo==0xffffffffu || hi==0xffffffffu)return false;
                    BdwGen8TlbMmio<CopyOwner> mmio(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage));
                    BdwGen8TlbInvalidate invalidate;return invalidate.Run(mmio);
                }
                bool invalidateGpuTranslations(){return drainAndInvalidate();}
                bool resetBcs(BdwBcsResetIntent intent=BdwBcsResetIntent::IdleTakeover){
                    BdwBcsResetMmio<CopyOwner> reset(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),
                        reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
                    return reset.reset(intent)==BdwBcsResetResult::DomainAcknowledged;
                }
                bool quiescent(){
                    struct Reader {CopyOwner& o;bool owned(){return o.owned();}
                        bool read(uint32_t r,uint32_t& v){return o.io.read32(BdwDellBar0+r,v);}} reader{*this};
                    return owned() && BdwObserveEngine(reader,0x22000).submissionQuiescent;
                }
                bool admitPages(BdwOwnedPage* pages,unsigned count){
                    if(!owned() || !pages ||
                       (scanout?(count<=BdwBcsLegacyPrivatePages || count>BdwBcsMaxPrivatePages):count!=BdwBcsLegacyPrivatePages))
                        return false;
                    for(unsigned i=0;i<count;++i) {
                        const auto dma=pages[i].dma;
                        if(!dma || (dma&4095) || dma>=(uint64_t{16}<<30) ||
                           pages[i].cpu!=reinterpret_cast<uint8_t*>(DIRECT_MAP_BASE+dma))return false;
                        for(unsigned j=0;j<i;++j)if(dma==pages[j].dma)return false;
                    }
                    return true;
                }
                bool flushPage(const BdwOwnedPage& p){
                    if(!owned() || !p.cpu || !line || line>4096 || (4096%line))return false;
                    __asm__ volatile("mfence":::"memory");
                    for(uint32_t off=0;off<4096;off+=line)__asm__ volatile("clflush (%0)"::"r"(p.cpu+off):"memory");
                    __asm__ volatile("mfence":::"memory");return owned();
                }
                bool syncForCpu(const BdwOwnedPage& p){return flushPage(p);}
                bool syncForDevice(const BdwOwnedPage& p){return flushPage(p);}
                bool publish(BdwOwnedPage* pages,unsigned count){
                    if(!owned() || !pages || !count || count>BdwBcsMaxPrivatePages)return false;
                    for(unsigned i=0;i<count;++i)if(!flushPage(pages[i]))return false;
                    return true;
                }
                bool validateLease(const BdwGgttLease&){return owned();}
                bool validateTakeover(const BdwGgttLease&,uint64_t){return owned();}
                bool acknowledgeCsb(unsigned slot){
                    if(!owned() || slot>=6)return false;
                    uint32_t posted=0;
                    return io.write32(BdwDellBar0+0x223a0,0x07000000u|(slot<<8)) &&
                        io.read32(BdwDellBar0+0x223a0,posted) && posted!=0xffffffffu && ((posted>>8)&7u)==slot && owned();
                }
            } owner{held,khz,0,0};
            owner.scanout=scanoutCopy;
            uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
            owner.line=((b>>8)&255)*8;
            if(!(d&(1u<<19)) || owner.line<16 || owner.line>4096 || (owner.line&(owner.line-1)) ||
               4096%owner.line || !held.tsc(owner.origin))return false;
            copyResult->resetBeforeAttempted=true;copyResult->resetBefore=owner.resetBcs();
            if(!copyResult->resetBefore)return false;
            BdwPatMmio<CopyOwner> patMmio(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage),scanoutCopy);
            BdwPatLease pat;copyResult->patEstablished=pat.Establish(patMmio,0x119,scanoutCopy);
            if(!copyResult->patEstablished){copyResult->patUncertain=pat.State()==BdwPatLeaseState::Quarantined;return false;}
            owner.patActive=true;
            BdwBcsDeviceBackend<CopyOwner> backend(owner,reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6880000ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6101008ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),scratchIndex,copyToken,scratchDma);
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
            const bool copied=scanoutCopy?BdwRunPreparedBcsScanout(backend,scratchDma,*copyResult):
                BdwRunRetainedBcs(backend,scratchDma,*copyResult);
#else
            const bool copied=BdwRunRetainedBcs(backend,scratchDma,*copyResult);
#endif
            copyResult->ggttWriteAttempts=backend.ggttWriteAttempts();copyResult->portWriteAttempts=backend.portWriteAttempts();
            copyResult->engineSampled=held.read32(BdwDellBar0+0x22034,copyResult->head) &&
                held.read32(BdwDellBar0+0x22030,copyResult->tail) && held.read32(BdwDellBar0+0x2209c,copyResult->mode) &&
                held.read32(BdwDellBar0+0x2203c,copyResult->control) && held.read32(BdwDellBar0+0x22074,copyResult->acthd) &&
                held.read32(BdwDellBar0+0x22064,copyResult->ipeir) && held.read32(BdwDellBar0+0x22068,copyResult->ipehr) &&
                held.read32(BdwDellBar0+0x22234,copyResult->execlist);
            // A failed submission may still have reached the engine. Reset it
            // before changing the policy back; on uncertainty retain all pages.
            copyResult->resetAfter=owner.resetBcs(BdwBcsResetIntent::StopOwnedWork);
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
            if(scanoutCopy && copyResult->resetAfter)
                copyResult->scanoutRetired=BdwRetirePreparedBcsScanout(backend);
#endif
            if(copyResult->resetAfter)copyResult->patRestored=pat.Release(patMmio);
            copyResult->patUncertain=!copyResult->resetAfter || !copyResult->patRestored ||
                (scanoutCopy && !copyResult->scanoutRetired);
            owner.patActive=false;
            return copied && !copyResult->patUncertain;
        }
        struct Owner {
            Io& io;uint32_t khz;uint64_t origin;uint8_t* cpu;bool resetDone=false;uint32_t cacheLine=0;
            bool owned(){uint32_t a=0;return io.owned() && io.read32(BdwDellBar0+0x130044,a) && a!=0xffffffffu && (a&1);}
            bool allEnginesQuiescent(){return owned();} // checked immediately before callback
            bool quiescent(){return owned();}
            bool monotonicMs(uint64_t& ms){uint64_t ticks=0;if(!io.tsc(ticks) || ticks<origin)return false;ms=(ticks-origin)/khz;return true;}
            void relax(){io.relax();}
            bool resetVerified(){return resetDone && owned();}
            bool csbPrepared(uint64_t hws){return cpu && hws==0x10000000ull && resetVerified();}
            bool flushPage(){
                if(!owned() || !cpu || !cacheLine || cacheLine>4096 || (4096%cacheLine))return false;
                __asm__ volatile("mfence":::"memory");
                for(uint32_t off=0;off<4096;off+=cacheLine)__asm__ volatile("clflush (%0)"::"r"(cpu+off):"memory");
                __asm__ volatile("mfence":::"memory");return owned();
            }
            bool prepareCsbMemory(uint64_t hws){return csbPrepared(hws) && BdwCsbInitializeForReset(cpu,[&](uint8_t*){return flushPage();});}
            bool csbMemoryProducer(uint64_t hws,uint32_t& value){return csbPrepared(hws) && flushPage() && BdwCsbReadProducer(cpu,value);}
            bool invalidateGpuTranslations(){
                BdwGen8TlbMmio<Owner> mmio(*this,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwPrivatePatPage));
                BdwGen8TlbInvalidate invalidator;return invalidator.Run(mmio);
            }
        } owner{held,khz,0,scratchCpu};
        if(!held.tsc(owner.origin))return false;
        if(initResult) {
            if(!scratchCpu || reinterpret_cast<uintptr_t>(scratchCpu)!=DIRECT_MAP_BASE+scratchDma)return false;
            __asm__ volatile("lfence":::"memory");
            const uint64_t mapped=*reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6880000ull);
            __asm__ volatile("lfence":::"memory");
            if(mapped!=(scratchDma|1))return false;
            uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
            owner.cacheLine=((b>>8)&255)*8;
            if(!(d&(1u<<19)) || owner.cacheLine<16 || owner.cacheLine>4096 ||
               (owner.cacheLine&(owner.cacheLine-1)) || 4096%owner.cacheLine)return false;
            initResult->attempted=true;
            BdwBcsResetMmio<Owner> reset(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
            owner.resetDone=reset.reset()==BdwBcsResetResult::DomainAcknowledged;
            initResult->resetComplete=owner.resetDone;
            if(!owner.resetDone)return false;
            BdwBcsMmio<Owner> bcs(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull));
            initResult->initComplete=bcs.initializeExeclists(0x10000000ull);
            return initResult->initComplete;
        }
        BdwScratchMmio<Owner> mmio(owner,reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE+0xf6880000ull),
            reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6101008ull),scratchIndex,clearScratch?0:scratchDma);
        if(clearScratch)*scratchResult=BdwClearKnownScratchSlot(mmio,scratchIndex,scratchDma);
        else {
            BdwBcsResetMmio<Owner> reset(owner,reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+0xf6022000ull),
                reinterpret_cast<volatile uint32_t*>(DIRECT_MAP_BASE+BdwResetPage));
            auto resetBcs=[&](){return reset.reset()==BdwBcsResetResult::DomainAcknowledged;};
            *scratchResult=BdwResetAndTakeoverScratchSlot(mmio,scratchIndex,scratchDma,resetBcs);
        }
        return scratchResult->complete;
    };
    result=BdwDellForcewakeCriticalWithWork(io,khz,work,scopeCheck || scratchResult || initResult || patResult || copyResult || rcsReset || gtInit || renderResult,otherEngines,patRequested,tlbRequested,resetRequested);
    IrqSpinLockRelease(&g_kernelPtLock,irq);
    claim.finish(result);
}
void VmmDellForcewakeCriticalRun(uint32_t khz,BdwForcewakeAdapterResult& result,bool otherEngines,bool patRequested,bool tlbRequested,bool resetRequested,bool scopeCheck) {
    VmmDellForcewakeCriticalImpl(khz,result,otherEngines,patRequested,tlbRequested,resetRequested,scopeCheck,0,0,nullptr,nullptr,nullptr,false,nullptr,0,nullptr);
}
void VmmDellForcewakeScratchRun(uint32_t khz,uint32_t index,uint64_t dma,BdwForcewakeAdapterResult& result,BdwScratchTakeoverResult& scratch) {
    scratch={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,dma,&scratch,nullptr,nullptr,false,nullptr,0,nullptr);
}
void VmmDellForcewakeBcsInit(uint32_t khz,uint32_t index,uint64_t dma,uint8_t* cpu,BdwForcewakeAdapterResult& result,BdwBcsInitResult& init) {
    init={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,dma,nullptr,cpu,&init,false,nullptr,0,nullptr);
}
void VmmDellForcewakeClearStale(uint32_t khz,uint64_t expected,BdwForcewakeAdapterResult& result,BdwScratchTakeoverResult& clear) {
    clear={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,65536,expected,&clear,nullptr,nullptr,true,nullptr,0,nullptr);
}
void VmmDellForcewakePatTest(uint32_t khz,BdwForcewakeAdapterResult& result,BdwPatTestResult& pat) {
    pat={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,0,0,nullptr,nullptr,nullptr,false,&pat,0,nullptr);
}
void VmmDellBcsCopy(uint32_t khz,uint32_t index,uint64_t token,uint64_t scratch,BdwForcewakeAdapterResult& result,BdwBcsRunResult& copy) {
    copy={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,scratch,nullptr,nullptr,nullptr,false,nullptr,token,&copy);
}
#if defined(BROOK_SSD_AUTO_RECOVERY)
void VmmDellBcsScanout(uint32_t khz,uint32_t index,uint64_t token,uint64_t scratch,BdwForcewakeAdapterResult& result,BdwBcsRunResult& copy) {
    copy={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,scratch,nullptr,nullptr,nullptr,
        false,nullptr,token,&copy,nullptr,nullptr,nullptr,nullptr,true);
}
#endif
void VmmDellRcsReset(uint32_t khz,BdwForcewakeAdapterResult& result,BdwRcsResetTestResult& reset) {
    reset={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,0,0,nullptr,nullptr,nullptr,false,nullptr,0,nullptr,&reset);
}
void VmmDellRenderGtInit(uint32_t khz,BdwForcewakeAdapterResult& result,BdwRenderGtInitResult& init) {
    init={};VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,0,0,nullptr,nullptr,nullptr,false,nullptr,0,nullptr,nullptr,&init);
}
void VmmDellRenderRun(uint32_t khz,uint32_t index,uint64_t token,uint64_t scratch,uint32_t (&snapshot)[20*1024],BdwForcewakeAdapterResult& result,BdwRenderRunResult& render) {
    VmmDellForcewakeCriticalImpl(khz,result,true,true,false,false,false,index,scratch,nullptr,nullptr,nullptr,false,nullptr,token,nullptr,nullptr,nullptr,&render,&snapshot);
}

bool VmmClearAccessedIfMatches(PageTable pt,VirtualAddress address,PhysicalAddress expected) {
    if(!pt || pt.pml4==g_kernelCR3 || address.raw()>=0x0000800000000000ULL || (address.raw()&4095) || !expected)return false;
    uint64_t irq=IrqSpinLockAcquire(&g_userPtLock);
    bool large=false;auto* entry=WalkToPtr(pt,address,false,0,&large);bool changed=false;
    if(entry && !large) {
        uint64_t old=__atomic_load_n(entry,__ATOMIC_RELAXED);
        while((old&(VMM_PRESENT|VMM_USER|VMM_ACCESSED))==(VMM_PRESENT|VMM_USER|VMM_ACCESSED) &&
              !(old&PTE_COW_BIT) && VmmLeafPhysical(old)==expected) {
            (void)CaptureOwnedLeaf(entry,old,HostOwnerExperiment(pt));
            const uint64_t aged=old&~VMM_ACCESSED;
            if(__atomic_compare_exchange_n(entry,&old,aged,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) {
                changed=true;break;
            }
        }
    }
    IrqSpinLockRelease(&g_userPtLock,irq);return changed;
}

using LeafUpdate = uint64_t (*)(VirtualAddress,VmmOwnedLeaf,void*);
static bool WalkLeafRange(uint64_t* table, unsigned level, uint64_t base,
                           uint64_t begin, uint64_t end, bool detach,
                           VmmRetireLeaf retire, void* context, uint64_t& count,
                           LeafUpdate update = nullptr,bool extended=false,bool atomicUpdate=false,bool updateOwnsStore=false)
{
    const unsigned shift = 12 + 9 * (level - 1);
    const uint64_t size = uint64_t{1} << shift;
    uint64_t first = begin > base ? (begin-base) >> shift : 0;
    uint64_t last = (end-1-base) >> shift;
    if (last > 511) last = 511;
    for (uint64_t i = first; i <= last; ++i) {
        uint64_t leaf = __atomic_load_n(&table[i],__ATOMIC_RELAXED);
        if (level == 1 ? !VmmLeafBacked(leaf) : !(leaf & VMM_PRESENT)) continue;
        uint64_t address = base + i*size;
        if (level > 1) {
            if (leaf & (1ULL << 7)) return false;
            auto* child = reinterpret_cast<uint64_t*>(PhysToVirt(VmmLeafPhysical(leaf)).raw());
            const uint64_t low = begin > address ? begin : address;
            const uint64_t high = end < address+size ? end : address+size;
            if (!WalkLeafRange(child,level-1,address,low,high,detach,retire,context,count,update,extended,atomicUpdate,updateOwnsStore)) return false;
        } else {
            ++count;
            const auto captured=CaptureOwnedLeaf(&table[i],leaf,extended);
            if (detach) {
                retire(VirtualAddress(address),captured,context);
                if (leaf & VMM_USER) PmmMapDec(VmmLeafPhysical(leaf));
                ClearExtendedOwner(&table[i],captured,extended);
                table[i] = 0; Invlpg(VirtualAddress(address));
            } else if (update) {
                if (atomicUpdate) {
                    // Hardware may set A/D while the software page-table lock is held.
                    // Recompute from the latest word rather than losing those updates.
                    uint64_t expected=leaf;
                    for (;;) {
                        if (((expected^leaf)&~(VMM_ACCESSED|VMM_DIRTY))!=0) __builtin_trap();
                        const auto current=CaptureOwnedLeaf(&table[i],expected,extended);
                        const uint64_t updated=update(VirtualAddress(address),current,context);
                        if (updated==expected) break;
                        if (__atomic_compare_exchange_n(&table[i],&expected,updated,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) {
                            Invlpg(VirtualAddress(address));break;
                        }
                    }
                } else {
                    const uint64_t updated=update(VirtualAddress(address),captured,context);
                    if (updated!=leaf && !updateOwnsStore) { table[i]=updated;Invlpg(VirtualAddress(address)); }
                }
            }
        }
    }
    return true;
}
static bool ValidLeafRange(PageTable pt, uint64_t begin, uint64_t end) {
    return pt && pt.pml4!=g_kernelCR3 && begin < end && end <= 0x0000800000000000ULL && !(begin & 4095) && !(end & 4095);
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
bool VmmPrepareOwnerSidecar(PageTable pt,VirtualAddress address) {
    if (!ValidLeafRange(pt,address.raw(),address.raw()+4096) || (address.raw()&4095)) return false;
    const auto irq=IrqSpinLockAcquire(&g_userPtLock);
    bool large=false;
    auto* leaf=WalkToPtr(pt,address,false,0,&large);
    if(!large && !leaf)leaf=WalkToPtr(pt,address,true,0,&large);
    const bool ok=!large && leaf && PrepareOwnerSidecarLocked(leaf);
    IrqSpinLockRelease(&g_userPtLock,irq);return ok;
}
struct MoveLeaves { PageTable table;uint64_t source,destination;bool prepare,occupied,move,failed,extended; };
#if defined(BROOK_HOST_TEST) && !defined(BROOK_STANDALONE_TEST)
static thread_local uint64_t g_hostMoveAccessBits;
void VmmHostInjectMoveAccessBits(uint64_t bits) {g_hostMoveAccessBits=bits&(VMM_ACCESSED|VMM_DIRTY);}
#endif
static uint64_t MoveLeaf(VirtualAddress address,VmmOwnedLeaf owned,void* opaque) {
    auto leaf=owned.raw;
    auto& context=*static_cast<MoveLeaves*>(opaque);
    if (context.failed) return leaf;
    // Extended-owner transfers need destination metadata preflight. Until
    // that exists, reject during the non-mutating pass rather than truncate.
    if (!context.extended && owned.owner>=PTE_PID_CAPACITY) {context.failed=true;return leaf;}
    const VirtualAddress destination(context.destination+address.raw()-context.source);
    bool large=false;auto* target=WalkToPtr(context.table,destination,false,0,&large);
    if (!large && !target && context.prepare) target=WalkToPtr(context.table,destination,true,0);
    if (large || !target || (!context.occupied && VmmLeafBacked(*target))) { context.failed=true;return leaf; }
    if(context.extended && owned.owner>=PteOwnerSidecars::Escape) {
        if(context.prepare) {
            if(!PrepareOwnerSidecarLocked(target)){context.failed=true;return leaf;}
        } else if(!g_userOwnerSidecars.Find(OwnerTable(target))) {context.failed=true;return leaf;}
    }
    if (!context.move) return leaf;
    auto* source=WalkToPtr(context.table,address,false);
    if(!source)__builtin_trap();
#if defined(BROOK_HOST_TEST) && !defined(BROOK_STANDALONE_TEST)
    if(g_hostMoveAccessBits) {
        __atomic_fetch_or(source,g_hostMoveAccessBits,__ATOMIC_RELAXED);g_hostMoveAccessBits=0;
    }
#endif
    // Take the actual PTE once, including A/D updates made since the walk.
    // Remote stale translations remain subject to the transaction shootdown.
    const auto taken=__atomic_exchange_n(source,uint64_t{0},__ATOMIC_ACQ_REL);
    if(((taken^leaf)&~(VMM_ACCESSED|VMM_DIRTY))!=0)__builtin_trap();
    leaf=taken;owned.raw=taken;
    if(context.extended) {
        if(owned.owner>=PteOwnerSidecars::Escape) {
            if(!g_userOwnerSidecars.Set(OwnerTable(target),OwnerSlot(target),owned.owner))__builtin_trap();
        } else g_userOwnerSidecars.Clear(OwnerTable(target),OwnerSlot(target));
        ClearExtendedOwner(source,owned,true);
    }
    const auto inlineOwner=context.extended && owned.owner>=PteOwnerSidecars::Escape?PteOwnerSidecars::Escape:owned.owner;
    *target=WithInlineOwner(leaf,inlineOwner);
    if (leaf & VMM_USER) {
        auto* table=reinterpret_cast<uint64_t*>(PhysToVirt(context.table.pml4).raw());
        const uint64_t indices[]={Pml4Index(destination.raw()),PdptIndex(destination.raw()),PdIndex(destination.raw())};
        for (uint64_t index:indices) { table[index]|=VMM_USER;table=reinterpret_cast<uint64_t*>(PhysToVirt(VmmLeafPhysical(table[index])).raw()); }
    }
    Invlpg(address);Invlpg(destination);return 0; // transfer the PTE-owned reference, no ref/map-count change
}
static bool MoveLeafRange(PageTable pt,uint64_t begin,uint64_t end,uint64_t destination,
                           bool prepare,bool occupied,bool move) {
    if (!ValidLeafRange(pt,begin,end) || !ValidLeafRange(pt,destination,destination+(end-begin)) ||
        (destination<end && begin<destination+(end-begin))) return false;
    uint64_t irq=IrqSpinLockAcquire(&g_userPtLock);
    auto* table=reinterpret_cast<uint64_t*>(PhysToVirt(pt.pml4).raw());
    uint64_t count=0;
    const bool extended=HostOwnerExperiment(pt);
    MoveLeaves context{pt,begin,destination,prepare,occupied,false,false,extended};
    bool result=WalkLeafRange(table,4,0,begin,end,false,nullptr,nullptr,count,nullptr,extended);
    if (result && !occupied) {
        count=0;
        result=WalkLeafRange(table,4,0,destination,destination+(end-begin),false,nullptr,nullptr,count,nullptr,extended) && !count;
    }
    if (result) {
        count=0;
        result=WalkLeafRange(table,4,0,begin,end,false,nullptr,&context,count,MoveLeaf,extended) && !context.failed;
    }
    if (result && move) {
        context.move=true;count=0;
        if (!WalkLeafRange(table,4,0,begin,end,false,nullptr,&context,count,MoveLeaf,extended,false,true) || context.failed) __builtin_trap();
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
                        if (mappings==1 && !(entry&PTE_COW_BIT) &&
                            PmmGetRefCount(VmmLeafPhysical(entry))==1)
                            ++stats.exclusiveManagedPages;
                        stats.proportionalUnits += (4096ULL<<16)/mappings;
                    }
                }
            }
            walk.cursor=high;
        }
    }
    return true;
}
bool VmmMeasureRangeStep(PageTable pt, uint64_t begin, uint64_t end,
                         bool accountRam, VmmRangeStats* stats, uint64_t* next)
{
    if (!stats || !next) return false;
    *stats={};*next=begin;
    if (!ValidLeafRange(pt,begin,end)) return false;
    VmmRangeStats measured{};
    MeasureWalk walk{begin,4096,accountRam,&measured};
    const uint64_t irq=IrqSpinLockAcquire(&g_userPtLock);
    auto* table=reinterpret_cast<uint64_t*>(PhysToVirt(pt.pml4).raw());
    const bool valid=MeasureTable(table,4,0,begin,end,walk);
    IrqSpinLockRelease(&g_userPtLock,irq);
    if (!valid || walk.cursor<=begin) return false;
    measured.lockBatches=1;*stats=measured;*next=walk.cursor;
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
static uint64_t CollectAddress(VirtualAddress address, VmmOwnedLeaf leaf, void* opaque) {
    auto& collected = *static_cast<CollectAddresses*>(opaque);
    collected.addresses[collected.count++] = address.raw();
    return leaf.raw;
}
bool VmmCollectRangeAddresses(PageTable pt, uint64_t begin, uint64_t end,
                              uint64_t* addresses, uint64_t capacity, uint64_t* count) {
    if (!count || (capacity && !addresses) || !ValidLeafRange(pt,begin,end)) return false;
    *count = 0;
    uint64_t irq = IrqSpinLockAcquire(&g_userPtLock);
    auto* table = reinterpret_cast<uint64_t*>(PhysToVirt(pt.pml4).raw());
    uint64_t required = 0;
    bool result = WalkLeafRange(table,4,0,begin,end,false,nullptr,nullptr,required,nullptr,HostOwnerExperiment(pt)) && required <= capacity;
    if (result && required) {
        CollectAddresses collected{addresses,0};
        result = WalkLeafRange(table,4,0,begin,end,false,nullptr,&collected,*count,CollectAddress,HostOwnerExperiment(pt));
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
    bool result = WalkLeafRange(table,4,0,begin,end,false,nullptr,nullptr,required,nullptr,HostOwnerExperiment(pt)) && required <= capacity;
    if (result) {
        if (!WalkLeafRange(table,4,0,begin,end,true,retire,context,*count,nullptr,HostOwnerExperiment(pt))) {
            SerialPuts("VMM: range walk changed under page-table lock\n");
            __builtin_trap();
        }
    }
    IrqSpinLockRelease(&g_userPtLock,irq);
    return result;
}
struct ProtectLeaves { uint32_t protection; VmmPrivateLeafQuery query; void* context; };
static uint64_t ProtectLeaf(VirtualAddress address, VmmOwnedLeaf owned, void* opaque) {
    const auto leaf=owned.raw;
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
    const bool extended=HostOwnerExperiment(pt);
    bool result = WalkLeafRange(table,4,0,begin,end,false,nullptr,nullptr,count,nullptr,extended);
    if (result) {
        count = 0;
        if (!WalkLeafRange(table,4,0,begin,end,false,nullptr,&update,count,ProtectLeaf,extended,true)) __builtin_trap();
    }
    IrqSpinLockRelease(&g_userPtLock,irq); return result;
}

VmmOwnedLeaf VmmDetachOwnedPage(PageTable pt, VirtualAddress virtAddr)
{
    if(HostOwnerExperiment(pt) && virtAddr.raw()>=0x0000800000000000ULL)return {};
    IrqSpinLock& lock = PageTableLock(pt,virtAddr);
    uint64_t saved = IrqSpinLockAcquire(&lock);
    uint64_t* pte = WalkToPtr(pt, virtAddr, false);
    const bool extended=HostOwnerExperiment(pt) && virtAddr.raw()<0x0000800000000000ULL;
    const auto captured=pte && VmmLeafBacked(*pte)?CaptureOwnedLeaf(pte,*pte,extended):VmmOwnedLeaf{};
    const auto old=captured.raw;
    if (old) {
        if (old & VMM_USER) PmmMapDec(VmmLeafPhysical(old));
        ClearExtendedOwner(pte,captured,extended);
        *pte = 0;
        Invlpg(virtAddr);
    }
    IrqSpinLockRelease(&lock, saved);
    return captured;
}

VmmOwnedLeaf VmmDetachOwnedPageIfMatches(PageTable pt, VirtualAddress virtAddr, PhysicalAddress expected)
{
    if(HostOwnerExperiment(pt) && virtAddr.raw()>=0x0000800000000000ULL)return {};
    IrqSpinLock& lock = PageTableLock(pt,virtAddr);
    uint64_t saved = IrqSpinLockAcquire(&lock);
    uint64_t* pte = WalkToPtr(pt, virtAddr, false);
    const bool extended=HostOwnerExperiment(pt) && virtAddr.raw()<0x0000800000000000ULL;
    const auto captured=pte && VmmLeafBacked(*pte) && VmmLeafPhysical(*pte)==expected?CaptureOwnedLeaf(pte,*pte,extended):VmmOwnedLeaf{};
    const auto old=captured.raw;
    if (old) {
        if (old & VMM_USER) PmmMapDec(expected);
        ClearExtendedOwner(pte,captured,extended);
        *pte = 0;
        Invlpg(virtAddr);
    }
    IrqSpinLockRelease(&lock, saved);
    return captured;
}

uint64_t VmmDetachPage(PageTable pt, VirtualAddress virtAddr) { return HostOwnerExperiment(pt)?0:VmmDetachOwnedPage(pt,virtAddr).raw; }
uint64_t VmmDetachPageIfMatches(PageTable pt, VirtualAddress virtAddr, PhysicalAddress expected) {
    return HostOwnerExperiment(pt)?0:VmmDetachOwnedPageIfMatches(pt,virtAddr,expected).raw;
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
                const auto leaf=VmmDetachPage(KernelPageTable,v);
                if (leaf) PmmFreePage(VmmLeafPhysical(leaf));
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
                const auto leaf=VmmDetachPage(KernelPageTable,v);
                if (leaf) PmmFreePage(VmmLeafPhysical(leaf));
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
    if(HostOwnerExperiment(pt))return nullptr;
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
    const auto leaf=VmmReadOwnedLeaf(pt,virtAddr);
    return leaf.raw & VMM_PRESENT ? leaf.owner : KernelPid;
}

// BRO-176 diag: dump a physical page's ref/unref history ring (physical_memory.cpp).
extern "C" int PmmDumpFreeLog(uint64_t phys);

VmmCowResolution VmmCowResolveWrite(PageTable pt, VirtualAddress faultAddr, uint16_t pid)
{
    const bool extended=HostOwnerExperiment(pt);
    if (pid >= (extended?5120:PTE_PID_CAPACITY)) return {VmmCowResult::InvalidOwner,{}};
    if (!pt || pt.pml4==g_kernelCR3 || faultAddr.raw()>=0x0000800000000000ULL)
        return {VmmCowResult::NotCow,{}};
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
    const auto oldOwned=CaptureOwnedLeaf(pte,*pte,extended);

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
        if(extended && pid>=PteOwnerSidecars::Escape && !PrepareOwnerSidecarLocked(pte)) {
            IrqSpinLockRelease(&g_userPtLock,ptFlags);return {VmmCowResult::OutOfMemory,{}};
        }
        // Shared page: copy to a fresh page and drop our ref on the shared one.
        // Host-only fixture IDs are not PMM owners. Real high-PID support must
        // widen PMM ownership before the experiment can become production.
        PhysicalAddress newPhys = PmmAllocPage(MemTag::User, extended?KernelPid:pid);
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
        if(extended) {
            ClearExtendedOwner(pte,oldOwned,true);
            if(pid>=PteOwnerSidecars::Escape && !g_userOwnerSidecars.Set(OwnerTable(pte),OwnerSlot(pte),pid))__builtin_trap();
        }
        const auto inlineOwner=extended && pid>=PteOwnerSidecars::Escape?PteOwnerSidecars::Escape:pid;
        newPte = WithInlineOwner(newPte,inlineOwner);
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

    const uint64_t irq=IrqSpinLockAcquire(&g_kernelPtLock);
    for (uint64_t i = 256; i < 512; i++)
        newPml4[i] = kernPml4[i];
    IrqSpinLockRelease(&g_kernelPtLock,irq);

    return PageTable(pml4Phys);
}

#if defined(BROOK_HOST_TEST) && !defined(BROOK_STANDALONE_TEST)
PageTable VmmHostCreateOwnerExperiment() {
    auto root=VmmCreateUserPageTable();if(!root)return {};
    const auto irq=IrqSpinLockAcquire(&g_userPtLock);
    const bool available=!__atomic_load_n(&g_hostOwnerExperimentRoot,__ATOMIC_RELAXED);
    if(available)__atomic_store_n(&g_hostOwnerExperimentRoot,root.pml4.raw(),__ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_userPtLock,irq);
    if(!available){VmmDestroyUserPageTable(root);return {};}
    return root;
}
#endif

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
        // Teardown owns this address space exclusively after its final TLB
        // acknowledgement. Unlink before its L1 frame can be recycled.
        const auto irq=IrqSpinLockAcquire(&g_userPtLock);
        auto* sidecar=g_userOwnerSidecars.Remove(tablePhys.raw());
        IrqSpinLockRelease(&g_userPtLock,irq);
        if(sidecar)PmmFreePage(VirtToPhys(VirtualAddress(reinterpret_cast<uint64_t>(sidecar))));
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

#if defined(BROOK_HOST_TEST) && !defined(BROOK_STANDALONE_TEST)
    // Fixture owners must join all accessors before destruction. Clear the
    // experiment identity before the root frame can be allocated again.
    const auto irq=IrqSpinLockAcquire(&g_userPtLock);
    if(pt.pml4.raw()==__atomic_load_n(&g_hostOwnerExperimentRoot,__ATOMIC_RELAXED))
        __atomic_store_n(&g_hostOwnerExperimentRoot,0,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_userPtLock,irq);
#endif
    PmmFreePage(pt.pml4);
}

} // namespace brook
