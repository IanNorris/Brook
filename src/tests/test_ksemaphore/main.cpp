// Host-side unit tests for KSemaphore (counting semaphore).
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
    return &g_mockProcs[idx].ext;
}

static thread_local Process* tl_currentProcess = nullptr;

namespace brook {
    Process* SchedulerCurrentProcess() { return tl_currentProcess; }

    void SchedulerBlock(Process* p) {
        pthread_mutex_lock(&g_schedMutex);
        ProcessExt* ext = GetOrCreateExt(p);
        ext->blocked = true;
        while (ext->blocked)
            pthread_cond_wait(&ext->cond, &g_schedMutex);
        pthread_mutex_unlock(&g_schedMutex);
    }

    void SchedulerUnblock(Process* p) {
        pthread_mutex_lock(&g_schedMutex);
        ProcessExt* ext = GetOrCreateExt(p);
        ext->blocked = false;
        pthread_cond_signal(&ext->cond);
        pthread_mutex_unlock(&g_schedMutex);
    }
}

// ---------- KSemaphore implementation (host version) ----------
namespace brook {

struct KSemaphore {
    volatile int32_t count;
    Process*         waitHead;
    Process*         waitTail;
    pthread_mutex_t  guard;
};

void KSemaphoreInit(KSemaphore* sem, int32_t initialCount) {
    sem->count = initialCount;
    sem->waitHead = sem->waitTail = nullptr;
    pthread_mutex_init(&sem->guard, nullptr);
}

void KSemaphoreWait(KSemaphore* sem) {
    Process* self = SchedulerCurrentProcess();
    if (!self) return;
    pthread_mutex_lock(&sem->guard);
    if (sem->count > 0) {
        sem->count--;
        pthread_mutex_unlock(&sem->guard);
        return;
    }
    self->syncNext = nullptr;
    if (sem->waitTail) sem->waitTail->syncNext = self;
    else               sem->waitHead = self;
    sem->waitTail = self;
    pthread_mutex_unlock(&sem->guard);
    SchedulerBlock(self);
}

void KSemaphoreSignal(KSemaphore* sem) {
    pthread_mutex_lock(&sem->guard);
    Process* waiter = sem->waitHead;
    if (waiter) {
        sem->waitHead = waiter->syncNext;
        if (!sem->waitHead) sem->waitTail = nullptr;
        waiter->syncNext = nullptr;
        pthread_mutex_unlock(&sem->guard);
        SchedulerUnblock(waiter);
    } else {
        sem->count++;
        pthread_mutex_unlock(&sem->guard);
    }
}

bool KSemaphoreTryWait(KSemaphore* sem) {
    Process* self = SchedulerCurrentProcess();
    if (!self) return false;
    pthread_mutex_lock(&sem->guard);
    if (sem->count > 0) {
        sem->count--;
        pthread_mutex_unlock(&sem->guard);
        return true;
    }
    pthread_mutex_unlock(&sem->guard);
    return false;
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

TEST(basic_wait_signal) {
    KSemaphore sem;
    KSemaphoreInit(&sem, 1);
    Process p = {.pid = 1, .syncNext = nullptr};
    tl_currentProcess = &p;

    KSemaphoreWait(&sem);
    ASSERT(sem.count == 0);
    KSemaphoreSignal(&sem);
    ASSERT(sem.count == 1);
}

TEST(try_wait_success) {
    KSemaphore sem;
    KSemaphoreInit(&sem, 2);
    Process p = {.pid = 1, .syncNext = nullptr};
    tl_currentProcess = &p;

    ASSERT(KSemaphoreTryWait(&sem));
    ASSERT(sem.count == 1);
    ASSERT(KSemaphoreTryWait(&sem));
    ASSERT(sem.count == 0);
    ASSERT(!KSemaphoreTryWait(&sem));
}

TEST(signal_without_waiters) {
    KSemaphore sem;
    KSemaphoreInit(&sem, 0);

    KSemaphoreSignal(&sem);
    ASSERT(sem.count == 1);
    KSemaphoreSignal(&sem);
    ASSERT(sem.count == 2);
}

TEST(producer_consumer) {
    KSemaphore sem;
    KSemaphoreInit(&sem, 0);

    static const int N_ITEMS = 100;

    Process producer_p = {.pid = 1, .syncNext = nullptr};
    Process consumer_p = {.pid = 2, .syncNext = nullptr};
    pthread_t producer, consumer;

    struct PArgs { KSemaphore* sem; Process* p; std::atomic<int>* consumed; };
    std::atomic<int> consumed{0};
    PArgs pargs = {&sem, &producer_p, &consumed};
    PArgs cargs = {&sem, &consumer_p, &consumed};

    auto producerFn = [](void* arg) -> void* {
        auto* a = (PArgs*)arg;
        tl_currentProcess = a->p;
        for (int i = 0; i < N_ITEMS; i++) {
            usleep(10);
            KSemaphoreSignal(a->sem);
        }
        return nullptr;
    };

    auto consumerFn = [](void* arg) -> void* {
        auto* a = (PArgs*)arg;
        tl_currentProcess = a->p;
        for (int i = 0; i < N_ITEMS; i++) {
            KSemaphoreWait(a->sem);
            (*a->consumed)++;
        }
        return nullptr;
    };

    pthread_create(&producer, nullptr, producerFn, &pargs);
    pthread_create(&consumer, nullptr, consumerFn, &cargs);
    pthread_join(producer, nullptr);
    pthread_join(consumer, nullptr);

    ASSERT(consumed.load() == N_ITEMS);
    ASSERT(sem.count == 0);
}

TEST(counting_limit) {
    // Use semaphore as a resource limiter: max 3 concurrent.
    KSemaphore sem;
    KSemaphoreInit(&sem, 3);

    static const int N = 8;
    std::atomic<int> active{0};
    std::atomic<int> maxActive{0};
    std::atomic<bool> violation{false};

    Process procs[N];
    pthread_t threads[N];

    struct Args { KSemaphore* sem; Process* p;
                  std::atomic<int>* act; std::atomic<int>* maxA; std::atomic<bool>* viol; };
    Args args[N];
    for (int i = 0; i < N; i++) {
        procs[i] = {.pid = (uint16_t)(i+1), .syncNext = nullptr};
        args[i] = {&sem, &procs[i], &active, &maxActive, &violation};
    }

    auto fn = [](void* arg) -> void* {
        auto* a = (Args*)arg;
        tl_currentProcess = a->p;
        for (int j = 0; j < 200; j++) {
            KSemaphoreWait(a->sem);
            int a_val = ++(*a->act);
            if (a_val > 3) a->viol->store(true);
            int cur = a->maxA->load();
            while (a_val > cur && !a->maxA->compare_exchange_weak(cur, a_val)) {}
            usleep(10);
            (*a->act)--;
            KSemaphoreSignal(a->sem);
        }
        return nullptr;
    };

    for (int i = 0; i < N; i++)
        pthread_create(&threads[i], nullptr, fn, &args[i]);
    for (int i = 0; i < N; i++)
        pthread_join(threads[i], nullptr);

    ASSERT(!violation.load());
    ASSERT(maxActive.load() <= 3);
    ASSERT(maxActive.load() > 1); // Should have had some concurrency
}

TEST(stress_binary_semaphore) {
    // Binary semaphore (count=1) used as mutex.
    KSemaphore sem;
    KSemaphoreInit(&sem, 1);

    static const int N = 8;
    static const int ITERS = 5000;
    int64_t shared = 0;

    Process procs[N];
    pthread_t threads[N];
    struct Args { KSemaphore* sem; Process* p; int64_t* shared; };
    Args args[N];
    for (int i = 0; i < N; i++) {
        procs[i] = {.pid = (uint16_t)(i+1), .syncNext = nullptr};
        args[i] = {&sem, &procs[i], &shared};
    }

    auto fn = [](void* arg) -> void* {
        auto* a = (Args*)arg;
        tl_currentProcess = a->p;
        for (int j = 0; j < ITERS; j++) {
            KSemaphoreWait(a->sem);
            (*a->shared)++;
            KSemaphoreSignal(a->sem);
        }
        return nullptr;
    };

    for (int i = 0; i < N; i++)
        pthread_create(&threads[i], nullptr, fn, &args[i]);
    for (int i = 0; i < N; i++)
        pthread_join(threads[i], nullptr);

    ASSERT(shared == N * ITERS);
}

int main()
{
    printf("KSemaphore tests:\n");
    run_basic_wait_signal();
    run_try_wait_success();
    run_signal_without_waiters();
    run_producer_consumer();
    run_counting_limit();
    run_stress_binary_semaphore();

    printf("\n%d passed, %d failed\n", g_testsPassed, g_testsFailed);
    return g_testsFailed ? 1 : 0;
}
