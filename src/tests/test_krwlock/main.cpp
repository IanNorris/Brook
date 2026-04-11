// Host-side unit tests for KRwLock (reader-writer lock).
// Runs on Linux with pthreads mocking the kernel scheduler.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <pthread.h>
#include <unistd.h>
#include <atomic>

// ---------- Minimal Process stub ----------
namespace brook {
struct Process {
    uint16_t pid;
    Process* syncNext;
};
}
using brook::Process;

// ---------- Scheduler mock (per-process condvar) ----------
static pthread_mutex_t g_schedMutex = PTHREAD_MUTEX_INITIALIZER;

struct ProcessExt {
    pthread_cond_t cond;
    bool blocked;
    int pendingUnblocks;
};

static constexpr int MAX_MOCK_PROCS = 128;
static struct { Process* proc; ProcessExt ext; } g_mockProcs[MAX_MOCK_PROCS];
static int g_mockProcCount = 0;

static ProcessExt* GetOrCreateExt(Process* p) {
    for (int i = 0; i < g_mockProcCount; i++)
        if (g_mockProcs[i].proc == p) return &g_mockProcs[i].ext;
    int idx = g_mockProcCount++;
    g_mockProcs[idx].proc = p;
    pthread_cond_init(&g_mockProcs[idx].ext.cond, nullptr);
    g_mockProcs[idx].ext.blocked = false;
    g_mockProcs[idx].ext.pendingUnblocks = 0;
    return &g_mockProcs[idx].ext;
}

static thread_local Process* tl_currentProcess = nullptr;

namespace brook {
    Process* SchedulerCurrentProcess() { return tl_currentProcess; }

    void SchedulerBlock(Process* p) {
        pthread_mutex_lock(&g_schedMutex);
        ProcessExt* ext = GetOrCreateExt(p);
        if (ext->pendingUnblocks > 0) {
            ext->pendingUnblocks--;
            pthread_mutex_unlock(&g_schedMutex);
            return;
        }
        ext->blocked = true;
        while (ext->blocked)
            pthread_cond_wait(&ext->cond, &g_schedMutex);
        pthread_mutex_unlock(&g_schedMutex);
    }

    void SchedulerUnblock(Process* p) {
        pthread_mutex_lock(&g_schedMutex);
        ProcessExt* ext = GetOrCreateExt(p);
        if (ext->blocked) {
            ext->blocked = false;
            pthread_cond_signal(&ext->cond);
        } else {
            ext->pendingUnblocks++;
        }
        pthread_mutex_unlock(&g_schedMutex);
    }
}

// ---------- KRwLock implementation (host version) ----------
// Replace asm guard with pthread_mutex.
namespace brook {

struct KRwLock {
    volatile int32_t  readerCount;
    volatile uint32_t writerActive;
    volatile uint32_t writersWaiting;
    Process*          readWaitHead;
    Process*          readWaitTail;
    Process*          writeWaitHead;
    Process*          writeWaitTail;
    pthread_mutex_t   guard;
};

static inline void Enqueue(Process*& head, Process*& tail, Process* p) {
    p->syncNext = nullptr;
    if (tail) tail->syncNext = p;
    else      head = p;
    tail = p;
}

static inline Process* Dequeue(Process*& head, Process*& tail) {
    Process* p = head;
    if (!p) return nullptr;
    head = p->syncNext;
    if (!head) tail = nullptr;
    p->syncNext = nullptr;
    return p;
}

void KRwLockInit(KRwLock* rw) {
    rw->readerCount = 0;
    rw->writerActive = 0;
    rw->writersWaiting = 0;
    rw->readWaitHead = rw->readWaitTail = nullptr;
    rw->writeWaitHead = rw->writeWaitTail = nullptr;
    pthread_mutex_init(&rw->guard, nullptr);
}

void KRwLockReadLock(KRwLock* rw) {
    Process* self = SchedulerCurrentProcess();
    if (!self) return;
    pthread_mutex_lock(&rw->guard);
    if (!rw->writerActive) {
        rw->readerCount++;
        pthread_mutex_unlock(&rw->guard);
        return;
    }
    Enqueue(rw->readWaitHead, rw->readWaitTail, self);
    pthread_mutex_unlock(&rw->guard);
    SchedulerBlock(self);
}

void KRwLockReadUnlock(KRwLock* rw) {
    pthread_mutex_lock(&rw->guard);
    rw->readerCount--;
    if (rw->readerCount == 0 && rw->writeWaitHead) {
        Process* writer = Dequeue(rw->writeWaitHead, rw->writeWaitTail);
        rw->writerActive = 1;
        rw->writersWaiting--;
        pthread_mutex_unlock(&rw->guard);
        SchedulerUnblock(writer);
        return;
    }
    pthread_mutex_unlock(&rw->guard);
}

void KRwLockWriteLock(KRwLock* rw) {
    Process* self = SchedulerCurrentProcess();
    if (!self) return;
    pthread_mutex_lock(&rw->guard);
    if (rw->readerCount == 0 && !rw->writerActive) {
        rw->writerActive = 1;
        pthread_mutex_unlock(&rw->guard);
        return;
    }
    rw->writersWaiting++;
    Enqueue(rw->writeWaitHead, rw->writeWaitTail, self);
    pthread_mutex_unlock(&rw->guard);
    SchedulerBlock(self);
}

void KRwLockWriteUnlock(KRwLock* rw) {
    pthread_mutex_lock(&rw->guard);
    rw->writerActive = 0;
    if (rw->readWaitHead) {
        Process* readers[128];
        uint32_t count = 0;
        while (rw->readWaitHead && count < 128) {
            readers[count++] = Dequeue(rw->readWaitHead, rw->readWaitTail);
            rw->readerCount++;
        }
        pthread_mutex_unlock(&rw->guard);
        for (uint32_t i = 0; i < count; ++i)
            SchedulerUnblock(readers[i]);
        return;
    }
    if (rw->writeWaitHead) {
        Process* writer = Dequeue(rw->writeWaitHead, rw->writeWaitTail);
        rw->writerActive = 1;
        rw->writersWaiting--;
        pthread_mutex_unlock(&rw->guard);
        SchedulerUnblock(writer);
        return;
    }
    pthread_mutex_unlock(&rw->guard);
}

} // namespace brook

