#include "sync/krwlock.h"
#include "process.h"
#include "scheduler.h"
#ifndef BROOK_HOST_TEST
#include "serial.h"
// BRO-179: defined in process.cpp; dumps the Process free-log entry for a
// pointer (freeing caller symbol + incarnation). Used by the UAF guard below.
extern "C" int ProcessDumpFreeLog(void* ptr);

// BRO-179: in-flight tracker for KRwLockCleanupOnExit. FreeProcessStruct
// (process.cpp) consults this to detect a concurrent free racing the cleanup —
// the TOCTOU that lets cleanup read heap kfree-poison out of a just-freed
// Process struct (observed: kernel #GP, p->heldWriteLock = 0xDFDF..., magic
// already validated). Migration-proof: slots hold the proc POINTER, not a CPU
// id, so a thread that migrates between claim and release still releases its
// own slot. Concurrent cleanups are bounded by CPU count, so MAX_CPUS slots
// always suffice.
static brook::Process* volatile g_rwCleanupInFlight[64];
static volatile int g_rwCleanupInFlightCount = 0;

static inline int RwCleanupInFlightClaim(brook::Process* p) {
    for (uint32_t i = 0; i < 64; ++i) {
        brook::Process* expected = nullptr;
        if (__atomic_compare_exchange_n(&g_rwCleanupInFlight[i], &expected, p,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            __atomic_add_fetch(&g_rwCleanupInFlightCount, 1, __ATOMIC_ACQ_REL);
            return static_cast<int>(i);
        }
    }
    return -1;
}
static inline void RwCleanupInFlightRelease(int slot) {
    if (slot >= 0) {
        __atomic_store_n(&g_rwCleanupInFlight[slot], nullptr, __ATOMIC_RELEASE);
        __atomic_sub_fetch(&g_rwCleanupInFlightCount, 1, __ATOMIC_ACQ_REL);
    }
}
extern "C" int ProcessIsRwCleanupInFlight(void* p) {
    // O(1) when nothing is in-flight (the overwhelmingly-common case).
    if (__atomic_load_n(&g_rwCleanupInFlightCount, __ATOMIC_ACQUIRE) == 0)
        return 0;
    for (uint32_t i = 0; i < 64; ++i)
        if (__atomic_load_n(&g_rwCleanupInFlight[i], __ATOMIC_ACQUIRE)
            == reinterpret_cast<brook::Process*>(p))
            return 1;
    return 0;
}
#endif

namespace brook {

// ---------------------------------------------------------------------------
// Interrupt-flag save/disable + restore.
//
// On the kernel target these are the privileged cli/sti. Under host unit tests
// (BROOK_HOST_TEST) they compile to no-ops so the real lock logic in this file
// can be exercised in user space (cli/sti would #GP in ring 3). Kernel builds
// never define BROOK_HOST_TEST, so their codegen is unchanged.
// ---------------------------------------------------------------------------

static inline uint64_t RwSaveAndDisableIrq()
{
#ifdef BROOK_HOST_TEST
    return 0;
#else
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
#endif
}

static inline void RwRestoreIrq(uint64_t savedFlags)
{
#ifdef BROOK_HOST_TEST
    (void)savedFlags;
#else
    if (savedFlags & 0x200)
        __asm__ volatile("sti" ::: "memory");
#endif
}

// ---------------------------------------------------------------------------
// Internal guard lock (same pattern as KMutex)
// ---------------------------------------------------------------------------

static inline uint64_t RwGuardAcquire(KRwLock* rw)
{
    uint64_t flags = RwSaveAndDisableIrq();
    uint32_t ticket = __atomic_fetch_add(&rw->guardNext, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&rw->guardServing, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");
    return flags;
}

static inline void RwGuardRelease(KRwLock* rw, uint64_t savedFlags)
{
    __atomic_fetch_add(&rw->guardServing, 1, __ATOMIC_RELEASE);
    RwRestoreIrq(savedFlags);
}

extern Process* SchedulerCurrentProcess();

// ---------------------------------------------------------------------------
// Wait queue helpers
// ---------------------------------------------------------------------------

static inline void Enqueue(Process*& head, Process*& tail, Process* p)
{
    p->syncNext = nullptr;
    if (tail) tail->syncNext = p;
    else      head = p;
    tail = p;
}

static inline Process* Dequeue(Process*& head, Process*& tail)
{
    Process* p = head;
    if (!p) return nullptr;
    head = p->syncNext;
    if (!head) tail = nullptr;
    p->syncNext = nullptr;
    return p;
}

// ---------------------------------------------------------------------------
// KRwLock API
// ---------------------------------------------------------------------------

void KRwLockInit(KRwLock* rw)
{
    rw->readerCount    = 0;
    rw->writerActive   = 0;
    rw->writersWaiting = 0;
    rw->readWaitHead   = nullptr;
    rw->readWaitTail   = nullptr;
    rw->writeWaitHead  = nullptr;
    rw->writeWaitTail  = nullptr;
    rw->guardNext      = 0;
    rw->guardServing   = 0;
}

void KRwLockReadLock(KRwLock* rw)
{
    Process* self = SchedulerCurrentProcess();
    uint64_t flags = RwGuardAcquire(rw);

    // Grant immediately if no writer is active and none waiting.
    if (!rw->writerActive && rw->writersWaiting == 0)
    {
        rw->readerCount++;
        // BRO-162: record read ownership so KRwLockCleanupOnExit can drop this
        // reference if the thread exits while holding it.
        if (self) self->heldReadLock = rw;
        RwGuardRelease(rw, flags);
        return;
    }

    // Must block — need a valid process context.
    if (!self) {
        // No process context (early boot / ISR) — grant anyway to avoid
        // underflow when ReadUnlock is called later.  This is safe only
        // because the writer that triggered the wait is in a process that
        // hasn't been scheduled yet.
        rw->readerCount++;
        RwGuardRelease(rw, flags);
        return;
    }

    // Block — enqueue on reader wait list.
    self->blockedOnRwLock = rw;
    self->blockedAsWriter = false;
    __atomic_store_n(&self->pendingWakeup, 0, __ATOMIC_RELEASE);
    Enqueue(rw->readWaitHead, rw->readWaitTail, self);
    RwGuardRelease(rw, flags);
    SchedulerBlock(self);
    // Woken — the waker already incremented readerCount and recorded
    // heldReadLock on our behalf (BRO-162); just clear the blocked marker.
    self->blockedOnRwLock = nullptr;
}

void KRwLockReadUnlock(KRwLock* rw)
{
    // BRO-162: drop recorded read ownership (mirrors heldWriteLock handling).
    Process* self = SchedulerCurrentProcess();
    if (self && self->heldReadLock == rw)
        self->heldReadLock = nullptr;

    uint64_t flags = RwGuardAcquire(rw);
    rw->readerCount--;

    // If last reader and writers are waiting, wake the next writer.
    if (rw->readerCount == 0 && rw->writeWaitHead)
    {
        Process* writer = Dequeue(rw->writeWaitHead, rw->writeWaitTail);
        rw->writerActive = 1;
        rw->writersWaiting--;
        // BRO-162: record ownership on the woken writer BEFORE unblock so a kill
        // in the unblocked-but-not-yet-scheduled window is recoverable by cleanup.
        writer->heldWriteLock = rw;
        writer->blockedOnRwLock = nullptr;
        __atomic_store_n(&writer->pendingWakeup, 1, __ATOMIC_RELEASE);
        RwGuardRelease(rw, flags);
        SchedulerUnblock(writer);
        return;
    }

    RwGuardRelease(rw, flags);
}

void KRwLockWriteLock(KRwLock* rw)
{
    uint64_t flags = RwGuardAcquire(rw);

    // Grant immediately if no readers and no writer active.
    if (rw->readerCount == 0 && !rw->writerActive)
    {
        rw->writerActive = 1;
        RwGuardRelease(rw, flags);
        // Track ownership.
        Process* self = SchedulerCurrentProcess();
        if (self) self->heldWriteLock = rw;
        return;
    }

    // Must block — need a valid process context.
    Process* self = SchedulerCurrentProcess();
    if (!self) {
        // No process context — cannot sleep.  Force-grant to avoid hang.
        // This should only happen during very early boot before any
        // contention is possible.
        rw->writerActive = 1;
        RwGuardRelease(rw, flags);
        return;
    }

    // Block — enqueue on writer wait list.
    rw->writersWaiting++;
    self->blockedOnRwLock = rw;
    self->blockedAsWriter = true;
    __atomic_store_n(&self->pendingWakeup, 0, __ATOMIC_RELEASE);
    Enqueue(rw->writeWaitHead, rw->writeWaitTail, self);
    RwGuardRelease(rw, flags);
    SchedulerBlock(self);
    // Woken — we now hold the write lock.
    self->blockedOnRwLock = nullptr;
    self->heldWriteLock = rw;
}

void KRwLockWriteUnlock(KRwLock* rw)
{
    // Clear ownership tracking.
    Process* self = SchedulerCurrentProcess();
    if (self && self->heldWriteLock == rw)
        self->heldWriteLock = nullptr;

    uint64_t flags = RwGuardAcquire(rw);
    rw->writerActive = 0;

    // Prefer waking all queued readers (batch wakeup) over a single writer,
    // unless no readers are waiting.
    if (rw->readWaitHead)
    {
        // Collect all waiting readers.
        Process* readers[128];
        uint32_t count = 0;
        while (rw->readWaitHead && count < 128)
        {
            readers[count] = Dequeue(rw->readWaitHead, rw->readWaitTail);
            // BRO-162: record the grant on the woken reader BEFORE unblocking so
            // a kill in the unblocked-but-not-scheduled window is recoverable by
            // KRwLockCleanupOnExit (read case) instead of leaking readerCount.
            readers[count]->heldReadLock = rw;
            readers[count]->blockedOnRwLock = nullptr;
            __atomic_store_n(&readers[count]->pendingWakeup, 1, __ATOMIC_RELEASE);
            count++;
            rw->readerCount++;
        }
        RwGuardRelease(rw, flags);

        // Wake them all outside the guard.
        for (uint32_t i = 0; i < count; ++i)
            SchedulerUnblock(readers[i]);
        return;
    }

    // No readers — wake next writer if any.
    if (rw->writeWaitHead)
    {
        Process* writer = Dequeue(rw->writeWaitHead, rw->writeWaitTail);
        rw->writerActive = 1;
        rw->writersWaiting--;
        // BRO-162: record ownership on the woken writer before unblock.
        writer->heldWriteLock = rw;
        writer->blockedOnRwLock = nullptr;
        __atomic_store_n(&writer->pendingWakeup, 1, __ATOMIC_RELEASE);
        RwGuardRelease(rw, flags);
        SchedulerUnblock(writer);
        return;
    }

    RwGuardRelease(rw, flags);
}

// Remove a specific process from a singly-linked wait queue.
static inline bool RemoveFromQueue(Process*& head, Process*& tail, Process* target)
{
    if (!head || !target) return false;
    if (head == target) {
        head = target->syncNext;
        if (!head) tail = nullptr;
        target->syncNext = nullptr;
        return true;
    }
    for (Process* prev = head; prev->syncNext; prev = prev->syncNext) {
        if (prev->syncNext == target) {
            prev->syncNext = target->syncNext;
            if (tail == target) tail = prev;
            target->syncNext = nullptr;
            return true;
        }
    }
    return false;
}

// Called when a thread is about to exit. Releases any held write lock and
// removes the thread from any rwlock wait queue it may be blocked on.
void KRwLockCleanupOnExit(Process* p)
{
    if (!p) return;

    // BRO-179: a kernel #GP was observed here with p->heldWriteLock ==
    // 0xDFDFDFDFDFDFDFDF (heap kfree-poison): the Process struct was already
    // kfree'd when cleanup ran (a process-lifetime UAF in the exit/reap path).
    // Validate the struct magic before touching any lock fields. If it is
    // poisoned/zeroed, name the teardown path that freed it (ProcessDumpFreeLog
    // records the freeing caller + incarnation) and bail rather than
    // dereferencing poison into a non-canonical fault.
#ifndef BROOK_HOST_TEST
    if (p->magic != PROCESS_MAGIC) {
        SerialPrintf("BRO179-RWCLEANUP-UAF: KRwLockCleanupOnExit on FREED proc=%p "
                     "magic=0x%lx (expected 0x%lx) — Process struct already freed!\n",
                     p, p->magic, PROCESS_MAGIC);
        ProcessDumpFreeLog(p);
        return;
    }
    // BRO-179: a LIVE process (magic intact) was observed with a poison VALUE in
    // its heldWriteLock field (rax=0xDFDF... -> #GP at the lock acquire). That is
    // NOT a whole-struct UAF; it is corruption of the lock-pointer field itself
    // on an otherwise-live Process. Detect poison in each lock pointer, name the
    // field + process identity/state, and neutralize it (treat as null) so we
    // don't fault and the run survives to reveal the writer.
    auto isPoison = [](void* v) {
        uint64_t u = reinterpret_cast<uint64_t>(v);
        return (u >> 32) == 0xDFDFDFDFULL || (u & 0xFFFFFFFFULL) == 0xDFDFDFDFULL;
    };
    if (isPoison(p->heldWriteLock) || isPoison(p->heldReadLock) ||
        isPoison(p->blockedOnRwLock)) {
        SerialPrintf("BRO179-RWLOCKFIELD-POISON: LIVE proc=%p pid=%u incarnation=%lu "
                     "state=%d heldWriteLock=%p heldReadLock=%p blockedOnRwLock=%p "
                     "blockedAsWriter=%d — lock-pointer field holds heap poison!\n",
                     p, (unsigned)p->pid, p->incarnation, (int)p->state,
                     (void*)p->heldWriteLock, (void*)p->heldReadLock,
                     (void*)p->blockedOnRwLock, (int)p->blockedAsWriter);
        // Re-read magic NOW: if the struct has since been poisoned/zeroed, the
        // magic check above passed but a concurrent FreeProcessStruct(p) has
        // freed p underneath us — a TOCTOU between the magic read and the field
        // read (the racing free poisons the whole block, including these
        // fields). The free-log names the racing free site.
        SerialPrintf("  magic-now=0x%lx (expected 0x%lx) — %s\n",
                     p->magic, PROCESS_MAGIC,
                     p->magic == PROCESS_MAGIC ? "still live: genuine field corruption"
                                               : "POISONED NOW: concurrent free TOCTOU");
        ProcessDumpFreeLog(p);
        if (isPoison(p->heldWriteLock))  p->heldWriteLock  = nullptr;
        if (isPoison(p->heldReadLock))   p->heldReadLock   = nullptr;
        if (isPoison(p->blockedOnRwLock)) p->blockedOnRwLock = nullptr;
        return;
    }
    // Mark cleanup in-flight for p across the lock-manipulation body below, so
    // a concurrent FreeProcessStruct(p) can detect the race. Only the rare exit
    // that actually holds/awaits a kernel rwlock needs the slot (the crash is in
    // the heldWriteLock release path), so the overwhelmingly-common no-lock exit
    // stays O(1) — keeping perturbation low so the Heisenbug still reproduces.
    bool rwHasLockState = p->heldWriteLock || p->heldReadLock || p->blockedOnRwLock;
    int rwInFlightSlot = rwHasLockState ? RwCleanupInFlightClaim(p) : -1;
#endif

    // Case 1: Thread holds a write lock — release it.
    if (p->heldWriteLock) {
        KRwLock* rw = p->heldWriteLock;
        p->heldWriteLock = nullptr;
        // Perform the same logic as KRwLockWriteUnlock.
        uint64_t flags = RwGuardAcquire(rw);
        rw->writerActive = 0;

        // Wake all queued readers.
        if (rw->readWaitHead) {
            Process* readers[128];
            uint32_t count = 0;
            while (rw->readWaitHead && count < 128) {
                readers[count] = Dequeue(rw->readWaitHead, rw->readWaitTail);
                // BRO-162: record grant before unblock (see KRwLockWriteUnlock).
                readers[count]->heldReadLock = rw;
                readers[count]->blockedOnRwLock = nullptr;
                __atomic_store_n(&readers[count]->pendingWakeup, 1, __ATOMIC_RELEASE);
                count++;
                rw->readerCount++;
            }
            RwGuardRelease(rw, flags);
            for (uint32_t i = 0; i < count; ++i)
                SchedulerUnblock(readers[i]);
        } else if (rw->writeWaitHead) {
            Process* writer = Dequeue(rw->writeWaitHead, rw->writeWaitTail);
            rw->writerActive = 1;
            rw->writersWaiting--;
            // BRO-162: record grant before unblock.
            writer->heldWriteLock = rw;
            writer->blockedOnRwLock = nullptr;
            __atomic_store_n(&writer->pendingWakeup, 1, __ATOMIC_RELEASE);
            RwGuardRelease(rw, flags);
            SchedulerUnblock(writer);
        } else {
            RwGuardRelease(rw, flags);
        }
    }

    // Case 3 (BRO-162): Thread holds a read lock — drop its reference.  This
    // covers both fully-acquired read locks and reads granted in the
    // unblocked-but-not-yet-scheduled window (the waker recorded heldReadLock).
    if (p->heldReadLock) {
        KRwLock* rw = p->heldReadLock;
        p->heldReadLock = nullptr;
        uint64_t flags = RwGuardAcquire(rw);
        if (rw->readerCount > 0)
            rw->readerCount--;
        // If we were the last reader and a writer is waiting, hand off.
        if (rw->readerCount == 0 && rw->writeWaitHead) {
            Process* writer = Dequeue(rw->writeWaitHead, rw->writeWaitTail);
            rw->writerActive = 1;
            rw->writersWaiting--;
            writer->heldWriteLock = rw;
            writer->blockedOnRwLock = nullptr;
            __atomic_store_n(&writer->pendingWakeup, 1, __ATOMIC_RELEASE);
            RwGuardRelease(rw, flags);
            SchedulerUnblock(writer);
        } else {
            RwGuardRelease(rw, flags);
        }
    }

    // Case 2: Thread is blocked waiting on a rwlock — remove from queue.
    if (p->blockedOnRwLock) {
        KRwLock* rw = p->blockedOnRwLock;
        uint64_t flags = RwGuardAcquire(rw);
        if (p->blockedAsWriter) {
            if (RemoveFromQueue(rw->writeWaitHead, rw->writeWaitTail, p))
                rw->writersWaiting--;
        } else {
            RemoveFromQueue(rw->readWaitHead, rw->readWaitTail, p);
        }
        RwGuardRelease(rw, flags);
        p->blockedOnRwLock = nullptr;
    }

#ifndef BROOK_HOST_TEST
    RwCleanupInFlightRelease(rwInFlightSlot);
#endif
}

} // namespace brook
