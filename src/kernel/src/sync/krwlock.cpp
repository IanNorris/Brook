#include "sync/krwlock.h"
#include "process.h"
#include "scheduler.h"
#ifndef BROOK_HOST_TEST
#include "serial.h"
// Kept for the process free-log diagnostic ABI. Cleanup no longer mutates locks.
extern "C" int ProcessIsRwCleanupInFlight(void*) { return 0; }
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

// Called under the guard: reserve the whole batch before any recipient can
// run. Keep separate wake pointers because a recipient may reuse syncNext.
static constexpr uint32_t ReadWakeBatch = 128;
static uint32_t GrantReadBatch(KRwLock* rw, Process** readers) {
    uint32_t count = 0;
    while (rw->readWaitHead && count < ReadWakeBatch) {
        Process* reader = Dequeue(rw->readWaitHead,rw->readWaitTail);
        ProcessRef(reader);
        reader->heldReadLock = rw;
        reader->blockedOnRwLock = nullptr;
        __atomic_store_n(&reader->pendingWakeup,1,__ATOMIC_RELEASE);
        readers[count++] = reader;
        ++rw->readerCount;
    }
    return count;
}
static void WakeReadBatch(Process** readers,uint32_t count) {
    for (uint32_t i=0;i<count;++i) {
        SchedulerUnblock(readers[i]);
        ProcessUnref(readers[i]);
    }
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
    if (self) SchedulerKernelRegionEnter(self);
    uint64_t flags = RwGuardAcquire(rw);

    // Grant immediately only when no writer or deferred reader is waiting.
    if (!rw->writerActive && rw->writersWaiting == 0 && !rw->readWaitHead)
    {
        rw->readerCount++;
        // Track ownership for the terminal-state invariant check.
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

    // Block — enqueue on reader wait list (once).
    self->blockedOnRwLock = rw;
    self->blockedAsWriter = false;
    Enqueue(rw->readWaitHead, rw->readWaitTail, self);

    // Block in a LOOP until granted. BRO-196: SchedulerBlock can return without
    // a grant when the thread is woken for an unrelated reason — most importantly
    // ProcessSendSignal wakes ANY Blocked thread (pendingWakeup=1 +
    // SchedulerUnblock) so it can take a pending signal. The waker clears
    // blockedOnRwLock under the guard at the moment it grants us the read lock
    // (and increments readerCount + records heldReadLock on our behalf, BRO-162),
    // so a spurious wake leaves blockedOnRwLock == rw and we re-block instead of
    // wrongly proceeding as a reader without the lock (which corrupts readerCount
    // and the wait queue). Kernel rwlocks are not interruptible; a pending signal
    // is delivered after the syscall completes. We remain enqueued across
    // re-blocks (enqueued exactly once above; the waker dequeues us on grant).
    for (;;)
    {
        __atomic_store_n(&self->pendingWakeup, 0, __ATOMIC_RELEASE);
        RwGuardRelease(rw, flags);
        SchedulerBlock(self);
        flags = RwGuardAcquire(rw);
        if (self->blockedOnRwLock != rw)   // granted (waker cleared it)
            break;
    }
    RwGuardRelease(rw, flags);
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
        // Record the grant before unblock; the queued acquisition remains
        // protected and referenced until its eventual unlock.
        ProcessRef(writer);
        writer->heldWriteLock = rw;
        writer->blockedOnRwLock = nullptr;
        __atomic_store_n(&writer->pendingWakeup, 1, __ATOMIC_RELEASE);
        RwGuardRelease(rw, flags);
        SchedulerUnblock(writer);
        ProcessUnref(writer);
        if (self) SchedulerKernelRegionLeave(self);
        return;
    }

    // Writer unlock grants a bounded cohort. If no writer is waiting, the
    // last reader must carry remaining readers into the next cohort; leaving
    // this queue behind stranded readers beyond the first 128 indefinitely.
    if (rw->readerCount == 0 && rw->readWaitHead) {
        Process* readers[ReadWakeBatch];
        const uint32_t count = GrantReadBatch(rw,readers);
        RwGuardRelease(rw,flags);
        WakeReadBatch(readers,count);
        if (self) SchedulerKernelRegionLeave(self);
        return;
    }
    RwGuardRelease(rw, flags);
    if (self) SchedulerKernelRegionLeave(self);
}

