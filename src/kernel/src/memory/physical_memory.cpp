#include "physical_memory.h"
#include "serial.h"
#include "spinlock.h"
#include "portio.h"
#include "../memory/virtual_memory.h"  // PhysToVirt (direct map) for frame poison
#include "../ksym_addrs.h"             // KsymFindByAddr for callstack symbolization

// Forward-declared to avoid circular headers.
namespace brook {
    extern "C" void* kmalloc(uint64_t);
    VirtualAddress VmmAllocPages(uint64_t pageCount, uint64_t flags,
                                 MemTag tag, uint16_t pid);
    struct Process;
    using KernelThreadFn = void (*)(void* arg);
    Process* KernelThreadCreate(const char* name, KernelThreadFn fn, void* arg,
                                uint8_t priority);
    void SchedulerAddProcess(Process* proc);   // scheduler.h — enqueue onto ready queue
    extern "C" void SchedulerSleepMs(uint32_t ms);
}

// Linker-defined symbol — end of the kernel image (virtual address).
// Declared outside any namespace so the linker resolves it correctly.
extern "C" uint8_t __kernel_end_sym[] __asm__("__kernel_end");

// panic.h's KernelPanic, forward-declared to avoid pulling the header (and its
// transitive includes) into this low-level TU.
__attribute__((noreturn)) extern "C" void KernelPanic(const char* fmt, ...);

