// Host-side unit tests for the fd-table primitives (BRO-156).
//
// Exercises the REAL core logic from src/kernel/src/fd_table.h (not a
// reimplementation): FdTableAlloc / FdTableGet / FdTableClaim over a real
// FdEntry[] guarded by the real brook::SpinLock.
//
// The property under test is the one the BRO-156 fix guarantees: when many
// threads race to close() the same fd, FdTableClaim() reads-and-clears the
// slot atomically, so EXACTLY ONE caller observes the live slot and becomes
// the sole owner responsible for freeing the handle. The old FdGet()+FdFree()
// split dropped the lock in between, letting two closers both unref the same
// handle -> double free. Build under TSan (scripts/run-tsan-tests.sh) to also
// flag the data race directly.

#include <cstdio>
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>
#include <pthread.h>

#include "fd_table.h"
#include "sync_file.h"
#include "unix_lifetime_trace.h"
#include "pipe.h"
#include <chrono>

// These tests exercise FD ownership and pipe I/O, without a scheduler.
namespace brook { void PipePollNotify(PipeBuffer*, uint32_t) {} }

using brook::FdEntry;
using brook::FdType;
using brook::FdClaimResult;
using brook::MAX_FDS;
using brook::SpinLock;
using brook::FdTableAlloc;
using brook::FdTableFree;
using brook::FdTableGet;
using brook::FdTableClaim;
using brook::FdTablePin;
using brook::FdTableUnpin;
using brook::FdTableClose;
using brook::FdCloseResult;

static int g_failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__,      \
                         __LINE__);                                        \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// Encode a 1-based token as the handle pointer so we can recover it after a
// claim and verify each handle is freed exactly once.
static void* TokenHandle(uint32_t token1) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(token1));
}
static uint32_t HandleToken(void* h) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h));
}

// ---------------------------------------------------------------------------
// Single-threaded correctness.
// ---------------------------------------------------------------------------
static void TestBasic()
{
    static FdEntry fds[MAX_FDS];
    SpinLock lock;
    for (uint32_t i = 0; i < MAX_FDS; ++i) fds[i].type = FdType::None;

    // Fill the whole table.
    int handed[MAX_FDS];
    bool seen[MAX_FDS] = {};
    for (uint32_t i = 0; i < MAX_FDS; ++i) {
        handed[i] = FdTableAlloc(fds, &lock, FdType::Vnode, TokenHandle(i + 1));
        CHECK(handed[i] >= 0, "alloc within capacity");
        CHECK(!seen[handed[i]], "no slot handed out twice");
        seen[handed[i]] = true;
    }
    CHECK(FdTableAlloc(fds, &lock, FdType::Vnode, TokenHandle(999)) < 0,
          "alloc fails when full");

    // Claim returns the owning state and clears the slot.
    FdClaimResult c;
    CHECK(FdTableClaim(fds, &lock, handed[5], &c), "claim live slot");
    CHECK(c.type == FdType::Vnode, "claim returns type");
    CHECK(HandleToken(c.handle) == static_cast<uint32_t>(handed[5]) + 1 ||
          c.handle != nullptr, "claim returns handle");
    // Second claim of the same fd must fail (slot now None) — this is the
    // double-close guard.
    CHECK(!FdTableClaim(fds, &lock, handed[5], &c), "double-claim fails");
    CHECK(FdTableGet(fds, &lock, handed[5]) == nullptr, "claimed slot is free");

    // The freed slot is reusable.
    int re = FdTableAlloc(fds, &lock, FdType::Socket, TokenHandle(42));
    CHECK(re == handed[5], "claimed slot is reused");

    // Out-of-range / unused are handled.
    CHECK(!FdTableClaim(fds, &lock, -1, &c), "claim rejects negative fd");
    CHECK(!FdTableClaim(fds, &lock, MAX_FDS, &c), "claim rejects oob fd");
    CHECK(FdTableGet(fds, &lock, -1) == nullptr, "get rejects negative fd");

    // Drain.
    for (uint32_t i = 0; i < MAX_FDS; ++i)
        FdTableClaim(fds, &lock, static_cast<int>(i), &c);
    for (uint32_t i = 0; i < MAX_FDS; ++i)
        CHECK(fds[i].type == FdType::None, "all slots free after drain");
}

