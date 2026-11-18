// syscall_epoll.cpp -- epoll instances and epoll_create/ctl/wait.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "signal_fd.h"
#include "sync_file.h"
#include "memory/object_cache.h"
#include "epoll_diagnostics.h"
#include "net.h"
#include "smp.h"
#include "syscall_internal.h"
#include "syscall_signal.h"
#include "syscall_eventfd.h"
#include "syscall_timerfd.h"
#include "syscall_dsp.h"
#include "syscall_pipe.h"
#include "syscall_unix_socket.h"
#include "syscall_time.h"
#include "syscall_poll.h"
#include "syscall_epoll.h"
#include "syscall_netlink.h"
#include "syscall_handlers.h"

namespace brook {

struct EpollEvent {
    uint32_t events;
    union {
        int      fd;
        uint64_t u64;
        uint32_t u32;
        void*    ptr;
    } data;
} __attribute__((packed)); // Linux x86_64 ABI: packed to 12 bytes
void EpollFdHandleRef(void* handle) { EpollFdRef(static_cast<EpollInstance*>(handle)); }

// ---------------------------------------------------------------------------
// epoll — Linux epoll_create / epoll_ctl / epoll_wait
// ---------------------------------------------------------------------------
//
// Wayland compositors (weston, etc.) use epoll as their primary event loop.
// We implement a simple but correct epoll supporting EPOLLIN/EPOLLOUT on
// pipes, sockets, eventfds, timerfds, and other fd types.
//
// Data structures:
//   EpollInstance  — the epoll fd's state (interest list + ready list)
//   EpollEntry     — one registered fd (interest list entry)

static constexpr uint32_t EPOLLIN      = 0x001;
static constexpr uint32_t EPOLLOUT     = 0x004;
[[maybe_unused]] static constexpr uint32_t EPOLLRDHUP   = 0x2000;
[[maybe_unused]] static constexpr uint32_t EPOLLPRI     = 0x002;
static constexpr uint32_t EPOLLERR     = 0x008;
static constexpr uint32_t EPOLLHUP     = 0x010;
static constexpr uint32_t EPOLLET      = (1u << 31); // eventfd notifications; other sources remain LT
[[maybe_unused]] static constexpr uint32_t EPOLLONESHOT = (1u << 30);

[[maybe_unused]] static constexpr int EPOLL_CTL_ADD = 1;
static constexpr int EPOLL_CTL_DEL = 2;
static constexpr int EPOLL_CTL_MOD = 3;

// Serialize interest-list mutation and traversals, including graph validation.
// Never acquired by fd finalization, and never held across a userspace wait.
static KMutex g_epollGraphLock{};
struct EpollGraphGuard {
    EpollGraphGuard() { KMutexLock(&g_epollGraphLock); }
    ~EpollGraphGuard() { KMutexUnlock(&g_epollGraphLock); }
};
static constexpr int EPOLL_MAX_DEPTH = 5; // four epoll-to-epoll edges

// Check if a single fd is ready given the requested events mask.
// Returns the set of events that are actually ready.
static uint32_t EpollLeafReady(FdEntry* fde, uint32_t events)
{
    // A closed watch is not an fd producing a hangup event. In particular,
    // do not feed libuv orphan ERR|HUP notifications forever after close.
    if (!fde) return 0;

    if(fde->type==FdType::NetlinkSocket)return NetlinkReady(static_cast<NetlinkSocket*>(fde->handle),events);
    if(FdNamedFifo(fde))return NamedFifoReady(fde,events);
    if(fde->type==FdType::SyncFile && fde->handle)return SyncFileReady(*static_cast<SyncFileData*>(fde->handle),events);
    if(fde->type==FdType::SignalFd && fde->handle)
        return SignalFdReady(ProcessCurrent(),static_cast<SignalFd*>(fde->handle),events);

    if (fde->type == FdType::EventFd && fde->handle)
        return EventFdReady(static_cast<EventFdData*>(fde->handle), events).ready;

    uint32_t ready = 0;
    if (fde->type == FdType::Socket && fde->handle) {
        int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
        if (brook::SockPollHangup(si))
            ready |= EPOLLHUP | EPOLLRDHUP;
    }
    if (fde->type == FdType::UnixSocket && fde->handle)
        return UnixSocketReady(static_cast<UnixSocketData*>(fde->handle),events);
    if (fde->type == FdType::DevDsp) {
        // Walks pin the open description; scan only cached mixer state here.
        auto* dsp = static_cast<DspState*>(fde->handle);
        AudioStreamProgress progress{};
        if (!dsp || !AudioMixerGetProgress(dsp->mixerStreamId, &progress))
            return EPOLLERR;
        // Recovery occurs in write/query under ioLock, never in this scan.
        return (events & EPOLLOUT) && (progress.discontinuity || AudioStreamWritable(progress))
            ? EPOLLOUT : 0;
    }

    if (events & EPOLLIN) {
        bool readable = false;
        if (fde->type == FdType::Pipe && fde->handle) {
            auto* pb = static_cast<PipeBuffer*>(fde->handle);
            readable = (pb->count() > 0 || pb->writers == 0);
        } else if (fde->type == FdType::Socket && fde->handle) {
            int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
            readable = brook::SockPollReady(si, true, false);
        } else if (fde->type == FdType::TimerFd && fde->handle) {
            readable = TimerFdReady(static_cast<TimerFdData*>(fde->handle));
        } else if (fde->type == FdType::Vnode) {
            readable = true; // files always readable
        } else if (fde->type == FdType::DevTty && fde->handle) {
            auto* pair = static_cast<TtyDevicePair*>(fde->handle);
            auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
            readable = (rp->count() > 0 || rp->writers == 0);
        }
        if (readable) ready |= EPOLLIN;
    }

    if (events & EPOLLOUT) {
        bool writable = false;
        if (fde->type == FdType::Pipe && fde->handle) {
            // writable if write end and there's space
            if (fde->flags & 1) { // write end
                auto* pb = static_cast<PipeBuffer*>(fde->handle);
                writable = (pb->space() > 0);
            }
        } else if (fde->type == FdType::Socket && fde->handle) {
            int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
            writable = brook::SockPollReady(si, false, true);
        } else if (fde->type == FdType::Vnode || fde->type == FdType::DebugConsole) {
            writable = true;
        } else if (fde->type == FdType::DevTty) {
            writable = true;
        } else if (fde->type == FdType::MemFd) {
            writable = true;
        }
        if (writable) ready |= EPOLLOUT;
    }

    return ready;
}

// Heap-backed walk state: recursive kernel stacks must not contain MAX_FDS
// arrays. Pins survive registration/sleep/cleanup, even if ctl DEL or close
// changes the graph. Entries still use Brook's existing numeric-fd identity.
struct EpollWalk {
    Process* proc;
    FdEntry* pins[MAX_FDS];
    bool attempted[MAX_FDS];
    uint32_t masks[MAX_FDS];
    // Only selected events have delivery snapshots. Retained until copyout
    // succeeds under the graph mutex; peeking never changes a watch.
    struct Delivery { int index; EventFdSnapshot eventFd; } deliveries[MAX_FDS];
    struct Node {
        EpollInstance* ep;
        uint8_t ready[EPOLL_MAX_DEPTH]; // 0 unknown, 1 false, 2 true
        bool active;
        uint8_t collectedDepth;
        int height;
    } nodes[EPOLL_MAX_FDS + 1];
    int count;
    FdEntry* pin(int fd) {
        if (fd < 0 || fd >= EPOLL_MAX_FDS) return nullptr;
        if (!attempted[fd]) {
            attempted[fd] = true;
            pins[fd] = FdGetRef(proc, fd);
        }
        return pins[fd];
    }
    Node* node(EpollInstance* ep) {
        for (int i = 0; i < count; i++) if (nodes[i].ep == ep) return &nodes[i];
        if (count >= EPOLL_MAX_FDS + 1) return nullptr;
        auto* n = &nodes[count++]; *n = Node{}; n->ep = ep; return n;
    }
    bool any(EpollInstance* ep, int depth = 0) {
        if (depth >= EPOLL_MAX_DEPTH) return false;
        auto* n = node(ep);
        if (!n || n->active) return false; // guard fd-reuse-created cycles too
        if (n->ready[depth]) return n->ready[depth] == 2;
        n->active = true;
        bool result = false;
        for (int i = 0; i < ep->count && !result; i++) {
            auto e = ep->entries[i];
            if (e.fd >= 0) {
                if (!pin(e.fd)) { ep->entries[i].fd = -1; continue; }
                result = watchReady(e, depth);
            }
        }
        n->active = false; n->ready[depth] = result ? 2 : 1;
        return result;
    }
    uint32_t ready(int fd, uint32_t events, int depth) {
        auto* fde = pin(fd);
        if (fde && fde->type == FdType::EpollFd && fde->handle)
            return (events & EPOLLIN) && any(static_cast<EpollInstance*>(fde->handle), depth + 1) ? EPOLLIN : 0;
        return EpollLeafReady(fde, events);
    }
    uint32_t watchReady(const EpollEntry& entry, int depth, EventFdSnapshot* captured = nullptr) {
        auto* fde = pin(entry.fd);
        if(fde && fde->type==FdType::SyncFile && fde->handle && (entry.events&(EPOLLET|EPOLLONESHOT))) {
            auto* fence=static_cast<SyncFileData*>(fde->handle);
            EventFdSnapshot snapshot{fence->identity,1,0,SyncFileReady(*fence,entry.events)};
            if(captured)*captured=snapshot;
            return entry.eventFdIdentity!=fence->identity ? snapshot.ready : 0;
        }
        if (!(entry.events & EPOLLET) || !fde ||
            fde->type != FdType::EventFd || !fde->handle)
            return ready(entry.fd, entry.events, depth);
        auto snapshot = EventFdReady(static_cast<EventFdData*>(fde->handle), entry.events);
        if (captured) *captured = snapshot;
        const bool notified = entry.eventFdIdentity != snapshot.identity ||
            ((entry.events & EPOLLIN) && entry.inGeneration != snapshot.inGeneration) ||
            ((entry.events & EPOLLOUT) && entry.outGeneration != snapshot.outGeneration);
        return notified ? snapshot.ready : 0;
    }
    void collect(EpollInstance* ep, int depth = 0) {
        if (depth >= EPOLL_MAX_DEPTH) return;
        auto* n = node(ep);
        if (!n || n->collectedDepth >= EPOLL_MAX_DEPTH - depth) return;
        n->collectedDepth = EPOLL_MAX_DEPTH - depth;
        for (int i = 0; i < ep->count; i++) {
            auto e = ep->entries[i];
            auto* fde = pin(e.fd);
            if (!fde || !fde->handle) continue;
            if (fde->type == FdType::EpollFd) {
                if (e.events & EPOLLIN) collect(static_cast<EpollInstance*>(fde->handle), depth + 1);
            } else masks[e.fd] |= e.events | EPOLLERR | EPOLLHUP;
        }
    }
    // All edges participate in cycle/depth validation, regardless of masks.
    int height(EpollInstance* ep, int depth = 0) {
        if (depth >= EPOLL_MAX_DEPTH) return EPOLL_MAX_DEPTH + 1;
        auto* n = node(ep);
        if (!n || n->active) return EPOLL_MAX_DEPTH + 1;
        if (n->height) return n->height;
        n->active = true;
        int h = 1;
        for (int i = 0; i < ep->count; i++) {
            auto* fde = pin(ep->entries[i].fd);
            if (!fde || fde->type != FdType::EpollFd || !fde->handle) continue;
            int child = height(static_cast<EpollInstance*>(fde->handle), depth + 1) + 1;
            if (child > h) h = child;
            if (h > EPOLL_MAX_DEPTH) break;
        }
        n->active = false; n->height = h; return h;
    }
    // node() initialises each node it hands out, so forgetting the memo is
    // O(1). Clearing all EPOLL_MAX_FDS+1 nodes (about 24 KiB) here and the
    // whole walk (about 78 KiB) in EpollWalkOwner ran on every epoll_wait
    // iteration and was the 9% memset in the Dell gen 423 Torchlight II
    // profile (a busy epoll_wait caller), not GPU buffer zero-fill.
    void clearMemo() { count = 0; }
    // Fields that must start zeroed: pins/attempted (pin()), masks (OR-ed by
    // collect(), scanned by the waiter registration) and the memo. Delivery
    // records are written before they are read (EpollScanReady) and nodes
    // are initialised by node().
    void reset(Process* owner) {
        proc = owner;
        __builtin_memset(pins, 0, sizeof(pins));
        __builtin_memset(attempted, 0, sizeof(attempted));
        __builtin_memset(masks, 0, sizeof(masks));
        count = 0;
    }
};
// Reuse free scratch objects on hot epoll paths. Sleepers retain their own
// objects but do not occupy cache slots; any completed fallback can refill it.
// At most 32 free objects remain cached; concurrent waits retain heap fallback.
static ObjectCache<EpollWalk, 32> g_epollWalkCache{};
struct EpollWalkOwner {
    KernelOperationGuard operation; // unwind pins/storage before fatal cancellation
    EpollWalk* walk;
    explicit EpollWalkOwner(Process* proc) : operation(proc), walk(g_epollWalkCache.TryAcquire()) {
        if (!walk) walk = static_cast<EpollWalk*>(kmalloc(sizeof(EpollWalk)));
        if (walk) walk->reset(proc);
    }
    ~EpollWalkOwner() {
        if (!walk) return;
        for (int fd = 0; fd < EPOLL_MAX_FDS; fd++) if (walk->pins[fd]) FdPut(walk->proc, fd);
        if (!g_epollWalkCache.Release(walk)) kfree(walk);
    }
};
static bool g_epollDiagEnabled = false;
static EpollDiagSnapshot g_epollDiag[MAX_PROCESSES]{};
void EpollDiagEnable(bool enabled) { __atomic_store_n(&g_epollDiagEnabled, enabled, __ATOMIC_RELEASE); }
bool EpollDiagEnabled() { return __atomic_load_n(&g_epollDiagEnabled, __ATOMIC_ACQUIRE); }
bool EpollDiagTargetAvailable() {
#if defined(BROOK_EPOLL_TARGET_DIAGNOSTICS)
    return true;
#else
    return false;
#endif
}
bool EpollDiagRead(unsigned pid, EpollDiagSnapshot* output) {
    if (pid >= MAX_PROCESSES || !output) return false;
    const auto& row = g_epollDiag[pid];
    const uint64_t incarnation = __atomic_load_n(&row.incarnation, __ATOMIC_ACQUIRE);
    if (!incarnation) return false;
    for (unsigned i = 0; i < EpDiagCount; ++i)
        output->counters[i] = __atomic_load_n(&row.counters[i], __ATOMIC_RELAXED);
    output->incarnation = incarnation;
    return incarnation == __atomic_load_n(&row.incarnation, __ATOMIC_ACQUIRE);
}
static EpollDiagSnapshot* EpollDiagFor(Process* proc) {
    if (!EpollDiagEnabled() || proc->pid >= MAX_PROCESSES) return nullptr;
    auto* row = &g_epollDiag[proc->pid];
    // One live task owns a PID. Invalidate before reusing its diagnostic row.
    if (__atomic_load_n(&row->incarnation, __ATOMIC_RELAXED) != proc->incarnation) {
        __atomic_store_n(&row->incarnation, 0, __ATOMIC_RELEASE);
        for (auto& counter : row->counters) __atomic_store_n(&counter, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&row->incarnation, proc->incarnation, __ATOMIC_RELEASE);
    }
    return row;
}
static void EpollDiagAdd(EpollDiagSnapshot* row, EpollDiagCounter metric) {
    if (row) __atomic_fetch_add(&row->counters[metric], 1, __ATOMIC_RELAXED);
}
static int EpollScanReady(EpollWalk* walk, EpollInstance* ep,
                          EpollEvent* events, int maxevents, EpollDiagSnapshot* diag,
                          uint64_t eventsAddr) {
    EpollGraphGuard guard;
    walk->clearMemo();
    int n = 0;
    for (int i = 0; i < ep->count && n < maxevents; i++) {
        auto e = ep->entries[i];
        if (e.fd < 0) continue;
        if (!walk->pin(e.fd)) { ep->entries[i].fd = -1; continue; }
        EventFdSnapshot captured{};
        uint32_t ready = walk->watchReady(e, 0, &captured);
        if (ready) {
            walk->deliveries[n] = {i, captured};
            events[n].events = ready; events[n++].data.u64 = e.data;
            if (diag) {
                if (ready & EPOLLIN) EpollDiagAdd(diag, EpScanRead);
                if (ready & EPOLLOUT) EpollDiagAdd(diag, EpScanWrite);
                if (ready & EPOLLHUP) EpollDiagAdd(diag, EpScanHup);
                if (ready & EPOLLERR) EpollDiagAdd(diag, EpScanError);
                if (e.events & EPOLLET) EpollDiagAdd(diag, EpScanEdge);
                if (e.events & EPOLLONESHOT) EpollDiagAdd(diag, EpScanOneShot);
                __atomic_store_n(&diag->counters[EpLastFd], e.fd, __ATOMIC_RELAXED);
                __atomic_store_n(&diag->counters[EpLastType], static_cast<uint64_t>(walk->pins[e.fd]->type), __ATOMIC_RELAXED);
                __atomic_store_n(&diag->counters[EpLastRequested], e.events, __ATOMIC_RELAXED);
                __atomic_store_n(&diag->counters[EpLastReturned], ready, __ATOMIC_RELAXED);
            }
        }
    }
    if (n && !CopyToUser(eventsAddr, events, sizeof(EpollEvent) * n)) return -EFAULT;
    for (int i = 0; i < n; ++i) {
        const auto& delivery = walk->deliveries[i];
        if (!delivery.eventFd.identity) continue;
        auto& entry = ep->entries[delivery.index];
        // Commit exactly the snapshot returned to userspace, not newer source
        // generations that may have arrived during copyout.
        entry.eventFdIdentity = delivery.eventFd.identity;
        entry.inGeneration = delivery.eventFd.inGeneration;
        entry.outGeneration = delivery.eventFd.outGeneration;
    }
    return n;
}
int EpollPollReady(Process* proc, EpollInstance* ep) {
    EpollWalkOwner state(proc);
    if (!state.walk) return -ENOMEM;
    EpollGraphGuard guard;
    return state.walk->any(ep) ? 1 : 0;
}

static int64_t epoll_create_impl(uint64_t flagsVal)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    auto* ep = static_cast<EpollInstance*>(kmalloc(sizeof(EpollInstance)));
    if (!ep) return -ENOMEM;
    for (int i = 0; i < EPOLL_MAX_FDS; i++) ep->entries[i] = {-1, 0, 0};
    ep->count = 0;
    ep->refCount = 1;
    ep->waiter = nullptr;

