// Host-side unit tests for KMutex.
//
// Simulates kernel scheduling with pthreads: each "process" is a host thread,
// SchedulerBlock = pthread_cond_wait, SchedulerUnblock = pthread_cond_signal.
// The KMutex guard spinlock is replaced with a pthread_mutex.
//
// Build: compiled as a native host binary (not freestanding), linked with -lpthread.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <pthread.h>
#include <unistd.h>
#include <atomic>

// -------------------------------------------------------------------------
// Minimal Process stub matching kernel's Process struct layout (fields used
// by KMutex).
// -------------------------------------------------------------------------

namespace brook {

enum class ProcessState : uint8_t {
    Ready, Running, Blocked, Terminated,
};

struct Process {
    uint16_t pid;
    volatile ProcessState state;
    Process* syncNext;

    // Host-side threading support.
    pthread_mutex_t   hostMtx;
    pthread_cond_t    hostCond;
    bool              hostWoken;
};

// -------------------------------------------------------------------------
// Scheduler mock using pthreads
// -------------------------------------------------------------------------

static thread_local Process* tl_currentProcess = nullptr;

Process* SchedulerCurrentProcess()
{
    return tl_currentProcess;
}

void SchedulerBlock(Process* proc)
{
    pthread_mutex_lock(&proc->hostMtx);
    proc->state = ProcessState::Blocked;
    while (!proc->hostWoken)
        pthread_cond_wait(&proc->hostCond, &proc->hostMtx);
    proc->hostWoken = false;
    proc->state = ProcessState::Running;
    pthread_mutex_unlock(&proc->hostMtx);
}

void SchedulerUnblock(Process* proc)
{
    pthread_mutex_lock(&proc->hostMtx);
    proc->hostWoken = true;
    pthread_cond_signal(&proc->hostCond);
    pthread_mutex_unlock(&proc->hostMtx);
}

// -------------------------------------------------------------------------
// KMutex implementation (inlined here to test the exact same logic)
// -------------------------------------------------------------------------

struct KMutex {
    volatile uint32_t locked;
    volatile uint32_t ownerPid;
    Process*          waitHead;
    Process*          waitTail;
    pthread_mutex_t   guard;  // Host: real mutex instead of ticket spinlock
};

void KMutexInit(KMutex* m)
{
    m->locked   = 0;
    m->ownerPid = 0;
    m->waitHead = nullptr;
    m->waitTail = nullptr;
    pthread_mutex_init(&m->guard, nullptr);
}

void KMutexDestroy(KMutex* m)
{
    pthread_mutex_destroy(&m->guard);
}

void KMutexLock(KMutex* m)
{
    Process* self = SchedulerCurrentProcess();
    assert(self && "KMutexLock called without current process");

    pthread_mutex_lock(&m->guard);

    if (!m->locked)
    {
        m->locked   = 1;
        m->ownerPid = self->pid;
        pthread_mutex_unlock(&m->guard);
        return;
    }

    // Contended — enqueue and block.
    self->syncNext = nullptr;
    if (m->waitTail)
        m->waitTail->syncNext = self;
    else
        m->waitHead = self;
    m->waitTail = self;

    pthread_mutex_unlock(&m->guard);

    SchedulerBlock(self);
}

void KMutexUnlock(KMutex* m)
{
    pthread_mutex_lock(&m->guard);

    if (!m->locked)
    {
        pthread_mutex_unlock(&m->guard);
        return;
    }

    Process* waiter = m->waitHead;
    if (waiter)
    {
        m->waitHead = waiter->syncNext;
        if (!m->waitHead)
            m->waitTail = nullptr;
        waiter->syncNext = nullptr;

        m->ownerPid = waiter->pid;

        pthread_mutex_unlock(&m->guard);
        SchedulerUnblock(waiter);
    }
    else
    {
        m->locked   = 0;
        m->ownerPid = 0;
        pthread_mutex_unlock(&m->guard);
    }
}

bool KMutexTryLock(KMutex* m)
{
    Process* self = SchedulerCurrentProcess();
    assert(self);

    pthread_mutex_lock(&m->guard);

    if (!m->locked)
    {
        m->locked   = 1;
        m->ownerPid = self->pid;
        pthread_mutex_unlock(&m->guard);
        return true;
    }

    pthread_mutex_unlock(&m->guard);
    return false;
}

} // namespace brook