// ---------------------------------------------------------------------------
// Concurrent close stress: N threads scan the table racing to claim every
// slot. Each slot's handle token must be claimed (and therefore "freed")
// exactly once; a second free of the same token would mean two threads won
// the same slot -> the bug this fix prevents.
// ---------------------------------------------------------------------------
static FdEntry           g_fds[MAX_FDS];
static SpinLock          g_lock;
static std::atomic<int>  g_freed[MAX_FDS];   // per-token free count
static std::atomic<bool> g_doubleFree{false};
static std::atomic<int>  g_remaining{0};
static std::atomic<uint64_t> g_totalClaims{0};
static pthread_barrier_t g_claimStart, g_claimEnd;
static std::atomic<bool> g_claimStop{false};

static void CloserWorker()
{
    for (;;) {
        pthread_barrier_wait(&g_claimStart);
        if (g_claimStop.load()) break;
        while (g_remaining.load(std::memory_order_relaxed) > 0) {
            for (uint32_t fd = 0; fd < MAX_FDS; ++fd) {
                FdClaimResult c;
                if (!FdTableClaim(g_fds, &g_lock, static_cast<int>(fd), &c))
                    continue;

                g_totalClaims.fetch_add(1, std::memory_order_relaxed);
                g_remaining.fetch_sub(1, std::memory_order_relaxed);

                uint32_t tok = HandleToken(c.handle);
                if (tok == 0 || tok > MAX_FDS) {
                    g_doubleFree.store(true);  // corrupt handle == claim torn
                    continue;
                }
                // Simulate the single-owner unref: each token must transition
                // 0 -> 1 exactly once. A second increment means a double free.
                int prev = g_freed[tok - 1].fetch_add(1, std::memory_order_acq_rel);
                if (prev != 0)
                    g_doubleFree.store(true);
            }
            std::this_thread::yield();
        }
        pthread_barrier_wait(&g_claimEnd);
    }
}

static void TestConcurrent()
{
    constexpr int      THREADS = 16;
    // Keep all 500 rounds and 16 racing closers, but reuse worker threads.
    // Start/end rendezvous isolate resets without paying 8,000 create/joins.
    constexpr uint64_t ROUNDS = 500;
    pthread_barrier_init(&g_claimStart, nullptr, THREADS + 1);
    pthread_barrier_init(&g_claimEnd, nullptr, THREADS + 1);
    g_claimStop.store(false);
    std::vector<std::thread> workers;
    workers.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) workers.emplace_back(CloserWorker);

    uint64_t totalExpected = 0;
    for (uint64_t r = 0; r < ROUNDS; ++r) {
        // Refill the table with fresh tokens and reset the free shadow.
        for (uint32_t i = 0; i < MAX_FDS; ++i) {
            g_fds[i].type = FdType::None;
            g_freed[i].store(0, std::memory_order_relaxed);
        }
        for (uint32_t i = 0; i < MAX_FDS; ++i) {
            int fd = FdTableAlloc(g_fds, &g_lock, FdType::Vnode, TokenHandle(i + 1));
            (void)fd;
        }
        g_remaining.store(static_cast<int>(MAX_FDS), std::memory_order_relaxed);
        totalExpected += MAX_FDS;

        pthread_barrier_wait(&g_claimStart);
        pthread_barrier_wait(&g_claimEnd);

        // Every token freed exactly once; table fully drained.
        for (uint32_t i = 0; i < MAX_FDS; ++i) {
            if (g_freed[i].load() != 1) { g_doubleFree.store(true); }
            if (g_fds[i].type != FdType::None) { g_doubleFree.store(true); }
        }
        if (g_doubleFree.load()) break;  // fail fast
    }

    g_claimStop.store(true);
    pthread_barrier_wait(&g_claimStart);
    for (auto& w : workers) w.join();
    pthread_barrier_destroy(&g_claimEnd);
    pthread_barrier_destroy(&g_claimStart);
    CHECK(!g_doubleFree.load(), "no slot/handle ever claimed twice (no double free)");
    CHECK(g_totalClaims.load() == totalExpected, "exactly one claim per fd per round");

    std::printf("fd_table close stress: %llu total claims over %llu rounds x %u fds\n",
                static_cast<unsigned long long>(g_totalClaims.load()),
                static_cast<unsigned long long>(ROUNDS), MAX_FDS);
}

