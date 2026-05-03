#include "sync/ksemaphore.h"
#include "process.h"
#include "scheduler.h"

namespace brook {

// ---------------------------------------------------------------------------
// Internal guard lock (same pattern as KMutex/KRwLock)
// ---------------------------------------------------------------------------

static inline uint64_t SemGuardAcquire(KSemaphore* sem)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    uint32_t ticket = __atomic_fetch_add(&sem->guardNext, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&sem->guardServing, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");
    return flags;
}

static inline void SemGuardRelease(KSemaphore* sem, uint64_t savedFlags)
{
    __atomic_fetch_add(&sem->guardServing, 1, __ATOMIC_RELEASE);
    if (savedFlags & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

extern Process* SchedulerCurrentProcess();

// ---------------------------------------------------------------------------
// KSemaphore API
// ---------------------------------------------------------------------------

void KSemaphoreInit(KSemaphore* sem, int32_t initialCount)
{
    sem->count        = initialCount;
    sem->waitHead     = nullptr;
    sem->waitTail     = nullptr;
    sem->guardNext    = 0;
    sem->guardServing = 0;
}

void KSemaphoreWait(KSemaphore* sem)
{
    Process* self = SchedulerCurrentProcess();
    if (!self) return;

    uint64_t flags = SemGuardAcquire(sem);

    if (sem->count > 0)
    {
        sem->count--;
        SemGuardRelease(sem, flags);
        return;
    }

    // Block — enqueue on wait list.
    self->syncNext = nullptr;
    __atomic_store_n(&self->pendingWakeup, 0, __ATOMIC_RELEASE);
    if (sem->waitTail)
        sem->waitTail->syncNext = self;
    else
        sem->waitHead = self;
    sem->waitTail = self;

    SemGuardRelease(sem, flags);
    SchedulerBlock(self);
}

void KSemaphoreSignal(KSemaphore* sem)
{
    uint64_t flags = SemGuardAcquire(sem);

    Process* waiter = sem->waitHead;
    if (waiter)
    {
        // Wake the first waiter instead of incrementing count.
        sem->waitHead = waiter->syncNext;
        if (!sem->waitHead)
            sem->waitTail = nullptr;
        waiter->syncNext = nullptr;

        __atomic_store_n(&waiter->pendingWakeup, 1, __ATOMIC_RELEASE);
        SemGuardRelease(sem, flags);
        SchedulerUnblock(waiter);
    }
    else
    {
        sem->count++;
        SemGuardRelease(sem, flags);
    }
}

bool KSemaphoreTryWait(KSemaphore* sem)
{
    Process* self = SchedulerCurrentProcess();
    if (!self) return false;

    uint64_t flags = SemGuardAcquire(sem);

    if (sem->count > 0)
    {
        sem->count--;
        SemGuardRelease(sem, flags);
        return true;
    }

    SemGuardRelease(sem, flags);
    return false;
}

} // namespace brook