namespace brook {

// ---------------------------------------------------------------------------
// Bitmap storage — 4MB in BSS (zeroed by ELF loader), covers 128GB physical.
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


// SMP lock protecting the bitmap, free count, and page descriptors.
// IrqSpinLock: memory allocation can be called from any context, and a timer
// interrupt preempting a lock holder would deadlock if the new thread tries
// the same lock on the same CPU.
static IrqSpinLock g_pmmLock;

// ---------------------------------------------------------------------------
// BRO-176 DIAGNOSTIC: low-perturbation free-log.
// The double-free is a Heisenbug — heavy per-free page-table-walk audits slow
// the kernel enough to mask the race. This is the cheap alternative: every
// USER-page ref/unref events write a single ring record (phys, op, pid, count)
// with NO page-table walk and minimal locking (g_pmmLock already held). The
// crash handler (idt.cpp) translates the faulting pointer -> phys and dumps that
// frame's full ref/unref trail, naming the exact unmatched op (the COW undercount).
// TEMPORARY — strip with the rest of the BRO-176 instrumentation.
// ---------------------------------------------------------------------------
// op codes for FreeRec
enum : uint8_t {
    REFOP_ALLOC = 0,   // fresh allocation, refcount := 1
    REFOP_REF   = 1,   // PmmRefPage, refcount++
    REFOP_DEC   = 2,   // PmmUnrefPage/Free/Kill decremented (still shared)
    REFOP_FREE  = 3,   // actually freed (refcount hit 0)
};
// BRO-179 forensic provenance: each reflog record also carries a bounded kernel
// callstack (RBP frame-chain return addresses) so a frame's history names the
// DYNAMIC caller of each alloc/free, not just a static `site` label. The cheap
// O(1) mapCount counters can be defeated by the very accounting bug we hunt
// (they read 0 when a PTE is actually live); ground-truth callstacks cannot.
static constexpr uint8_t REFLOG_STACK_DEPTH = 6;
struct FreeRec {
    uint64_t phys; uint32_t seq; uint16_t ownerPid; uint8_t op; uint8_t count;
    const char* site;
    uint64_t stack[REFLOG_STACK_DEPTH];   // [0]=leaf caller .. via RBP chain
    uint8_t  stackDepth;
};
// BRO-179: logging ALL alloc/free tags (not just User) ~triples record volume,
// so a frame's cross-domain history is held for a shorter seq window than the
// old User-only log — but recent kernel use of a frame (the SIG1 gap) still
// shows. Ring kept at 2^17 (the known-good size; 2^19 overflowed the kernel
// load region and faulted at boot).
static constexpr uint32_t FREELOG_SIZE = 1u << 17; // 131072 records
static FreeRec  g_freeLog[FREELOG_SIZE];
static uint32_t g_freeLogSeq = 0;
static bool     g_freeLogOn  = false;

// Walk the caller's RBP frame chain and capture up to `max` return addresses
// into `out`. Bounded, lock-free, no allocation — safe under g_pmmLock. Mirrors
// the profiler's kernel-stack walk (profiler.cpp). `startRbp` is the frame
// pointer to begin from (the caller passes __builtin_frame_address(0)).
static inline uint8_t CaptureKernelStack(uint64_t startRbp, uint64_t* out, uint8_t max)
{
    constexpr uint64_t KBASE = 0xffffffff80000000ULL;
    constexpr uint64_t KEND  = 0xffffffffffffffffULL;
    uint64_t rbp = startRbp;
    uint8_t depth = 0;
    while (depth < max)
    {
        if (rbp < KBASE || rbp >= KEND - 16 || (rbp & 7) != 0) break;
        const uint64_t* frame = reinterpret_cast<const uint64_t*>(rbp);
        uint64_t retAddr = frame[1];
        if (retAddr < KBASE || retAddr >= KEND) break;
        out[depth++] = retAddr;
        uint64_t nextRbp = frame[0];
        if (nextRbp <= rbp) break;   // stack grows down; prevent loops
        rbp = nextRbp;
    }
    return depth;
}

// Caller MUST hold g_pmmLock.
static inline void RefLogRecord(uint64_t phys, uint16_t ownerPid, uint8_t op,
                                uint8_t count, const char* site)
{
    if (!g_freeLogOn) return;
    uint32_t i = g_freeLogSeq & (FREELOG_SIZE - 1);
    FreeRec& r = g_freeLog[i];
    r.phys = phys & ~0xFFFULL;
    r.seq = ++g_freeLogSeq;
    r.ownerPid = ownerPid;
    r.op = op;
    r.count = count;
    r.site = site;
    r.stackDepth = CaptureKernelStack(
        reinterpret_cast<uint64_t>(__builtin_frame_address(0)),
        r.stack, REFLOG_STACK_DEPTH);
}
// Back-compat shim for the existing free sites (op=FREE, count=0).
static inline void FreeLogRecord(uint64_t phys, uint16_t ownerPid, const char* site)
{
    RefLogRecord(phys, ownerPid, REFOP_FREE, 0, site);
}

// ---------------------------------------------------------------------------
// Ownership tracking — dynamically allocated after PmmEnableTracking().
// Null until then; all tag/pid operations are no-ops before that.
// ---------------------------------------------------------------------------

static PageDescriptor* g_pageDescs = nullptr;  // [g_totalPages], via kmalloc

// Per-PID doubly-linked page lists.  Static (16KB), indexed by PID.
static PidList g_pidLists[PMM_MAX_PIDS] = {};

// ---------------------------------------------------------------------------
// Internal list helpers (all require g_pageDescs != nullptr)
// ---------------------------------------------------------------------------

static inline PageDescriptor& Desc(uint32_t idx) { return g_pageDescs[idx]; }

// Remove a page from whatever PID list it currently belongs to.
// Does NOT reset the descriptor's tag/pid — caller must do that.
// Idempotent: a no-op if the page is not currently on its PID's list, so it
// is safe to call repeatedly (e.g. when a shared page is unref'd by several
// owners during teardown — see PmmUnrefPage/PmmKillPid, BRO-161).
static void ListRemove(uint32_t idx)
{
    PageDescriptor& d = Desc(idx);
    uint16_t pid = d.pid;

    // Not linked: no prev/next and not the list head -> already removed.
    if (d.prev == PMM_NULL_PAGE && d.next == PMM_NULL_PAGE &&
        g_pidLists[pid].head != idx)
        return;

    if (d.prev != PMM_NULL_PAGE)
        Desc(d.prev).next = d.next;
    else
        g_pidLists[pid].head = d.next;  // idx was the head

    if (d.next != PMM_NULL_PAGE)
        Desc(d.next).prev = d.prev;
    else
        g_pidLists[pid].tail = d.prev;  // idx was the tail

    if (g_pidLists[pid].pageCount > 0)
        g_pidLists[pid].pageCount--;

    d.next = d.prev = PMM_NULL_PAGE;
}

// Append a page to the tail of a PID's list and set its tag.
// The page must NOT currently be in any list (next/prev == PMM_NULL_PAGE).
static void ListAppend(uint32_t idx, uint16_t pid, MemTag tag)
{
    PageDescriptor& d = Desc(idx);
    d.pid  = pid;
    d.tag  = static_cast<uint8_t>(tag);
    d.next = PMM_NULL_PAGE;
    d.prev = g_pidLists[pid].tail;

    if (g_pidLists[pid].tail != PMM_NULL_PAGE)
        Desc(g_pidLists[pid].tail).next = idx;
    else
        g_pidLists[pid].head = idx;  // list was empty

    g_pidLists[pid].tail = idx;
    g_pidLists[pid].pageCount++;
}

// BRO-176: lock-free reflog dump (defined later); callers below already hold
// g_pmmLock, so they must use this variant — PmmDumpFreeLog re-takes the lock.
static int PmmDumpFreeLogLocked(uint64_t phys);

static inline void TrackAlloc(uint32_t pageIdx, MemTag tag, uint16_t pid)
{
    // Free pages are not in any list; just append to the new owner's list.
    if (!g_pageDescs) return;

    // BRO-179 cross-domain double-alloc detector (the SIG1 root). A frame pulled
    // from the free list MUST have a cleanly-freed descriptor: refCount==0 and
    // tag==Free (PmmFreePage resets both). If refCount!=0 or tag!=Free here, the
    // frame was still OWNED when the allocator handed it out — i.e. it is on the
    // free bitmap AND live in another domain at once. That is exactly the SIG1
    // signature: a frame simultaneously the kernel heap's (tag=Heap, which later
    // writes 0xDFDF kfree-poison) and a user's (this alloc), so the user reads
    // kernel heap poison. Causes include PmmRefPage resurrecting a free frame
    // (refCount 0→2 without setting the used bit) and any free of a still-owned
    // frame. Catch it HERE, naming the STALE owner (tag+pid) before we overwrite.
    {
        PageDescriptor& pd = Desc(pageIdx);
        uint8_t  preTag = pd.tag;
        int32_t  preRc  = pd.refCount;
        if (preRc != 0 || preTag != static_cast<uint8_t>(MemTag::Free))
        {
            static const char* kTagName[8] = {
                "Free","KernelCode","KernelData","PageTable",
                "Heap","Device","User","System"
            };
            SerialPrintf("BRO179-DOUBLEALLOC: phys=0x%lx handed to pid=%u (tag=%u) but "
                         "descriptor still OWNED: refCount=%d tag=%s ownerPid=%u — "
                         "cross-domain frame reuse (SIG1 0xDFDF source)!\n",
                         static_cast<uint64_t>(pageIdx) * PAGE_SIZE, (unsigned)pid,
                         (unsigned)tag, preRc,
                         preTag < 8 ? kTagName[preTag] : "?", (unsigned)pd.pid);
            PmmDumpFreeLogLocked(static_cast<uint64_t>(pageIdx) * PAGE_SIZE);
        }
    }

    ListAppend(pageIdx, pid, tag);
    Desc(pageIdx).refCount = 1;  // exclusive ownership on fresh allocation
    // BRO-176 stale-mapping detector (ALLOC side). A frame returned by the
    // allocator must have NO existing USER PTE mapping it — it was on the free
    // list. If mapCount is already nonzero here, some process still maps this
    // physical frame even though we are about to hand it to a NEW owner: a PTE
    // outlived its frame's free (the stale-mapping bug). The free-time check
    // MISSES this case because the stale mapping was uncounted in the freeing
    // process's generation and the doomed process's later teardown MapDec brings
    // the count back to 0. Catch it HERE, at the instant of the colliding
    // allocation, before we reset — naming both the new owner and the frame.
    uint16_t preMap = __atomic_load_n(&Desc(pageIdx).mapCount, __ATOMIC_RELAXED);
    if (preMap != 0)
    {
        SerialPrintf("BRO176-ALLOCMAPPED: phys=0x%lx handed to pid=%u but mapCount=%u "
                     "— still mapped by a stale PTE (freed-while-mapped, missed at free)!\n",
                     static_cast<uint64_t>(pageIdx) * PAGE_SIZE, (unsigned)pid,
                     (unsigned)preMap);
        PmmDumpFreeLogLocked(static_cast<uint64_t>(pageIdx) * PAGE_SIZE);
    }
    // A freshly (re)allocated frame has no mappers yet. Reset the map accounting
    // so a recycled page starts clean regardless of how it was freed.
    __atomic_store_n(&Desc(pageIdx).mapCount, 0, __ATOMIC_RELAXED);
    // BRO-179: log EVERY allocation tag (not just User) so a frame's free-log
    // trail reveals cross-domain transitions — e.g. a frame that goes
    // User→(free)→Heap→(free)→User, which is the SIG1 path where a recycled
    // user frame briefly served the kernel heap (and got 0xDFDF kfree-poison)
    // between two user lives. The tag name rides in the `site` string.
    static const char* kAllocSite[8] = {
        "alloc:Free","alloc:KCode","alloc:KData","alloc:PageTable",
        "alloc:Heap","alloc:Device","alloc:User","alloc:System"
    };
    uint8_t t = static_cast<uint8_t>(tag);
    RefLogRecord(static_cast<uint64_t>(pageIdx) * PAGE_SIZE, pid, REFOP_ALLOC, 1,
                 t < 8 ? kAllocSite[t] : "alloc:?");
}

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

// ---------------------------------------------------------------------------
// BRO-179: physical-frame free quarantine with an all-CPU TLB-barrier drain.
//
// Root class (proven): cross-domain physical-frame reuse / "freed-while-mapped".
// A frame is freed and immediately re-issued to a different domain (e.g. a freed
// user page becomes a kernel page-table frame) while a stale 4KiB TLB entry for
// its previous life still lives on some CPU; the old owner writes through it and
// corrupts the new use (reserved-bit #PF in a recycled PT frame; 0xDFDF heap
// poison in live structs — SIG1).
//
// The fix closes the window with a PROVABLE, throughput-independent invariant:
//   A freed frame is not returned to the allocatable pool until EVERY online CPU
//   has performed a full, unconditional TLB flush (CR3 reload) AFTER the free.
//
// Mechanism (batched drain — see DOCS.md design note):
//  * Free paths (PmmFreePage/PmmUnrefPage/PmmKillPid/PmmFreeByTag) do NOT release
//    a frame; they push its index into a quarantine buffer (the frame stays
//    bitmap-USED so the allocator skips it) via RetireFrameLocked, under g_pmmLock.
//  * A single dedicated kernel thread (PmmDrainThread, IF=1, no spinlocks held)
//    drains: it SNAPSHOTS the quarantine buffer under g_pmmLock into a private
//    batch and empties the live buffer; drops g_pmmLock; calls
//    TlbFlushAllCpusBarrier() (unconditional all-CPU flush, positive ack, panics
//    on timeout — never forgives); re-acquires g_pmmLock and releases the batch's
//    frames to the bitmap. Frames freed DURING the barrier land in the now-empty
//    live buffer and ride the NEXT drain — correct, because the barrier that frees
//    a frame must have STARTED after that frame's free.
//  * Single drainer: only PmmDrainThread drains, so the old per-free shootdown
//    livelock (many CPUs cross-contending the shootdown lock) cannot recur.
//  * Back-pressure: if the live buffer reaches a hard cap before the drainer
//    keeps up, the FREEING path that overflows falls back to immediate release
//    of the oldest entry (degrades to rate-based for that frame only) and flags
//    the drainer to run — bounded memory, never blocks the allocator with IF=0.
//
// LIMITATION: the barrier evicts already-cached stale entries; it does NOT save a
// frame freed while a still-VALID PTE points at it (a CPU re-walks and re-caches
// after the barrier). The captured signature is stale-TLB (PTE already removed),
// which this fixes. A live-PTE refcount undercount, if it exists, needs a
// separate fix; the reverse-map diagnostic would surface it.
// ---------------------------------------------------------------------------
// BRO-179 quarantine: a single descriptor-linked FIFO/LIFO of retired frames,
// threaded through each PageDescriptor's `next` field (free at retire time — the
// caller just reset the descriptor). No fixed-size array (so no BSS cost and no
// capacity ceiling), and "snapshot the whole batch" is an O(1) list splice. The
// drain thread accumulates frees for a short interval, then releases the WHOLE
// accumulated batch behind ONE all-CPU TLB-epoch barrier — amortizing the
// system-wide flush over many frees (per-batch barriers thrash every CPU's TLB
// and collapse throughput at high CPU counts).
static uint32_t g_quarHead  = PMM_NULL_PAGE;  // list of frames awaiting drain
static uint32_t g_quarCount = 0;              // length of g_quarHead
static uint32_t g_quarPeak  = 0;              // high-water mark (diagnostic)
#ifdef BROOK_HOST_TEST
static bool     g_quarantineOn = false;   // no drainer in host tests; release now
#else
// BRO-179 (fix B): quarantine ON. A freed frame is returned to the allocatable
// pool only after the drain thread has put it through an all-CPU TLB barrier —
// closing the cross-domain reuse window for EVERY local-only-invlpg free path
// (e.g. VmmFreePages freeing a shared kernel page with a local invlpg only, the
// proven BRO-179 root cause). The overflow list (below) guarantees no frame ever
// bypasses the barrier, even under burst pressure. (Set false for A/B testing.)
static bool     g_quarantineOn = true;
#endif

// BRO-179 forensic frame poison. When enabled, a frame that is actually freed is
// filled with a pattern that (a) keeps the recognizable 0xDFDF marker in the high
// 16 bits of EVERY 32-bit word — so existing 0xDFDF detectors/greps still fire —
// and (b) encodes, in the low 16 bits, the frame's ORIGINAL owner PID and the
// reflog sequence of its free, alternating word by word. A 64-bit field of a
// frame wrongly reused while still mapped then reads e.g. 0xDFDF1A2C_DFDF0005,
// decoding to "owner pid=5, free-seq=0x1A2C" — the corruption names its origin.
// Gated (diagnostic, ~4KiB write per free); off by default in production.
static bool g_framePoisonOn = false;
static constexpr uint32_t POISON_MARK = 0xDFDF0000u;

// Caller MUST hold g_pmmLock. Fill a freed RAM frame with the PID/seq-encoded
// pattern via the direct map. `ownerPid` is the frame's owner at free time;
// `seq` is the reflog sequence of this free (0 if reflog disabled).
static inline void PoisonFrameLocked(uint32_t idx, uint16_t ownerPid, uint32_t seq)
{
    if (!g_framePoisonOn) return;
    uint32_t* p = reinterpret_cast<uint32_t*>(
        PhysToVirt(PhysicalAddress(static_cast<uint64_t>(idx) * PAGE_SIZE)).raw());
    const uint32_t wpid = POISON_MARK | (ownerPid & 0xFFFFu);
    const uint32_t wseq = POISON_MARK | (seq & 0xFFFFu);
    for (uint32_t i = 0; i < PAGE_SIZE / 4; i += 2)
    {
        p[i]     = wpid;
        p[i + 1] = wseq;
    }
}

// Caller MUST hold g_pmmLock. Release a frame's bitmap bit + free accounting.
static inline void ReleaseFrameLocked(uint32_t idx)
{
    SetFree(idx);
    g_freePages++;
    if (idx < g_nextHint) g_nextHint = idx;
}

// Caller MUST hold g_pmmLock. Retire a frame that reached refcount 0 (descriptor
// already reset). Push it onto the quarantine list (threaded through the now-
// unused descriptor `next` link) instead of releasing it — it stays bitmap-USED
// so the allocator skips it until the drainer puts it through an all-CPU TLB
// barrier. `ownerPid` is the frame's owner captured BEFORE the descriptor reset,
// used to stamp the forensic poison (the "thing that owned it originally").
static inline void RetireFrameLocked(uint32_t idx, uint16_t ownerPid)
{
    PoisonFrameLocked(idx, ownerPid, g_freeLogSeq);
    // Pre-tracking / pre-SMP (g_pageDescs not yet allocated) OR quarantine off:
    // a single CPU is running before SMP, so no other CPU can hold a stale TLB
    // entry — the local invlpg already sufficed and immediate release is correct.
    if (!g_quarantineOn || !g_pageDescs)
    {
        ReleaseFrameLocked(idx);
        return;
    }
    Desc(idx).next = g_quarHead;     // frame stays bitmap-USED (not allocatable)
    g_quarHead = idx;
    g_quarCount++;
    if (g_quarCount > g_quarPeak) g_quarPeak = g_quarCount;
    // Liveness guard: the drainer should keep this list short. If it grows past
    // half of RAM the drainer is wedged — fail loud rather than silently exhaust
    // memory or be tempted to bypass the barrier (corruption). Fail loud.
    if (g_totalPages && g_quarCount > (g_totalPages / 2))
        KernelPanic("BRO-179: quarantine runaway (%u frames) — drain thread "
                    "wedged; refusing to bypass the TLB barrier", g_quarCount);
}

// The drain thread body. Runs forever in a kernel thread (IF=1, no spinlocks
// held across the barrier). See the design note above.
#ifndef BROOK_HOST_TEST
void TlbFlushAllCpusBarrier();   // apic.cpp (namespace brook)

// BRO-179 drain diagnostics (gated). Counts drain cycles and frames released so
// we can tell whether the drainer is running and keeping up vs being starved.
static bool     g_drainDebug   = false;
static uint64_t g_drainCycles  = 0;
static uint64_t g_drainedTotal = 0;

static void PmmDrainThread(void* /*arg*/)
{
    // Drain interval. The whole batch accumulated during this window is released
    // behind ONE all-CPU TLB-epoch barrier, amortizing the system-wide flush. A
    // per-batch barrier (draining continuously) forces every CPU to reload CR3
    // constantly and collapses throughput at high CPU counts; ~4ms batches keep
    // the all-CPU flush rate modest while bounding how long a freed frame waits.
    constexpr uint32_t DRAIN_INTERVAL_MS = 4;
    if (g_drainDebug)
        SerialPrintf("PMM-DRAIN: thread started\n");
    for (;;)
    {
        SchedulerSleepMs(DRAIN_INTERVAL_MS);

        // Splice out the entire accumulated batch in O(1) (descriptor-linked).
        uint64_t f = IrqSpinLockAcquire(&g_pmmLock);
        uint32_t head = g_quarHead;
        uint32_t n    = g_quarCount;
        g_quarHead = PMM_NULL_PAGE;
        g_quarCount = 0;
        IrqSpinLockRelease(&g_pmmLock, f);

        if (head == PMM_NULL_PAGE)
            continue;

        // One barrier for the whole batch: every frame in `head` was freed before
        // this point, so a single all-CPU flush since now covers all of them.
        // MUST run with no g_pmmLock held (it busy-waits for remote CPUs).
        TlbFlushAllCpusBarrier();

        // Now safe to return the batch's frames to the allocatable pool. Walk the
        // list (reading next BEFORE releasing, since release may let the frame be
        // reallocated and its descriptor reused).
        f = IrqSpinLockAcquire(&g_pmmLock);
        uint32_t idx = head;
        while (idx != PMM_NULL_PAGE)
        {
            uint32_t next = Desc(idx).next;
            ReleaseFrameLocked(idx);
            idx = next;
        }
        IrqSpinLockRelease(&g_pmmLock, f);

        if (g_drainDebug)
        {
            g_drainCycles++;
            g_drainedTotal += n;
            if ((g_drainCycles & 0x3F) == 0)   // every 64 cycles
                SerialPrintf("PMM-DRAIN: cycles=%lu drained=%lu last=%u peak=%u\n",
                             g_drainCycles, g_drainedTotal, n, g_quarPeak);
        }
    }
}

void PmmStartDrainThread()
{
    Process* t = KernelThreadCreate("pmm_drain", PmmDrainThread, nullptr, 2);
    if (t)
        SchedulerAddProcess(t);   // enqueue onto the ready queue (else it never runs)
    else
        KernelPanic("BRO-179: pmm_drain thread creation failed — quarantine cannot "
                    "drain, refusing to run with the frame-reuse gate disabled");
}
#else
// Host unit tests don't link the scheduler/apic; the quarantine drain is a
// no-op (RetireFrameLocked's safety-valve releases frames immediately).
void PmmStartDrainThread() {}
#endif

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

PhysicalAddress PmmAllocPage(MemTag tag, uint16_t pid)
{
    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);

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
            if (idx >= g_totalPages) { IrqSpinLockRelease(&g_pmmLock, pmmFlags); return PhysicalAddress{}; }