// ---------------------------------------------------------------------------
// Pinned fget/fput single-threaded semantics (BRO-156).
// ---------------------------------------------------------------------------
static void TestPinBasic()
{
    static FdEntry fds[MAX_FDS];
    SpinLock lock;
    for (uint32_t i = 0; i < MAX_FDS; ++i) fds[i].type = FdType::None;

    int fd = FdTableAlloc(fds, &lock, FdType::Vnode, TokenHandle(7));
    CHECK(fd >= 0, "pin: alloc");

    // Pin succeeds on a live slot and returns the entry.
    FdEntry* p = FdTablePin(fds, &lock, fd);
    CHECK(p != nullptr, "pin live slot");
    CHECK(p->pinCount == 1, "pin bumps pinCount");

    // Nested pin is allowed (reentrancy) — pinCount counts.
    FdEntry* p2 = FdTablePin(fds, &lock, fd);
    CHECK(p2 == p && p->pinCount == 2, "nested pin counts");

    // Closing a pinned slot defers: slot stays live, marked closing, no snapshot
    // ownership handed out yet.
    FdClaimResult c;
    CHECK(FdTableClose(fds, &lock, fd, &c) == FdCloseResult::Deferred,
          "close while pinned defers");
    CHECK(fds[fd].type == FdType::Vnode, "deferred close leaves slot live");
    CHECK(fds[fd].closing == 1, "deferred close marks closing");

    // Once closing, no new pins are admitted.
    CHECK(FdTablePin(fds, &lock, fd) == nullptr, "pin refused while closing");
    // A second close() also sees it as already-going and returns NotFound.
    CHECK(FdTableClose(fds, &lock, fd, &c) == FdCloseResult::NotFound,
          "second close on closing slot is NotFound");

    // Dropping a non-last pin does not finalize.
    CHECK(!FdTableUnpin(fds, &lock, fd, &c), "non-last unpin does not finalize");
    CHECK(fds[fd].type == FdType::Vnode, "slot still live after non-last unpin");

    // Dropping the last pin finalizes: snapshot handed back, slot cleared.
    CHECK(FdTableUnpin(fds, &lock, fd, &c), "last unpin finalizes deferred close");
    CHECK(HandleToken(c.handle) == 7, "finalize snapshot carries the handle");
    CHECK(fds[fd].type == FdType::None, "slot cleared after deferred finalize");

    // Closing an UNpinned slot claims immediately (ClaimedNow), like FdTableClaim.
    int fd2 = FdTableAlloc(fds, &lock, FdType::Socket, TokenHandle(9));
    CHECK(FdTableClose(fds, &lock, fd2, &c) == FdCloseResult::ClaimedNow,
          "close unpinned slot claims now");
    CHECK(HandleToken(c.handle) == 9, "claim-now snapshot carries the handle");
    CHECK(fds[fd2].type == FdType::None, "slot cleared after claim-now");

    // Unpin/close out-of-range is a no-op / NotFound.
    CHECK(!FdTableUnpin(fds, &lock, -1, &c), "unpin rejects oob");
    CHECK(FdTableClose(fds, &lock, MAX_FDS, &c) == FdCloseResult::NotFound,
          "close rejects oob");
}

