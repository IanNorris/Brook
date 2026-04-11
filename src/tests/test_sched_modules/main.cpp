// test_sched_modules — Host-side tests for scheduler modules via dlopen.
//
// Loads each scheduler .so through the SchedOps vtable interface,
// validating that the dynamic library ABI works correctly.
//
// Tests run against BOTH sched_rr and sched_mlfq unless noted.

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <dlfcn.h>
#include <climits>

// Include the shared ABI header via relative path (no -I on kernel/src
// to avoid kernel string.h shadowing system headers).
#include "../../kernel/src/sched_ops.h"

using namespace brook;

// ---------------------------------------------------------------------------
// Test framework
// ---------------------------------------------------------------------------

static int g_passed = 0, g_failed = 0;
static const char* g_currentModule = "";

#define TEST(name) \
    static void run_##name(const SchedOps* ops, void* state); \
    static void invoke_##name(const SchedOps* ops, void* state) { \
        ops->Init(state); \
        printf("  %-50s ", #name); \
        run_##name(ops, state); \
        printf("PASS\n"); g_passed++; \
    } \
    static void run_##name(const SchedOps* ops, void* state)

#define ASSERT(expr) do { \
    if (!(expr)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++; return; \
    } \
} while(0)

static constexpr uint16_t NONE = UINT16_MAX;

// ---------------------------------------------------------------------------
// dlopen harness
// ---------------------------------------------------------------------------

struct LoadedModule {
    void*           handle;
    const SchedOps* ops;
    void*           state;
};

static LoadedModule LoadScheduler(const char* soPath) {
    LoadedModule m = {};
    m.handle = dlopen(soPath, RTLD_NOW);
    if (!m.handle) {
        fprintf(stderr, "dlopen(%s): %s\n", soPath, dlerror());
        exit(1);
    }
    auto fn = (const SchedOps*(*)()) dlsym(m.handle, "GetSchedOps");
    if (!fn) {
        fprintf(stderr, "dlsym(GetSchedOps): %s\n", dlerror());
        exit(1);
    }
    m.ops = fn();
    if (!m.ops || !m.ops->name) {
        fprintf(stderr, "GetSchedOps() returned null or invalid ops\n");
        exit(1);
    }
    m.state = calloc(1, m.ops->stateSize);
    if (!m.state) {
        fprintf(stderr, "Failed to allocate %zu bytes for state\n", m.ops->stateSize);
        exit(1);
    }
    return m;
}

static void UnloadScheduler(LoadedModule* m) {
    free(m->state);
    dlclose(m->handle);
    m->handle = nullptr;
    m->ops = nullptr;
    m->state = nullptr;
}

// ---------------------------------------------------------------------------
// Tests — run for BOTH schedulers
// ---------------------------------------------------------------------------

TEST(vtable_complete) {
    (void)state;
    // Every function pointer must be non-null.
    ASSERT(ops->name != nullptr);
    ASSERT(ops->stateSize > 0);
    ASSERT(ops->Init != nullptr);
    ASSERT(ops->InitProcess != nullptr);
    ASSERT(ops->Enqueue != nullptr);
    ASSERT(ops->PickNext != nullptr);
    ASSERT(ops->Remove != nullptr);
    ASSERT(ops->TimesliceExpired != nullptr);
    ASSERT(ops->VoluntaryYield != nullptr);
    ASSERT(ops->Tick != nullptr);
    ASSERT(ops->Timeslice != nullptr);
    ASSERT(ops->ReadyCount != nullptr);
}

TEST(empty_pick_returns_none) {
    ASSERT(ops->PickNext(state) == NONE);
    ASSERT(ops->ReadyCount(state) == 0);
}

TEST(single_enqueue_dequeue) {
    ops->InitProcess(state, 1, 2);
    ops->Enqueue(state, 1);
    ASSERT(ops->ReadyCount(state) == 1);
    uint16_t pid = ops->PickNext(state);
    ASSERT(pid == 1);
    ASSERT(ops->ReadyCount(state) == 0);
    ASSERT(ops->PickNext(state) == NONE);
}

TEST(fifo_within_same_priority) {
    // All processes at same priority — should come out FIFO.
    for (uint16_t i = 0; i < 8; i++)
        ops->InitProcess(state, i, 2); // all NORMAL
    for (uint16_t i = 0; i < 8; i++)
        ops->Enqueue(state, i);
    for (uint16_t i = 0; i < 8; i++) {
        uint16_t pid = ops->PickNext(state);
        ASSERT(pid == i);
    }
}