            SetUsed(idx);
            g_freePages--;
            g_nextHint = idx + 1;
            TrackAlloc(static_cast<uint32_t>(idx), tag, pid);
            IrqSpinLockRelease(&g_pmmLock, pmmFlags);

            return PhysicalAddress(idx * PAGE_SIZE);
        }
    }
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
    return PhysicalAddress{}; // out of memory
}

PhysicalAddress PmmAllocPages(uint64_t count, MemTag tag, uint16_t pid)
{
    if (count == 0) return PhysicalAddress{};
    if (count == 1) return PmmAllocPage(tag, pid);

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);

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
                for (uint64_t i = 0; i < count; i++)
                {
                    SetUsed(runStart + i);
                    g_freePages--;
                    TrackAlloc(static_cast<uint32_t>(runStart + i), tag, pid);
                }
                IrqSpinLockRelease(&g_pmmLock, pmmFlags);
                return PhysicalAddress(runStart * PAGE_SIZE);
            }
        }
        else
        {
            runLen = 0;
        }
    }
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
    return PhysicalAddress{}; // no contiguous run found
}

// BRO-176: forward declarations for the stale-mapping leak check (defined after
// PmmUnrefPage). Used by the free sites below.
extern "C" int PmmDumpFreeLog(uint64_t phys);
static inline void MapLeakCheckLocked(uint32_t idx, const char* site);