// ---------------------------------------------------------------------------
// Concurrent pin/use/close stress (BRO-156): user threads pin a slot, read its
// handle (the "in use" window), then unpin; closer threads race to close the
// same slots. The invariants:
//   * a handle is finalized (freed) EXACTLY ONCE, and
//   * a handle is NEVER finalized while any thread holds a pin on it — i.e. a
//     pinned user never observes its handle as already-freed (the UAF the fix
//     prevents). Both the immediate-close and deferred-close paths must hold.
// Build under TSan to also flag the underlying data race directly.
// ---------------------------------------------------------------------------
static constexpr int PIN_SLOTS = 8;
static FdEntry            pin_fds[MAX_FDS];
static SpinLock           pin_lock;
static std::atomic<int>   pin_alive[PIN_SLOTS];     // 1 = handle valid, 0 = freed
static std::atomic<int>   pin_finalized[PIN_SLOTS]; // times finalized (must end == 1)
static std::atomic<int>   pin_remaining{0};         // slots not yet finalized
static std::atomic<bool>  pin_uaf{false};           // pinned user saw freed handle
static std::atomic<bool>  pin_doubleFree{false};

static void PinFinalize(uint32_t tok)
{
    int s = static_cast<int>(tok) - 1;
    if (s < 0 || s >= PIN_SLOTS) { pin_doubleFree.store(true); return; }
    // Mark the handle dead. A pinned user reading pin_alive must never see 0.
    int wasAlive = pin_alive[s].exchange(0, std::memory_order_acq_rel);
    if (wasAlive != 1) pin_doubleFree.store(true);
    if (pin_finalized[s].fetch_add(1, std::memory_order_acq_rel) != 0)
        pin_doubleFree.store(true);
    pin_remaining.fetch_sub(1, std::memory_order_acq_rel);
}

static pthread_barrier_t g_pinStart, g_pinEnd;
static std::atomic<bool> g_pinStop{false};

static void PinUserWorker(uint32_t index)
{
    for (uint64_t round = 0;; ++round) {
        pthread_barrier_wait(&g_pinStart);
        if (g_pinStop.load()) break;
        uint32_t seed = static_cast<uint32_t>(round * 131 + index + 1);
        uint32_t x = seed * 2654435761u + 1;
        while (pin_remaining.load(std::memory_order_relaxed) > 0) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            int fd = static_cast<int>(x % PIN_SLOTS);
            FdEntry* e = FdTablePin(pin_fds, &pin_lock, fd);
            if (!e) {
                std::this_thread::yield();
                continue;
            }
            // Yield with the descriptor pinned, after releasing the ticket lock.
            // Closers can exercise deferred teardown even on a single CPU.
            std::this_thread::yield();
            uint32_t tok = HandleToken(e->handle);
            // The "use" window: while pinned, the handle MUST be live.
            for (int spin = 0; spin < 8; ++spin) {
                if (pin_alive[tok - 1].load(std::memory_order_acquire) != 1) {
                    pin_uaf.store(true);
                    break;
                }
            }
            FdClaimResult c;
            if (FdTableUnpin(pin_fds, &pin_lock, fd, &c))
                PinFinalize(HandleToken(c.handle));
        }
        pthread_barrier_wait(&g_pinEnd);
    }
}

static void PinCloserWorker(uint32_t index)
{
    for (uint64_t round = 0;; ++round) {
        pthread_barrier_wait(&g_pinStart);
        if (g_pinStop.load()) break;
        uint32_t seed = static_cast<uint32_t>(round * 977 + index + 1);
        uint32_t x = seed * 40503u + 7;
        while (pin_remaining.load(std::memory_order_relaxed) > 0) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            int fd = static_cast<int>(x % PIN_SLOTS);
            FdClaimResult c;
            if (FdTableClose(pin_fds, &pin_lock, fd, &c) == FdCloseResult::ClaimedNow)
                PinFinalize(HandleToken(c.handle));
            // Deferred: the last unpin finalizes. NotFound: already gone.
            std::this_thread::yield();
        }
        pthread_barrier_wait(&g_pinEnd);
    }
}