TEST(remove_from_queue) {
    ops->InitProcess(state, 0, 2);
    ops->InitProcess(state, 1, 2);
    ops->InitProcess(state, 2, 2);
    ops->Enqueue(state, 0);
    ops->Enqueue(state, 1);
    ops->Enqueue(state, 2);
    ASSERT(ops->ReadyCount(state) == 3);
    ops->Remove(state, 1); // remove middle
    ASSERT(ops->ReadyCount(state) == 2);
    ASSERT(ops->PickNext(state) == 0);
    ASSERT(ops->PickNext(state) == 2);
}

TEST(remove_head) {
    ops->InitProcess(state, 0, 2);
    ops->InitProcess(state, 1, 2);
    ops->Enqueue(state, 0);
    ops->Enqueue(state, 1);
    ops->Remove(state, 0);
    ASSERT(ops->PickNext(state) == 1);
}

TEST(remove_tail) {
    ops->InitProcess(state, 0, 2);
    ops->InitProcess(state, 1, 2);
    ops->Enqueue(state, 0);
    ops->Enqueue(state, 1);
    ops->Remove(state, 1);
    ASSERT(ops->PickNext(state) == 0);
}

TEST(remove_not_queued_is_noop) {
    ops->InitProcess(state, 5, 2);
    ops->Remove(state, 5); // not queued — should not crash
    ASSERT(ops->ReadyCount(state) == 0);
}

TEST(double_enqueue_ignored) {
    ops->InitProcess(state, 3, 2);
    ops->Enqueue(state, 3);
    ops->Enqueue(state, 3); // second enqueue should be ignored
    ASSERT(ops->ReadyCount(state) == 1);
    ASSERT(ops->PickNext(state) == 3);
    ASSERT(ops->ReadyCount(state) == 0);
}

TEST(timeslice_positive) {
    ops->InitProcess(state, 0, 2);
    uint64_t ts = ops->Timeslice(state, 0);
    ASSERT(ts > 0);
    ASSERT(ts <= 100); // reasonable upper bound
}

TEST(round_robin_fairness) {
    // Simulate round-robin: N processes, each gets re-enqueued after pick.
    // Count how many times each process is scheduled over many cycles.
    // All should get equal share (within tolerance).
    static constexpr int N = 8;
    static constexpr int CYCLES = 1000;

    for (uint16_t i = 0; i < N; i++)
        ops->InitProcess(state, i, 2); // all same priority
    for (uint16_t i = 0; i < N; i++)
        ops->Enqueue(state, i);

    int counts[N] = {};
    for (int c = 0; c < CYCLES; c++) {
        uint16_t pid = ops->PickNext(state);
        ASSERT(pid != NONE);
        ASSERT(pid < N);
        counts[pid]++;
        ops->Enqueue(state, pid); // re-enqueue
    }

    int expected = CYCLES / N; // 125
    for (int i = 0; i < N; i++) {
        ASSERT(counts[i] == expected); // exact for FIFO-based schedulers
    }
}

TEST(no_starvation_all_get_time) {
    // Even with different priorities, every process must eventually run.
    // Simulate with Tick calls to trigger anti-starvation boosts.
    static constexpr int N = 16;
    static constexpr int TOTAL_PICKS = 5000;

    for (uint16_t i = 0; i < N; i++)
        ops->InitProcess(state, i, (uint8_t)(i % 4)); // mixed priorities
    for (uint16_t i = 0; i < N; i++)
        ops->Enqueue(state, i);

    int counts[N] = {};
    uint64_t simTime = 0;

    for (int c = 0; c < TOTAL_PICKS; c++) {
        // Tick every 10 picks to simulate time passing
        if (c % 10 == 0) {
            simTime += 100; // 100ms per batch
            ops->Tick(state, simTime);
        }
        uint16_t pid = ops->PickNext(state);
        ASSERT(pid != NONE);
        ASSERT(pid < N);
        counts[pid]++;

        // Simulate timeslice expiry for half, voluntary yield for others
        if (c % 3 == 0)
            ops->TimesliceExpired(state, pid);
        else
            ops->VoluntaryYield(state, pid);

        ops->Enqueue(state, pid);
    }

    // Every process must have run at least 1% of total picks.
    int minExpected = TOTAL_PICKS / 100;
    for (int i = 0; i < N; i++) {
        ASSERT(counts[i] >= minExpected);
    }
}

