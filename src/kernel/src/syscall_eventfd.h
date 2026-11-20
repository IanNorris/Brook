#pragma once

// syscall_eventfd.h -- internal declarations shared from syscall_eventfd.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include "sync/kmutex.h"
#include "memory/heap.h"

namespace brook {

struct EventFdWaitNode;

// EventFd data structure (used in read/write/poll/close)
struct EventFdData {
    KMutex lock; // graph -> eventfd; never acquire graph while holding this
    uint64_t counter;
    uint64_t inGeneration;
    uint64_t outGeneration;
    uint64_t identity;
    uint32_t flags;
    volatile uint32_t refCount;
    // Every blocked read, write, poll and epoll_wait on this eventfd, under
    // lock. A list rather than single slots: concurrent waiters each get the
    // wake instead of the last registration winning.
    EventFdWaitNode* waiters;
};

struct EventFdWaitNode {
    EventFdData* efd;
    Process* process;
    EventFdWaitNode* next;      // efd->waiters
    EventFdWaitNode* nextGroup; // owning EventFdWaitGroup
};

// Wake every registered waiter; any counter change can satisfy readers
// (POLLIN) or writers (POLLOUT). Caller holds efd->lock.
void EventFdWakeLocked(EventFdData* efd);

// Registrations owned by one blocking operation. Each holds a reference on
// the eventfd and the process; the destructor unregisters all of them.
struct EventFdWaitGroup {
    EventFdWaitNode* nodes = nullptr;
    EventFdWaitGroup() = default;
    EventFdWaitGroup(const EventFdWaitGroup&) = delete;
    EventFdWaitGroup& operator=(const EventFdWaitGroup&) = delete;
    // Register before the final readiness check; false on allocation failure.
    bool add(EventFdData* efd, Process* process);
    ~EventFdWaitGroup();
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