static void TestPinConcurrent()
{
    constexpr int      USERS   = 12;
    constexpr int      CLOSERS = 4;
    // Preserve all race rounds and seeds while reusing the same competitors.
    constexpr uint64_t ROUNDS  = 800;
    // All competitors must exist before work begins. Without a rendezvous,
    // early users busy-spin until closers are created; on one CPU this also
    // delays the parent creating those closers and barely exercises races.
    pthread_barrier_init(&g_pinStart, nullptr, USERS + CLOSERS + 1);
    pthread_barrier_init(&g_pinEnd, nullptr, USERS + CLOSERS + 1);
    g_pinStop.store(false);
    std::vector<std::thread> workers;
    workers.reserve(USERS + CLOSERS);
    for (int t = 0; t < USERS; ++t)
        workers.emplace_back(PinUserWorker, static_cast<uint32_t>(t));
    for (int t = 0; t < CLOSERS; ++t)
        workers.emplace_back(PinCloserWorker, static_cast<uint32_t>(t));

    for (uint32_t i = 0; i < MAX_FDS; ++i) pin_fds[i].type = FdType::None;

    for (uint64_t r = 0; r < ROUNDS; ++r) {
        // Refill PIN_SLOTS fds with fresh, live tokens 1..PIN_SLOTS.
        for (int s = 0; s < PIN_SLOTS; ++s) {
            pin_alive[s].store(1, std::memory_order_relaxed);
            pin_finalized[s].store(0, std::memory_order_relaxed);
            int fd = FdTableAlloc(pin_fds, &pin_lock, FdType::Vnode,
                                  TokenHandle(static_cast<uint32_t>(s) + 1));
            CHECK(fd == s, "pin stress: slot allocated in order");
        }
        pin_remaining.store(PIN_SLOTS, std::memory_order_release);

        pthread_barrier_wait(&g_pinStart);
        pthread_barrier_wait(&g_pinEnd);

        for (int s = 0; s < PIN_SLOTS; ++s) {
            if (pin_finalized[s].load() != 1) pin_doubleFree.store(true);
            if (pin_fds[s].type != FdType::None) pin_doubleFree.store(true);
        }
        if (pin_uaf.load() || pin_doubleFree.load()) break;  // fail fast
    }

    g_pinStop.store(true);
    pthread_barrier_wait(&g_pinStart);
    for (auto& w : workers) w.join();
    pthread_barrier_destroy(&g_pinEnd);
    pthread_barrier_destroy(&g_pinStart);
    CHECK(!pin_uaf.load(), "no pinned user ever saw a freed handle (no UAF)");
    CHECK(!pin_doubleFree.load(), "every handle finalized exactly once");

    std::printf("fd_table pin stress: %d users + %d closers over %llu rounds x %d slots\n",
                USERS, CLOSERS,
                static_cast<unsigned long long>(ROUNDS), PIN_SLOTS);
}

static std::atomic<int> duplicate_retains;
static void DuplicateRetain(FdEntry*) { duplicate_retains.fetch_add(1); }

static void TestDuplicate()
{
    FdEntry fds[MAX_FDS] = {};
    SpinLock lock;
    int source = FdTableAlloc(fds, &lock, FdType::MemFd, TokenHandle(1));
    fds[source].dirPath[0] = '/';
    fds[source].dirPath[1] = 'x';
    fds[source].dirPath[2] = 0;
    FdEntry* pinned = brook::FdTablePin(fds, &lock, source);
    CHECK(pinned != nullptr, "duplicate source is pinned");
    duplicate_retains.store(0);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&] {
        for (int i = 0; i < 500; ++i) {
            int fd = brook::FdTableDuplicatePinned(fds, &lock, pinned, 4, 1, DuplicateRetain);
            if (fd < 4) { ++errors; continue; }
            FdEntry* copy = brook::FdTablePin(fds, &lock, fd);
            if (!copy || copy->handle != TokenHandle(1) || copy->fdFlags != 1 ||
                copy->closing || copy->dirPath[1] != 'x') ++errors;
            if (copy) {
                FdClaimResult unused;
                brook::FdTableUnpin(fds, &lock, fd, &unused);
            }
            FdClaimResult claimed;
            if (!brook::FdTableClaim(fds, &lock, fd, &claimed)) ++errors;
        }
    });
    for (auto& thread : threads) thread.join();
    CHECK(errors.load() == 0, "concurrent duplicate slots preserve flags, path and handle");
    CHECK(duplicate_retains.load() == 2000, "every published duplicate retains its handle");
    FdClaimResult unused;
    brook::FdTableUnpin(fds, &lock, source, &unused);
}