// ---------- Test infrastructure ----------
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST(name) \
    static void test_##name(); \
    static void run_##name() { \
        printf("  %-50s ", #name); \
        g_mockProcCount = 0; \
        test_##name(); \
        printf("PASS\n"); \
        g_testsPassed++; \
    } \
    static void test_##name()

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_testsFailed++; \
        return; \
    } \
} while(0)

using namespace brook;

// ---------- Tests ----------

TEST(basic_read_lock) {
    KRwLock rw;
    KRwLockInit(&rw);
    Process p = {.pid = 1, .syncNext = nullptr};
    tl_currentProcess = &p;

    KRwLockReadLock(&rw);
    ASSERT(rw.readerCount == 1);
    KRwLockReadUnlock(&rw);
    ASSERT(rw.readerCount == 0);
}

TEST(basic_write_lock) {
    KRwLock rw;
    KRwLockInit(&rw);
    Process p = {.pid = 1, .syncNext = nullptr};
    tl_currentProcess = &p;

    KRwLockWriteLock(&rw);
    ASSERT(rw.writerActive == 1);
    KRwLockWriteUnlock(&rw);
    ASSERT(rw.writerActive == 0);
}

TEST(concurrent_readers) {
    KRwLock rw;
    KRwLockInit(&rw);

    static const int N = 8;
    Process procs[N];
    std::atomic<int> holdCount{0};
    std::atomic<int> maxHold{0};
    pthread_t threads[N];

    struct Args { KRwLock* rw; Process* p; std::atomic<int>* hold; std::atomic<int>* maxH; };
    Args args[N];
    for (int i = 0; i < N; i++) {
        procs[i] = {.pid = (uint16_t)(i+1), .syncNext = nullptr};
        args[i] = {&rw, &procs[i], &holdCount, &maxHold};
    }

    auto fn = [](void* arg) -> void* {
        auto* a = (Args*)arg;
        tl_currentProcess = a->p;
        for (int j = 0; j < 1000; j++) {
            KRwLockReadLock(a->rw);
            int h = ++(*a->hold);
            int cur = a->maxH->load();
            while (h > cur && !a->maxH->compare_exchange_weak(cur, h)) {}
            (*a->hold)--;
            KRwLockReadUnlock(a->rw);
        }
        return nullptr;
    };

    for (int i = 0; i < N; i++)
        pthread_create(&threads[i], nullptr, fn, &args[i]);
    for (int i = 0; i < N; i++)
        pthread_join(threads[i], nullptr);

    // Multiple readers should have been concurrent.
    ASSERT(maxHold.load() > 1);
}

