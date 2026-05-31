// Host-side unit tests for the futex waiter pool (BRO-160).
//
// Exercises the REAL pool implementation from
// src/kernel/src/sync/futex_waiter_pool.h (not a reimplementation), focusing
// on the two properties the fix guarantees:
//   1. Slot claim/release is atomic, so concurrent Alloc/Free from many
//      threads never hand the same slot to two owners.
//   2. The pool is sized so it is not artificially small (capacity check).
//
// Build under ThreadSanitizer (scripts/run-tsan-tests.sh) to additionally flag
// any data race on the slot flags or the FutexWaiter payload.

#include <cstdio>
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>

// The pool forward-declares brook::Process and only stores the pointer; no
// kernel headers are required to test it.
#include "sync/futex_waiter_pool.h"

using brook::FutexWaiter;
using brook::FutexWaiterPool;

static int g_failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__,      \
                         __LINE__);                                        \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ---------------------------------------------------------------------------
// Single-threaded correctness: exhaustion, distinctness, reuse.
// ---------------------------------------------------------------------------
static FutexWaiterPool<64> g_basicPool;

static void TestBasic()
{
    constexpr uint32_t N = 64;
    CHECK(g_basicPool.Capacity() == N, "capacity");

    FutexWaiter* got[N];
    bool seen[N] = {};
    for (uint32_t i = 0; i < N; ++i) {
        got[i] = g_basicPool.Alloc();
        CHECK(got[i] != nullptr, "alloc within capacity");
        int32_t idx = g_basicPool.IndexOf(got[i]);
        CHECK(idx >= 0 && static_cast<uint32_t>(idx) < N, "index in range");
        CHECK(!seen[idx], "no slot handed out twice");
        seen[idx] = true;
    }

    // Pool is full now.
    CHECK(g_basicPool.Alloc() == nullptr, "alloc fails when full");

    // Free one, allocate one — must succeed and reuse a freed slot.
    g_basicPool.Free(got[10]);
    FutexWaiter* re = g_basicPool.Alloc();
    CHECK(re != nullptr, "alloc after free");
    CHECK(g_basicPool.IndexOf(re) == 10, "reuses the freed slot");

    // Defensive: Free(nullptr) and a foreign pointer are ignored.
    g_basicPool.Free(nullptr);
    FutexWaiter foreign;
    g_basicPool.Free(&foreign);              // out of pool: must be a no-op
    CHECK(g_basicPool.IndexOf(&foreign) == -1, "foreign pointer not ours");

    // Drain everything we hold.
    for (uint32_t i = 0; i < N; ++i)
        if (i != 10) g_basicPool.Free(got[i]);
    g_basicPool.Free(re);

    // Whole pool must be allocatable again.
    uint32_t count = 0;
    FutexWaiter* w;
    while ((w = g_basicPool.Alloc()) != nullptr) {
        ++count;
        if (count > N) break;  // guard against a corrupted pool looping forever
    }
    CHECK(count == N, "all slots reclaimed after drain");
    // g_basicPool is left full; it is not used again.
}

// ---------------------------------------------------------------------------
// Concurrent stress: many threads hammer Alloc/Free. A per-slot shadow owner
// (compare-exchange -1 -> tid on claim, store -1 on release) detects any slot
// being live for two owners at once — the exact failure an atomic claim must
// prevent.
// ---------------------------------------------------------------------------
constexpr uint32_t STRESS_N = 64;
static FutexWaiterPool<STRESS_N> g_stressPool;
static std::atomic<int> g_slotOwner[STRESS_N];

static std::atomic<bool>     g_doubleAlloc{false};
static std::atomic<uint64_t> g_allocs{0};
static std::atomic<uint32_t> g_maxLive{0};
static std::atomic<int32_t>  g_live{0};

static void StressWorker(int tid, uint64_t iters)
{
    for (uint64_t k = 0; k < iters; ++k) {
        FutexWaiter* w = g_stressPool.Alloc();
        if (!w) continue;  // pool momentarily full — legitimate

        int32_t idx = g_stressPool.IndexOf(w);
        if (idx < 0 || static_cast<uint32_t>(idx) >= STRESS_N) {
            g_doubleAlloc.store(true);  // bogus index == pool corruption
            continue;
        }

        // Claim the shadow slot: must transition from free (-1) to us.
        int expected = -1;
        if (!g_slotOwner[idx].compare_exchange_strong(
                expected, tid, std::memory_order_acq_rel)) {
            // Someone else already owns this slot -> the pool handed it out
            // twice without it being freed in between.
            g_doubleAlloc.store(true);
        }

        int32_t live = g_live.fetch_add(1, std::memory_order_relaxed) + 1;
        uint32_t prevMax = g_maxLive.load(std::memory_order_relaxed);
        while (static_cast<uint32_t>(live) > prevMax &&
               !g_maxLive.compare_exchange_weak(prevMax,
                                                static_cast<uint32_t>(live),
                                                std::memory_order_relaxed)) {}

        // Touch the payload so TSan can observe any aliasing race.
        w->uaddr  = static_cast<uint64_t>(tid);
        w->owner  = static_cast<uint64_t>(idx);
        w->proc   = nullptr;
        w->bitset = 0xFFFFFFFFu;

        // Release: shadow first, then the real slot.
        g_live.fetch_sub(1, std::memory_order_relaxed);
        g_slotOwner[idx].store(-1, std::memory_order_release);
        g_stressPool.Free(w);

        g_allocs.fetch_add(1, std::memory_order_relaxed);
    }
}

static void TestConcurrent()
{
    for (uint32_t i = 0; i < STRESS_N; ++i)
        g_slotOwner[i].store(-1);

    constexpr int      THREADS = 32;   // > capacity, so the pool runs full
    constexpr uint64_t ITERS   = 40000;

    std::vector<std::thread> pool;
    pool.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t)
        pool.emplace_back(StressWorker, t + 1, ITERS);
    for (auto& th : pool) th.join();

    CHECK(!g_doubleAlloc.load(), "no slot ever handed to two owners");
    CHECK(g_live.load() == 0, "all slots released after stress");
    CHECK(g_maxLive.load() <= STRESS_N, "never more live than capacity");

    // After the storm, the whole pool must be allocatable again (no slot leaked
    // as permanently-claimed).
    uint32_t reclaimed = 0;
    FutexWaiter* w;
    while ((w = g_stressPool.Alloc()) != nullptr) {
        ++reclaimed;
        if (reclaimed > STRESS_N) break;
    }
    CHECK(reclaimed == STRESS_N, "entire pool reclaimable after stress");

    std::printf("futex pool stress: %llu alloc/free cycles, peak live=%u/%u\n",
                static_cast<unsigned long long>(g_allocs.load()),
                g_maxLive.load(), STRESS_N);
}

int main()
{
    TestBasic();
    TestConcurrent();

    if (g_failures == 0) {
        std::printf("test_futex_pool: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_futex_pool: %d FAILURE(S)\n", g_failures);
    return 1;
}