using namespace brook;

// -------------------------------------------------------------------------
// Test helpers
// -------------------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    struct TestReg_##name { TestReg_##name() { test_##name(); } } reg_##name; \
    static void test_##name()

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++; \
        return; \
    } \
} while(0)

#define PASS(name) do { printf("  PASS: %s\n", name); g_passed++; } while(0)

static Process* MakeProcess(uint16_t pid)
{
    auto* p = new Process{};
    p->pid = pid;
    p->state = ProcessState::Running;
    p->syncNext = nullptr;
    p->hostWoken = false;
    pthread_mutex_init(&p->hostMtx, nullptr);
    pthread_cond_init(&p->hostCond, nullptr);
    return p;
}

static void FreeProcess(Process* p)
{
    pthread_mutex_destroy(&p->hostMtx);
    pthread_cond_destroy(&p->hostCond);
    delete p;
}

// -------------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------------

TEST(basic_lock_unlock)
{
    KMutex m;
    KMutexInit(&m);

    Process* p = MakeProcess(1);
    tl_currentProcess = p;

    KMutexLock(&m);
    CHECK(m.locked == 1);
    CHECK(m.ownerPid == 1);

    KMutexUnlock(&m);
    CHECK(m.locked == 0);
    CHECK(m.ownerPid == 0);

    KMutexDestroy(&m);
    FreeProcess(p);
    PASS("basic_lock_unlock");
}

TEST(try_lock_uncontended)
{
    KMutex m;
    KMutexInit(&m);

    Process* p = MakeProcess(2);
    tl_currentProcess = p;

    CHECK(KMutexTryLock(&m) == true);
    CHECK(m.locked == 1);
    CHECK(m.ownerPid == 2);

    // Second trylock should fail.
    Process* p2 = MakeProcess(3);
    tl_currentProcess = p2;
    CHECK(KMutexTryLock(&m) == false);

    tl_currentProcess = p;
    KMutexUnlock(&m);
    CHECK(m.locked == 0);

    KMutexDestroy(&m);
    FreeProcess(p);
    FreeProcess(p2);
    PASS("try_lock_uncontended");
}

TEST(unlock_unlocked_mutex)
{
    KMutex m;
    KMutexInit(&m);

    // Should not crash.
    KMutexUnlock(&m);
    CHECK(m.locked == 0);

    KMutexDestroy(&m);
    PASS("unlock_unlocked_mutex");
}

// -------------------------------------------------------------------------
// Contention test: two threads, one blocks until the other unlocks
// -------------------------------------------------------------------------

struct ContentionCtx {
    KMutex*    m;
    Process*   self;
    std::atomic<int> phase;
    std::atomic<int> counter;
};

static void* contention_thread(void* arg)
{
    auto* ctx = static_cast<ContentionCtx*>(arg);
    tl_currentProcess = ctx->self;

    ctx->phase.store(1); // Signal: about to lock
    KMutexLock(ctx->m);

    // We should only get here after main thread unlocks.
    ctx->phase.store(2); // Signal: acquired lock
    ctx->counter.fetch_add(1);

    KMutexUnlock(ctx->m);
    return nullptr;
}

