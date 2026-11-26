// test_epoll_instance_lock — real syscall_epoll.cpp under host pthreads.
//
// Covers the per-instance epoll locking (see "Locking" in syscall_epoll.cpp):
//  * a readiness check stalled inside one instance's scan must not block
//    epoll_wait/epoll_ctl/poll on an unrelated instance (the Dell gen 433
//    Xwayland/Steam convoy behind the old global graph mutex);
//  * nested epoll readiness and ELOOP validation;
//  * epoll_ctl racing epoll_wait on one instance, several waiters;
//  * closing watched fds (including a nested epoll fd) during waits.
//
// KMutex is a real (spin-yield) mutex here, each thread is its own task
// sharing one fd table, and user memory is host memory. Readiness sources
// are fakes: sockets with a per-socket "gate" that parks the scanning thread
// inside SockPollReady, and eventfds whose counter the test drives.
//
// Only C headers and pthreads: libc++ <thread>/<mutex> pull <cerrno>, whose
// E* macros collide with the kernel's errno constants, and glibc string.h,
// which collides with the kernel one (selected here as in
// test_syscall_user_memory).
#include "spinlock.h"
#include "string.h"
#define __BROOK_KERNEL__
#include "pipe.h"
#undef __BROOK_KERNEL__
#include "syscall_epoll.cpp"
#include "intel_bdw_prompt_wake.h"

#include <initializer_list>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

using namespace brook;

static unsigned g_failures = 0;
#define CHECK(c) do { if (!(c)) { ++g_failures; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)

// ---------------------------------------------------------------------------
// Kernel stubs
// ---------------------------------------------------------------------------
template <typename T> static T Ld(const T& v) { return __atomic_load_n(&v, __ATOMIC_ACQUIRE); }
template <typename T> static void St(T& v, T x) { __atomic_store_n(&v, x, __ATOMIC_RELEASE); }
template <typename T> static void Inc(T& v) { __atomic_fetch_add(&v, T(1), __ATOMIC_ACQ_REL); }
template <typename T> static void Add(T& v, T x) { __atomic_fetch_add(&v, x, __ATOMIC_ACQ_REL); }
static void SleepUs(long us) { ::timespec t{us / 1000000, (us % 1000000) * 1000}; ::nanosleep(&t, nullptr); }
static void SleepMs(long ms) { SleepUs(ms * 1000); }
struct LockGuard {
    pthread_mutex_t* m;
    explicit LockGuard(pthread_mutex_t* mutex) : m(mutex) { pthread_mutex_lock(m); }
    ~LockGuard() { pthread_mutex_unlock(m); }
};
template <typename F> struct Thread {
    F fn;
    pthread_t id;
    explicit Thread(F f) : fn(f) { pthread_create(&id, nullptr, &Thread::Run, this); }
    Thread(const Thread&) = delete;
    static void* Run(void* self) { static_cast<Thread*>(self)->fn(); return nullptr; }
    void join() { pthread_join(id, nullptr); }
};

static Process* g_proc;
static pthread_mutex_t g_fdLock = PTHREAD_MUTEX_INITIALIZER;
static constexpr int kSockets = 16;
static bool g_sockGate[kSockets];
static bool g_sockInside[kSockets];
static bool g_sockReadable[kSockets];
static unsigned g_eventFdFrees;
static unsigned g_epollFrees;

