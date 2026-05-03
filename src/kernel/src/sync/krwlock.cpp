#include "sync/krwlock.h"
#include "process.h"
#include "scheduler.h"

namespace brook {

// ---------------------------------------------------------------------------
// Internal guard lock (same pattern as KMutex)
// ---------------------------------------------------------------------------

static inline uint64_t RwGuardAcquire(KRwLock* rw)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    uint32_t ticket = __atomic_fetch_add(&rw->guardNext, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&rw->guardServing, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");
    return flags;
}

static inline void RwGuardRelease(KRwLock* rw, uint64_t savedFlags)
{
    __atomic_fetch_add(&rw->guardServing, 1, __ATOMIC_RELEASE);
    if (savedFlags & 0x200)
        __asm__ volatile("sti" ::: "memory");
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
    if (!self) return;

    uint64_t flags = RwGuardAcquire(rw);

    // Grant immediately if no writer is active and none waiting.
    if (!rw->writerActive && rw->writersWaiting == 0)
    {
        rw->readerCount++;
        RwGuardRelease(rw, flags);
        return;
    }

    // Block — enqueue on reader wait list.
    __atomic_store_n(&self->pendingWakeup, 0, __ATOMIC_RELEASE);
    Enqueue(rw->readWaitHead, rw->readWaitTail, self);
    RwGuardRelease(rw, flags);
    SchedulerBlock(self);
}

void KRwLockReadUnlock(KRwLock* rw)
{
    uint64_t flags = RwGuardAcquire(rw);
    rw->readerCount--;

    // If last reader and writers are waiting, wake the next writer.
    if (rw->readerCount == 0 && rw->writeWaitHead)
    {
        Process* writer = Dequeue(rw->writeWaitHead, rw->writeWaitTail);
        rw->writerActive = 1;
        rw->writersWaiting--;
        __atomic_store_n(&writer->pendingWakeup, 1, __ATOMIC_RELEASE);
        RwGuardRelease(rw, flags);
        SchedulerUnblock(writer);
        return;
    }

    RwGuardRelease(rw, flags);
}

void KRwLockWriteLock(KRwLock* rw)
{
    Process* self = SchedulerCurrentProcess();
    if (!self) return;

    uint64_t flags = RwGuardAcquire(rw);

    // Grant immediately if no readers and no writer active.
    if (rw->readerCount == 0 && !rw->writerActive)
    {
        rw->writerActive = 1;
        RwGuardRelease(rw, flags);
        return;
    }

    // Block — enqueue on writer wait list.
    rw->writersWaiting++;
    __atomic_store_n(&self->pendingWakeup, 0, __ATOMIC_RELEASE);
    Enqueue(rw->writeWaitHead, rw->writeWaitTail, self);
    RwGuardRelease(rw, flags);
    SchedulerBlock(self);
}

void KRwLockWriteUnlock(KRwLock* rw)
{
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
        __atomic_store_n(&writer->pendingWakeup, 1, __ATOMIC_RELEASE);
        RwGuardRelease(rw, flags);
        SchedulerUnblock(writer);
        return;
    }

    RwGuardRelease(rw, flags);
}

} // namespace brook
