#pragma once

// syscall_epoll.h -- internal declarations shared from syscall_epoll.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include "process.h"
#include "sync/kmutex.h"

namespace brook {

static constexpr int EPOLL_MAX_FDS = MAX_FDS;

struct EpollEntry {
    int      fd;      // watched fd (-1 = free)
    uint32_t events;  // requested events
    uint64_t data;    // user data (uint64)
    uint64_t eventFdIdentity = 0; // zero means initial/MOD delivery is pending
    uint64_t inGeneration = 0;
    uint64_t outGeneration = 0;
#if defined(BROOK_EPOLL_TARGET_DIAGNOSTICS)
    // Diagnostic only: a changed type/handle proves a representation mismatch,
    // not necessarily close/reuse (aliases may have distinct wrappers).
    // Equality does not prove identity: handles can themselves be reused.
    void* diagHandle = nullptr;
    FdType diagType = FdType::None;
    bool diagTargetValid = false;
#endif
};

struct EpollInstance {
    // `lock` protects entries, count and ctlGeneration. It is a leaf lock:
    // it is never held together with another instance's lock, across a
    // readiness check, a user copy or a sleep. See syscall_epoll.cpp.
    KMutex     lock;
    EpollEntry entries[EPOLL_MAX_FDS];
    int        count;
    volatile uint32_t refCount; // +1 per fd sharing this epoll instance
    Process*   waiter; // process blocked in epoll_wait
    uint64_t   ctlGeneration; // bumped by epoll_ctl under `lock`
};

static inline void EpollFdRef(EpollInstance* ep)
{
    if (ep) __atomic_fetch_add(&ep->refCount, 1, __ATOMIC_RELEASE);
}

static inline bool EpollFdUnref(EpollInstance* ep)
{
    if (!ep) return false;
    uint32_t prev = __atomic_fetch_sub(&ep->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1) {
        kfree(ep);
        return true;
    }
    return false;
}

int EpollPollReady(Process* proc, EpollInstance* ep);

} // namespace brook