void KRwLockWriteLock(KRwLock* rw)
{
    Process* self = SchedulerCurrentProcess();
    if (self) SchedulerKernelRegionEnter(self);
    uint64_t flags = RwGuardAcquire(rw);

    // Grant immediately if no readers and no writer active.
    if (rw->readerCount == 0 && !rw->writerActive)
    {
        rw->writerActive = 1;
        RwGuardRelease(rw, flags);
        // Track ownership.
        if (self) self->heldWriteLock = rw;
        return;
    }

    // Must block — need a valid process context.
    if (!self) {
        // No process context — cannot sleep.  Force-grant to avoid hang.
        // This should only happen during very early boot before any
        // contention is possible.
        rw->writerActive = 1;
        RwGuardRelease(rw, flags);
        return;
    }

    // Block — enqueue on writer wait list (once).
    rw->writersWaiting++;
    self->blockedOnRwLock = rw;
    self->blockedAsWriter = true;
    Enqueue(rw->writeWaitHead, rw->writeWaitTail, self);

    // Block in a LOOP until granted (BRO-196 — same rationale as KRwLockReadLock).
    // A spurious wake (signal delivery via ProcessSendSignal) returns from
    // SchedulerBlock without a grant; the waker clears blockedOnRwLock under the
    // guard when it actually hands us the write lock (and sets writerActive +
    // records heldWriteLock + decrements writersWaiting on our behalf). On a
    // spurious wake blockedOnRwLock == rw, so we re-block rather than wrongly
    // running as the writer. writersWaiting is incremented exactly once above and
    // decremented by the waker on the real grant, so re-blocks don't miscount.
    for (;;)
    {
        __atomic_store_n(&self->pendingWakeup, 0, __ATOMIC_RELEASE);
        RwGuardRelease(rw, flags);
        SchedulerBlock(self);
        flags = RwGuardAcquire(rw);
        if (self->blockedOnRwLock != rw)   // granted (waker cleared it)
            break;
    }
    RwGuardRelease(rw, flags);
    self->heldWriteLock = rw;   // already set by the waker; reasserted for clarity
}

void KRwLockWriteUnlock(KRwLock* rw)
{
    // Clear ownership tracking.
    Process* self = SchedulerCurrentProcess();
    if (self && self->heldWriteLock == rw)
        self->heldWriteLock = nullptr;

    uint64_t flags = RwGuardAcquire(rw);
    rw->writerActive = 0;

    // Prefer one bounded reader cohort over a single writer. The final
    // reader carries deferred cohorts forward, alternating with queued writers.
    if (rw->readWaitHead)
    {
        Process* readers[ReadWakeBatch];
        const uint32_t count = GrantReadBatch(rw,readers);
        RwGuardRelease(rw,flags);
        WakeReadBatch(readers,count);
        if (self) SchedulerKernelRegionLeave(self);
        return;
    }

    // No readers — wake next writer if any.
    if (rw->writeWaitHead)
    {
        Process* writer = Dequeue(rw->writeWaitHead, rw->writeWaitTail);
        rw->writerActive = 1;
        rw->writersWaiting--;
        // BRO-162: record ownership on the woken writer before unblock.
        ProcessRef(writer);
        writer->heldWriteLock = rw;
        writer->blockedOnRwLock = nullptr;
        __atomic_store_n(&writer->pendingWakeup, 1, __ATOMIC_RELEASE);
        RwGuardRelease(rw, flags);
        SchedulerUnblock(writer);
        ProcessUnref(writer);
        if (self) SchedulerKernelRegionLeave(self);
        return;
    }

    RwGuardRelease(rw, flags);
    if (self) SchedulerKernelRegionLeave(self);
}

// Terminal publication is forbidden while queued or holding a sleeping lock.
// Releasing an abandoned filesystem lock could expose half-written metadata.
void KRwLockCleanupOnExit(Process* p)
{
    if (p && (p->heldWriteLock || p->heldReadLock || p->blockedOnRwLock)) {
#ifndef BROOK_HOST_TEST
        SerialPrintf("KRwLock: terminal process still owns lock state pid=%u\n", p->pid);
#endif
        __builtin_trap();
    }
}

} // namespace brook
