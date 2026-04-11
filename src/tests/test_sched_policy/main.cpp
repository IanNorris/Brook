// Host-side unit tests for SchedPolicy (MLFQ scheduling policy).
// Tests the scheduling logic in isolation without kernel dependencies.

#include <cstdio>
#include <cstdlib>
#include <cassert>

// Include the real header — sched_policy.h has no kernel dependencies.
#include "../../kernel/src/sched_policy.h"

// ---------- Test infrastructure ----------
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST(name) \
    static void test_##name(); \
    static void run_##name() { \
        printf("  %-55s ", #name); \
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

// ---------- Helper ----------
static SchedProcess MakeProc(uint16_t pid, uint8_t prio = SCHED_PRIORITY_NORMAL)
{
    SchedProcess p;
    p.pid = pid;
    SchedPolicyInitProcess(&p, prio);
    return p;
}

// ---------- Tests ----------

TEST(init_empty) {
    SchedPolicyState state;
    SchedPolicyInit(&state);
    ASSERT(SchedPolicyReadyCount(&state) == 0);
    ASSERT(SchedPolicyPickNext(&state) == nullptr);
}

TEST(enqueue_dequeue_single) {
    SchedPolicyState state;
    SchedPolicyInit(&state);
    SchedProcess p = MakeProc(1);
    SchedPolicyEnqueue(&state, &p);
    ASSERT(SchedPolicyReadyCount(&state) == 1);
    SchedProcess* picked = SchedPolicyPickNext(&state);
    ASSERT(picked == &p);
    ASSERT(SchedPolicyReadyCount(&state) == 0);
}

TEST(fifo_same_priority) {
    SchedPolicyState state;
    SchedPolicyInit(&state);
    SchedProcess p1 = MakeProc(1);
    SchedProcess p2 = MakeProc(2);
    SchedProcess p3 = MakeProc(3);
    SchedPolicyEnqueue(&state, &p1);
    SchedPolicyEnqueue(&state, &p2);
    SchedPolicyEnqueue(&state, &p3);

    ASSERT(SchedPolicyPickNext(&state)->pid == 1);
    ASSERT(SchedPolicyPickNext(&state)->pid == 2);
    ASSERT(SchedPolicyPickNext(&state)->pid == 3);
    ASSERT(SchedPolicyPickNext(&state) == nullptr);
}

TEST(priority_ordering) {
    SchedPolicyState state;
    SchedPolicyInit(&state);
    SchedProcess low  = MakeProc(1, SCHED_PRIORITY_LOW);
    SchedProcess high = MakeProc(2, SCHED_PRIORITY_HIGH);
    SchedProcess norm = MakeProc(3, SCHED_PRIORITY_NORMAL);

    // Enqueue in wrong order — pick should respect priority.
    SchedPolicyEnqueue(&state, &low);
    SchedPolicyEnqueue(&state, &norm);
    SchedPolicyEnqueue(&state, &high);

    ASSERT(SchedPolicyPickNext(&state)->pid == 2); // high
    ASSERT(SchedPolicyPickNext(&state)->pid == 3); // normal
    ASSERT(SchedPolicyPickNext(&state)->pid == 1); // low
}

TEST(timeslice_by_priority) {
    SchedProcess rt   = MakeProc(1, SCHED_PRIORITY_REALTIME);
    SchedProcess high = MakeProc(2, SCHED_PRIORITY_HIGH);
    SchedProcess norm = MakeProc(3, SCHED_PRIORITY_NORMAL);
    SchedProcess low  = MakeProc(4, SCHED_PRIORITY_LOW);

    ASSERT(SchedPolicyTimeslice(&rt)   == 5);
    ASSERT(SchedPolicyTimeslice(&high) == 10);
    ASSERT(SchedPolicyTimeslice(&norm) == 20);
    ASSERT(SchedPolicyTimeslice(&low)  == 40);
}

TEST(timeslice_expired_demotes) {
    SchedProcess p = MakeProc(1, SCHED_PRIORITY_HIGH);
    ASSERT(p.sched.priority == SCHED_PRIORITY_HIGH);

    SchedPolicyTimesliceExpired(&p);
    ASSERT(p.sched.priority == SCHED_PRIORITY_NORMAL);
    ASSERT(p.sched.preemptCount == 1);

    SchedPolicyTimesliceExpired(&p);
    ASSERT(p.sched.priority == SCHED_PRIORITY_LOW);

    // Can't go below LOW.
    SchedPolicyTimesliceExpired(&p);
    ASSERT(p.sched.priority == SCHED_PRIORITY_LOW);
}

TEST(voluntary_yield_boosts) {
    SchedProcess p = MakeProc(1, SCHED_PRIORITY_LOW);

    SchedPolicyVoluntaryYield(&p);
    ASSERT(p.sched.priority == SCHED_PRIORITY_NORMAL);
    ASSERT(p.sched.yieldCount == 1);

    SchedPolicyVoluntaryYield(&p);
    ASSERT(p.sched.priority == SCHED_PRIORITY_HIGH);

    // Can't go above HIGH (REALTIME is reserved).
    SchedPolicyVoluntaryYield(&p);
    ASSERT(p.sched.priority == SCHED_PRIORITY_HIGH);
}

TEST(remove_from_queue) {
    SchedPolicyState state;
    SchedPolicyInit(&state);
    SchedProcess p1 = MakeProc(1);
    SchedProcess p2 = MakeProc(2);
    SchedProcess p3 = MakeProc(3);
    SchedPolicyEnqueue(&state, &p1);
    SchedPolicyEnqueue(&state, &p2);
    SchedPolicyEnqueue(&state, &p3);

    // Remove middle element.
    SchedPolicyRemove(&state, &p2);
    ASSERT(SchedPolicyReadyCount(&state) == 2);
    ASSERT(SchedPolicyPickNext(&state)->pid == 1);
    ASSERT(SchedPolicyPickNext(&state)->pid == 3);
}

TEST(remove_head) {
    SchedPolicyState state;
    SchedPolicyInit(&state);
    SchedProcess p1 = MakeProc(1);
    SchedProcess p2 = MakeProc(2);
    SchedPolicyEnqueue(&state, &p1);
    SchedPolicyEnqueue(&state, &p2);

    SchedPolicyRemove(&state, &p1);
    ASSERT(SchedPolicyPickNext(&state)->pid == 2);
}

TEST(remove_tail) {
    SchedPolicyState state;
    SchedPolicyInit(&state);
    SchedProcess p1 = MakeProc(1);
    SchedProcess p2 = MakeProc(2);
    SchedPolicyEnqueue(&state, &p1);
    SchedPolicyEnqueue(&state, &p2);

    SchedPolicyRemove(&state, &p2);
    ASSERT(SchedPolicyPickNext(&state)->pid == 1);
}

TEST(boost_all_prevents_starvation) {
    SchedPolicyState state;
    SchedPolicyInit(&state);

    SchedProcess p1 = MakeProc(1, SCHED_PRIORITY_LOW);
    SchedProcess p2 = MakeProc(2, SCHED_PRIORITY_LOW);
    SchedProcess p3 = MakeProc(3, SCHED_PRIORITY_NORMAL);
    SchedPolicyEnqueue(&state, &p1);
    SchedPolicyEnqueue(&state, &p2);
    SchedPolicyEnqueue(&state, &p3);

    // Boost at time 1000ms.
    SchedPolicyBoostAll(&state, 1000);

    // All should now be HIGH priority.
    ASSERT(p1.sched.priority == SCHED_PRIORITY_HIGH);
    ASSERT(p2.sched.priority == SCHED_PRIORITY_HIGH);
    ASSERT(p3.sched.priority == SCHED_PRIORITY_HIGH);

    // Order: NORMAL queue drained first (p3), then LOW queue (p1, p2).
    ASSERT(SchedPolicyPickNext(&state)->pid == 3);
    ASSERT(SchedPolicyPickNext(&state)->pid == 1);
    ASSERT(SchedPolicyPickNext(&state)->pid == 2);
}

TEST(boost_respects_interval) {
    SchedPolicyState state;
    SchedPolicyInit(&state);

    SchedProcess p = MakeProc(1, SCHED_PRIORITY_LOW);
    SchedPolicyEnqueue(&state, &p);

    // Boost at 500ms — too early (interval is 1000ms), should not boost.
    SchedPolicyBoostAll(&state, 500);
    ASSERT(p.sched.priority == SCHED_PRIORITY_LOW);

    // Boost at 1000ms — should boost.
    SchedPolicyBoostAll(&state, 1000);
    ASSERT(p.sched.priority == SCHED_PRIORITY_HIGH);
}

TEST(mlfq_simulation) {
    // Simulate MLFQ behavior over time:
    // - CPU-bound process gets demoted
    // - I/O-bound process stays at high priority
    SchedPolicyState state;
    SchedPolicyInit(&state);

    SchedProcess cpu_bound = MakeProc(1, SCHED_PRIORITY_NORMAL);
    SchedProcess io_bound  = MakeProc(2, SCHED_PRIORITY_NORMAL);

    // Simulate 10 scheduling rounds.
    for (int round = 0; round < 10; ++round) {
        SchedPolicyEnqueue(&state, &cpu_bound);
        SchedPolicyEnqueue(&state, &io_bound);

        SchedProcess* p = SchedPolicyPickNext(&state);
        (void)p; // First pick (whatever priority is higher)

        p = SchedPolicyPickNext(&state);
        (void)p; // Second pick

        // CPU-bound always exhausts timeslice.
        SchedPolicyTimesliceExpired(&cpu_bound);
        // I/O-bound always yields early.
        SchedPolicyVoluntaryYield(&io_bound);
    }

    // After 10 rounds: CPU-bound should be at LOW, I/O-bound at HIGH.
    ASSERT(cpu_bound.sched.priority == SCHED_PRIORITY_LOW);
    ASSERT(io_bound.sched.priority == SCHED_PRIORITY_HIGH);
    ASSERT(cpu_bound.sched.preemptCount == 10);
    ASSERT(io_bound.sched.yieldCount == 10);
}

TEST(many_processes_round_robin) {
    // 32 processes at same priority — all should get scheduled.
    SchedPolicyState state;
    SchedPolicyInit(&state);

    static const int N = 32;
    SchedProcess procs[N];
    int scheduled[N] = {};

    for (int i = 0; i < N; ++i)
        procs[i] = MakeProc((uint16_t)(i + 1));

    // Run 5 rounds.
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < N; ++i)
            SchedPolicyEnqueue(&state, &procs[i]);

        for (int i = 0; i < N; ++i) {
            SchedProcess* p = SchedPolicyPickNext(&state);
            ASSERT(p != nullptr);
            scheduled[p->pid - 1]++;
        }
        ASSERT(SchedPolicyPickNext(&state) == nullptr);
    }

    // Each should have been scheduled exactly 5 times.
    for (int i = 0; i < N; ++i)
        ASSERT(scheduled[i] == 5);
}

int main()
{
    printf("SchedPolicy (MLFQ) tests:\n");
    run_init_empty();
    run_enqueue_dequeue_single();
    run_fifo_same_priority();
    run_priority_ordering();
    run_timeslice_by_priority();
    run_timeslice_expired_demotes();
    run_voluntary_yield_boosts();
    run_remove_from_queue();
    run_remove_head();
    run_remove_tail();
    run_boost_all_prevents_starvation();
    run_boost_respects_interval();
    run_mlfq_simulation();
    run_many_processes_round_robin();

    printf("\n%d passed, %d failed\n", g_testsPassed, g_testsFailed);
    return g_testsFailed ? 1 : 0;
}