extern "C" void* kmalloc(uint64_t size) { return calloc(1, size ? size : 1); }
extern "C" void kfree(void* p) { free(p); }
namespace brook {
uint32_t HidrawReady(HidrawFile*, uint32_t) { return 0; }
extern "C" void SerialPrintf(const char*, ...) {}
extern "C" void SerialPuts(const char*) {}
// Each thread is its own task sharing the leader's fd table (CLONE_FILES),
// so per-task fields such as wakeupTick are not shared between threads.
Process* ProcessCurrent() {
    static thread_local Process* task;
    if (!task) {
        task = static_cast<Process*>(calloc(1, sizeof(Process)));
        task->pid = g_proc->pid; task->fds = g_proc->fds;
    }
    return task;
}
void SchedulerKernelRegionEnter(Process*) {}
void SchedulerKernelRegionLeave(Process*) {}
uint32_t SmpCurrentCpuIndex() { return 0; }

void KMutexInit(KMutex* m) { __builtin_memset(m, 0, sizeof(*m)); }
void KMutexLock(KMutex* m) {
    while (__atomic_exchange_n(&m->locked, 1u, __ATOMIC_ACQUIRE)) sched_yield();
}
void KMutexUnlock(KMutex* m) { __atomic_store_n(&m->locked, 0u, __ATOMIC_RELEASE); }
bool KMutexTryLock(KMutex* m) { return !__atomic_exchange_n(&m->locked, 1u, __ATOMIC_ACQUIRE); }

static void Finalize(FdType type, void* handle) {
    if (type == FdType::EpollFd) { if (EpollFdUnref(static_cast<EpollInstance*>(handle))) Inc(g_epollFrees); }
    else if (type == FdType::EventFd) { kfree(handle); Inc(g_eventFdFrees); }
}
FdEntry* FdGetRef(Process* proc, int fd) {
    LockGuard g(&g_fdLock);
    if (!proc || fd < 0 || fd >= static_cast<int>(MAX_FDS)) return nullptr;
    auto& e = proc->fds[fd];
    if (e.type == FdType::None || e.closing) return nullptr;
    ++e.pinCount; return &e;
}
void FdPut(Process* proc, int fd) {
    FdType type; void* handle;
    {
        LockGuard g(&g_fdLock);
        auto& e = proc->fds[fd];
        if (--e.pinCount || !e.closing) return;
        type = e.type; handle = e.handle; FdSlotClear(e);
    }
    Finalize(type, handle);
}
int FdAlloc(Process* proc, FdType type, void* handle, uint32_t, uint8_t flags, uint8_t fdFlags) {
    LockGuard g(&g_fdLock);
    for (int i = 0; i < static_cast<int>(MAX_FDS); ++i) {
        auto& e = proc->fds[i];
        if (e.type != FdType::None) continue;
        FdSlotClear(e); e.type = type; e.handle = handle; e.flags = flags; e.fdFlags = fdFlags;
        return i;
    }
    return -EMFILE;
}
static void TestClose(int fd) {
    FdType type; void* handle;
    {
        LockGuard g(&g_fdLock);
        auto& e = g_proc->fds[fd];
        if (e.type == FdType::None || e.closing) return;
        if (e.pinCount) { e.closing = 1; return; } // last FdPut finalizes
        type = e.type; handle = e.handle; FdSlotClear(e);
    }
    Finalize(type, handle);
}

uint64_t CopyFromUserBytes(void* d, uint64_t s, uint64_t n) { __builtin_memcpy(d, reinterpret_cast<const void*>(s), n); return n; }
uint64_t CopyToUserBytes(uint64_t d, const void* s, uint64_t n) { __builtin_memcpy(reinterpret_cast<void*>(d), s, n); return n; }
bool UserBufferReadable(uint64_t, uint64_t) { return true; }
bool UserBufferWritable(uint64_t, uint64_t) { return true; }

static thread_local bool t_signal = false;
bool HasPendingSignals() { return t_signal; }
int InstallWaitSignalMask(uint64_t, uint64_t) { return 0; }
static uint64_t NowNs() {
    ::timespec ts; ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}
uint64_t ClockNowNs(uint64_t) { return NowNs(); }
uint64_t WaitDeadlineNs(uint64_t ms) { return ms == ~0ull ? ~0ull : NowNs() + ms * 1000000ull; }
uint64_t WaitWakeupTick(uint64_t, uint64_t) { return 0; }
static BdwFence* g_fenceOnBlock;
static unsigned g_fenceWakes;
void SchedulerUnblock(Process* task) {
    if (task == ProcessCurrent()) ++g_fenceWakes;
}
// A "sleep" is a short host sleep, unless a deterministic fence test
// publishes the completion with the production waiter already armed.
void SchedulerBlockInterruptible(Process*) {
    if (g_fenceOnBlock) {
        auto* fence = g_fenceOnBlock;
        g_fenceOnBlock = nullptr;
        CHECK(BdwFenceComplete(fence, false));
    } else SleepUs(200);
}

EventFdSnapshot EventFdReady(EventFdData* efd, uint32_t events) {
    EventFdSnapshot s{};
    s.identity = __atomic_load_n(&efd->identity, __ATOMIC_ACQUIRE);
    s.inGeneration = __atomic_load_n(&efd->inGeneration, __ATOMIC_ACQUIRE);
    s.outGeneration = __atomic_load_n(&efd->outGeneration, __ATOMIC_ACQUIRE);
    if ((events & EPOLLIN) && __atomic_load_n(&efd->counter, __ATOMIC_ACQUIRE)) s.ready |= EPOLLIN;
    return s;
}
bool EventFdWaitGroup::add(EventFdData*, Process*) { return true; }
EventFdWaitGroup::~EventFdWaitGroup() {}
bool SockPollReady(int si, bool read, bool) {
    if (si >= 0 && si < kSockets && Ld(g_sockGate[si])) {
        St(g_sockInside[si], true);
        while (Ld(g_sockGate[si])) SleepUs(100);
        St(g_sockInside[si], false);
    }
    return read && si >= 0 && si < kSockets && Ld(g_sockReadable[si]);
}
bool SockPollHangup(int) { return false; }
void SockSetPollWaiter(int, Process*) {}
void SockClearPollWaiter(int, Process*) {}
uint64_t NetTcpPoolWaitRegister(Process*) { return 0; }
void NetTcpPoolWaitUnregister(uint64_t) {}
// Fd types the tests never create.
uint32_t NetlinkReady(NetlinkSocket*, uint32_t) { return 0; }
void NetlinkRef(NetlinkSocket*) {}
void NetlinkUnref(NetlinkSocket*) {}
uint32_t NamedFifoReady(const FdEntry*, uint32_t) { return 0; }
NamedFifoOpen* FdNamedFifo(const FdEntry*) { return nullptr; }
uint32_t SignalFdReady(Process*, SignalFd*, uint32_t) { return 0; }
uint32_t UnixSocketReady(UnixSocketData*, uint32_t) { return 0; }
bool TimerFdReady(TimerFdData*) { return false; }
uint64_t TimerFdDeadline(TimerFdData*, uint64_t*) { return ~0ull; }
bool AudioMixerGetProgress(AudioStreamId, AudioStreamProgress*) { return false; }
} // namespace brook

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static int Create() { return static_cast<int>(sys_epoll_create1(0, 0, 0, 0, 0, 0)); }
static int Ctl(int ep, int op, int fd, uint32_t events, uint64_t data) {
    EpollEvent ev{}; ev.events = events; ev.data.u64 = data;
    return static_cast<int>(sys_epoll_ctl(ep, op, fd, reinterpret_cast<uint64_t>(&ev), 0, 0));
}
static int Wait(int ep, EpollEvent* out, int max, int timeoutMs) {
    return static_cast<int>(sys_epoll_wait(ep, reinterpret_cast<uint64_t>(out), max,
                                           static_cast<uint64_t>(static_cast<int64_t>(timeoutMs)), 0, 0));
}
static int MakeSocket(int index) {
    return FdAlloc(g_proc, FdType::Socket, reinterpret_cast<void*>(static_cast<uintptr_t>(index + 1)), 0, 0, 0);
}
static EventFdData* g_efd[MAX_FDS];
static int EventFd(uint64_t counter) {
    auto* e = static_cast<EventFdData*>(kmalloc(sizeof(EventFdData)));
    static uint64_t identity = 1;
    e->identity = __atomic_fetch_add(&identity, 1, __ATOMIC_ACQ_REL); e->counter = counter; e->refCount = 1;
    int fd = FdAlloc(g_proc, FdType::EventFd, e, 0, 0, 0);
    g_efd[fd] = e;
    return fd;
}
static void Signal(int fd, uint64_t counter) {
    __atomic_store_n(&g_efd[fd]->counter, counter, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_efd[fd]->inGeneration, 1, __ATOMIC_ACQ_REL);
}
static EpollInstance* Instance(int ep) { return static_cast<EpollInstance*>(g_proc->fds[ep].handle); }

