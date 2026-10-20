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
static void testRw(bool writerOwner,bool nested=false) {
    KRwLock rw{}; if(nested)KRwLockInitNested(&rw);else KRwLockInit(&rw);
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
static void testNestedHints() {
    KRwLock outerRead{},outerWrite{},inner{};
    KRwLockInit(&outerRead);KRwLockInit(&outerWrite);KRwLockInitNested(&inner);
    Process owner{20,2001},waiter{21,2002};current=&owner;
    KRwLockReadLock(&outerRead);KRwLockWriteLock(&outerWrite);KRwLockWriteLock(&inner);
    assert(owner.depth==3 && owner.heldReadLock==&outerRead && owner.heldWriteLock==&outerWrite);
    std::thread t([&]{
        current=&waiter;KRwLockReadLock(&inner);
        assert(waiter.depth==1 && !waiter.heldReadLock && !waiter.heldWriteLock && !waiter.terminal);
        KRwLockReadUnlock(&inner);boundary(waiter);
    });
    waitFor([&]{return waiter.blocked;});kill(owner);kill(waiter);
    assert(!owner.terminal && !waiter.terminal);
    KRwLockWriteUnlock(&inner);
    assert(owner.depth==2 && owner.heldReadLock==&outerRead && owner.heldWriteLock==&outerWrite);
    KRwLockWriteUnlock(&outerWrite);KRwLockReadUnlock(&outerRead);boundary(owner);t.join();
    assert(owner.terminal && waiter.terminal && !owner.refCount && !waiter.refCount);
    KRwLockCleanupOnExit(&owner);KRwLockCleanupOnExit(&waiter);
}
static void testTryReadAdmission() {
    KRwLock rw{};KRwLockInitNested(&rw);
    Process owner{22,2003},reader{23,2004},writer{24,2005};current=&owner;
    KRwLockWriteLock(&rw);current=&reader;
    assert(!KRwLockTryReadLock(&rw) && !reader.depth && !reader.refCount);boundary(reader);
    current=&owner;KRwLockWriteUnlock(&rw);boundary(owner);
    current=&reader;assert(KRwLockTryReadLock(&rw) && reader.depth==1);
    std::thread t([&]{current=&writer;KRwLockWriteLock(&rw);KRwLockWriteUnlock(&rw);boundary(writer);});
    waitFor([&]{return writer.blocked;});current=&owner;
    assert(!KRwLockTryReadLock(&rw) && !owner.depth && !owner.refCount);
    assert(rw.readerCount==1);boundary(owner);
    current=&reader;KRwLockReadUnlock(&rw);boundary(reader);t.join();
    assert(!rw.readerCount && !rw.writerActive && !rw.writersWaiting && !writer.refCount);
}
static void testLargeReaderQueue(unsigned count,bool mixedWriter,bool delayedWake) {
    KRwLock rw{};KRwLockInit(&rw);Process owner{10,200};current=&owner;
    KRwLockWriteLock(&rw);
    std::vector<Process> readers;readers.reserve(count);
    for(unsigned i=0;i<count;++i)readers.push_back(Process{100+i,300+i});
    std::vector<std::thread> threads;threads.reserve(count);
    for(unsigned i=0;i<count;++i)threads.emplace_back([&,i] {
        current=&readers[i];KRwLockReadLock(&rw);KRwLockReadUnlock(&rw);boundary(readers[i]);
        std::lock_guard<std::mutex> lock(ready);readers[i].released=true;changed.notify_all();
    });
    waitFor([&] { for(const auto& reader:readers)if(!reader.blocked)return false;return true; });
    Process writer{20,250};std::thread writerThread;
    if(mixedWriter) {
        writerThread=std::thread([&] {
            current=&writer;KRwLockWriteLock(&rw);KRwLockWriteUnlock(&rw);boundary(writer);
            std::lock_guard<std::mutex> lock(ready);writer.released=true;changed.notify_all();
        });
        waitFor([&] {return writer.blocked;});
    }
    // Pending cancellation cannot abandon a granted reader or the remainder
    // of a wake batch when its first recipient has already unlocked/exited.
    for(unsigned i=0;i<count;i+=128)kill(readers[i]);
    if(delayedWake) {
        { std::lock_guard<std::mutex> lock(ready);holdWake=true;wakeEntered=allowWake=false; }
        std::thread unlocker([&] {current=&owner;KRwLockWriteUnlock(&rw);boundary(owner);});
        waitFor([&] {
            if(!wakeEntered)return false;
            for(const auto& reader:readers)if(reader.released)return true;
            return false;
        });
        kill(owner);assert(owner.depth && !owner.terminal);
        { std::lock_guard<std::mutex> lock(ready);allowWake=true;changed.notify_all(); }
        unlocker.join();assert(owner.terminal && !owner.depth && !owner.refCount);
        { std::lock_guard<std::mutex> lock(ready);holdWake=false; }
    } else { KRwLockWriteUnlock(&rw);boundary(owner); }
    {
    std::unique_lock<std::mutex> lock(ready);
    const bool complete=changed.wait_for(lock,std::chrono::seconds(5),[&] {
        for(const auto& reader:readers)if(!reader.released)return false;
        return !mixedWriter || writer.released;
    });
    if(!complete){
        const auto ticket=__atomic_fetch_add(&rw.guardNext,1,__ATOMIC_RELAXED);
        while(__atomic_load_n(&rw.guardServing,__ATOMIC_ACQUIRE)!=ticket)std::this_thread::yield();
        unsigned done=0,blocked=0;
        for(const auto& reader:readers){done+=reader.released;blocked+=reader.blocked;}
        std::fprintf(stderr,"RW TIMEOUT count=%u released=%u blocked=%u active_readers=%d writer=%u queued_read=%p queued_write=%p\n",
            count,done,blocked,rw.readerCount,rw.writerActive,(void*)rw.readWaitHead,(void*)rw.writeWaitHead);
        for(const auto& reader:readers)if(!reader.released && reader.blocked)
            std::fprintf(stderr,"RW BLOCKED pid=%u pending=%u waiting=%p\n",reader.pid,
                __atomic_load_n(&reader.pendingWakeup,__ATOMIC_ACQUIRE),(void*)reader.blockedOnRwLock);
        __atomic_fetch_add(&rw.guardServing,1,__ATOMIC_RELEASE);
    }
    assert(complete);
    }
    if(writerThread.joinable())writerThread.join();
    assert(!writer.depth && !writer.refCount && !writer.heldWriteLock && !writer.blockedOnRwLock);
    for(auto& thread:threads)thread.join();
    assert(!rw.readerCount && !rw.writerActive && !rw.readWaitHead && !rw.writeWaitHead);
    for(auto& reader:readers) {
        assert(!reader.depth && !reader.refCount && !reader.heldReadLock && !reader.blockedOnRwLock);
        KRwLockCleanupOnExit(&reader);
    }
    KRwLockCleanupOnExit(&owner);
    std::printf("RW LARGE QUEUE PASS readers=%u mixed-writer=%u delayed-wake=%u no stranded waiters/references\n",count,mixedWriter,delayedWake);
}
static void testDeferredReaderAdmission() {
    constexpr unsigned count=129;
    KRwLock rw{};KRwLockInit(&rw);Process owner{30,500},arrival{31,501};current=&owner;
    KRwLockWriteLock(&rw);
    std::vector<Process> readers;readers.reserve(count);
    for(unsigned i=0;i<count;++i)readers.push_back(Process{100+i,600+i});
    unsigned acquired=0,released=0;bool allowUnlock=false,arrivalAcquired=false;
    auto reader=[&](Process& process,bool newcomer) {
        current=&process;KRwLockReadLock(&rw);
        {
            std::unique_lock<std::mutex> lock(ready);
            ++acquired;if(newcomer)arrivalAcquired=true;changed.notify_all();
            changed.wait(lock,[&]{return allowUnlock;});
        }
        KRwLockReadUnlock(&rw);boundary(process);
        std::lock_guard<std::mutex> lock(ready);++released;changed.notify_all();
    };
    std::vector<std::thread> threads;threads.reserve(count);
    for(auto& process:readers)threads.emplace_back([&reader,&process]{reader(process,false);});
    waitFor([&]{for(const auto& process:readers)if(!process.blocked)return false;return true;});
    KRwLockWriteUnlock(&rw);boundary(owner);
    waitFor([&]{return acquired==128;});
    std::thread newcomer([&]{reader(arrival,true);});
    waitFor([&]{return arrival.blocked || arrivalAcquired;});
    {
        std::lock_guard<std::mutex> lock(ready);
        assert(arrival.blocked && !arrivalAcquired && acquired==128);
        allowUnlock=true;changed.notify_all();
    }
    waitFor([&]{return released==count+1;});
    for(auto& thread:threads)thread.join();newcomer.join();
    assert(!rw.readerCount && !rw.writerActive && !rw.readWaitHead && !rw.writeWaitHead);
    std::puts("RW DEFERRED ADMISSION PASS new reader cannot bypass a deferred cohort");
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    alarm(30);
    testNestedHints();testTryReadAdmission();
    testDeferredReaderAdmission();
    testLargeReaderQueue(128,false,false);
    testLargeReaderQueue(129,false,false);
    testLargeReaderQueue(257,false,true);
    testLargeReaderQueue(257,true,true);
    for (int i = 0; i < 64; ++i) { testMutex(i % 2); testRw(i % 2);testRw(i%2,true); }
    std::puts("test_krwlock_cleanup: actual KMutex/KRwLock cooperative lifetime PASS (64 rounds)");
}
