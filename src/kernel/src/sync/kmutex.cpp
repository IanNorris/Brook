#include "sync/kmutex.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"

namespace brook {

// ---------------------------------------------------------------------------
// Internal guard lock (interrupt-disabling ticket spinlock)
// Held only for a handful of instructions — never across a sleep.
// ---------------------------------------------------------------------------

static inline uint64_t GuardAcquire(KMutex* m)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    uint32_t ticket = __atomic_fetch_add(&m->guardNext, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&m->guardServing, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");
    return flags;
}

static inline void GuardRelease(KMutex* m, uint64_t savedFlags)
{
    __atomic_fetch_add(&m->guardServing, 1, __ATOMIC_RELEASE);
    if (savedFlags & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

// ---------------------------------------------------------------------------
// Get current process (from scheduler per-CPU state)
// ---------------------------------------------------------------------------

extern Process* SchedulerCurrentProcess();

// ---------------------------------------------------------------------------
// KMutex API
// ---------------------------------------------------------------------------

void KMutexInit(KMutex* m)
{
    m->locked       = 0;
    m->ownerPid     = 0;
    m->waitHead     = nullptr;
    m->waitTail     = nullptr;
    m->guardNext    = 0;
    m->guardServing = 0;
}

void KMutexLock(KMutex* m)
{
    Process* self = SchedulerCurrentProcess();
    if (!self) return; // Not in process context (early boot)

    uint64_t flags = GuardAcquire(m);

    if (!m->locked)
    {
        // Uncontended fast path.
        m->locked   = 1;
        m->ownerPid = self->pid;
        GuardRelease(m, flags);
        return;
    }

    // Contended — add ourselves to the wait queue and block.
    self->mutexNext = nullptr;
    if (m->waitTail)
        m->waitTail->mutexNext = self;
    else
        m->waitHead = self;
    m->waitTail = self;

    // Set process state to Blocked while we hold the guard, then release
    // the guard and yield. We use SchedulerBlock which handles the
    // cli-across-yield pattern internally, but we need to release our
    // guard first.
    GuardRelease(m, flags);

    // SchedulerBlock sets state=Blocked and yields to another process.
    // When we wake up (via KMutexUnlock calling SchedulerUnblock),
    // we'll resume here with the mutex held.
    SchedulerBlock(self);
}

void KMutexUnlock(KMutex* m)
{
    uint64_t flags = GuardAcquire(m);

    if (!m->locked)
    {
        // Unlocking an unlocked mutex — programming error, but don't crash.
        GuardRelease(m, flags);
        return;
    }

    Process* waiter = m->waitHead;
    if (waiter)
    {
        // Transfer ownership to the first waiter.
        m->waitHead = waiter->mutexNext;
        if (!m->waitHead)
            m->waitTail = nullptr;
        waiter->mutexNext = nullptr;

        m->ownerPid = waiter->pid;
        // m->locked remains 1 — ownership transferred, not released.

        GuardRelease(m, flags);

        // Wake the waiter. It will resume in KMutexLock after SchedulerBlock.
        SchedulerUnblock(waiter);
    }
    else
    {
        // No waiters — release the mutex.
        m->locked   = 0;
        m->ownerPid = 0;
        GuardRelease(m, flags);
    }
}

bool KMutexTryLock(KMutex* m)
{
    Process* self = SchedulerCurrentProcess();
    if (!self) return false;

    uint64_t flags = GuardAcquire(m);

    if (!m->locked)
    {
        m->locked   = 1;
        m->ownerPid = self->pid;
        GuardRelease(m, flags);
        return true;
    }

    GuardRelease(m, flags);
    return false;
}

} // namespace brook