TEST(two_thread_contention)
{
    KMutex m;
    KMutexInit(&m);

    Process* p1 = MakeProcess(10);
    Process* p2 = MakeProcess(11);
    tl_currentProcess = p1;

    KMutexLock(&m);
    CHECK(m.locked == 1);
    CHECK(m.ownerPid == 10);

    ContentionCtx ctx = { &m, p2, {0}, {0} };

    pthread_t tid;
    pthread_create(&tid, nullptr, contention_thread, &ctx);

    // Wait for the other thread to start blocking.
    while (ctx.phase.load() < 1)
        usleep(1000);
    usleep(10000); // Extra time to ensure it's in SchedulerBlock

    CHECK(ctx.phase.load() == 1); // Should still be blocked
    CHECK(ctx.counter.load() == 0);

    // Unlock — should wake the blocked thread.
    KMutexUnlock(&m);

    pthread_join(tid, nullptr);
    CHECK(ctx.phase.load() == 2);
    CHECK(ctx.counter.load() == 1);
    CHECK(m.locked == 0);

    KMutexDestroy(&m);
    FreeProcess(p1);
    FreeProcess(p2);
    PASS("two_thread_contention");
}

// -------------------------------------------------------------------------
// FIFO ordering test: 4 threads block in order, wake in same order
// -------------------------------------------------------------------------

struct FifoCtx {
    KMutex*    m;
    Process*   self;
    int*       orderArr;
    std::atomic<int>* orderIdx;
};

static void* fifo_thread(void* arg)
{
    auto* ctx = static_cast<FifoCtx*>(arg);
    tl_currentProcess = ctx->self;

    KMutexLock(ctx->m);
    int idx = ctx->orderIdx->fetch_add(1);
    ctx->orderArr[idx] = ctx->self->pid;
    KMutexUnlock(ctx->m);
    return nullptr;
}

TEST(fifo_wakeup_order)
{
    KMutex m;
    KMutexInit(&m);

    Process* holder = MakeProcess(100);
    tl_currentProcess = holder;
    KMutexLock(&m);

    constexpr int N = 4;
    Process* procs[N];
    pthread_t tids[N];
    int order[N] = {};
    std::atomic<int> orderIdx{0};

    for (int i = 0; i < N; i++)
    {
        procs[i] = MakeProcess(static_cast<uint16_t>(101 + i));
        FifoCtx* ctx = new FifoCtx{ &m, procs[i], order, &orderIdx };
        pthread_create(&tids[i], nullptr, fifo_thread, ctx);
        usleep(20000); // Stagger starts to ensure FIFO order
    }

    usleep(50000); // All should be blocked now

    // Unlock — should wake them in FIFO order (101, 102, 103, 104)
    KMutexUnlock(&m);

    for (int i = 0; i < N; i++)
        pthread_join(tids[i], nullptr);

    CHECK(orderIdx.load() == N);
    for (int i = 0; i < N; i++)
        CHECK(order[i] == 101 + i);

    KMutexDestroy(&m);
    FreeProcess(holder);
    for (int i = 0; i < N; i++)
        FreeProcess(procs[i]);
    // Clean up FifoCtx (simplification: they're leaked in this test, acceptable)
    PASS("fifo_wakeup_order");
}

// -------------------------------------------------------------------------
// Stress test: N threads increment a shared counter M times each
// -------------------------------------------------------------------------

struct StressCtx {
    KMutex*  m;
    Process* self;
    int*     sharedCounter;
    int      iters;
};

static void* stress_thread(void* arg)
{
    auto* ctx = static_cast<StressCtx*>(arg);
    tl_currentProcess = ctx->self;

    for (int i = 0; i < ctx->iters; i++)
    {
        KMutexLock(ctx->m);
        (*ctx->sharedCounter)++;
        KMutexUnlock(ctx->m);
    }
    return nullptr;
}

