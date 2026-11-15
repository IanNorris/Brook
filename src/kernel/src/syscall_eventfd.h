#pragma once

// syscall_eventfd.h -- internal declarations shared from syscall_eventfd.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include "sync/kmutex.h"
#include "memory/heap.h"

namespace brook {

// EventFd data structure (used in read/write/poll/close)
struct EventFdData {
    KMutex lock; // graph -> eventfd; never acquire graph while holding this
    uint64_t counter;
    uint64_t inGeneration;
    uint64_t outGeneration;
    uint64_t identity;
    uint32_t flags;
    volatile uint32_t refCount;
    Process* readerWaiter;
    Process* epollWaiter;  // process blocked in epoll_wait watching this eventfd
};

extern uint64_t g_eventFdIdentity;

struct EventFdGuard {
    EventFdData* efd;
    explicit EventFdGuard(EventFdData* value) : efd(value) { KMutexLock(&efd->lock); }
    ~EventFdGuard() { KMutexUnlock(&efd->lock); }
};

struct EventFdSnapshot {
    uint64_t identity, inGeneration, outGeneration;
    uint32_t ready;
};

EventFdSnapshot EventFdReady(EventFdData* efd, uint32_t events);
static constexpr uint32_t EFD_SEMAPHORE = 0x01;
static constexpr uint32_t EFD_NONBLOCK  = 0x800;

static inline void EventFdRef(EventFdData* efd)
{
    if (efd) __atomic_fetch_add(&efd->refCount, 1, __ATOMIC_RELEASE);
}

static inline bool EventFdUnref(EventFdData* efd)
{
    if (!efd) return false;
    uint32_t prev = __atomic_fetch_sub(&efd->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1) {
        kfree(efd);
        return true;
    }
    return false;
}

} // namespace brook