TEST(writer_excludes_readers) {
    KRwLock rw;
    KRwLockInit(&rw);

    static std::atomic<int> activeReaders{0};
    static std::atomic<int> activeWriters{0};
    static std::atomic<bool> violation{false};

    static const int N = 6;
    Process procs[N];
    pthread_t threads[N];

    struct Args { KRwLock* rw; Process* p; bool isWriter; };
    Args args[N];
    for (int i = 0; i < N; i++) {
        procs[i] = {.pid = (uint16_t)(i+1), .syncNext = nullptr};
        args[i] = {&rw, &procs[i], i < 2}; // First 2 are writers
    }

    auto fn = [](void* arg) -> void* {
        auto* a = (Args*)arg;
        tl_currentProcess = a->p;
        for (int j = 0; j < 500; j++) {
            if (a->isWriter) {
                KRwLockWriteLock(a->rw);
                activeWriters++;
                if (activeReaders.load() > 0 || activeWriters.load() > 1)
                    violation.store(true);
                usleep(1);
                activeWriters--;
                KRwLockWriteUnlock(a->rw);
            } else {
                KRwLockReadLock(a->rw);
                activeReaders++;
                if (activeWriters.load() > 0)
                    violation.store(true);
                usleep(1);
                activeReaders--;
                KRwLockReadUnlock(a->rw);
            }
        }
        return nullptr;
    };

    for (int i = 0; i < N; i++)
        pthread_create(&threads[i], nullptr, fn, &args[i]);
    for (int i = 0; i < N; i++)
        pthread_join(threads[i], nullptr);

    ASSERT(!violation.load());
}

TEST(writer_blocks_during_read) {
    // Writer blocks while reader holds, then completes after reader unlocks.
    KRwLock rw;
    KRwLockInit(&rw);

    Process reader1 = {.pid = 1, .syncNext = nullptr};
    Process writer  = {.pid = 2, .syncNext = nullptr};

    // reader1 acquires read lock
    tl_currentProcess = &reader1;
    KRwLockReadLock(&rw);
    ASSERT(rw.readerCount == 1);

    // writer tries to acquire — blocks because reader is active
    pthread_t wt;
    struct WArgs { KRwLock* rw; Process* p; std::atomic<bool> done; };
    WArgs wargs = {&rw, &writer, {false}};
    auto writerFn = [](void* arg) -> void* {
        auto* a = (WArgs*)arg;
        tl_currentProcess = a->p;
        KRwLockWriteLock(a->rw);
        a->done.store(true);
        KRwLockWriteUnlock(a->rw);
        return nullptr;
    };
    pthread_create(&wt, nullptr, writerFn, &wargs);
    usleep(10000); // Let writer block
    ASSERT(!wargs.done.load());
    ASSERT(rw.writersWaiting == 1);

    // Release reader1 — writer should wake and complete
    tl_currentProcess = &reader1;
    KRwLockReadUnlock(&rw);
    pthread_join(wt, nullptr);
    ASSERT(wargs.done.load());
}

TEST(stress_mixed) {
    KRwLock rw;
    KRwLockInit(&rw);

    static const int N = 8;
    static const int ITERS = 2000;
    std::atomic<int64_t> shared_value{0};
    std::atomic<bool> violation{false};
    std::atomic<int> activeWriters{0};
    std::atomic<int> activeReaders{0};

    Process procs[N];
    pthread_t threads[N];

    struct Args { KRwLock* rw; Process* p; int id;
                  std::atomic<int64_t>* val; std::atomic<bool>* viol;
                  std::atomic<int>* aw; std::atomic<int>* ar; };
    Args args[N];
    for (int i = 0; i < N; i++) {
        procs[i] = {.pid = (uint16_t)(i+1), .syncNext = nullptr};
        args[i] = {&rw, &procs[i], i, &shared_value, &violation, &activeWriters, &activeReaders};
    }

    auto fn = [](void* arg) -> void* {
        auto* a = (Args*)arg;
        tl_currentProcess = a->p;
        for (int j = 0; j < ITERS; j++) {
            if (j % 5 == 0) { // 20% writes
                KRwLockWriteLock(a->rw);
                (*a->aw)++;
                if (a->ar->load() > 0 || a->aw->load() > 1)
                    a->viol->store(true);
                (*a->val)++;
                (*a->aw)--;
                KRwLockWriteUnlock(a->rw);
            } else {
                KRwLockReadLock(a->rw);
                (*a->ar)++;
                if (a->aw->load() > 0)
                    a->viol->store(true);
                (void)a->val->load(); // read
                (*a->ar)--;
                KRwLockReadUnlock(a->rw);
            }
        }
        return nullptr;
    };

    for (int i = 0; i < N; i++)
        pthread_create(&threads[i], nullptr, fn, &args[i]);
    for (int i = 0; i < N; i++)
        pthread_join(threads[i], nullptr);

    ASSERT(!violation.load());
    // Each thread does ITERS/5 writes = 400, 8 threads = 3200
    ASSERT(shared_value.load() == N * (ITERS / 5));
}

int main()
{
    printf("KRwLock tests:\n");
    run_basic_read_lock();
    run_basic_write_lock();
    run_concurrent_readers();
    run_writer_excludes_readers();
    run_writer_blocks_during_read();
    run_stress_mixed();

    printf("\n%d passed, %d failed\n", g_testsPassed, g_testsFailed);
    return g_testsFailed ? 1 : 0;
}