void PmmFreePage(PhysicalAddress physAddr)
{
    if (!physAddr) return;
    if ((physAddr.raw() & (PAGE_SIZE - 1)) != 0) return;

    uint64_t idx = physAddr.raw() / PAGE_SIZE;
    if (idx >= g_totalPages) return;

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);

    if (!IsUsed(idx)) { IrqSpinLockRelease(&g_pmmLock, pmmFlags); return; }

    // BRO-179 quarantine: a frame already retired (descriptor reset to Free) but
    // still bitmap-USED because it is sitting in the quarantine ring. A second
    // free of it must NOT re-quarantine (double free → frame released twice).
    if (g_pageDescs && Desc(static_cast<uint32_t>(idx)).tag
                       == static_cast<uint8_t>(MemTag::Free))
    {
        IrqSpinLockRelease(&g_pmmLock, pmmFlags);
        return;
    }

    // If refcounted and shared, just decrement — don't free yet. Also drop the
    // page from this owner's PID list: once a page is shared, the page-table
    // walk (FreeTableLevel) is the single freeing authority and decrements once
    // per mapper at each process's exit. Leaving the page on the original
    // owner's list would let PmmKillPid later free it out from under a live
    // co-owner (BRO-161). ListRemove is idempotent, so repeated unrefs are safe.
    if (g_pageDescs && Desc(static_cast<uint32_t>(idx)).refCount > 1)
    {
        auto& dd = Desc(static_cast<uint32_t>(idx));
        dd.refCount--;
        if (dd.tag == static_cast<uint8_t>(MemTag::User))
            RefLogRecord(physAddr.raw(), dd.pid, REFOP_DEC, dd.refCount, "PmmFreePage");
        ListRemove(static_cast<uint32_t>(idx));
        IrqSpinLockRelease(&g_pmmLock, pmmFlags);
        return;
    }

    uint16_t freedOwnerPid = KernelPid;
    if (g_pageDescs)
    {
        PageDescriptor& d = Desc(static_cast<uint32_t>(idx));
        freedOwnerPid = d.pid;
        MapLeakCheckLocked(static_cast<uint32_t>(idx), "PmmFreePage");
        // BRO-179: log frees of ALL tags (not just User) so the trail shows
        // kernel-domain (Heap/PageTable/KData) frees too — the cross-domain
        // transition we're hunting.
        static const char* kFreeSite[8] = {
            "free:Free","free:KCode","free:KData","free:PageTable",
            "free:Heap","free:Device","free:User","free:System"
        };
        FreeLogRecord(physAddr.raw(), d.pid,
                      d.tag < 8 ? kFreeSite[d.tag] : "free:?");
        ListRemove(static_cast<uint32_t>(idx));
        d.pid = 0;
        d.tag = static_cast<uint8_t>(MemTag::Free);
        d.refCount = 0;
        __atomic_store_n(&d.mapCount, 0, __ATOMIC_RELAXED);
    }

    // Retire (quarantine then release the oldest) — defers reuse so a stale TLB
    // entry for this frame is gone before it is handed to a new owner.
    RetireFrameLocked(static_cast<uint32_t>(idx), freedOwnerPid);

    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
}

