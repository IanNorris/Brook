// Real-code host harness for KRwLock cleanup-on-exit (BRO-162).
//
// Unlike test_krwlock (which reimplements the primitive as a mock), this test
// compiles and drives the ACTUAL kernel src/kernel/src/sync/krwlock.cpp. It
// reproduces the wake-grant race described in BRO-162: a waiter that has been
// dequeued and granted the lock, but killed before it runs the instructions
// that record ownership, must not leak lock state.
//
// The scheduler mock deliberately decouples "granted/runnable" (SchedulerUnblock)
// from "actually scheduled" (a test-controlled resume gate). That gap is exactly
// the window BRO-162 lives in: the thread is runnable with the grant applied to
// the lock, but has not yet executed its post-block bookkeeping.

#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <unistd.h>
#include <pthread.h>
#include <ctime>

#include "process.h"          // shim
#include "scheduler.h"        // shim
#include "sync/krwlock.h"     // real

using brook::Process;
using brook::KRwLock;

// ---------------------------------------------------------------------------
// Scheduler mock
// ---------------------------------------------------------------------------

namespace {

struct MockState {
    Process*       p            = nullptr;
    bool           inBlock      = false;   // thread parked inside SchedulerBlock
    bool           resume       = false;   // test permits SchedulerBlock to return
    int            unblockCount = 0;       // SchedulerUnblock calls (grants)
    pthread_cond_t cond         = PTHREAD_COND_INITIALIZER;
};

constexpr int MAX_PROCS = 16;
MockState       g_states[MAX_PROCS];
int             g_stateCount = 0;
pthread_mutex_t g_m          = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  g_event      = PTHREAD_COND_INITIALIZER; // blocked/granted changes

thread_local Process* tl_current = nullptr;

MockState* StateFor(Process* p) {
    for (int i = 0; i < g_stateCount; ++i)
        if (g_states[i].p == p) return &g_states[i];
    MockState* s = &g_states[g_stateCount++];
    s->p = p;
    pthread_cond_init(&s->cond, nullptr);
    return s;
}

// Absolute deadline `secs` from now — guards against a buggy lock hanging ctest.
void Deadline(timespec* ts, int secs) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += secs;
}

} // namespace

namespace brook {

Process* SchedulerCurrentProcess() { return tl_current; }

void SchedulerBlock(Process* p) {
    pthread_mutex_lock(&g_m);
    MockState* s = StateFor(p);
    s->inBlock = true;
    pthread_cond_broadcast(&g_event);          // wake WaitUntilBlocked
    // Park until the test explicitly resumes us (models "scheduled to run").
    while (!s->resume) {
        timespec ts; Deadline(&ts, 10);
        if (pthread_cond_timedwait(&s->cond, &g_m, &ts) == ETIMEDOUT) {
            std::fprintf(stderr, "FATAL: SchedulerBlock parked >10s (lock logic hung)\n");
            std::fflush(stderr);
            pthread_mutex_unlock(&g_m);
            _exit(2);
        }
    }
    s->resume  = false;
    s->inBlock = false;
    pthread_mutex_unlock(&g_m);
}

void SchedulerUnblock(Process* p) {
    pthread_mutex_lock(&g_m);
    MockState* s = StateFor(p);
    s->unblockCount++;                         // granted/runnable — NOT auto-resumed
    pthread_cond_broadcast(&g_event);          // wake WaitUntilGranted
    pthread_mutex_unlock(&g_m);
}

} // namespace brook

// ---------------------------------------------------------------------------
// Test orchestration helpers
// ---------------------------------------------------------------------------

namespace {

void WaitUntilBlocked(Process* p) {
    pthread_mutex_lock(&g_m);
    MockState* s = StateFor(p);
    while (!s->inBlock) {
        timespec ts; Deadline(&ts, 10);
        if (pthread_cond_timedwait(&g_event, &g_m, &ts) == ETIMEDOUT) {
            std::fprintf(stderr, "FATAL: victim never blocked (lock logic hung)\n");
            pthread_mutex_unlock(&g_m); _exit(2);
        }
    }
    pthread_mutex_unlock(&g_m);
}

void WaitUntilGranted(Process* p) {
    pthread_mutex_lock(&g_m);
    MockState* s = StateFor(p);
    while (s->unblockCount == 0) {
        timespec ts; Deadline(&ts, 10);
        if (pthread_cond_timedwait(&g_event, &g_m, &ts) == ETIMEDOUT) {
            std::fprintf(stderr, "FATAL: victim never granted (lock logic hung)\n");
            pthread_mutex_unlock(&g_m); _exit(2);
        }
    }
    pthread_mutex_unlock(&g_m);
}

void AllowResume(Process* p) {
    pthread_mutex_lock(&g_m);
    MockState* s = StateFor(p);
    s->resume = true;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&g_m);
}