// Reproduce the old publish-then-retain ordering with the actual table
// operations: a closer can consume the only reference while source is pinned.
static void TestOldPublicationNegative()
{
    FdEntry fds[MAX_FDS] = {};
    SpinLock lock;
    int references = 1;
    int source = FdTableAlloc(fds, &lock, FdType::UnixSocket, &references);
    FdEntry* pinned = FdTablePin(fds, &lock, source);
    int copy = FdTableAlloc(fds, &lock, pinned->type, pinned->handle);
    FdClaimResult retired{};
    CHECK(FdTableClose(fds, &lock, copy, &retired) == FdCloseResult::ClaimedNow,
          "old negative: published duplicate can close before retain");
    --references;
    CHECK(references == 0 && pinned->handle == &references,
          "old negative: source pin does not prevent unearned duplicate release");
    FdTableUnpin(fds, &lock, source, &retired);
    std::printf("fd ownership old negative: premature last release reproduced\n");
}

static void TestOwnedPublication()
{
    FdEntry fds[MAX_FDS] = {};
    SpinLock lock;
    int references = 1;
    int source = FdTableAlloc(fds, &lock, FdType::UnixSocket, &references);
    FdEntry* pinned = FdTablePin(fds, &lock, source);
    FdEntry owned = *pinned;
    ++references; // production prepares its owned copy before publication
    owned.fdFlags = 1;
    FdClaimResult retired{};
    int target = brook::FdTablePublishOwned(fds, &lock, owned, 4, -1, &retired);
    CHECK(target == 4 && fds[target].pinCount == 0 && fds[target].fdFlags == 1,
          "owned publication has complete flags and independent pin state");
    CHECK(FdTableClose(fds, &lock, target, &retired) == FdCloseResult::ClaimedNow,
          "new duplicate can close immediately");
    --references;
    CHECK(references == 1, "source backing survives immediate duplicate close");
    int other = 1;
    target = FdTableAlloc(fds, &lock, FdType::UnixSocket, &other);
    FdEntry* targetPin = FdTablePin(fds, &lock, target);
    ++references;
    CHECK(brook::FdTablePublishOwned(fds, &lock, owned, 0, target, &retired) == -16,
          "replacement of a pinned destination returns EBUSY");
    CHECK(targetPin->handle == &other && other == 1 && references == 2,
          "failed replacement preserves destination and caller ownership");
    FdTableUnpin(fds, &lock, target, &retired);
    CHECK(brook::FdTablePublishOwned(fds, &lock, owned, 0, target, &retired) == target,
          "unpinned destination is replaced atomically");
    CHECK(retired.handle == &other, "replacement returns exactly the old owner");
    --other;
    CHECK(other == 0 && references == 2, "old owner is finalized outside table lock");
    FdTableClose(fds, &lock, target, &retired); --references;
    FdTableUnpin(fds, &lock, source, &retired);
    FdTableClose(fds, &lock, source, &retired); --references;
    CHECK(references == 0, "all owned copies balance after replacement");
}
static void TestSyncFileCopyoutRollback()
{
    FdEntry fds[MAX_FDS]={};SpinLock lock;FdClaimResult retired{};
    brook::SyncFileData file{1,123,1};
    FdEntry owned{};owned.type=FdType::SyncFile;owned.handle=&file;owned.fdFlags=1;
    const int fd=brook::FdTablePublishOwnedPinned(fds,&lock,owned);
    CHECK(fd==0 && fds[fd].pinCount==1,"publication atomically adopts one pin");
    CHECK(FdTableClose(fds,&lock,fd,&retired)==FdCloseResult::Deferred,"sibling close waits for copyout pin");
    CHECK(brook::FdTablePublishOwned(fds,&lock,owned,0,fd,nullptr)==-16,"same-OFD alias cannot replace publication");
    FdEntry replacement=owned;replacement.type=FdType::EventFd;
    CHECK(brook::FdTablePublishOwned(fds,&lock,replacement,0,fd,nullptr)==-16,"unrelated replacement also blocked");
    CHECK(FdTableClose(fds,&lock,fd,&retired)==FdCloseResult::NotFound,"failed copyout does not close twice");
    CHECK(FdTableUnpin(fds,&lock,fd,&retired),"adopted pin finalizes deferred close");
    CHECK(retired.handle==&file && fds[fd].type==FdType::None,"original publication alone retired");
    CHECK(!FdTableUnpin(fds,&lock,fd,&retired),"retirement occurs exactly once");
    CHECK(brook::FdTablePublishOwnedPinned(fds,&lock,owned)==fd,"successful copyout publication");
    CHECK(!FdTableUnpin(fds,&lock,fd,&retired) && !fds[fd].pinCount,"successful copyout releases pin without closing");
    CHECK(FdTableClose(fds,&lock,fd,&retired)==FdCloseResult::ClaimedNow,"success leaves descriptor normally closable");
    CHECK(brook::FdTablePublishOwnedPinned(fds,&lock,owned)==fd,"failed copyout without sibling");
    CHECK(FdTableClose(fds,&lock,fd,&retired)==FdCloseResult::Deferred,"rollback stays under publication pin");
    CHECK(FdTableUnpin(fds,&lock,fd,&retired),"rollback releases original exactly once");
    for(unsigned i=0;i<MAX_FDS;++i)fds[i]=owned;
    CHECK(brook::FdTablePublishOwnedPinned(fds,&lock,owned)==-24,"full table rejects publication without installing pin");
    for(unsigned i=0;i<MAX_FDS;++i)CHECK(!fds[i].pinCount,"failed publication adds no pin");
}