MemTag PmmGetTag(PhysicalAddress physAddr)
{
    if (!g_pageDescs) return MemTag::KernelData;
    uint64_t idx = physAddr.raw() / PAGE_SIZE;
    if (idx >= g_totalPages) return MemTag::System;
    return static_cast<MemTag>(Desc(static_cast<uint32_t>(idx)).tag);
}

uint16_t PmmGetPid(PhysicalAddress physAddr)
{
    if (!g_pageDescs) return KernelPid;
    uint64_t idx = physAddr.raw() / PAGE_SIZE;
    if (idx >= g_totalPages) return KernelPid;
    return Desc(static_cast<uint32_t>(idx)).pid;
}

// ---------------------------------------------------------------------------
// COW reference counting
// ---------------------------------------------------------------------------

void PmmRefPage(PhysicalAddress physAddr)
{
    if (!g_pageDescs || !physAddr) return;
    uint64_t idx64 = physAddr.raw() / PAGE_SIZE;
    if (idx64 >= g_totalPages) return;
    uint32_t idx = static_cast<uint32_t>(idx64);

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);
    auto& d = Desc(idx);
    // BRO-179: a ref on a frame that is FREE in the bitmap is a resurrection bug —
    // the frame is on the free list yet we are taking a reference to it, so a
    // subsequent PmmAllocPage will hand it to a new owner while this reference
    // still treats it as live = cross-domain double-alloc (SIG1). Catch the
    // resurrection at its source, naming the caller's stale view.
    if (!IsUsed(idx))
    {
        SerialPrintf("BRO179-REFFREE: PmmRefPage on FREE frame phys=0x%lx "
                     "(refCount=%d tag=%u) — resurrecting a freed frame; next alloc "
                     "will double-own it (SIG1 source)!\n",
                     physAddr.raw(), d.refCount, (unsigned)d.tag);
        PmmDumpFreeLogLocked(physAddr.raw());
    }
    if (d.refCount == 0)
        d.refCount = 2;  // legacy page: count existing owner + new sharer
    else if (d.refCount < 255)
        d.refCount++;
    if (d.tag == static_cast<uint8_t>(MemTag::User))
        RefLogRecord(physAddr.raw(), d.pid, REFOP_REF, d.refCount, "PmmRefPage");
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
}

// BRO-179: atomically pin a frame ONLY if it is still live (used + refCount>0).
// Returns true and increments the refcount if the frame is alive; returns false
// (and does nothing) if the frame is already free. Unlike PmmRefPage this never
// RESURRECTS a freed frame. Used by VmmCowResolveWrite to pin the COW source
// across the page memcpy: the teardown unref path (FreeTableLevel→PmmUnrefPage,
// from ProcessDestroy) does NOT take g_userPtLock, so without this pin a
// concurrent unref could drop the source to 0, free it, and let it be recycled
// (to the kernel heap → 0xDFDF poison) while the memcpy reads it. The
// check+increment are one atomic step under g_pmmLock, closing the TOCTOU
// between the caller's PTE-present check and the copy.
bool PmmRefPageIfAlive(PhysicalAddress physAddr)
{
    if (!g_pageDescs || !physAddr) return false;
    uint64_t idx64 = physAddr.raw() / PAGE_SIZE;
    if (idx64 >= g_totalPages) return false;
    uint32_t idx = static_cast<uint32_t>(idx64);

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);
    auto& d = Desc(idx);
    if (!IsUsed(idx) || d.refCount == 0)
    {
        IrqSpinLockRelease(&g_pmmLock, pmmFlags);
        return false;  // already free — do NOT resurrect
    }
    if (d.refCount < 255)
        d.refCount++;
    if (d.tag == static_cast<uint8_t>(MemTag::User))
        RefLogRecord(physAddr.raw(), d.pid, REFOP_REF, d.refCount, "PmmRefPin");
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
    return true;
}

void PmmUnrefPage(PhysicalAddress physAddr)
{
    if (!g_pageDescs || !physAddr) return;
    uint64_t idx64 = physAddr.raw() / PAGE_SIZE;
    if (idx64 >= g_totalPages) return;
    uint32_t idx = static_cast<uint32_t>(idx64);

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);
    auto& d = Desc(idx);
    // BRO-179 quarantine: already retired (tag reset to Free) but still
    // bitmap-USED while it sits in the quarantine ring — a further unref must
    // not retire it again (double free).
    if (IsUsed(idx) && d.tag == static_cast<uint8_t>(MemTag::Free)
                    && d.refCount == 0)
    {
        IrqSpinLockRelease(&g_pmmLock, pmmFlags);
        return;
    }
    if (d.refCount > 1)
    {
        d.refCount--;
        if (d.tag == static_cast<uint8_t>(MemTag::User))
            RefLogRecord(physAddr.raw(), d.pid, REFOP_DEC, d.refCount, "PmmUnrefPage");
        // Drop the page from its owner's PID list — see PmmFreePage and BRO-161.
        // Once shared, freeing is driven solely by the page-table walk (one
        // unref per mapper); leaving it listed lets PmmKillPid free it under a
        // live co-owner. ListRemove is idempotent.
        ListRemove(idx);
        IrqSpinLockRelease(&g_pmmLock, pmmFlags);
        return; // still shared, don't free
    }
    // refCount is 0 or 1 — this was the last (or only) reference, actually free
    bool retire = false;
    uint16_t freedOwnerPid = d.pid;
    if (IsUsed(idx))
    {
        MapLeakCheckLocked(idx, "PmmUnrefPage");
        if (d.tag == static_cast<uint8_t>(MemTag::User))
            FreeLogRecord(physAddr.raw(), d.pid, "PmmUnrefPage");
        retire = true;
    }
    ListRemove(idx);
    d = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0,
          static_cast<uint8_t>(MemTag::Free), 0, 0 };
    // Quarantine the frame (defer reuse) instead of releasing it immediately,
    // so a stale TLB entry is gone before the frame is handed to a new owner.
    if (retire)
        RetireFrameLocked(idx, freedOwnerPid);
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
}