int g_failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "  FAIL: %s\n", (msg)); ++g_failures; } \
    else         { std::fprintf(stderr, "  ok:   %s\n", (msg)); } \
} while (0)

void ResetProc(Process& p, int id) {
    p = Process{};
    p.id = id;
}

void ResetMock() {
    pthread_mutex_lock(&g_m);
    g_stateCount = 0;
    pthread_mutex_unlock(&g_m);
}

KRwLock g_rw;

// Victim that only acquires (models a thread killed while/after being granted).
struct AcqArg { Process* p; bool write; };
void* VictimAcquire(void* arg) {
    AcqArg* a = static_cast<AcqArg*>(arg);
    tl_current = a->p;
    if (a->write) brook::KRwLockWriteLock(&g_rw);
    else          brook::KRwLockReadLock(&g_rw);
    tl_current = nullptr;
    return nullptr;
}

// Victim that acquires then releases (normal handoff).
void* VictimAcquireRelease(void* arg) {
    AcqArg* a = static_cast<AcqArg*>(arg);
    tl_current = a->p;
    if (a->write) { brook::KRwLockWriteLock(&g_rw);  brook::KRwLockWriteUnlock(&g_rw); }
    else          { brook::KRwLockReadLock(&g_rw);   brook::KRwLockReadUnlock(&g_rw);  }
    tl_current = nullptr;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void T_uncontended_read() {
    std::fprintf(stderr, "[T1] uncontended read lock/unlock\n");
    ResetMock(); brook::KRwLockInit(&g_rw);
    Process self; ResetProc(self, 1); tl_current = &self;
    brook::KRwLockReadLock(&g_rw);
    EXPECT(g_rw.readerCount == 1, "readerCount==1 after read lock");
    EXPECT(self.heldReadLock == &g_rw, "heldReadLock recorded");
    brook::KRwLockReadUnlock(&g_rw);
    EXPECT(g_rw.readerCount == 0, "readerCount==0 after read unlock");
    EXPECT(self.heldReadLock == nullptr, "heldReadLock cleared");
    tl_current = nullptr;
}

static void T_uncontended_write() {
    std::fprintf(stderr, "[T2] uncontended write lock/unlock\n");
    ResetMock(); brook::KRwLockInit(&g_rw);
    Process self; ResetProc(self, 1); tl_current = &self;
    brook::KRwLockWriteLock(&g_rw);
    EXPECT(g_rw.writerActive == 1, "writerActive==1 after write lock");
    EXPECT(self.heldWriteLock == &g_rw, "heldWriteLock recorded");
    brook::KRwLockWriteUnlock(&g_rw);
    EXPECT(g_rw.writerActive == 0, "writerActive==0 after write unlock");
    EXPECT(self.heldWriteLock == nullptr, "heldWriteLock cleared");
    tl_current = nullptr;
}

// BRO-162(a): writer granted in the wake window, then killed before recording.
static void T_writer_wake_window_leak() {
    std::fprintf(stderr, "[T3] writer wake-window: kill after grant must not leak writerActive\n");
    ResetMock(); brook::KRwLockInit(&g_rw);
    Process H, V; ResetProc(H, 1); ResetProc(V, 2);

    tl_current = &H; brook::KRwLockWriteLock(&g_rw); tl_current = nullptr; // H holds
    EXPECT(g_rw.writerActive == 1, "precondition: H holds write lock");

    AcqArg arg{ &V, true };
    pthread_t th; pthread_create(&th, nullptr, VictimAcquire, &arg);
    WaitUntilBlocked(&V);
    EXPECT(g_rw.writeWaitHead == &V, "precondition: V queued as writer");

    tl_current = &H; brook::KRwLockWriteUnlock(&g_rw); tl_current = nullptr; // grant to V
    WaitUntilGranted(&V);  // V is runnable with the grant applied, but still parked

    brook::KRwLockCleanupOnExit(&V);  // kill lands in the window
    EXPECT(g_rw.writerActive == 0, "writerActive released after killing granted writer");

    AllowResume(&V); pthread_join(th, nullptr);
}

// BRO-162(b): reader granted (readerCount++) in the wake window, then killed.
static void T_reader_wake_window_leak() {
    std::fprintf(stderr, "[T4] reader wake-window: kill after grant must not leak readerCount\n");
    ResetMock(); brook::KRwLockInit(&g_rw);
    Process H, V; ResetProc(H, 1); ResetProc(V, 2);

    tl_current = &H; brook::KRwLockWriteLock(&g_rw); tl_current = nullptr; // H holds write
    EXPECT(g_rw.writerActive == 1, "precondition: H holds write lock");

    AcqArg arg{ &V, false };
    pthread_t th; pthread_create(&th, nullptr, VictimAcquire, &arg);
    WaitUntilBlocked(&V);
    EXPECT(g_rw.readWaitHead == &V, "precondition: V queued as reader");

    tl_current = &H; brook::KRwLockWriteUnlock(&g_rw); tl_current = nullptr; // batch-wake readers
    WaitUntilGranted(&V);
    EXPECT(g_rw.readerCount == 1, "waker counted the grant on V's behalf");

    brook::KRwLockCleanupOnExit(&V);  // kill lands in the window
    EXPECT(g_rw.readerCount == 0, "readerCount restored after killing granted reader");

    AllowResume(&V); pthread_join(th, nullptr);
}

// Regression: a granted writer that actually resumes acquires then releases cleanly.
static void T_writer_handoff_normal() {
    std::fprintf(stderr, "[T5] writer handoff (normal resume) releases cleanly\n");
    ResetMock(); brook::KRwLockInit(&g_rw);
    Process H, V; ResetProc(H, 1); ResetProc(V, 2);

    tl_current = &H; brook::KRwLockWriteLock(&g_rw); tl_current = nullptr;

    AcqArg arg{ &V, true };
    pthread_t th; pthread_create(&th, nullptr, VictimAcquireRelease, &arg);
    WaitUntilBlocked(&V);

    tl_current = &H; brook::KRwLockWriteUnlock(&g_rw); tl_current = nullptr;
    AllowResume(&V);                 // V resumes, holds, then releases
    pthread_join(th, nullptr);

    EXPECT(g_rw.writerActive == 0, "writerActive==0 after full handoff");
    EXPECT(V.heldWriteLock == nullptr, "V released its write ownership");
}

// Regression: batch reader wake — both readers resume, hold, and release.
static void T_reader_batch_handoff_normal() {
    std::fprintf(stderr, "[T6] reader batch handoff (normal resume) releases cleanly\n");
    ResetMock(); brook::KRwLockInit(&g_rw);
    Process H, R1, R2; ResetProc(H, 1); ResetProc(R1, 2); ResetProc(R2, 3);

    tl_current = &H; brook::KRwLockWriteLock(&g_rw); tl_current = nullptr;

    AcqArg a1{ &R1, false }, a2{ &R2, false };
    pthread_t t1, t2;
    pthread_create(&t1, nullptr, VictimAcquireRelease, &a1);
    WaitUntilBlocked(&R1);
    pthread_create(&t2, nullptr, VictimAcquireRelease, &a2);
    WaitUntilBlocked(&R2);

    tl_current = &H; brook::KRwLockWriteUnlock(&g_rw); tl_current = nullptr; // wakes both
    AllowResume(&R1); AllowResume(&R2);
    pthread_join(t1, nullptr); pthread_join(t2, nullptr);

    EXPECT(g_rw.readerCount == 0, "readerCount==0 after both readers release");
    EXPECT(g_rw.writerActive == 0, "no writer active");
}

int main() {
    T_uncontended_read();
    T_uncontended_write();
    T_writer_wake_window_leak();
    T_reader_wake_window_leak();
    T_writer_handoff_normal();
    T_reader_batch_handoff_normal();

    if (g_failures == 0) {
        std::fprintf(stderr, "test_krwlock_cleanup: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_krwlock_cleanup: %d FAILURE(S)\n", g_failures);
    return 1;
}
