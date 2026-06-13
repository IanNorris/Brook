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
    // BRO-196: detect a recursive (self) acquisition. KMutex is non-recursive,
    // so a thread that already owns the mutex blocking on it self-deadlocks
    // forever (it can never unlock to wake itself). Log loudly with both pids.
    if (m->locked && m->ownerPid == self->pid) {
        SerialPrintf("KMutex: SELF-DEADLOCK pid=%u re-locking mutex it already owns "
                     "(mutex=%p)\n", self->pid, (void*)m);
    }
    // Enqueue ourselves once on the FIFO wait queue.
    self->syncNext = nullptr;
    if (m->waitTail)
        m->waitTail->syncNext = self;
    else
        m->waitHead = self;
    m->waitTail = self;

    // Block in a LOOP until KMutexUnlock grants us ownership (m->ownerPid == us).
    //
    // BRO-196: a bare "block once, then assume we hold the lock" is WRONG.
    // SchedulerBlock can return WITHOUT an ownership grant when the thread is
    // woken for an unrelated reason — most importantly ProcessSendSignal wakes
    // ANY Blocked thread (pendingWakeup=1 + SchedulerUnblock) so it can take a
    // pending signal. A signal-woken mutex waiter would otherwise fall through
    // and run the critical section WITHOUT the lock, while still sitting in the
    // wait queue — corrupting the queue (observed: cyclic waiter list) and
    // making a later KMutexUnlock transfer ownership to a thread that has moved
    // on (lost wakeup → whole-FS deadlock). So we re-check the grant under the
    // guard and re-block on any spurious wake. Kernel mutexes are not
    // interruptible: the pending signal stays pending and is delivered when the
    // syscall returns, after we have completed the critical section.
    //
    // The grant is published by KMutexUnlock under this same guard (it sets
    // m->ownerPid = our pid AND dequeues us), so checking m->ownerPid == self->pid
    // while holding the guard is race-free.
    for (;;)
    {
        if (m->ownerPid == self->pid)
        {
            // Granted — KMutexUnlock has already dequeued us and set m->locked.
            GuardRelease(m, flags);
            return;
        }
        __atomic_store_n(&self->pendingWakeup, 0, __ATOMIC_RELEASE);
        GuardRelease(m, flags);
        // SchedulerBlock checks pendingWakeup to avoid a lost wakeup if
        // KMutexUnlock fires between GuardRelease and here.
        SchedulerBlock(self);
        flags = GuardAcquire(m);
    }
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
        m->waitHead = waiter->syncNext;
        if (!m->waitHead)
            m->waitTail = nullptr;
        waiter->syncNext = nullptr;

        m->ownerPid = waiter->pid;
        // m->locked remains 1 — ownership transferred, not released.

        // Signal the waiter BEFORE releasing the guard.  If the waiter
        // hasn't called SchedulerBlock yet (still between GuardRelease
        // and SchedulerBlock in KMutexLock), SchedulerUnblock would bail
        // because the process is still Running.  pendingWakeup lets
        // SchedulerBlock detect the missed wake and skip blocking.
        __atomic_store_n(&waiter->pendingWakeup, 1, __ATOMIC_RELEASE);

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

void KMutexForceUnlock(KMutex* m, uint32_t pid)
{
    uint64_t flags = GuardAcquire(m);

    if (!m->locked || m->ownerPid != pid)
    {
        GuardRelease(m, flags);
        return;
    }

    // The dying process holds this mutex. Release it.
    Process* waiter = m->waitHead;
    if (waiter)
    {
        m->waitHead = waiter->syncNext;
        if (!m->waitHead)
            m->waitTail = nullptr;
        waiter->syncNext = nullptr;
        m->ownerPid = waiter->pid;
        __atomic_store_n(&waiter->pendingWakeup, 1, __ATOMIC_RELEASE);
        GuardRelease(m, flags);
        SchedulerUnblock(waiter);
    }
    else
    {
        m->locked   = 0;
        m->ownerPid = 0;
        GuardRelease(m, flags);
    }

    SerialPrintf("KMutex: force-unlocked for dying pid %u\n", pid);
}

} // namespace brook
