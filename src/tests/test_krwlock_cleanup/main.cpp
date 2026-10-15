// Actual kernel KMutex/KRwLock implementation, with a cooperative scheduler mock.
// Scheduling and death publication are modeled under one ready lock; lock queues,
// grants, spurious wake handling and all acquisition/wake reference counts are real.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>
#include "process.h"
#include "scheduler.h"
#include "sync/kmutex.h"
#include "sync/krwlock.h"
using namespace brook;
static std::mutex ready;
static std::condition_variable changed;
static thread_local Process* current;
static bool holdWake = false, wakeEntered = false, allowWake = false;
namespace brook {
Process* SchedulerCurrentProcess() { return current; }
void SchedulerKernelRegionEnter(Process* p) {
    std::lock_guard<std::mutex> lock(ready);
    assert(!p->terminal);
    ++p->depth; p->protectedOperation = true; ProcessRef(p);
}
void SchedulerKernelRegionLeave(Process* p) {
    std::lock_guard<std::mutex> lock(ready);
    assert(p->depth); --p->depth; ProcessUnref(p); changed.notify_all();
}
void SchedulerBlock(Process* p) {
    std::unique_lock<std::mutex> lock(ready);
    p->blocked = true; changed.notify_all();
    changed.wait(lock, [&] { return __atomic_load_n(&p->pendingWakeup, __ATOMIC_ACQUIRE); });
    p->blocked = false;
}
void SchedulerUnblock(Process* p) {
    std::unique_lock<std::mutex> lock(ready);
    assert(p->refCount > 0);
    changed.notify_all();
    if (holdWake) {
        wakeEntered = true; changed.notify_all();
        changed.wait(lock, [] { return allowWake; });
        assert(p->refCount > 0); // Recipient may already be terminal.
    }
}
}
static void kill(Process& p) {
    std::lock_guard<std::mutex> lock(ready);
    if (p.depth || p.protectedOperation) p.pending = true;
    else p.terminal = true;
}
static void boundary(Process& p) {
    std::lock_guard<std::mutex> lock(ready);
    assert(!p.depth); p.protectedOperation = false;
    if (p.pending) p.terminal = true;
    changed.notify_all();
}
template<class Predicate> static void waitFor(Predicate f) {
    std::unique_lock<std::mutex> lock(ready);
    assert(changed.wait_for(lock, std::chrono::seconds(5), f));
}
static void testMutex(bool delayedWake) {
    KMutex mutex{}, nested{}; KMutexInit(&mutex); KMutexInit(&nested);
    Process owner{1, 101}, waiter{2, 102};
    current = &owner; KMutexLock(&mutex); KMutexLock(&nested);
    kill(owner); assert(!owner.terminal && owner.depth == 2);
    KMutexUnlock(&nested); assert(owner.depth == 1);
    std::thread thread([&] {
        current = &waiter; KMutexLock(&mutex);
        assert(mutex.owner == &waiter && mutex.ownerIncarnation == waiter.incarnation);
        KMutexUnlock(&mutex);
        // A gap between two backend locks must not deliver cancellation.
        assert(waiter.protectedOperation && !waiter.terminal);
        KMutexLock(&nested); KMutexUnlock(&nested);
        boundary(waiter);
        { std::lock_guard<std::mutex> lock(ready); waiter.released = true; changed.notify_all(); }
    });
    waitFor([&] { return waiter.blocked; });
    kill(waiter); assert(!waiter.terminal);
    // Unrelated wake must never grant the mutex or enqueue the waiter twice.
    __atomic_store_n(&waiter.pendingWakeup, 1, __ATOMIC_RELEASE); changed.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    { std::lock_guard<std::mutex> lock(ready); holdWake = delayedWake; wakeEntered = allowWake = false; }
    std::thread unlocker([&] { current = &owner; KMutexUnlock(&mutex); boundary(owner); });
    if (delayedWake) {
        waitFor([] { return wakeEntered; });
        waitFor([&] { return waiter.released; });
        assert(waiter.refCount == 1); // Only outstanding Unblock call owns it.
        { std::lock_guard<std::mutex> lock(ready); allowWake = true; changed.notify_all(); }
    }
    unlocker.join(); thread.join();
    assert(owner.terminal && waiter.terminal && !owner.refCount && !waiter.refCount);
    assert(!mutex.locked && !mutex.waitHead && !mutex.owner);
    holdWake = false;
    // Reuse PID with a new lifetime must get a fresh identity.
    Process recycled{2, 103}; current = &recycled;
    assert(KMutexTryLock(&mutex)); assert(mutex.ownerIncarnation == 103);
    assert(!KMutexTryLock(&mutex)); KMutexUnlock(&mutex); boundary(recycled);
    assert(!recycled.refCount);
}
static void testRw(bool writerOwner) {
    KRwLock rw{}; KRwLockInit(&rw);
    Process owner{3, 104}, waiter{4, 105}; current = &owner;
    if (writerOwner) KRwLockWriteLock(&rw); else KRwLockReadLock(&rw);
    std::thread t([&] {
        current = &waiter;
        if (writerOwner) KRwLockReadLock(&rw); else KRwLockWriteLock(&rw);
        if (writerOwner) KRwLockReadUnlock(&rw); else KRwLockWriteUnlock(&rw);
        boundary(waiter);
    });
    waitFor([&] { return waiter.blocked; }); kill(owner); kill(waiter);
    assert(!owner.terminal && !waiter.terminal);
    if (writerOwner) KRwLockWriteUnlock(&rw); else KRwLockReadUnlock(&rw);
    boundary(owner); t.join();
    assert(!rw.readerCount && !rw.writerActive && !rw.writersWaiting);
    assert(owner.terminal && waiter.terminal && !owner.refCount && !waiter.refCount);
    KRwLockCleanupOnExit(&owner); KRwLockCleanupOnExit(&waiter);
}
int main() {
    alarm(30);
    for (int i = 0; i < 64; ++i) { testMutex(i % 2); testRw(i % 2); }
    std::puts("test_krwlock_cleanup: actual KMutex/KRwLock cooperative lifetime PASS (64 rounds)");
}