// BRO-176/179 stale-mapping detector --------------------------------------------
// Called at an ACTUAL free (refCount reached 0) while holding g_pmmLock. If a
// frame is freed while a present USER PTE still maps it (mapCount != 0), the
// mapping outlived its reference — the BRO-176 stale-mapping bug. Name it
// red-handed, at the instant of the erroneous free, with its full ref/unref
// trail, BEFORE the page is recycled and poison is ever read.
//
// BRO-179: this checks ALL tags, not just User. mapCount only counts USER PTEs
// (PmmMapInc/Dec fire on user-PTE install/remove), so a NON-User frame (e.g.
// Heap — the observed SIG1 victim is a Process struct in a Heap frame) with
// mapCount != 0 means that frame is simultaneously a kernel object AND user-
// mapped: the cross-domain double-ownership we are hunting. The old User-only
// guard structurally skipped exactly the frame that got corrupted.
static inline void MapLeakCheckLocked(uint32_t idx, const char* site)
{
    PageDescriptor& d = Desc(idx);
    uint16_t mc = __atomic_load_n(&d.mapCount, __ATOMIC_RELAXED);
    if (mc != 0)
    {
        static const char* kTagName[8] = {
            "Free","KCode","KData","PageTable","Heap","Device","User","System" };
        SerialPrintf("BRO176-MAPLEAK: phys=0x%lx FREED via %s with mapCount=%u still "
                     "mapped (owner pid=%u tag=%s refCount=%u) — PTE outlived its "
                     "reference (cross-domain if tag!=User)!\n",
                     static_cast<uint64_t>(idx) * PAGE_SIZE, site, (unsigned)mc,
                     (unsigned)d.pid, d.tag < 8 ? kTagName[d.tag] : "?",
                     (unsigned)d.refCount);
        PmmDumpFreeLogLocked(static_cast<uint64_t>(idx) * PAGE_SIZE);
    }
}

// PmmMapInc/PmmMapDec — O(1) USER-PTE map accounting (see header). Updated under
// the VMM page-table lock (g_userPtLock), which is independent of g_pmmLock, so
// mapCount is touched atomically everywhere.
void PmmMapInc(PhysicalAddress physAddr)
{
    if (!g_pageDescs || !physAddr) return;
    uint64_t idx = physAddr.raw() / PAGE_SIZE;
    if (idx >= g_totalPages) return;
    __atomic_add_fetch(&Desc(static_cast<uint32_t>(idx)).mapCount, 1, __ATOMIC_RELAXED);
}

void PmmMapDec(PhysicalAddress physAddr)
{
    if (!g_pageDescs || !physAddr) return;
    uint64_t idx = physAddr.raw() / PAGE_SIZE;
    if (idx >= g_totalPages) return;
    PageDescriptor& d = Desc(static_cast<uint32_t>(idx));
    if (__atomic_load_n(&d.mapCount, __ATOMIC_RELAXED) == 0)
    {
        // Unmap without a matching map — the opposite-end accounting bug from a
        // leak (a PTE removed twice, or removed for a frame it never mapped).
        SerialPrintf("BRO176-MAPUNDERFLOW: phys=0x%lx mapDec at mapCount=0 "
                     "(pid=%u tag=%u)\n", physAddr.raw(), (unsigned)d.pid, (unsigned)d.tag);
        return;
    }
    __atomic_sub_fetch(&d.mapCount, 1, __ATOMIC_RELAXED);
}

uint8_t PmmGetRefCount(PhysicalAddress physAddr)
{
    if (!g_pageDescs || !physAddr) return 0;
    uint64_t idx64 = physAddr.raw() / PAGE_SIZE;
    if (idx64 >= g_totalPages) return 0;
    return Desc(static_cast<uint32_t>(idx64)).refCount;
}

// BRO-176 crash-time discriminator: report the PMM's current view of a frame so
// the user-#GP sweep can tell apart (a) frame still owned by a USER process
// (stale PTE / shared), (b) frame currently FREE in the bitmap (freed-while-
// mapped), or (c) frame owned by the KERNEL HEAP (tag=Heap) = a frame reachable
// by a user mapping AND the kernel heap at once (the 0xDF source). Lock-free
// read — intended for the fault path only.
extern "C" void PmmDescribe(uint64_t phys, uint32_t* used, uint32_t* refCount,
                            uint32_t* mapCount, uint32_t* tag, uint32_t* ownerPid)
{
    uint64_t idx64 = phys / PAGE_SIZE;
    if (!g_pageDescs || idx64 >= g_totalPages)
    {
        if (used) *used = 0xFF;
        return;
    }
    uint32_t idx = static_cast<uint32_t>(idx64);
    PageDescriptor& d = Desc(idx);
    if (used)     *used     = IsUsed(idx) ? 1u : 0u;
    if (refCount) *refCount = d.refCount;
    if (mapCount) *mapCount = __atomic_load_n(&d.mapCount, __ATOMIC_RELAXED);
    if (tag)      *tag      = d.tag;
    if (ownerPid) *ownerPid = d.pid;
}

void PmmEnableTracking()
{
    // Allocate descriptor array via VmmAllocPages — too large for a single
    // kmalloc call (g_totalPages * 12B can be ~768KB). As a permanent
    // system-lifetime allocation, bypassing the heap is appropriate.
    static constexpr uint64_t PAGE_SIZE_LOCAL = 4096;
    static constexpr uint64_t VMM_WRITABLE_LOCAL = (1ULL << 1);
    uint64_t descBytes = g_totalPages * sizeof(PageDescriptor);
    uint64_t descPages = (descBytes + PAGE_SIZE_LOCAL - 1) / PAGE_SIZE_LOCAL;
    VirtualAddress descVirt = VmmAllocPages(descPages, VMM_WRITABLE_LOCAL,
                                            MemTag::KernelData, KernelPid);
    g_pageDescs = reinterpret_cast<PageDescriptor*>(descVirt.raw());

    if (!descVirt)
    {
        g_pageDescs = nullptr;
        SerialPuts("PMM: WARNING: tracking allocation failed — ownership disabled\n");
        return;
    }

    // Initialise all lists to empty.
    for (uint32_t i = 0; i < PMM_MAX_PIDS; i++)
        g_pidLists[i] = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0, 0 };

    // Initialise all descriptors to a known state before building lists.
    for (uint32_t i = 0; i < static_cast<uint32_t>(g_totalPages); i++)
    {
        g_pageDescs[i] = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0,
                           static_cast<uint8_t>(MemTag::Free), 0, 0 };
    }

    // Backfill: add used pages to KernelPid's list; free pages are left
    // out of all lists (the bitmap IS the free pool — no list needed).
    uint32_t usedCount = 0, freeCount = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(g_totalPages); i++)
    {
        if (IsUsed(i))
        {
            ListAppend(i, KernelPid, MemTag::KernelData);
            Desc(i).refCount = 1;  // exclusive owner
            usedCount++;
        }
        else
        {
            // Free page: descriptor stays initialised to Free/0/PMM_NULL_PAGE.
            freeCount++;
        }
    }

    SerialPrintf("PMM: tracking enabled — %u pages (%u used, %u free), "
                 "descriptors: %u KB\n",
                 static_cast<uint32_t>(g_totalPages),
                 usedCount, freeCount,
                 static_cast<uint32_t>(g_totalPages * sizeof(PageDescriptor) / 1024));

    // BRO-179 (fix B validated): root cause was VmmFreePages (and ~8 sibling VMM
    // paths) doing a LOCAL-only invlpg when freeing a shared kernel page, leaving
    // stale kernel translations on other CPUs that corrupt the recycled frame.
    // Fix: quarantine ON (the all-CPU TLB-barrier drain, with the overflow list
    // closing the old release-without-barrier bypass) gates frame REUSE for every
    // such path at once. Keep the PMM reflog ON: it is the diagnostic that named
    // this root cause (the fault-time frame-history dump) and its cost is modest;
    // it stays as a permanent safety net for this expensive bug class. Frame
    // poison is OFF — its job (proving cross-domain reuse) is done and the 4 KiB
    // fill per free is too costly for steady state. (Set g_framePoisonOn=true and
    // g_quarantineOn=false to re-arm a forensic A/B hunt.)
    g_freeLogOn = true;
    g_framePoisonOn = false;
