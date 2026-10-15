#pragma once
#include <stdint.h>
namespace brook {
struct KRwLock;
struct Process {
    uint32_t pid;
    uint64_t incarnation;
    Process* syncNext = nullptr;
    volatile uint32_t pendingWakeup = 0;
    KRwLock *blockedOnRwLock = nullptr, *heldWriteLock = nullptr, *heldReadLock = nullptr;
    bool blockedAsWriter = false;
    int refCount = 0;
    unsigned depth = 0;
    bool protectedOperation = false, pending = false, terminal = false;
    bool blocked = false, released = false;
};
inline void ProcessRef(Process* p) { __atomic_add_fetch(&p->refCount, 1, __ATOMIC_ACQ_REL); }
inline void ProcessUnref(Process* p) { __atomic_sub_fetch(&p->refCount, 1, __ATOMIC_ACQ_REL); }
}