static std::atomic<bool> snapshotInside, snapshotCloseAttempted;
static bool RetainSnapshotForRace(FdEntry* entry)
{
    snapshotInside.store(true, std::memory_order_release);
    while (!snapshotCloseAttempted.load(std::memory_order_acquire)) std::this_thread::yield();
    auto* refs = static_cast<std::atomic<int>*>(entry->handle);
    return refs->fetch_add(1) > 0;
}
static void TestForkSnapshotRace()
{
    FdEntry source[MAX_FDS] = {}, child[MAX_FDS] = {};
    SpinLock lock;
    std::atomic<int> references{1};
    int fd = FdTableAlloc(source, &lock, FdType::UnixSocket, &references);
    source[fd].fdFlags = 1;
    snapshotInside.store(false); snapshotCloseAttempted.store(false);
    std::thread closer([&] {
        while (!snapshotInside.load(std::memory_order_acquire)) std::this_thread::yield();
        snapshotCloseAttempted.store(true, std::memory_order_release);
        FdClaimResult old{};
        if (FdTableClose(source, &lock, fd, &old) == FdCloseResult::ClaimedNow)
            static_cast<std::atomic<int>*>(old.handle)->fetch_sub(1);
    });
    bool copied = brook::FdTableForkSnapshot(child, source, &lock, RetainSnapshotForRace);
    closer.join();
    CHECK(copied && references.load() == 1, "fork retains before concurrent source close");
    CHECK(child[fd].handle == &references && child[fd].fdFlags == 1 && !child[fd].pinCount,
          "fork snapshot preserves exact handle and resets transient pins");
    FdClaimResult old{};
    FdTableClose(child, &lock, fd, &old);
    static_cast<std::atomic<int>*>(old.handle)->fetch_sub(1);
    CHECK(references.load() == 0, "fork child owns and releases its independent reference");
}