#ifndef BROOK_HOST_TEST
    g_drainDebug = false;  // drainer verified to run/keep up; enable for diagnosis
#endif

    // Silence the high-frequency hot-path serial logs (TLB-shootdown forgiveness,
    // per-execve PROFILE, compositor stats). At 64 CPUs these per-operation prints
    // throttle fork/exit throughput to serial speed; the reflog provenance is
    // captured in-memory and only printed at a fault. (Revert to restore verbose.)
    g_hotLogQuiet = true;
}

// BRO-176 diagnostic: print every recorded free of `phys` (most recent first).
// Quiet on miss (so it can be called per-leaf during a whole-page-table sweep).
// Returns the number of matching records found. Takes g_pmmLock.
//
// Uses RAW serial port polling rather than SerialPrintf: this is called from the
// #GP/#PF crash handler AFTER ExcForceSerialLock sets g_panicInProgress, which
// silences SerialPrintf/SerialPuts. Raw port writes bypass that so the owner/site
// (the whole point of the free-log) actually reaches the serial log.
static inline void FlRawChar(char c)
{
    if (c == '\n') {
        while ((inb(0x3FD) & 0x20) == 0) {}
        outb(0x3F8, '\r');
    }
    while ((inb(0x3FD) & 0x20) == 0) {}
    outb(0x3F8, static_cast<uint8_t>(c));
}
static inline void FlRawStr(const char* s) { if (s) while (*s) FlRawChar(*s++); }
static inline void FlRawHex(uint64_t v)
{
    FlRawChar('0'); FlRawChar('x');
    for (int sh = 60; sh >= 0; sh -= 4) {
        int n = (int)((v >> sh) & 0xF);
        FlRawChar((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}
static inline void FlRawDec(uint64_t v)
{
    char b[20]; int i = 0;
    if (!v) b[i++] = '0';
    while (v) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) FlRawChar(b[--i]);
}

// Lock-free reflog dump — caller MUST hold g_pmmLock. Used by the leak/alloc
// checks which run inside the PMM critical section (re-taking g_pmmLock here
// would self-deadlock the non-recursive ticket lock).
static int PmmDumpFreeLogLocked(uint64_t phys)
{
    static const char* opName[4] = { "ALLOC", "REF  ", "DEC  ", "FREE " };
    uint64_t target = phys & ~0xFFFULL;
    int found = 0;
    uint32_t total = g_freeLogSeq;
    uint32_t scan = (total < FREELOG_SIZE) ? total : FREELOG_SIZE;
    static constexpr int MAXSHOW = 24;
    uint32_t hits[MAXSHOW]; int nh = 0;
    for (uint32_t n = 1; n <= scan && nh < MAXSHOW; ++n)
    {
        uint32_t i = (g_freeLogSeq - n) & (FREELOG_SIZE - 1);
        FreeRec& r = g_freeLog[i];
        if (r.phys != target || !r.site) continue;
        hits[nh++] = i;
    }
    for (int k = nh - 1; k >= 0; --k)
    {
        FreeRec& r = g_freeLog[hits[k]];
        FlRawStr("  BRO176-REFLOG phys="); FlRawHex(r.phys);
        FlRawStr(" seq="); FlRawDec(r.seq);
        FlRawStr(" "); FlRawStr(r.op < 4 ? opName[r.op] : "?");
        FlRawStr(" ->count="); FlRawDec(r.count);
        FlRawStr(" pid="); FlRawDec(r.ownerPid);
        // r.site is a string literal in kernel .rodata. The fault-context
        // (no-lock) caller can race a writer mid-update, so validate the pointer
        // is in kernel range before dereferencing it — a torn read must never
        // send FlRawStr off into unmapped memory (nested fault).
        {
            uint64_t sp = reinterpret_cast<uint64_t>(r.site);
            FlRawStr(" ");
            if (sp >= 0xffffffff80000000ULL)
                FlRawStr(r.site);
            else
                FlRawStr("<site?>");
        }
        FlRawChar('\n');
        // BRO-179: the dynamic callstack of this alloc/free, symbolized — names
        // WHO performed the operation (the cheap mapCount counters can't).
        for (uint8_t s = 0; s < r.stackDepth; ++s)
        {
            FlRawStr("      #"); FlRawDec(s); FlRawStr(" ");
            FlRawHex(r.stack[s]);
            const char* nm = nullptr; uint64_t off = 0;
#ifndef BROOK_HOST_TEST
            if (KsymFindByAddr(r.stack[s], &nm, &off) && nm)
            {
                FlRawStr("  "); FlRawStr(nm); FlRawStr("+0x"); FlRawHex(off);
            }
#else
            (void)nm; (void)off;
#endif
            FlRawChar('\n');
        }
        ++found;
    }
    return found;
}

// BRO-179: decode a forensic-poison qword seen at a crime scene. If either 32-bit
// half carries the 0xDFDF marker, print the embedded owner PID / free-seq and
// dump that frame's full provenance. `addrParity` (bit 3 of the field's address,
// i.e. which 8-byte slot) disambiguates which half is pid vs seq when known;
// pass UINT32_MAX if unknown (both interpretations are printed). Caller must hold
// g_pmmLock (it calls PmmDumpFreeLogLocked).
static bool PmmDecodePoisonLocked(uint64_t qword)
{
    uint32_t lo = static_cast<uint32_t>(qword & 0xFFFFFFFFu);
    uint32_t hi = static_cast<uint32_t>(qword >> 32);
    bool loMark = (lo & 0xFFFF0000u) == POISON_MARK;
    bool hiMark = (hi & 0xFFFF0000u) == POISON_MARK;
    if (!loMark && !hiMark) return false;
    // Frame fill is [pid, seq, pid, seq, ...] in ascending words, so the lower-
    // addressed 32-bit half is the PID and the higher is the SEQ.
    uint16_t pid = static_cast<uint16_t>(lo & 0xFFFFu);
    uint16_t seq = static_cast<uint16_t>(hi & 0xFFFFu);
    FlRawStr("  BRO179-POISON-DECODE qword="); FlRawHex(qword);
    FlRawStr(" -> owner pid="); FlRawDec(pid);
    FlRawStr(" free-seq(low16)="); FlRawHex(seq);
    FlRawChar('\n');
    // Find the matching free record by its low-16 seq and dump that frame's
    // history (which names the exact alloc/free callstacks).
    uint32_t scan = (g_freeLogSeq < FREELOG_SIZE) ? g_freeLogSeq : FREELOG_SIZE;
    for (uint32_t n = 1; n <= scan; ++n)
    {
        uint32_t i = (g_freeLogSeq - n) & (FREELOG_SIZE - 1);
        FreeRec& r = g_freeLog[i];
        if ((r.seq & 0xFFFFu) == seq && r.ownerPid == pid && r.site)
        {
            FlRawStr("  BRO179-POISON matches reflog seq="); FlRawDec(r.seq);
            FlRawStr(" phys="); FlRawHex(r.phys); FlRawChar('\n');
            PmmDumpFreeLogLocked(r.phys);
            return true;
        }
    }
    FlRawStr("  BRO179-POISON: no live reflog record for that seq (ring wrapped)\n");
    return true;
}

extern "C" bool PmmDecodePoison(uint64_t qword)
{
    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);
    bool decoded = PmmDecodePoisonLocked(qword);
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
    return decoded;
}

extern "C" int PmmDumpFreeLog(uint64_t phys)
{
    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);
    int found = PmmDumpFreeLogLocked(phys);
    IrqSpinLockRelease(&g_pmmLock, pmmFlags);
    return found;
}