    // Published complete with EPOLL_CLOEXEC (L-57).
    int fd = FdAlloc(proc, FdType::EpollFd, ep, 2, 0, (flagsVal & 0x80000) ? 1 : 0);
    if (fd < 0) { kfree(ep); return fd; }

    DbgPrintf("EPOLL: create fd=%d pid=%u tgid=%u flags=0x%lx ep=0x%lx\n",
              fd, proc->pid, proc->tgid, flagsVal,
              reinterpret_cast<uint64_t>(ep));
    return fd;
}

int64_t sys_epoll_create(uint64_t size, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    (void)size; // size hint ignored since Linux 2.6.8
    return epoll_create_impl(0);
}

int64_t sys_epoll_create1(uint64_t flags, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    return epoll_create_impl(flags);
}

int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t watchfd,
                        uint64_t eventAddr, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef epref(proc, static_cast<int>(epfd));
    FdRef watchref(proc, static_cast<int>(watchfd));
    if (!epref || !watchref || epref->type==FdType::Path || watchref->type==FdType::Path) return -EBADF;
    if (epref->type != FdType::EpollFd || !epref->handle) return -EINVAL;
    if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL) return -EINVAL;
    auto* ep = static_cast<EpollInstance*>(epref->handle);
    if (watchref->type == FdType::EpollFd && watchref->handle == ep) return -EINVAL;
    EpollEvent ev{};
    if (op != EPOLL_CTL_DEL) {
        if (!CopyFromUser(&ev, eventAddr, sizeof(ev))) return -EFAULT;
    }
    // Regular files have poll() readiness but cannot be watched by epoll.
    // Accepting them produces perpetual readiness and prevents async runtimes
    // from selecting their EPERM fallback. Named FIFOs use real pipe readiness.
    if ((watchref->type == FdType::Vnode && !FdNamedFifo(watchref.get())) ||
        watchref->type == FdType::MemFd || watchref->type == FdType::SyntheticMem)
        return -EPERM;
    EpollWalkOwner state(proc);
    if (!state.walk) return -ENOMEM;
    EpollGraphGuard guard;
    int existing = -1, free = -1;
    for (int i = 0; i < EPOLL_MAX_FDS; i++) {
        if (ep->entries[i].fd == static_cast<int>(watchfd)) existing = i;
        if (ep->entries[i].fd < 0 && free < 0) free = i;
    }
    if (op == EPOLL_CTL_DEL) {
        if (existing < 0) return -ENOENT;
        ep->entries[existing].fd = -1;
        return 0;
    }
    if (op == EPOLL_CTL_MOD && existing < 0) return -ENOENT;
    if (op == EPOLL_CTL_ADD && existing >= 0) {
        auto* diag=EpollDiagFor(proc);
        EpollDiagAdd(diag,EpCtlAddExists);
#if defined(BROOK_EPOLL_TARGET_DIAGNOSTICS)
        const auto& previous=ep->entries[existing];
        if(diag && previous.diagTargetValid &&
           (previous.diagHandle!=watchref->handle || previous.diagType!=watchref->type))
            EpollDiagAdd(diag,EpCtlAddChangedTarget);
#endif
        return -EEXIST;
    }
    int slot = op == EPOLL_CTL_MOD ? existing : free;
    if (slot < 0) return -ENOMEM;
    const int oldCount = ep->count;
    const auto old = ep->entries[slot];
    ep->entries[slot] = {static_cast<int>(watchfd), ev.events, ev.data.u64};