static void TestUnixLifeCore()
{
    static brook::UnixLifeCore core{};
    using brook::UnixLifeEvent;
    brook::UnixLifeRecord event{};
    event.event = UnixLifeEvent::Allocate;
    event.handle = 0x10000; event.size = 256;
    core.Watch(event);
    CHECK(core.records[0].sequence == 1 && core.records[0].generation == 1,
          "watch record committed with generation");
    event.event = UnixLifeEvent::HeapFree; event.other = 0xff00; event.size = 512;
    core.Heap(event, true);
    CHECK(core.records[1].handle == 0x10000 && core.records[1].previous == 1 &&
          core.watches[0].active == 0, "coalesced range catches live watched object");
    event.event = UnixLifeEvent::HeapAllocate;
    core.Heap(event, false);
    CHECK(core.records[2].generation == 1 && core.records[2].previous == 0,
          "allocation reuse retains freed watch tombstone");
    event.event = UnixLifeEvent::Allocate; event.handle = 0x10000; event.size = 256;
    core.Watch(event);
    CHECK(core.records[3].generation == 2 && core.watches[0].active == 1,
          "address reuse receives distinct generation");
    for (unsigned i = 1; i < 129; ++i) {
        event.handle = 0x10000 + i * 0x1000; core.Watch(event);
    }
    CHECK(core.saturated == 1, "watch capacity exhaustion is counted");
    auto begin = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 100000; ++i) core.Event(event);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();
    CHECK(core.overwritten == core.sequence - 4096, "ring overwrite count is exact");
    for (uint64_t seq = core.sequence - 4095; seq <= core.sequence; ++seq)
        CHECK(core.records[(seq - 1) % 4096].sequence == seq,
              "ring contains complete latest committed sequence window");
    std::printf("unix lifetime core: PASS bytes=%zu record=%zu host_event_ns=%lld\n",
                sizeof(core), sizeof(event), static_cast<long long>(ns / 100000));
}

static void TestPipeReadRegistration()
{
    using brook::PipeBuffer;
    using brook::PipeReadWait;
    char storage[16] = {};
    PipeBuffer pipe{};
    pipe.data = storage; pipe.capacity = sizeof(storage); pipe.writers = 1;
    auto* self = reinterpret_cast<brook::Process*>(uintptr_t(0x1000));
    char byte = 0;
    CHECK(pipe.read(&byte, 1) == 0, "initial read observes empty pipe");
    CHECK(pipe.write("x", 1) == 1, "producer wins before wait preparation");
    CHECK(pipe.prepareReadWait(self, false) == PipeReadWait::Retry,
          "arrival before registration retries instead of sleeping");
    CHECK(pipe.readerWaiter == nullptr, "retry publishes no waiter");
    CHECK(pipe.read(&byte, 1) == 1 && byte == 'x', "retry drains arriving byte");

    CHECK(pipe.read(&byte, 1) == 0, "read before final write observes empty");
    CHECK(pipe.write("y", 1) == 1, "final write arrives before close");
    brook::SpinLockAcquire(&pipe.lock);
    __atomic_store_n(&pipe.writers, 0, __ATOMIC_RELEASE);
    brook::SpinLockRelease(&pipe.lock);
    CHECK(pipe.prepareReadWait(self, false) == PipeReadWait::Retry,
          "final bytes win over EOF after writer closes");
    CHECK(pipe.prepareReadWait(self, true) == PipeReadWait::Retry,
          "final bytes also win for nonblocking read");
    CHECK(pipe.read(&byte, 1) == 1 && byte == 'y', "final byte survives close race");
    CHECK(pipe.prepareReadWait(self, false) == PipeReadWait::Eof,
          "EOF only after all buffered bytes drain");
    pipe.writers = 1;
    CHECK(pipe.prepareReadWait(self, true) == PipeReadWait::WouldBlock,
          "empty live nonblocking pipe returns EAGAIN");
    CHECK(pipe.readerWaiter == nullptr, "nonblocking read publishes no waiter");
    CHECK(pipe.prepareReadWait(self, false) == PipeReadWait::Registered,
          "empty live blocking pipe registers waiter");
    CHECK(pipe.readerWaiter == self, "waiter is visible before blocking");
    pipe.removeReadWait(self);
    CHECK(pipe.readerWaiter == nullptr, "wake or interruption removes registration");
}

int main()
{
    TestPipeReadRegistration();
    TestUnixLifeCore();
    TestBasic();
    TestConcurrent();
    TestPinBasic();
    TestPinConcurrent();
    TestDuplicate();
    TestOldPublicationNegative();
    TestOwnedPublication();
    TestSyncFileCopyoutRollback();
    TestForkSnapshotRace();

    if (g_failures == 0) {
        std::printf("test_fd_table: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_fd_table: %d FAILURE(S)\n", g_failures);
    return 1;
}