TEST(many_processes_32) {
    // 32 processes, all same priority, verify exact round-robin fairness.
    static constexpr int N = 32;
    static constexpr int CYCLES = 3200; // 100 per process

    for (uint16_t i = 0; i < N; i++)
        ops->InitProcess(state, i, 2);
    for (uint16_t i = 0; i < N; i++)
        ops->Enqueue(state, i);

    int counts[N] = {};
    for (int c = 0; c < CYCLES; c++) {
        uint16_t pid = ops->PickNext(state);
        ASSERT(pid != NONE);
        counts[pid]++;
        ops->Enqueue(state, pid);
    }

    for (int i = 0; i < N; i++)
        ASSERT(counts[i] == CYCLES / N);
}

TEST(enqueue_after_remove_works) {
    ops->InitProcess(state, 0, 2);
    ops->Enqueue(state, 0);
    ops->Remove(state, 0);
    ASSERT(ops->ReadyCount(state) == 0);
    ops->Enqueue(state, 0); // re-enqueue after remove
    ASSERT(ops->ReadyCount(state) == 1);
    ASSERT(ops->PickNext(state) == 0);
}

TEST(timeslice_expired_then_yield_cycle) {
    // Repeatedly expire and yield — process should keep working.
    ops->InitProcess(state, 0, 2);
    for (int i = 0; i < 100; i++) {
        ops->Enqueue(state, 0);
        ASSERT(ops->PickNext(state) == 0);
        if (i % 2 == 0)
            ops->TimesliceExpired(state, 0);
        else
            ops->VoluntaryYield(state, 0);
    }
}

// ---------------------------------------------------------------------------
// MLFQ-specific tests (only run for mlfq)
// ---------------------------------------------------------------------------

static void run_mlfq_priority_ordering(const SchedOps* ops, void* state) {
    ops->Init(state);
    // Higher priority (lower number) should be picked first.
    ops->InitProcess(state, 0, 3); // LOW
    ops->InitProcess(state, 1, 0); // REALTIME
    ops->InitProcess(state, 2, 2); // NORMAL
    ops->InitProcess(state, 3, 1); // HIGH
    ops->Enqueue(state, 0);
    ops->Enqueue(state, 1);
    ops->Enqueue(state, 2);
    ops->Enqueue(state, 3);
    assert(ops->PickNext(state) == 1); // REALTIME first
    assert(ops->PickNext(state) == 3); // HIGH second
    assert(ops->PickNext(state) == 2); // NORMAL third
    assert(ops->PickNext(state) == 0); // LOW last
}

static void run_mlfq_demotion(const SchedOps* ops, void* state) {
    ops->Init(state);
    // Process at NORMAL, expire timeslice → should demote to LOW.
    ops->InitProcess(state, 0, 2); // NORMAL
    ops->InitProcess(state, 1, 2); // NORMAL
    ops->TimesliceExpired(state, 0); // demote 0 → LOW
    ops->Enqueue(state, 0);
    ops->Enqueue(state, 1);
    assert(ops->PickNext(state) == 1); // 1 is NORMAL, picked first
    assert(ops->PickNext(state) == 0); // 0 is LOW, picked second
}

static void run_mlfq_boost(const SchedOps* ops, void* state) {
    ops->Init(state);
    // Process demoted to LOW, then boost should bring it back to HIGH.
    ops->InitProcess(state, 0, 3); // LOW
    ops->Enqueue(state, 0);
    ops->Tick(state, 0);    // initial time
    ops->Tick(state, 1001); // past boost interval
    // Process should now be at HIGH priority
    uint64_t ts = ops->Timeslice(state, 0);
    assert(ts == 10); // HIGH timeslice
}

static void run_mlfq_timeslice_by_priority(const SchedOps* ops, void* state) {
    ops->Init(state);
    ops->InitProcess(state, 0, 0); // REALTIME
    ops->InitProcess(state, 1, 1); // HIGH
    ops->InitProcess(state, 2, 2); // NORMAL
    ops->InitProcess(state, 3, 3); // LOW
    assert(ops->Timeslice(state, 0) == 5);
    assert(ops->Timeslice(state, 1) == 10);
    assert(ops->Timeslice(state, 2) == 20);
    assert(ops->Timeslice(state, 3) == 40);
}