#if defined(BROOK_EPOLL_TARGET_DIAGNOSTICS)
    if(EpollDiagEnabled()) {
        ep->entries[slot].diagHandle=watchref->handle;
        ep->entries[slot].diagType=watchref->type;
        ep->entries[slot].diagTargetValid=true;
    }
#endif
    if (slot >= ep->count) ep->count = slot + 1;
    if (op == EPOLL_CTL_ADD && watchref->type == FdType::EpollFd) {
        // Include ancestors: adding at the bottom can also exceed the limit.
        bool valid = state.walk->height(ep) <= EPOLL_MAX_DEPTH;
        for (int fd = 0; fd < EPOLL_MAX_FDS && valid; fd++) {
            auto* fde = state.walk->pin(fd);
            if (fde && fde->type == FdType::EpollFd && fde->handle)
                valid = state.walk->height(static_cast<EpollInstance*>(fde->handle)) <= EPOLL_MAX_DEPTH;
        }
        if (!valid) { ep->entries[slot] = old; ep->count = oldCount; return -ELOOP; }
    }
    return 0;
}

static int64_t epoll_wait_impl(Process* proc, EpollInstance* ep,
                                EpollEvent* kEvents, int maxevents,
                                int timeout_ms, EpollDiagSnapshot* diag, uint64_t eventsAddr)
{
    const uint64_t deadline = WaitDeadlineNs(timeout_ms < 0 ? ~0ULL : static_cast<uint64_t>(timeout_ms));
    bool firstScan = true;
    for (;;) {
        EpollDiagAdd(diag, EpIterations);
        EpollWalkOwner state(proc);
        auto* walk = state.walk;
        if (!walk) return -ENOMEM;
        int n = EpollScanReady(walk, ep, kEvents, maxevents, diag, eventsAddr);
        if (n < 0) return n;
        if (n > 0) { EpollDiagAdd(diag, firstScan ? EpInitialReady : EpLaterReady); return n; }
        if (timeout_ms == 0) { EpollDiagAdd(diag, EpTimeouts); return 0; }
        firstScan = false;
        {
            EpollGraphGuard guard;
            walk->clearMemo();
            walk->collect(ep);
        }
        NetlinkWaitGroup netlinkWaiters;
        TimerWaitGroup timerWaiters;
        PipePollGroup namedWaiters;bool namedWaitFailed=false;
        // Pipes, AF_UNIX streams and eventfds use per-object waiter lists, so
        // several epoll_wait/poll/read callers each get their wake. Without
        // a registration (allocation failure) the wait polls every 5 ms.
        PipePollGroup pipeWaiters;
        EventFdWaitGroup eventWaiters;
        SyncFileWait syncWait;
        bool fallbackTick=false;
        uint64_t tcpPoolWaitToken=0;
        auto registerWaiters = [&]() {
            for (int wfd = 0; wfd < EPOLL_MAX_FDS; wfd++) {
                FdEntry* fde = walk->pins[wfd];
                if (!walk->masks[wfd]) continue;
                if (!fde || !fde->handle) continue;
                if(fde->type==FdType::NetlinkSocket) {
                    if(!netlinkWaiters.add(static_cast<NetlinkSocket*>(fde->handle),proc))namedWaitFailed=true;
                } else if(FdNamedFifo(fde)) {
                    if(!namedWaiters.add(FdPipe(fde),proc,walk->masks[wfd]|16|8))namedWaitFailed=true;
                } else if (fde->type == FdType::Pipe) {
                    if(!pipeWaiters.add(static_cast<PipeBuffer*>(fde->handle),proc,walk->masks[wfd]|EPOLLHUP|EPOLLERR))fallbackTick=true;
                } else if (fde->type == FdType::EventFd) {
                    if(!eventWaiters.add(static_cast<EventFdData*>(fde->handle),proc))fallbackTick=true;
                } else if (fde->type == FdType::SyncFile) {
                    if (walk->masks[wfd] & EPOLLIN) syncWait.Want(*static_cast<SyncFileData*>(fde->handle));
                } else if (fde->type == FdType::TimerFd) {
                    if (walk->masks[wfd] & EPOLLIN)
                        timerWaiters.add(static_cast<TimerFdData*>(fde->handle), proc);
                } else if (fde->type == FdType::Socket) {
                    int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                    brook::SockSetPollWaiter(si, proc);
                    if((walk->masks[wfd] & EPOLLOUT) && !tcpPoolWaitToken)
                        tcpPoolWaitToken=brook::NetTcpPoolWaitRegister(proc);
                } else if (fde->type == FdType::UnixSocket) {
                    auto* usd = static_cast<UnixSocketData*>(fde->handle);
                    uint32_t events = walk->masks[wfd];
                    if ((events & EPOLLIN) && usd->state == UnixSocketData::State::Listening) {
                        usd->epollWaiter = proc;
                    } else if ((events & (EPOLLIN | EPOLLRDHUP)) &&
                               usd->state == UnixSocketData::State::Connected && usd->rxPipe) {
                        if(!pipeWaiters.add(usd->rxPipe,proc,EPOLLIN))fallbackTick=true;
                    }
                    if ((events & EPOLLOUT) &&
                        usd->state == UnixSocketData::State::Connected && usd->txPipe)
                        if(!pipeWaiters.add(usd->txPipe,proc,EPOLLOUT))fallbackTick=true;
                }
            }
        };
        auto unregisterWaiters = [&]() {
            brook::NetTcpPoolWaitUnregister(tcpPoolWaitToken);tcpPoolWaitToken=0;
            for (int wfd = 0; wfd < EPOLL_MAX_FDS; wfd++) {
                FdEntry* fde = walk->pins[wfd];
                if (!walk->masks[wfd]) continue;
                if (!fde || !fde->handle) continue;
                if (fde->type == FdType::Socket) {
                    int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                    brook::SockClearPollWaiter(si, proc);
                } else if (fde->type == FdType::UnixSocket) {
                    auto* usd = static_cast<UnixSocketData*>(fde->handle);
                    if (usd->epollWaiter == proc) usd->epollWaiter = nullptr;
                }
            }
        };

        registerWaiters();
        if(namedWaitFailed) {unregisterWaiters();return -ENOMEM;}
        // A fence retired since the snapshot: rescan without sleeping.
        const bool fenceRetired = syncWait.Arm();
        // The graph mutex can sleep. Arm the timeout only after this scan:
        // SchedulerUnblock clears wakeupTick when granting a contended mutex.
        n = EpollScanReady(walk, ep, kEvents, maxevents, diag, eventsAddr);
        if (!n && !fenceRetired && !HasPendingSignals() && (timeout_ms < 0 || ClockNowNs(1) < deadline)) {
            uint64_t safetyMs = (timerWaiters.allocationFailed || fallbackTick) ? 5 : 100;
            for (int fd = 0; fd < EPOLL_MAX_FDS; fd++) {
                auto* fde = walk->pins[fd];
                if (!walk->masks[fd] || !fde) continue;
                if (fde->type == FdType::DevDsp) {
                    safetyMs = 5;
                    continue;
                }
                // Signals aimed at another thread have no wake source yet.
                if (fde->type == FdType::SignalFd && safetyMs > 10) safetyMs = 10;
            }
            safetyMs = syncWait.SafetyMs(safetyMs);
            const uint64_t timerDeadline = timerWaiters.deadline(deadline);
            proc->wakeupTick = WaitWakeupTick(timerDeadline, safetyMs);
            EpollDiagAdd(diag, EpBlocks);
            SchedulerBlockInterruptible(proc);
        }
        unregisterWaiters();
        if (n < 0) return n;
        if (n > 0) { EpollDiagAdd(diag, EpLaterReady); return n; }
        if (HasPendingSignals()) { EpollDiagAdd(diag, EpInterrupts); return -EINTR; }
        n = EpollScanReady(walk, ep, kEvents, maxevents, diag, eventsAddr);
        if (n < 0) return n;
        if (n > 0) { EpollDiagAdd(diag, EpLaterReady); return n; }
        if (timeout_ms >= 0 && ClockNowNs(1) >= deadline) {
            EpollDiagAdd(diag, EpTimeouts); return 0;
        }
        // Release pins only after unregistering the exact flattened leaves.
    }
}