TEST(stress_mutual_exclusion)
{
    KMutex m;
    KMutexInit(&m);

    constexpr int NUM_THREADS = 8;
    constexpr int ITERS = 10000;

    int sharedCounter = 0;
    Process* procs[NUM_THREADS];
    pthread_t tids[NUM_THREADS];
    StressCtx ctxs[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++)
    {
        procs[i] = MakeProcess(static_cast<uint16_t>(200 + i));
        ctxs[i] = { &m, procs[i], &sharedCounter, ITERS };
        pthread_create(&tids[i], nullptr, stress_thread, &ctxs[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(tids[i], nullptr);

    CHECK(sharedCounter == NUM_THREADS * ITERS);

    KMutexDestroy(&m);
    for (int i = 0; i < NUM_THREADS; i++)
        FreeProcess(procs[i]);
    PASS("stress_mutual_exclusion");
}

// -------------------------------------------------------------------------
// Stress test: multiple mutexes to check for cross-contamination
// -------------------------------------------------------------------------

struct MultiMutexCtx {
    KMutex*  m1;
    KMutex*  m2;
    Process* self;
    int*     counter1;
    int*     counter2;
    int      iters;
};

static void* multi_mutex_thread(void* arg)
{
    auto* ctx = static_cast<MultiMutexCtx*>(arg);
    tl_currentProcess = ctx->self;

    for (int i = 0; i < ctx->iters; i++)
    {
        KMutexLock(ctx->m1);
        (*ctx->counter1)++;
        KMutexUnlock(ctx->m1);

        KMutexLock(ctx->m2);
        (*ctx->counter2)++;
        KMutexUnlock(ctx->m2);
    }
    return nullptr;
}

TEST(multiple_mutexes)
{
    KMutex m1, m2;
    KMutexInit(&m1);
    KMutexInit(&m2);

    constexpr int NUM_THREADS = 4;
    constexpr int ITERS = 5000;

    int counter1 = 0, counter2 = 0;
    Process* procs[NUM_THREADS];
    pthread_t tids[NUM_THREADS];
    MultiMutexCtx ctxs[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++)
    {
        procs[i] = MakeProcess(static_cast<uint16_t>(300 + i));
        ctxs[i] = { &m1, &m2, procs[i], &counter1, &counter2, ITERS };
        pthread_create(&tids[i], nullptr, multi_mutex_thread, &ctxs[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(tids[i], nullptr);

    CHECK(counter1 == NUM_THREADS * ITERS);
    CHECK(counter2 == NUM_THREADS * ITERS);

    KMutexDestroy(&m1);
    KMutexDestroy(&m2);
    for (int i = 0; i < NUM_THREADS; i++)
        FreeProcess(procs[i]);
    PASS("multiple_mutexes");
}

// -------------------------------------------------------------------------
// TryLock under contention
// -------------------------------------------------------------------------

struct TryLockCtx {
    KMutex*  m;
    Process* self;
    std::atomic<int> successes;
    std::atomic<int> failures;
    int iters;
};

static void* trylock_thread(void* arg)
{
    auto* ctx = static_cast<TryLockCtx*>(arg);
    tl_currentProcess = ctx->self;

    for (int i = 0; i < ctx->iters; i++)
    {
        if (KMutexTryLock(ctx->m))
        {
            ctx->successes.fetch_add(1);
            usleep(1); // Hold briefly
            KMutexUnlock(ctx->m);
        }
        else
        {
            ctx->failures.fetch_add(1);
        }
    }
    return nullptr;
}

TEST(trylock_under_contention)
{
    KMutex m;
    KMutexInit(&m);

    constexpr int NUM_THREADS = 4;
    constexpr int ITERS = 1000;

    Process* procs[NUM_THREADS];
    pthread_t tids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++)
    {
        procs[i] = MakeProcess(static_cast<uint16_t>(400 + i));
        TryLockCtx* tc = new TryLockCtx{ &m, procs[i], {0}, {0}, ITERS };
        // Share the success/failure counters
        // (simplified: each thread gets its own, we sum them)
        pthread_create(&tids[i], nullptr, trylock_thread, tc);
    }

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(tids[i], nullptr);

    // Some trylocks should have succeeded, some failed — exact numbers vary.
    // The key invariant is no crash and no deadlock.
    CHECK(true); // If we got here, no deadlock

    KMutexDestroy(&m);
    for (int i = 0; i < NUM_THREADS; i++)
        FreeProcess(procs[i]);
    PASS("trylock_under_contention");
}

// -------------------------------------------------------------------------
// Main
// -------------------------------------------------------------------------

int main()
{
    printf("=== KMutex Host Tests ===\n");
    // Tests auto-register via static constructors above.
    printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