// BRO-179 fault-context reflog dump. Does NOT take g_pmmLock: an exception
// handler runs at IF=0, and after SmpHaltAllAPs a remote CPU may have been
// halted while holding g_pmmLock — taking it here would deadlock. Caller MUST
// guarantee all other CPUs are halted (one-shot crash dump). The ring read is
// racy in theory but the records for a frame freed before the fault are stable.
extern "C" int PmmDumpFreeLogNoLock(uint64_t phys)
{
    return PmmDumpFreeLogLocked(phys);
}

// Fault-context poison decode (no g_pmmLock; APs halted). Same contract as
// PmmDumpFreeLogNoLock.
extern "C" bool PmmDecodePoisonNoLock(uint64_t qword)
{
    return PmmDecodePoisonLocked(qword);
}

void PmmKillPid(uint16_t pid)
{
    if (!g_pageDescs) return;
    if (pid == KernelPid) return;  // never kill kernel pages

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);

    uint32_t idx = g_pidLists[pid].head;
    [[maybe_unused]] uint32_t count = 0;
    [[maybe_unused]] uint32_t shared = 0;

    while (idx != PMM_NULL_PAGE)
    {
        uint32_t next = Desc(idx).next;

        if (Desc(idx).refCount > 1)
        {
            // COW shared page — decrement refcount, remove from this PID's list
            Desc(idx).refCount--;
            if (Desc(idx).tag == static_cast<uint8_t>(MemTag::User))
                RefLogRecord(static_cast<uint64_t>(idx) * PAGE_SIZE, pid, REFOP_DEC,
                             Desc(idx).refCount, "PmmKillPid");
            ListRemove(idx);  // update neighbours so list stays consistent
            shared++;
        }
        else
        {
            // Exclusive page — actually free it (via quarantine)
            bool retire = false;
            if (IsUsed(idx))
            {
                MapLeakCheckLocked(idx, "PmmKillPid");
                if (Desc(idx).tag == static_cast<uint8_t>(MemTag::User))
                    FreeLogRecord(static_cast<uint64_t>(idx) * PAGE_SIZE, pid, "PmmKillPid");
                retire = true;
            }
            Desc(idx) = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0,
                          static_cast<uint8_t>(MemTag::Free), 0, 0 };
            if (retire)
                RetireFrameLocked(idx, pid);
        }
        count++;

        idx = next;
    }

    g_pidLists[pid] = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0, 0 };

    IrqSpinLockRelease(&g_pmmLock, pmmFlags);

    DbgPrintf("PMM: PmmKillPid(%u): processed %u pages (%u shared, refcount decremented)\n",
                 static_cast<uint32_t>(pid), count, shared);
}

void PmmFreeByTag(uint16_t pid, MemTag tag)
{
    if (!g_pageDescs) return;
    if (pid == KernelPid) return;

    uint64_t pmmFlags = IrqSpinLockAcquire(&g_pmmLock);

    uint32_t idx = g_pidLists[pid].head;
    uint32_t count = 0;

    while (idx != PMM_NULL_PAGE)
    {
        uint32_t next = Desc(idx).next;

        if (static_cast<MemTag>(Desc(idx).tag) == tag && IsUsed(idx))
        {
            // Remove from PID list
            uint32_t prev = Desc(idx).prev;
            if (prev != PMM_NULL_PAGE)
                Desc(prev).next = next;
            else
                g_pidLists[pid].head = next;
            if (next != PMM_NULL_PAGE)
                Desc(next).prev = prev;
            else
                g_pidLists[pid].tail = prev;
            g_pidLists[pid].pageCount--;

            // Free the page (via quarantine)
            Desc(idx) = { PMM_NULL_PAGE, PMM_NULL_PAGE, 0,
                          static_cast<uint8_t>(MemTag::Free), 0, 0 };
            RetireFrameLocked(idx, pid);
            count++;
        }

        idx = next;
    }

    IrqSpinLockRelease(&g_pmmLock, pmmFlags);

    (void)count;
    DbgPrintf("PMM: PmmFreeByTag(%u, %u): freed %u pages\n",
              static_cast<uint32_t>(pid), static_cast<uint32_t>(tag), count);
}

void PmmEnumeratePid(uint16_t pid,
                     bool (*callback)(PhysicalAddress physAddr, MemTag tag, void* ctx),
                     void* ctx)
{
    if (!g_pageDescs) return;

    uint32_t idx = g_pidLists[pid].head;
    while (idx != PMM_NULL_PAGE)
    {
        uint32_t next = Desc(idx).next;
        if (!callback(PhysicalAddress(static_cast<uint64_t>(idx) * PAGE_SIZE),
                      static_cast<MemTag>(Desc(idx).tag), ctx))
            break;
        idx = next;
    }
}

void PmmDumpPidStats()
{
    if (!g_pageDescs)
    {
        SerialPuts("PMM: tracking not enabled\n");
        return;
    }
    SerialPuts("PMM: per-PID page counts:\n");
    for (uint32_t p = 0; p < PMM_MAX_PIDS; p++)
    {
        if (g_pidLists[p].pageCount > 0)
            SerialPrintf("  PID %u: %u pages\n", p, g_pidLists[p].pageCount);
    }
}

uint64_t PmmGetFreePageCount()  { return g_freePages;  }
uint64_t PmmGetTotalPageCount() { return g_totalPages; }

} // namespace brook