int64_t sys_epoll_wait(uint64_t epfd, uint64_t eventsAddr,
                        uint64_t maxevents, uint64_t timeout_ms,
                        uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    // BRO-156: pin epfd for the whole wait — epoll_wait_impl dereferences `ep`
    // (= epfde->handle) across SchedulerBlock, so a sibling close() of epfd must
    // not free the EpollInstance mid-wait.
    FdRef epfde_ref(proc, static_cast<int>(epfd));
    FdEntry* epfde = epfde_ref.get();
    if (!epfde || epfde->type != FdType::EpollFd || !epfde->handle) {
        // BRO-005 diagnostic: cross-check ProcessCurrent() against the pid
        // the scheduler stamped into cpuEnv->currentPid (gs:40) at the most
        // recent context switch. If these disagree, ProcessCurrent() is
        // returning a stale per-CPU pointer and the bug is in the scheduler,
        // not the fd table.
        uint32_t curPidGs = 0;
        __asm__ volatile("movl %%gs:40, %0" : "=r"(curPidGs));
        Process* proc2 = ProcessCurrent();
        SerialPrintf("sys_epoll_wait: EBADF epfd=%lu pid=%u tgid=%u "
                     "fde=%p type=%d handle=%p | cpu=%u gs:currentPid=%u "
                     "proc=%p proc2=%p proc2pid=%u\n",
                     epfd, proc->pid, proc->tgid, epfde,
                     epfde ? static_cast<int>(epfde->type) : -1,
                     epfde ? epfde->handle : nullptr,
                     SmpCurrentCpuIndex(), curPidGs,
                     proc, proc2, proc2 ? proc2->pid : 0);
        return -EBADF;
    }
    auto* ep = static_cast<EpollInstance*>(epfde->handle);

    if (maxevents <= 0 || maxevents > 1024) return -EINVAL;

    // Small event batches do not need to visit the global heap. Keep the stack
    // budget fixed (192 bytes), with the existing heap path for larger batches.
    EpollEvent localEvents[16];
    auto* kEvents = maxevents <= 16 ? localEvents : static_cast<EpollEvent*>(
        kmalloc(sizeof(EpollEvent) * static_cast<uint32_t>(maxevents)));
    if (!kEvents) return -ENOMEM;

    auto* diag = EpollDiagFor(proc);
    EpollDiagAdd(diag, EpCalls);
    if (static_cast<int>(static_cast<int64_t>(timeout_ms)) == 0) EpollDiagAdd(diag, EpZeroTimeout);
    int n = epoll_wait_impl(proc, ep, kEvents,
                             static_cast<int>(maxevents),
                             static_cast<int>(static_cast<int64_t>(timeout_ms)), diag, eventsAddr);

    if (kEvents != localEvents) kfree(kEvents);
    return n;
}

int64_t sys_epoll_pwait(uint64_t epfd, uint64_t eventsAddr,
                         uint64_t maxevents, uint64_t timeout_ms,
                         uint64_t sigmaskAddr, uint64_t sigsetsize)
{
    int error = InstallWaitSignalMask(sigmaskAddr, sigsetsize);
    if (error) return error;
    return sys_epoll_wait(epfd, eventsAddr, maxevents, timeout_ms, 0, 0);
}

} // namespace brook