// Run fn on a thread; false if it did not finish within ms.
template <typename F>
static bool FinishesWithin(int ms, F fn) {
    bool done = false;
    Thread t([&] { fn(); St(done, true); });
    const uint64_t end = NowNs() + static_cast<uint64_t>(ms) * 1000000ull;
    while (!Ld(done) && NowNs() < end) SleepMs(1);
    const bool finished = Ld(done);
    if (!finished) {
        // Release every gate so the stuck thread can be joined.
        for (auto& gate : g_sockGate) St(gate, false);
    }
    t.join();
    return finished;
}
static void WaitInside(int si) {
    for (int i = 0; i < 5000 && !Ld(g_sockInside[si]); ++i) SleepMs(1);
    CHECK(Ld(g_sockInside[si]));
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// One instance's scan is parked inside a readiness check. Other instances'
// waits, ctl and poll readiness proceed; the stalled instance's own ctl
// proceeds too (no epoll lock is held across watchReady).
static void TestIndependentInstances() {
    const int ep1 = Create(), ep2 = Create(), parent = Create(), child = Create();
    const int s0 = MakeSocket(0), e2 = EventFd(1), e3 = EventFd(0), s1 = MakeSocket(1);
    CHECK(Ctl(ep1, EPOLL_CTL_ADD, s0, EPOLLIN, 100) == 0);
    CHECK(Ctl(ep2, EPOLL_CTL_ADD, e2, EPOLLIN, 200) == 0);
    // A nested pair whose leaf stalls: parent -> child -> s1.
    CHECK(Ctl(child, EPOLL_CTL_ADD, s1, EPOLLIN, 1) == 0);
    CHECK(Ctl(parent, EPOLL_CTL_ADD, child, EPOLLIN, 2) == 0);

    St(g_sockGate[0], true); St(g_sockGate[1], true);
    Thread stalled([&] { EpollEvent ev[4]; CHECK(Wait(ep1, ev, 4, 0) == 0); });
    Thread stalledNested([&] { EpollEvent ev[4]; CHECK(Wait(parent, ev, 4, 0) == 0); });
    WaitInside(0); WaitInside(1);

    CHECK(FinishesWithin(2000, [&] {
        EpollEvent ev[4];
        CHECK(Wait(ep2, ev, 4, 0) == 1);
        CHECK(ev[0].data.u64 == 200);
        CHECK(Wait(ep2, ev, 4, -1) == 1);
        CHECK(EpollPollReady(ProcessCurrent(), Instance(ep2)) == 1);
        CHECK(Ctl(ep2, EPOLL_CTL_ADD, e3, EPOLLIN, 300) == 0);
        CHECK(Ctl(ep2, EPOLL_CTL_MOD, e3, EPOLLIN, 301) == 0);
        CHECK(Ctl(ep2, EPOLL_CTL_DEL, e3, 0, 0) == 0);
        // The stalled instances themselves accept ctl.
        CHECK(Ctl(ep1, EPOLL_CTL_ADD, e3, EPOLLIN, 101) == 0);
        CHECK(Ctl(ep1, EPOLL_CTL_DEL, e3, 0, 0) == 0);
        CHECK(Ctl(child, EPOLL_CTL_ADD, e3, EPOLLIN, 3) == 0);
        CHECK(Ctl(child, EPOLL_CTL_DEL, e3, 0, 0) == 0);
        // A new epoll edge elsewhere validates the graph by walking
        // instances, never readiness, so it also proceeds.
        const int a = Create(), b = Create();
        CHECK(Ctl(a, EPOLL_CTL_ADD, b, EPOLLIN, 0) == 0);
        TestClose(a); TestClose(b);
    }));
    CHECK(Ld(g_sockInside[0]) && Ld(g_sockInside[1])); // still parked
    St(g_sockGate[0], false); St(g_sockGate[1], false);
    stalled.join(); stalledNested.join();
    for (int fd : {ep1, ep2, parent, child, s0, e2, e3, s1}) TestClose(fd);
}

static void TestNested() {
    const int parent = Create(), child = Create(), grand = Create();
    const int e = EventFd(0), s = MakeSocket(2);
    CHECK(Ctl(grand, EPOLL_CTL_ADD, e, EPOLLIN, 7) == 0);
    CHECK(Ctl(child, EPOLL_CTL_ADD, grand, EPOLLIN, 8) == 0);
    CHECK(Ctl(parent, EPOLL_CTL_ADD, child, EPOLLIN, 9) == 0);
    CHECK(Ctl(parent, EPOLL_CTL_ADD, s, EPOLLIN, 10) == 0);
    EpollEvent ev[4];
    CHECK(Wait(parent, ev, 4, 0) == 0);
    CHECK(EpollPollReady(ProcessCurrent(), Instance(parent)) == 0);
    Signal(e, 1);
    CHECK(Wait(parent, ev, 4, 0) == 1 && ev[0].data.u64 == 9 && ev[0].events == EPOLLIN);
    CHECK(Wait(child, ev, 4, 0) == 1 && ev[0].data.u64 == 8);
    CHECK(EpollPollReady(ProcessCurrent(), Instance(parent)) == 1);
    St(g_sockReadable[2], true);
    CHECK(Wait(parent, ev, 4, 0) == 2);
    CHECK(Wait(parent, ev, 1, 0) == 1); // maxevents bound
    St(g_sockReadable[2], false);
    Signal(e, 0);
    CHECK(Wait(parent, ev, 4, 0) == 0);
    // A blocked waiter on the parent wakes for a grandchild leaf.
    Thread waiter([&] { EpollEvent w[4]; CHECK(Wait(parent, w, 4, -1) == 1 && w[0].data.u64 == 9); });
    SleepMs(20);
    Signal(e, 1);
    waiter.join();
    Signal(e, 0);

    // Cycles and depth are refused and leave no entry behind.
    CHECK(Ctl(grand, EPOLL_CTL_ADD, parent, EPOLLIN, 0) == -ELOOP);
    CHECK(Ctl(grand, EPOLL_CTL_DEL, parent, 0, 0) == -ENOENT);
    CHECK(Ctl(child, EPOLL_CTL_ADD, child, EPOLLIN, 0) == -EINVAL);
    CHECK(Ctl(parent, EPOLL_CTL_ADD, child, EPOLLIN, 0) == -EEXIST);
    int chain[6];
    for (int& c : chain) c = Create();
    for (int i = 0; i < 4; ++i) CHECK(Ctl(chain[i], EPOLL_CTL_ADD, chain[i + 1], EPOLLIN, 0) == 0);
    CHECK(Ctl(chain[4], EPOLL_CTL_ADD, chain[5], EPOLLIN, 0) == -ELOOP); // 5 edges
    CHECK(Ctl(chain[5], EPOLL_CTL_ADD, chain[0], EPOLLIN, 0) == -ELOOP); // ancestor side
    CHECK(Ctl(chain[4], EPOLL_CTL_DEL, chain[5], 0, 0) == -ENOENT);
    CHECK(Ctl(chain[0], EPOLL_CTL_DEL, chain[1], 0, 0) == 0);
    CHECK(Ctl(chain[4], EPOLL_CTL_ADD, chain[5], EPOLLIN, 0) == 0);
    for (int c : chain) TestClose(c);
    for (int fd : {parent, child, grand, e, s}) TestClose(fd);
}

// Edge-triggered eventfd: one report per generation, and a ctl between scan
// and commit must not lose the edge.
static void TestEdgeTriggered() {
    const int ep = Create(), e = EventFd(1);
    CHECK(Ctl(ep, EPOLL_CTL_ADD, e, EPOLLIN | EPOLLET, 5) == 0);
    EpollEvent ev[2];
    CHECK(Wait(ep, ev, 2, 0) == 1);
    CHECK(Wait(ep, ev, 2, 0) == 0);
    Signal(e, 1);
    CHECK(Wait(ep, ev, 2, 0) == 1);
    CHECK(Wait(ep, ev, 2, 0) == 0);
    CHECK(Ctl(ep, EPOLL_CTL_MOD, e, EPOLLIN | EPOLLET, 6) == 0); // MOD re-arms
    CHECK(Wait(ep, ev, 2, 0) == 1 && ev[0].data.u64 == 6);
    CHECK(Wait(ep, ev, 2, 0) == 0);
    TestClose(e); TestClose(ep);
}

// Several threads wait on one instance while others ADD/MOD/DEL and signal.
static void TestCtlWaitStress() {
    const int ep = Create();
    constexpr int kFds = 48;
    int fds[kFds];
    for (int& fd : fds) fd = EventFd(0);
    for (int i = 0; i < kFds; i += 2) CHECK(Ctl(ep, EPOLL_CTL_ADD, fds[i], EPOLLIN, 1000 + i) == 0);
    bool stop = false;
    unsigned reports = 0, bad = 0;
    auto waitLoop = [&](int w) {
        EpollEvent ev[64];
        while (!Ld(stop)) {
            int n = Wait(ep, ev, 64, w & 1 ? 2 : 0);
            if (n < 0) { Inc(bad); continue; }
            for (int i = 0; i < n; ++i) {
                const uint64_t d = ev[i].data.u64;
                if (d < 1000 || d >= 1000 + 2 * kFds) Inc(bad);
                if (!(ev[i].events & EPOLLIN)) Inc(bad);
            }
            Add(reports, static_cast<unsigned>(n));
        }
    };
    auto ctlLoop = [&](int c) {
        unsigned seed = 1234 + c;
        while (!Ld(stop)) {
            seed = seed * 1103515245 + 12345;
            const int i = (seed >> 8) % kFds;
            const int op = ((seed >> 20) % 3) + 1;
            const uint32_t flags = (seed >> 24) & 1 ? EPOLLIN | EPOLLET : EPOLLIN;
            int r = Ctl(ep, op, fds[i], flags, 1000 + i + ((seed >> 25) & 1) * kFds);
            if (r != 0 && r != -EEXIST && r != -ENOENT) Inc(bad);
        }
    };
    Thread w0([&] { waitLoop(0); });
    Thread w1([&] { waitLoop(1); });
    Thread w2([&] { waitLoop(2); });
    Thread w3([&] { waitLoop(3); });
    Thread c0([&] { ctlLoop(0); });
    Thread c1([&] { ctlLoop(1); });
    Thread signaller([&] {
        unsigned seed = 99;
        while (!Ld(stop)) {
            seed = seed * 1103515245 + 12345;
            Signal(fds[(seed >> 8) % kFds], (seed >> 16) & 1);
        }
    });
    SleepMs(600);
    St(stop, true);
    w0.join(); w1.join(); w2.join(); w3.join(); c0.join(); c1.join(); signaller.join();
    CHECK(bad == 0);
    CHECK(reports > 0);
    // Quiesced: the interest list answers exactly.
    for (int i = 0; i < kFds; ++i) { Ctl(ep, EPOLL_CTL_DEL, fds[i], 0, 0); Signal(fds[i], i % 3 == 0); }
    for (int i = 0; i < kFds; ++i) CHECK(Ctl(ep, EPOLL_CTL_ADD, fds[i], EPOLLIN, i) == 0);
    EpollEvent ev[64];
    const int n = Wait(ep, ev, 64, 0);
    CHECK(n == (kFds + 2) / 3);
    for (int i = 0; i < n; ++i) CHECK(ev[i].data.u64 % 3 == 0);
    // Every waiter of one instance sees level-triggered readiness.
    for (int i = 0; i < kFds; ++i) Signal(fds[i], 0);
    int woke = 0;
    auto levelWaiter = [&] { EpollEvent v[4]; if (Wait(ep, v, 4, 3000) == 1) Inc(woke); };
    Thread l0(levelWaiter), l1(levelWaiter), l2(levelWaiter);
    SleepMs(20);
    Signal(fds[5], 1);
    l0.join(); l1.join(); l2.join();
    CHECK(woke == 3);
    for (int fd : fds) TestClose(fd);
    TestClose(ep);
}

// Watched fds (a leaf and a nested epoll fd) close while waits are scanning
// and sleeping. Pins keep them alive; the entry is dropped afterwards.
static void TestCloseDuringWait() {
    const int ep = Create(), child = Create();
    const int gone = EventFd(0), childLeaf = EventFd(0), wake = EventFd(0), s = MakeSocket(3);
    CHECK(Ctl(child, EPOLL_CTL_ADD, childLeaf, EPOLLIN, 1) == 0);
    CHECK(Ctl(child, EPOLL_CTL_ADD, s, EPOLLIN, 4) == 0);
    CHECK(Ctl(ep, EPOLL_CTL_ADD, gone, EPOLLIN, 2) == 0);
    CHECK(Ctl(ep, EPOLL_CTL_ADD, child, EPOLLIN, 3) == 0);
    CHECK(Ctl(ep, EPOLL_CTL_ADD, wake, EPOLLIN, 5) == 0);
    const unsigned eventFdFrees = Ld(g_eventFdFrees), epollFrees = Ld(g_epollFrees);
    // Park a scan inside the child's socket, then close both watched fds.
    St(g_sockGate[3], true);
    Thread waiter([&] {
        EpollEvent ev[4];
        const int n = Wait(ep, ev, 4, -1);
        CHECK(n == 1 && ev[0].data.u64 == 5);
    });
    WaitInside(3);
    TestClose(gone);
    TestClose(child);
    CHECK(Ld(g_eventFdFrees) == eventFdFrees); // pinned by the parked walk
    CHECK(Ld(g_epollFrees) == epollFrees);
    St(g_sockGate[3], false);
    SleepMs(20); // sleeping now
    Signal(wake, 1);
    waiter.join();
    CHECK(Ld(g_eventFdFrees) == eventFdFrees + 1);
    CHECK(Ld(g_epollFrees) == epollFrees + 1);
    // The next scan cannot pin the closed fds and drops their entries.
    Signal(wake, 0);
    EpollEvent ev[4];
    CHECK(Wait(ep, ev, 4, 0) == 0);
    bool stale = false;
    for (int i = 0; i < Instance(ep)->count; ++i)
        stale |= Instance(ep)->entries[i].fd == gone || Instance(ep)->entries[i].fd == child;
    CHECK(!stale);
    // Fd-number reuse after the lazy drop: a new fd at the old number is not
    // watched until added.
    const int reused = EventFd(1);
    CHECK(reused == gone || reused == child);
    CHECK(Wait(ep, ev, 4, 0) == 0);
    for (int fd : {ep, childLeaf, wake, s, reused}) TestClose(fd);
}

static void TestSyncFileWake() {
    const int ep = Create(), nested = Create();
    auto* producer = BdwFenceCreate(false);
    SyncFileData file{1, 0, 77, BdwFenceCreate(true, nullptr, 0, producer)};
    const int fd = FdAlloc(g_proc, FdType::SyncFile, &file, 0, 0, 0);
    CHECK(Ctl(nested, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLONESHOT, 123) == 0);
    CHECK(Ctl(ep, EPOLL_CTL_ADD, nested, EPOLLIN, 456) == 0);
    EpollEvent event{};
    CHECK(Wait(ep, &event, 1, 0) == 0);
    BdwPromptWakeSet(true);
    g_fenceWakes = 0;
    g_fenceOnBlock = producer;
    CHECK(Wait(ep, &event, 1, 1000) == 1);
    CHECK(event.events == EPOLLIN && event.data.u64 == 456);
    CHECK(!g_fenceOnBlock && g_fenceWakes == 1);
    CHECK(Wait(nested, &event, 1, 0) == 1);
    CHECK(event.events == EPOLLIN && event.data.u64 == 123);
    CHECK(SyncFileState(file) == 2); // failed fences are still readable
    CHECK(Wait(ep, &event, 1, 0) == 0); // nested one-shot was consumed
    BdwGpuEventSignal(BdwGpuEvent::Retire);
    CHECK(g_fenceWakes == 1);
    BdwPromptWakeSet(false);
    TestClose(ep); TestClose(nested); TestClose(fd);
    BdwFenceUnref(file.fence);
    BdwFenceUnref(producer);
}

int main() {
    g_proc = static_cast<Process*>(calloc(1, sizeof(Process)));
    g_proc->pid = 7;
    g_proc->fds = static_cast<FdEntry*>(calloc(MAX_FDS, sizeof(FdEntry)));
    TestIndependentInstances();
    TestNested();
    TestEdgeTriggered();
    TestCtlWaitStress();
    TestCloseDuringWait();
    TestSyncFileWake();
    if (g_failures) { fprintf(stderr, "%u failure(s)\n", g_failures); return 1; }
    printf("test_epoll_instance_lock: all passed\n");
    return 0;
}
