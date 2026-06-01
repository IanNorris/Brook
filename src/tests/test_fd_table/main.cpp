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

#include "fd_table.h"

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

static void CloserWorker()
{
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
    }
}

static void TestConcurrent()
{
    constexpr int      THREADS = 16;
    constexpr uint64_t ROUNDS  = 4000;

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

        std::vector<std::thread> workers;
        workers.reserve(THREADS);
        for (int t = 0; t < THREADS; ++t)
            workers.emplace_back(CloserWorker);
        for (auto& w : workers) w.join();

        // Every token freed exactly once; table fully drained.
        for (uint32_t i = 0; i < MAX_FDS; ++i) {
            if (g_freed[i].load() != 1) { g_doubleFree.store(true); }
            if (g_fds[i].type != FdType::None) { g_doubleFree.store(true); }
        }
        if (g_doubleFree.load()) break;  // fail fast
    }

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

static void PinUserWorker(uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 1;
    while (pin_remaining.load(std::memory_order_relaxed) > 0) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        int fd = static_cast<int>(x % PIN_SLOTS);
        FdEntry* e = FdTablePin(pin_fds, &pin_lock, fd);
        if (!e) continue;
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
}

static void PinCloserWorker(uint32_t seed)
{
    uint32_t x = seed * 40503u + 7;
    while (pin_remaining.load(std::memory_order_relaxed) > 0) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        int fd = static_cast<int>(x % PIN_SLOTS);
        FdClaimResult c;
        if (FdTableClose(pin_fds, &pin_lock, fd, &c) == FdCloseResult::ClaimedNow)
            PinFinalize(HandleToken(c.handle));
        // Deferred: the last unpin finalizes. NotFound: already gone.
    }
}

static void TestPinConcurrent()
{
    constexpr int      USERS   = 12;
    constexpr int      CLOSERS = 4;
    constexpr uint64_t ROUNDS  = 6000;

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

        std::vector<std::thread> workers;
        workers.reserve(USERS + CLOSERS);
        for (int t = 0; t < USERS; ++t)
            workers.emplace_back(PinUserWorker, static_cast<uint32_t>(r * 131 + t + 1));
        for (int t = 0; t < CLOSERS; ++t)
            workers.emplace_back(PinCloserWorker, static_cast<uint32_t>(r * 977 + t + 1));
        for (auto& w : workers) w.join();

        for (int s = 0; s < PIN_SLOTS; ++s) {
            if (pin_finalized[s].load() != 1) pin_doubleFree.store(true);
            if (pin_fds[s].type != FdType::None) pin_doubleFree.store(true);
        }
        if (pin_uaf.load() || pin_doubleFree.load()) break;  // fail fast
    }

    CHECK(!pin_uaf.load(), "no pinned user ever saw a freed handle (no UAF)");
    CHECK(!pin_doubleFree.load(), "every handle finalized exactly once");

    std::printf("fd_table pin stress: %d users + %d closers over %llu rounds x %d slots\n",
                USERS, CLOSERS,
                static_cast<unsigned long long>(ROUNDS), PIN_SLOTS);
}

int main()
{
    TestBasic();
    TestConcurrent();
    TestPinBasic();
    TestPinConcurrent();

    if (g_failures == 0) {
        std::printf("test_fd_table: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_fd_table: %d FAILURE(S)\n", g_failures);
    return 1;
}