static void run_mlfq_voluntary_yield_boosts(const SchedOps* ops, void* state) {
    ops->Init(state);
    ops->InitProcess(state, 0, 3); // LOW
    ops->VoluntaryYield(state, 0); // boost → NORMAL
    assert(ops->Timeslice(state, 0) == 20);
    ops->VoluntaryYield(state, 0); // boost → HIGH
    assert(ops->Timeslice(state, 0) == 10);
    ops->VoluntaryYield(state, 0); // already HIGH, should stay
    assert(ops->Timeslice(state, 0) == 10);
}

static void run_mlfq_priority_fairness(const SchedOps* ops, void* state) {
    // Within each priority band, processes should get equal time.
    // Create 4 HIGH and 4 LOW processes.
    ops->Init(state);
    for (uint16_t i = 0; i < 4; i++)
        ops->InitProcess(state, i, 1); // HIGH
    for (uint16_t i = 4; i < 8; i++)
        ops->InitProcess(state, i, 3); // LOW
    for (uint16_t i = 0; i < 8; i++)
        ops->Enqueue(state, i);

    int counts[8] = {};
    uint64_t simTime = 0;

    for (int c = 0; c < 4000; c++) {
        if (c % 10 == 0) {
            simTime += 100;
            ops->Tick(state, simTime);
        }
        uint16_t pid = ops->PickNext(state);
        assert(pid != NONE);
        counts[pid]++;
        ops->Enqueue(state, pid);
    }

    // HIGH processes should have gotten MORE time than LOW.
    int highMin = INT_MAX, highMax = 0;
    int lowMin = INT_MAX, lowMax = 0;
    for (int i = 0; i < 4; i++) {
        if (counts[i] < highMin) highMin = counts[i];
        if (counts[i] > highMax) highMax = counts[i];
    }
    for (int i = 4; i < 8; i++) {
        if (counts[i] < lowMin) lowMin = counts[i];
        if (counts[i] > lowMax) lowMax = counts[i];
    }

    // HIGH processes should get more than LOW.
    assert(highMin > lowMax);

    // Within each band, fairness: max/min ratio < 2.
    assert(highMax <= highMin * 2);
    assert(lowMax <= lowMin * 2);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static void RunCommonTests(const SchedOps* ops, void* state) {
    invoke_vtable_complete(ops, state);
    invoke_empty_pick_returns_none(ops, state);
    invoke_single_enqueue_dequeue(ops, state);
    invoke_fifo_within_same_priority(ops, state);
    invoke_remove_from_queue(ops, state);
    invoke_remove_head(ops, state);
    invoke_remove_tail(ops, state);
    invoke_remove_not_queued_is_noop(ops, state);
    invoke_double_enqueue_ignored(ops, state);
    invoke_timeslice_positive(ops, state);
    invoke_round_robin_fairness(ops, state);
    invoke_no_starvation_all_get_time(ops, state);
    invoke_many_processes_32(ops, state);
    invoke_enqueue_after_remove_works(ops, state);
    invoke_timeslice_expired_then_yield_cycle(ops, state);
}

static void RunMlfqTests(const SchedOps* ops, void* state) {
    printf("  %-50s ", "mlfq_priority_ordering");
    run_mlfq_priority_ordering(ops, state);
    printf("PASS\n"); g_passed++;

    printf("  %-50s ", "mlfq_demotion");
    run_mlfq_demotion(ops, state);
    printf("PASS\n"); g_passed++;

    printf("  %-50s ", "mlfq_boost");
    run_mlfq_boost(ops, state);
    printf("PASS\n"); g_passed++;

    printf("  %-50s ", "mlfq_timeslice_by_priority");
    run_mlfq_timeslice_by_priority(ops, state);
    printf("PASS\n"); g_passed++;

    printf("  %-50s ", "mlfq_voluntary_yield_boosts");
    run_mlfq_voluntary_yield_boosts(ops, state);
    printf("PASS\n"); g_passed++;

    printf("  %-50s ", "mlfq_priority_fairness");
    run_mlfq_priority_fairness(ops, state);
    printf("PASS\n"); g_passed++;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <sched_rr.so> [sched_mlfq.so]\n", argv[0]);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        LoadedModule mod = LoadScheduler(argv[i]);
        printf("\n=== %s (%s) ===\n", mod.ops->name, argv[i]);
        g_currentModule = mod.ops->name;

        RunCommonTests(mod.ops, mod.state);

        if (strcmp(mod.ops->name, "mlfq") == 0)
            RunMlfqTests(mod.ops, mod.state);

        UnloadScheduler(&mod);
    }

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
