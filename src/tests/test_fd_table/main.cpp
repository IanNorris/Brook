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

int main()
{
    TestBasic();
    TestConcurrent();

    if (g_failures == 0) {
        std::printf("test_fd_table: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_fd_table: %d FAILURE(S)\n", g_failures);
    return 1;
}
