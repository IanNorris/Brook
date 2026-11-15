#pragma once

// syscall_timerfd.h -- internal declarations shared from syscall_timerfd.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include "timerfd_state.h"
#include "sync/kmutex.h"
#include "memory/heap.h"
#include "syscall_time.h"

namespace brook {

struct TimerWaitNode;

struct TimerFdData {
    KMutex lock; // graph -> timer; no graph acquisition under this mutex
    brook::TimerFdState state;
    int clockId;
    volatile uint32_t refCount;
    TimerWaitNode* waiters;
    uint64_t rearmGeneration;
};

struct TimerFdGuard {
    TimerFdData* timer;
    explicit TimerFdGuard(TimerFdData* t) : timer(t) { KMutexLock(&t->lock); }
    ~TimerFdGuard() { KMutexUnlock(&timer->lock); }
};

bool TimerFdReady(TimerFdData* timer);
uint64_t TimerFdDeadline(TimerFdData* timer, uint64_t* generation = nullptr);

static inline void TimerFdRef(TimerFdData* tfd)
{
    if (tfd) __atomic_fetch_add(&tfd->refCount, 1, __ATOMIC_RELEASE);
}

static inline bool TimerFdUnref(TimerFdData* tfd)
{
    if (!tfd) return false;
    uint32_t prev = __atomic_fetch_sub(&tfd->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1) {
        kfree(tfd);
        return true;
    }
    return false;
}

// Each syscall owns its registrations and retains the timer independently of
// numeric fd reuse. Rearm/disarm wakes every reader, poller and epoll waiter.
struct TimerWaitNode {
    TimerFdData* timer;
    Process* process;
    TimerWaitNode* nextTimer;
    TimerWaitNode* nextGroup;
    uint64_t observedGeneration;
};

struct TimerWaitGroup {
    TimerWaitNode* nodes = nullptr;
    bool allocationFailed = false;
    void add(TimerFdData* timer, Process* process) {
        for (auto* n = nodes; n; n = n->nextGroup) if (n->timer == timer) return;
        auto* n = static_cast<TimerWaitNode*>(kmalloc(sizeof(TimerWaitNode)));
        if (!n) { allocationFailed = true; return; }
        TimerFdRef(timer);
        TimerFdGuard guard(timer);
        *n = {timer, process, timer->waiters, nodes, 0};
        timer->waiters = n; nodes = n;
    }
    uint64_t deadline(uint64_t outer) {
        for (auto* n = nodes; n; n = n->nextGroup) {
            const uint64_t candidate = TimerFdDeadline(n->timer, &n->observedGeneration);
            if (candidate < outer) outer = candidate;
        }
        // A rearm wake can be consumed while acquiring a later timer's
        // contended mutex. Check every snapshot again without sleeping locks.
        // A rearm after this pass leaves pendingWakeup set until the block.
        for (auto* n = nodes; n; n = n->nextGroup)
            if (__atomic_load_n(&n->timer->rearmGeneration, __ATOMIC_ACQUIRE) != n->observedGeneration)
                return ClockNowNs(1);
        return outer;
    }
    ~TimerWaitGroup() {
        while (nodes) {
            auto* n = nodes; nodes = n->nextGroup;
            {
                TimerFdGuard guard(n->timer);
                auto** link = &n->timer->waiters;
                while (*link && *link != n) link = &(*link)->nextTimer;
                if (*link) *link = n->nextTimer;
            }
            TimerFdUnref(n->timer); kfree(n);
        }
    }
};

} // namespace brook
