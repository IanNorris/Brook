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
#include <deque>
#include <array>
#include <vector>

// Include the shared ABI header via relative path (no -I on kernel/src
// to avoid kernel string.h shadowing system headers).
#include "../../kernel/src/sched_ops.h"
#include "../../kernel/src/sched_restore.h"
#include "../../kernel/src/sched_decision_trace.h"

struct MlfqObservation {uint64_t calls=0,usedNs=0,quantumNs=0;uint16_t pid=0;uint8_t priority=0,reason=0,higher=0;bool serviceSeen=false;};
static MlfqObservation observedMlfq;
extern "C" void SchedulerObserveMlfqDecision(uint16_t pid,uint8_t priority,uint64_t usedNs,uint64_t quantumNs,uint8_t reason,bool serviceSeen,uint8_t higher){
    ++observedMlfq.calls;observedMlfq.pid=pid;observedMlfq.priority=priority;observedMlfq.usedNs=usedNs;observedMlfq.quantumNs=quantumNs;observedMlfq.reason=reason;observedMlfq.serviceSeen=serviceSeen;observedMlfq.higher=higher;
}

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
        const int before=g_failed; \
        run_##name(ops, state); \
        if(g_failed==before) {printf("PASS\n");g_passed++;} \
    } \
    static void run_##name(const SchedOps* ops, void* state)

#define ASSERT(expr) do { \
    if (!(expr)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++; return; \
    } \
} while(0)

static constexpr uint16_t NONE = brook::SCHED_PID_NONE;

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
    ASSERT(ops->stateSize <= SCHED_POLICY_STATE_CAPACITY);
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
    // Ordinary application bands must not starve one another. Band zero is
    // reserved for bounded critical kernel work, not an always-runnable app.
    // Its peer rotation and return to ordinary work after blocking are tested
    // separately; strict reserved priority cannot promise app service while
    // enough reserved tasks remain permanently runnable to occupy every CPU.
    static constexpr int N = 16;
    static constexpr int TOTAL_PICKS = 5000;

    for (uint16_t i = 0; i < N; i++)
        ops->InitProcess(state, i, (uint8_t)(1 + i % 3)); // ordinary mixed priorities
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
// Idle transition tests — verify correct behavior when queue empties/refills
// ---------------------------------------------------------------------------

TEST(idle_transition_all_blocked) {
    // Simulate all processes blocking: queue empties, PickNext returns NONE.
    // Then unblock one — it should be picked immediately.
    static constexpr int N = 4;
    for (uint16_t i = 1; i <= N; i++)
        ops->InitProcess(state, i, 2);
    for (uint16_t i = 1; i <= N; i++)
        ops->Enqueue(state, i);
    ASSERT(ops->ReadyCount(state) == N);

    // Simulate blocking: remove all from queue.
    for (uint16_t i = 1; i <= N; i++)
        ops->Remove(state, i);
    ASSERT(ops->ReadyCount(state) == 0);

    // CPU should be idle now — PickNext returns NONE.
    ASSERT(ops->PickNext(state) == NONE);

    // Unblock one process.
    ops->Enqueue(state, 3);
    ASSERT(ops->ReadyCount(state) == 1);
    ASSERT(ops->PickNext(state) == 3);
    ASSERT(ops->ReadyCount(state) == 0);
}

TEST(idle_transition_processes_exit) {
    // Simulate processes exiting one by one until queue is empty.
    // After each pick, do NOT re-enqueue (process has exited).
    static constexpr int N = 8;
    for (uint16_t i = 1; i <= N; i++)
        ops->InitProcess(state, i, 2);
    for (uint16_t i = 1; i <= N; i++)
        ops->Enqueue(state, i);

    for (int i = 0; i < N; i++) {
        ASSERT(ops->ReadyCount(state) == (uint32_t)(N - i));
        uint16_t pid = ops->PickNext(state);
        ASSERT(pid != NONE);
        // Process exits — not re-enqueued.
    }

    // All exited — queue empty, CPU goes idle.
    ASSERT(ops->ReadyCount(state) == 0);
    ASSERT(ops->PickNext(state) == NONE);
    ASSERT(ops->PickNext(state) == NONE); // repeated calls are safe
}

TEST(idle_wake_after_all_exit_new_process) {
    // All processes exit, then a brand new process appears.
    ops->InitProcess(state, 1, 2);
    ops->Enqueue(state, 1);
    ASSERT(ops->PickNext(state) == 1);
    // Process 1 exits. Queue empty.
    ASSERT(ops->PickNext(state) == NONE);

    // New process created and enqueued.
    ops->InitProcess(state, 5, 2);
    ops->Enqueue(state, 5);
    ASSERT(ops->ReadyCount(state) == 1);
    ASSERT(ops->PickNext(state) == 5);
}

TEST(idle_repeated_block_unblock_cycle) {
    // A single process repeatedly blocks (removed) and unblocks (enqueued).
    // Between each cycle, PickNext should return NONE (idle), then the process.
    ops->InitProcess(state, 1, 2);
    for (int i = 0; i < 50; i++) {
        ops->Enqueue(state, 1);
        ASSERT(ops->PickNext(state) == 1);
        // Process blocks — not in queue.
        ASSERT(ops->PickNext(state) == NONE);
        // Process unblocks.
    }
}

TEST(idle_simulation) {
    // Simulate the kernel scheduling loop with idle processes.
    // The kernel has one idle per CPU, ALL with pid=0, NONE registered
    // with the policy module. If PickNext returns NONE, that CPU runs idle.
    //
    // This test models 4 CPUs with 8 processes and verifies:
    //  1. When all processes block, all CPUs go idle (PickNext → NONE)
    //  2. When processes unblock, they preempt idle immediately
    //  3. Idle (pid=0) never appears in the policy queue
    //  4. Multiple CPUs calling PickNext concurrently drain the queue correctly
    //  5. ReadyCount is 0 when only idle processes are running

    static constexpr uint16_t IDLE_PID = 0;
    static constexpr int NCPUS = 4;
    static constexpr int NPROCS = 8;

    // Register real processes (NOT idle — idle is never InitProcess'd).
    for (uint16_t i = 1; i <= NPROCS; i++)
        ops->InitProcess(state, i, 2);
    for (uint16_t i = 1; i <= NPROCS; i++)
        ops->Enqueue(state, i);

    // --- Phase 1: Each CPU picks a process ---
    uint16_t cpuRunning[NCPUS];
    for (int c = 0; c < NCPUS; c++) {
        cpuRunning[c] = ops->PickNext(state);
        ASSERT(cpuRunning[c] != NONE);
        ASSERT(cpuRunning[c] != IDLE_PID);
    }
    // 4 remaining in queue.
    ASSERT(ops->ReadyCount(state) == NPROCS - NCPUS);

    // --- Phase 2: Timeslice tick — each CPU re-enqueues + picks ---
    for (int tick = 0; tick < 20; tick++) {
        for (int c = 0; c < NCPUS; c++) {
            ops->TimesliceExpired(state, cpuRunning[c]);
            ops->Enqueue(state, cpuRunning[c]);
            uint16_t next = ops->PickNext(state);
            ASSERT(next != NONE);
            ASSERT(next != IDLE_PID);
            cpuRunning[c] = next;
        }
    }

    // --- Phase 3: All processes block — every CPU goes idle ---
    for (int c = 0; c < NCPUS; c++)
        ops->Enqueue(state, cpuRunning[c]);
    for (uint16_t i = 1; i <= NPROCS; i++)
        ops->Remove(state, i);
    ASSERT(ops->ReadyCount(state) == 0);

    for (int c = 0; c < NCPUS; c++) {
        ASSERT(ops->PickNext(state) == NONE);
        cpuRunning[c] = IDLE_PID; // all CPUs now idle
    }

    // --- Phase 4: Processes unblock one at a time ---
    // Each unblock should wake exactly one CPU from idle.
    for (uint16_t i = 1; i <= NPROCS && i <= (uint16_t)NCPUS; i++) {
        ops->Enqueue(state, i);
        uint16_t woken = ops->PickNext(state);
        ASSERT(woken == i);
        ASSERT(woken != IDLE_PID);
        cpuRunning[i - 1] = woken;
    }

    // --- Phase 5: All back, verify round-robin resumes ---
    for (int c = 0; c < NCPUS; c++)
        ops->Enqueue(state, cpuRunning[c]);
    for (uint16_t i = NCPUS + 1; i <= NPROCS; i++)
        ops->Enqueue(state, i);
    ASSERT(ops->ReadyCount(state) == NPROCS);

    int counts[NPROCS + 1] = {};
    for (int tick = 0; tick < NPROCS * 10; tick++) {
        uint16_t next = ops->PickNext(state);
        ASSERT(next != NONE);
        ASSERT(next != IDLE_PID);
        counts[next]++;
        ops->Enqueue(state, next);
    }
    for (uint16_t i = 1; i <= NPROCS; i++)
        ASSERT(counts[i] > 0);
    ASSERT(counts[IDLE_PID] == 0);
}

TEST(full_capacity_recycle) {
    for(uint16_t pid=0;pid<SCHED_MAX_PIDS;++pid) {
        ops->InitProcess(state,pid,2);ops->Enqueue(state,pid);
    }
    ASSERT(ops->ReadyCount(state)==SCHED_MAX_PIDS);
    for(uint16_t pid=0;pid<SCHED_MAX_PIDS;pid+=2) {
        ops->Remove(state,pid);
        ops->InitProcess(state,pid,2);ops->Enqueue(state,pid);
    }
    ASSERT(ops->ReadyCount(state)==SCHED_MAX_PIDS);
    bool seen[SCHED_MAX_PIDS]{};
    for(unsigned i=0;i<SCHED_MAX_PIDS;++i) {
        uint16_t pid=ops->PickNext(state);
        ASSERT(pid<SCHED_MAX_PIDS && !seen[pid]);seen[pid]=true;
    }
    ASSERT(ops->ReadyCount(state)==0 && ops->PickNext(state)==NONE);
    ops->Enqueue(state,NONE);ops->Remove(state,NONE);
    ASSERT(ops->ReadyCount(state)==0);
}

TEST(reinitialize_queued_pid_preserves_neighbours) {
    // Re-registration must retire the old queue membership before resetting
    // links, including when the replacement has a different priority.
    for (uint16_t replaced = 1; replaced <= 3; ++replaced) {
        for (uint8_t priority = 0; priority < 4; ++priority) {
            ops->Init(state);
            for (uint16_t pid = 1; pid <= 3; ++pid) {
                ops->InitProcess(state, pid, 2);
                ops->Enqueue(state, pid);
            }
            ops->InitProcess(state, replaced, priority);
            ASSERT(ops->ReadyCount(state) == 2);
            for (uint16_t pid = 1; pid <= 3; ++pid)
                if (pid != replaced) ASSERT(ops->PickNext(state) == pid);
            ASSERT(ops->PickNext(state) == NONE);
            ops->Enqueue(state, replaced);
            ops->Enqueue(state, replaced);
            ASSERT(ops->ReadyCount(state) == 1);
            ASSERT(ops->PickNext(state) == replaced);
            ASSERT(ops->ReadyCount(state) == 0);
            ASSERT(ops->PickNext(state) == NONE);
        }
    }
}

TEST(mlfq_model_interleaved_cpus) {
    constexpr unsigned N=96,CPUS=8;
    std::array<std::deque<uint16_t>,4> ready;
    std::array<unsigned,N> priority{};
    std::array<bool,N> queued{},running{};
    std::array<uint16_t,CPUS> current;current.fill(NONE);
    auto enqueue=[&](uint16_t pid) {
        if(queued[pid])return;
        queued[pid]=true;ready[priority[pid]].push_back(pid);
    };
    auto pick=[&]() -> uint16_t {
        for(auto& q:ready)if(!q.empty()) {
            auto pid=q.front();q.pop_front();queued[pid]=false;return pid;
        }
        return NONE;
    };
    auto remove=[&](uint16_t pid) {
        if(!queued[pid])return;
        auto& q=ready[priority[pid]];
        for(auto it=q.begin();it!=q.end();++it)if(*it==pid){q.erase(it);break;}
        queued[pid]=false;
    };
    for(uint16_t pid=0;pid<N;++pid) {
        priority[pid]=pid%4;ops->InitProcess(state,pid,priority[pid]);
        enqueue(pid);ops->Enqueue(state,pid);
    }
    uint64_t now=0,lastBoost=0;uint32_t random=0x42524f4f;
    for(unsigned step=0;step<100000;++step) {
        random^=random<<13;random^=random>>17;random^=random<<5;
        const unsigned cpu=(random>>8)%CPUS,action=random%5;
        const uint16_t pid=(random>>16)%N,old=current[cpu];
        if(action==0 && old!=NONE) {
            if(priority[old]>0 && priority[old]<3)++priority[old];ops->TimesliceExpired(state,old);
            uint16_t expected=pick(),actual=ops->PickNext(state);ASSERT(actual==expected);
            if(actual!=NONE) {
                running[old]=false;enqueue(old);ops->Enqueue(state,old);
                current[cpu]=actual;ASSERT(!running[actual]);running[actual]=true;
            }
        } else if(action==1 && old!=NONE) {
            ops->Remove(state,old);remove(old);
            if(priority[old]>1)--priority[old];ops->VoluntaryYield(state,old);
            running[old]=false;current[cpu]=NONE;
        } else if(action==2 && !running[pid]) {
            enqueue(pid);ops->Enqueue(state,pid); // includes duplicate wakeups
        } else if(action==3 && !running[pid]) {
            remove(pid);ops->Remove(state,pid);
            priority[pid]=(random>>12)%4;ops->InitProcess(state,pid,priority[pid]);
        } else if(action==4) {
            now+=137;ops->Tick(state,now);
            if(now-lastBoost>=1000) {
                lastBoost=now;
                for(unsigned level=2;level<4;++level)while(!ready[level].empty()) {
                    auto moved=ready[level].front();ready[level].pop_front();
                    priority[moved]=1;ready[1].push_back(moved);
                }
            }
        }
        if(current[cpu]==NONE) {
            uint16_t expected=pick(),actual=ops->PickNext(state);ASSERT(actual==expected);
            current[cpu]=actual;
            if(actual!=NONE){ASSERT(!running[actual]);running[actual]=true;}
        }
        unsigned count=0;for(auto& q:ready)count+=q.size();
        ASSERT(ops->ReadyCount(state)==count);
        constexpr uint64_t slices[]={5,10,20,40};
        for(uint16_t task=0;task<N;++task)ASSERT(ops->Timeslice(state,task)==slices[priority[task]]);
        if(step%127==0) {
            std::vector<unsigned char> copy(ops->stateSize);std::memcpy(copy.data(),state,copy.size());
            for(auto& q:ready)for(auto task:q)ASSERT(ops->PickNext(copy.data())==task);
            ASSERT(ops->PickNext(copy.data())==NONE && ops->ReadyCount(copy.data())==0);
        }
    }
}

// ---------------------------------------------------------------------------
// MLFQ-specific tests (only run for mlfq)
// ---------------------------------------------------------------------------

TEST(mlfq_wakeup_preemption_preserves_budget) {
    ops->InitProcess(state,1,2);ops->InitProcess(state,2,1);
    ops->Enqueue(state,1);ASSERT(ops->PickNext(state)==1);
    // Three partial dispatches must consume the original 20 ms budget, not
    // renew it or demote merely because a higher-priority task woke up.
    for(unsigned part=0;part<2;++part){
        ops->AccountService(state,1,7000000);ops->Enqueue(state,2);
        ASSERT(ops->ShouldPreempt(state,1,0));ops->TimesliceExpired(state,1);
        ASSERT(ops->Timeslice(state,1)==20);
        ASSERT(ops->PickNext(state)==2);ops->VoluntaryYield(state,2);
        ops->Enqueue(state,1);ASSERT(ops->PickNext(state)==1);
    }
    ASSERT(!ops->ShouldPreempt(state,1,100000)); // wall time cannot spend CPU budget
    ops->AccountService(state,1,5999999);ASSERT(!ops->ShouldPreempt(state,1,100000));
    ops->AccountService(state,1,1);ASSERT(ops->ShouldPreempt(state,1,0));
    ops->TimesliceExpired(state,1);ASSERT(ops->Timeslice(state,1)==40);
    ASSERT(!ops->ShouldPreempt(state,1,100000));
    // Equal priority does not trigger an early preemption.
    ops->InitProcess(state,3,3);ops->Enqueue(state,3);
    ASSERT(!ops->ShouldPreempt(state,1,100000));
    ASSERT(ops->DebugDump!=nullptr);SchedDebugInfo debug{};ops->DebugDump(state,1,&debug);
    ASSERT(debug.hasServiceInfo && debug.serviceNs==20000000 && !debug.saturations);
}

TEST(mlfq_many_early_preemptions_and_overrun) {
    ops->InitProcess(state,1,3);ops->InitProcess(state,2,1);
    ops->Enqueue(state,1);ASSERT(ops->PickNext(state)==1);
    for(unsigned n=0;n<39;++n){
        ops->AccountService(state,1,1000000);ops->Enqueue(state,2);
        ASSERT(ops->ShouldPreempt(state,1,0));ops->TimesliceExpired(state,1);
        ASSERT(ops->Timeslice(state,1)==40 && ops->PickNext(state)==2);
        ops->VoluntaryYield(state,2);ops->Enqueue(state,1);ASSERT(ops->PickNext(state)==1);
    }
    ASSERT(!ops->ShouldPreempt(state,1,100000));
    ops->AccountService(state,1,1000000);ASSERT(ops->ShouldPreempt(state,1,0));
    ops->TimesliceExpired(state,1);ASSERT(!ops->ShouldPreempt(state,1,100000));
    // Service beyond the old quantum carries into the next level exactly.
    ops->InitProcess(state,1,2);ops->AccountService(state,1,25000001);
    ASSERT(ops->ShouldPreempt(state,1,0));ops->TimesliceExpired(state,1);
    ASSERT(ops->Timeslice(state,1)==40);
    ops->AccountService(state,1,34999998);ASSERT(!ops->ShouldPreempt(state,1,100000));
    ops->AccountService(state,1,1);ASSERT(ops->ShouldPreempt(state,1,0));
    ops->TimesliceExpired(state,1);ASSERT(!ops->ShouldPreempt(state,1,100000));
}

TEST(mlfq_early_preemption_without_eligible_peer) {
    ops->InitProcess(state,1,2);ops->InitProcess(state,2,1);
    ops->AccountService(state,1,7000000);ops->Enqueue(state,2);
    ASSERT(ops->ShouldPreempt(state,1,0));ops->TimesliceExpired(state,1);
    // Model the core rejecting the peer for CPU ownership/affinity. Policy
    // continuation must preserve both priority and the already consumed budget.
    ASSERT(ops->PickNext(state)==2 && ops->PickNext(state)==NONE);
    ASSERT(ops->Timeslice(state,1)==20 && !ops->ShouldPreempt(state,1,100000));
    ops->AccountService(state,1,12999999);ASSERT(!ops->ShouldPreempt(state,1,0));
    ops->AccountService(state,1,1);ASSERT(ops->ShouldPreempt(state,1,0));
    ops->TimesliceExpired(state,1);ASSERT(ops->Timeslice(state,1)==40);
}

TEST(mlfq_expiry_wins_over_wakeup_and_retains_debt) {
    ops->InitProcess(state,1,2);ops->InitProcess(state,2,1);
    ops->AccountService(state,1,100000001);ops->Enqueue(state,2);
    // A wakeup coincident with expiry must not turn expiry into a free
    // interruption. Consume 20 ms, then two 40 ms quanta at lowest priority.
    ASSERT(ops->ShouldPreempt(state,1,0));ops->TimesliceExpired(state,1);
    ASSERT(ops->Timeslice(state,1)==40 && ops->PickNext(state)==2);
    for(unsigned i=0;i<2;++i){
        ASSERT(ops->ShouldPreempt(state,1,0));ops->TimesliceExpired(state,1);
        ASSERT(ops->Timeslice(state,1)==40);
    }
    ASSERT(!ops->ShouldPreempt(state,1,100000));
    ops->AccountService(state,1,39999998);ASSERT(!ops->ShouldPreempt(state,1,0));
    ops->AccountService(state,1,1);ASSERT(ops->ShouldPreempt(state,1,0));
}

TEST(mlfq_block_charge_reset_and_switch_tail) {
    ops->InitProcess(state,1,2);
    // ReadyQueueRemoveLocked flushes the current owner's interval before
    // SchedulerBlockImpl calls VoluntaryYield. The later dispatch flush can
    // legitimately charge only the remaining switch/bookkeeping tail.
    ops->AccountService(state,1,19000000);ops->Remove(state,1);
    ops->SetRunnable(state,1,false);ops->VoluntaryYield(state,1);
    ops->AccountService(state,1,1000000);
    ASSERT(ops->Timeslice(state,1)==10 && !ops->ShouldPreempt(state,1,100000));
    ops->AccountService(state,1,8999999);ASSERT(!ops->ShouldPreempt(state,1,0));
    ops->AccountService(state,1,1);ASSERT(ops->ShouldPreempt(state,1,0));
    SchedDebugInfo debug{};ops->DebugDump(state,1,&debug);
    ASSERT(debug.serviceNs==29000000);
}

TEST(mlfq_service_identity_and_queue_relink) {
    ops->InitProcess(state,1,2);ops->InitProcess(state,2,2);
    ops->Enqueue(state,1);ops->Enqueue(state,2);
    // Diagnostic/administrative queued notifications must not strand a node
    // under its old priority or change the ready count.
    ops->TimesliceExpired(state,1);ASSERT(ops->ReadyCount(state)==2);
    ASSERT(ops->PickNext(state)==2 && ops->PickNext(state)==1);
    ops->Enqueue(state,1);ops->Enqueue(state,2);ops->VoluntaryYield(state,1);
    ASSERT(ops->ReadyCount(state)==2 && ops->PickNext(state)==2 && ops->PickNext(state)==1);
    ops->AccountService(state,1,~uint64_t(0));ops->AccountService(state,1,1);
    SchedDebugInfo debug{};ops->DebugDump(state,1,&debug);
    ASSERT(debug.serviceNs==~uint64_t(0) && debug.saturations);
    ops->InitProcess(state,1,2);ASSERT(!ops->ShouldPreempt(state,1,0));
    ops->Init(state);ops->AccountService(state,900,123);
    ops->DebugDump(state,900,&debug);ASSERT(!debug.active && debug.inactiveServiceNs==123 && debug.serviceNs==123);
}

TEST(mlfq_service_smp_periodic_model) {
    constexpr unsigned CPUS=8,CPU_TASKS=32,PERIODIC=4,N=CPU_TASKS+PERIODIC;
    std::array<uint16_t,CPUS> current{};
    std::array<bool,N+1> queued{},running{};
    std::array<uint64_t,N+1> service{},wakeAt{},readyAt{},wakeCount{};
    uint64_t maximumWakeDelay=0;
    for(unsigned p=1;p<=N;++p){ops->InitProcess(state,p,p<=CPU_TASKS?2:0);wakeAt[p]=10;}
    for(unsigned p=1;p<=CPU_TASKS;++p){ops->Enqueue(state,p);queued[p]=true;}
    for(unsigned c=0;c<CPUS;++c){auto p=ops->PickNext(state);ASSERT(p!=NONE && queued[p] && !running[p]);queued[p]=false;running[p]=true;current[c]=p;}
    for(uint64_t now=1;now<=10000;++now){
        // All service belongs to the previous interval's CPU owner; policy
        // calls serialize, but no PID may execute on two model CPUs at once.
        for(unsigned c=0;c<CPUS;++c){auto p=current[c];ASSERT(p && running[p]);ops->AccountService(state,p,1000000);++service[p];
            if(p>CPU_TASKS){ops->VoluntaryYield(state,p);running[p]=false;current[c]=0;wakeAt[p]=now+10;}
        }
        for(unsigned p=CPU_TASKS+1;p<=N;++p)if(!queued[p] && !running[p] && now>=wakeAt[p]){
            ops->Enqueue(state,p);queued[p]=true;readyAt[p]=now;
        }
        ops->Tick(state,now);
        for(unsigned c=0;c<CPUS;++c){auto old=current[c];
            if(old && !ops->ShouldPreempt(state,old,0))continue;
            if(old)ops->TimesliceExpired(state,old);
            auto p=ops->PickNext(state);if(p==NONE)continue;
            ASSERT(p<=N && queued[p] && !running[p]);queued[p]=false;
            if(old){ASSERT(!queued[old] && running[old]);running[old]=false;ops->Enqueue(state,old);queued[old]=true;}
            running[p]=true;current[c]=p;
            if(p>CPU_TASKS){auto delay=now-readyAt[p];if(delay>maximumWakeDelay)maximumWakeDelay=delay;++wakeCount[p];}
        }
        unsigned readyCount=0;for(unsigned p=1;p<=N;++p){ASSERT(!(running[p] && queued[p]));readyCount+=queued[p];}
        ASSERT(ops->ReadyCount(state)==readyCount);
    }
    uint64_t minimum=~uint64_t(0),maximum=0;
    for(unsigned p=1;p<=CPU_TASKS;++p){if(service[p]<minimum)minimum=service[p];if(service[p]>maximum)maximum=service[p];}
    ASSERT(minimum>1000 && maximum*100<minimum*120 && maximumWakeDelay<=1);
    for(unsigned p=CPU_TASKS+1;p<=N;++p)ASSERT(wakeCount[p]>800);
    SchedDebugInfo debug{};ops->DebugDump(state,1,&debug);
    ASSERT(debug.serviceNs==uint64_t(CPUS)*10000*1000000 && !debug.saturations);
}

TEST(mlfq_priority_ordering) {
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
    ASSERT(ops->PickNext(state) == 1); // REALTIME first
    ASSERT(ops->PickNext(state) == 3); // HIGH second
    ASSERT(ops->PickNext(state) == 2); // NORMAL third
    ASSERT(ops->PickNext(state) == 0); // LOW last
}

TEST(mlfq_demotion) {
    ops->Init(state);
    // Process at NORMAL, expire timeslice → should demote to LOW.
    ops->InitProcess(state, 0, 2); // NORMAL
    ops->InitProcess(state, 1, 2); // NORMAL
    ops->TimesliceExpired(state, 0); // demote 0 → LOW
    ops->Enqueue(state, 0);
    ops->Enqueue(state, 1);
    ASSERT(ops->PickNext(state) == 1); // 1 is NORMAL, picked first
    ASSERT(ops->PickNext(state) == 0); // 0 is LOW, picked second
}

TEST(mlfq_realtime_budget_preserves_band_and_rotates_peers) {
    ops->InitProcess(state,1,0);ops->InitProcess(state,2,0);
    ops->InitProcess(state,3,1);ops->InitProcess(state,4,2);
    for (uint16_t pid=1;pid<=4;++pid) ops->Enqueue(state,pid);
    uint16_t current=ops->PickNext(state);ASSERT(current==1);
    for (unsigned turn=0;turn<100;++turn) {
        ops->AccountService(state,current,5000000);
        ASSERT(ops->ShouldPreempt(state,current,0));
        ops->TimesliceExpired(state,current);
        ASSERT(ops->Timeslice(state,current)==5); // reserved band survives expiry
        const auto next=ops->PickNext(state);
        ASSERT(next==(current==1?2:1)); // peers still rotate at the quantum
        ops->Enqueue(state,current);current=next;
    }
    // Blocking is not an admission to the ordinary interactive band either.
    ops->VoluntaryYield(state,current);ASSERT(ops->Timeslice(state,current)==5);
    const auto other=ops->PickNext(state);ASSERT(other==(current==1?2:1));
    ops->VoluntaryYield(state,other);ASSERT(ops->Timeslice(state,other)==5);
    ASSERT(ops->PickNext(state)==3);ASSERT(ops->PickNext(state)==4);
    // Expiry keeps excess consumed service, including for a reserved task.
    ops->AccountService(state,current,12000000);
    ASSERT(ops->ShouldPreempt(state,current,0));ops->TimesliceExpired(state,current);
    ASSERT(ops->ShouldPreempt(state,current,0));ops->TimesliceExpired(state,current);
    ASSERT(!ops->ShouldPreempt(state,current,100000));
    ASSERT(ops->Timeslice(state,current)==5);
}

TEST(mlfq_boost) {
    ops->Init(state);
    // Process demoted to LOW, then boost should bring it back to HIGH.
    ops->InitProcess(state, 0, 3); // LOW
    ops->Enqueue(state, 0);
    ops->Tick(state, 0);    // initial time
    ops->Tick(state, 1001); // past boost interval
    // Process should now be at HIGH priority
    [[maybe_unused]] uint64_t ts = ops->Timeslice(state, 0);
    ASSERT(ts == 10); // HIGH timeslice
}

TEST(mlfq_timeslice_by_priority) {
    ops->Init(state);
    ops->InitProcess(state, 0, 0); // REALTIME
    ops->InitProcess(state, 1, 1); // HIGH
    ops->InitProcess(state, 2, 2); // NORMAL
    ops->InitProcess(state, 3, 3); // LOW
    ASSERT(ops->Timeslice(state, 0) == 5);
    ASSERT(ops->Timeslice(state, 1) == 10);
    ASSERT(ops->Timeslice(state, 2) == 20);
    ASSERT(ops->Timeslice(state, 3) == 40);
}

TEST(mlfq_voluntary_yield_boosts) {
    ops->Init(state);
    ops->InitProcess(state, 0, 3); // LOW
    ops->VoluntaryYield(state, 0); // boost → NORMAL
    ASSERT(ops->Timeslice(state, 0) == 20);
    ops->VoluntaryYield(state, 0); // boost → HIGH
    ASSERT(ops->Timeslice(state, 0) == 10);
    ops->VoluntaryYield(state, 0); // already HIGH, should stay
    ASSERT(ops->Timeslice(state, 0) == 10);
}

TEST(mlfq_priority_fairness) {
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
        ASSERT(pid != NONE);
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
    ASSERT(highMin > lowMax);

    // Within each band, fairness: max/min ratio < 2.
    ASSERT(highMax <= highMin * 2);
    ASSERT(lowMax <= lowMin * 2);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

TEST(policy_registration_contract) {
    (void)state;
    ASSERT(SchedOpsUsable(ops));
    ASSERT(!SchedOpsUsable(nullptr));
    SchedOps copy = *ops;
    copy.DebugDump = nullptr; // explicitly optional
    ASSERT(SchedOpsUsable(&copy));
    copy.stateSize = SCHED_POLICY_STATE_CAPACITY + 1;
    ASSERT(!SchedOpsUsable(&copy));
    copy = *ops; copy.stateSize = 0;
    ASSERT(!SchedOpsUsable(&copy));
    copy = *ops; copy.name = nullptr;
    ASSERT(!SchedOpsUsable(&copy));
    copy = *ops; copy.name = "";
    ASSERT(!SchedOpsUsable(&copy));
#define REJECT_MISSING(member) do { copy = *ops; copy.member = nullptr; \
    ASSERT(!SchedOpsUsable(&copy)); } while (0)
    REJECT_MISSING(Init); REJECT_MISSING(InitProcess);
    REJECT_MISSING(Enqueue); REJECT_MISSING(PickNext); REJECT_MISSING(Remove);
    REJECT_MISSING(TimesliceExpired); REJECT_MISSING(VoluntaryYield);
    REJECT_MISSING(Tick); REJECT_MISSING(Timeslice); REJECT_MISSING(ReadyCount);
    REJECT_MISSING(AccountService); REJECT_MISSING(SetRunnable); REJECT_MISSING(ShouldPreempt);
    copy=*ops;copy.abiVersion=0;ASSERT(!SchedOpsUsable(&copy));
    copy=*ops;copy.opsSize=0;ASSERT(!SchedOpsUsable(&copy));
#undef REJECT_MISSING
}

TEST(policy_restore_ownership_and_priority) {
    ASSERT(!SchedRestoreProcess(ops, state, 0, 0, false, true, -1));
    ASSERT(!SchedRestoreProcess(ops, state, SCHED_PID_NONE, 0, false, true, -1));
    // Blocked tasks retain a policy entry, ready tasks still on a CPU do not
    // become dispatchable, and terminated tasks are not resurrected.
    ASSERT(!SchedRestoreProcess(ops, state, 1, 0, false, false, -1));
    ASSERT(!SchedRestoreProcess(ops, state, 2, 1, false, true, 3));
    ASSERT(SchedRestoreProcess(ops, state, 3, 3, false, true, -1));
    ASSERT(!SchedRestoreProcess(ops, state, 4, 0, true, true, -1));
    ASSERT(!SchedRestoreProcess(ops, state, 5, 2, false, false, 2));
    ASSERT(ops->ReadyCount(state) == 1);
    ASSERT(ops->PickNext(state) == 3);
    ASSERT(ops->PickNext(state) == NONE);
    for (uint16_t pid = 1; pid <= 3; ++pid) {
        const uint8_t priorities[] = {0, 0, 1, 3};
        ops->InitProcess(state, pid + 10, priorities[pid]);
        ASSERT(ops->Timeslice(state, pid) == ops->Timeslice(state, pid + 10));
    }
    // Normal unblock and post-switch publication must both work after restore.
    ops->Enqueue(state, 1);
    ASSERT(ops->PickNext(state) == 1);
    ops->Enqueue(state, 2);
    ASSERT(ops->PickNext(state) == 2);
    ASSERT(ops->PickNext(state) == NONE);
}

static void RunCommonTests(const SchedOps* ops, void* state) {
    invoke_policy_registration_contract(ops,state);
    invoke_policy_restore_ownership_and_priority(ops,state);
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
    invoke_idle_transition_all_blocked(ops, state);
    invoke_idle_transition_processes_exit(ops, state);
    invoke_idle_wake_after_all_exit_new_process(ops, state);
    invoke_idle_repeated_block_unblock_cycle(ops, state);
    invoke_idle_simulation(ops, state);
    invoke_full_capacity_recycle(ops,state);
    invoke_reinitialize_queued_pid_preserves_neighbours(ops,state);
}

TEST(mlfq_decision_observations){
    observedMlfq={};ops->InitProcess(state,1,2);ops->AccountService(state,1,1);
    ASSERT(!ops->ShouldPreempt(state,1,99999) && !observedMlfq.calls);
    ops->InitProcess(state,2,1);ops->Enqueue(state,2);
    ASSERT(ops->ShouldPreempt(state,1,0));
    ASSERT(observedMlfq.calls==1 && observedMlfq.pid==1 && observedMlfq.priority==2);
    ASSERT(observedMlfq.usedNs==1 && observedMlfq.quantumNs==20000000 && observedMlfq.serviceSeen);
    ASSERT(observedMlfq.reason==uint8_t(MlfqDecisionReason::HigherPriority) && observedMlfq.higher==1);
    ops->TimesliceExpired(state,1);ASSERT(ops->PickNext(state)==2);ASSERT(!ops->ShouldPreempt(state,1,0));
    ops->Enqueue(state,2);ops->AccountService(state,1,20000000);ASSERT(ops->ShouldPreempt(state,1,0));
    ASSERT(observedMlfq.reason==uint8_t(MlfqDecisionReason::Service) && observedMlfq.usedNs==20000001 && observedMlfq.higher==255);
    ops->TimesliceExpired(state,1);ASSERT(ops->Timeslice(state,1)==40);
    ops->InitProcess(state,3,0);ASSERT(ops->ShouldPreempt(state,3,5));
    ASSERT(observedMlfq.reason==uint8_t(MlfqDecisionReason::LegacyWall) && !observedMlfq.serviceSeen && observedMlfq.quantumNs==5000000);
}

static void RunMlfqTests(const SchedOps* ops, void* state) {
    invoke_mlfq_decision_observations(ops,state);
    invoke_mlfq_priority_ordering(ops,state);
    invoke_mlfq_demotion(ops,state);
    invoke_mlfq_realtime_budget_preserves_band_and_rotates_peers(ops,state);
    invoke_mlfq_boost(ops,state);
    invoke_mlfq_timeslice_by_priority(ops,state);
    invoke_mlfq_voluntary_yield_boosts(ops,state);
    invoke_mlfq_priority_fairness(ops,state);
    invoke_mlfq_model_interleaved_cpus(ops,state);
    invoke_mlfq_wakeup_preemption_preserves_budget(ops,state);
    invoke_mlfq_many_early_preemptions_and_overrun(ops,state);
    invoke_mlfq_early_preemption_without_eligible_peer(ops,state);
    invoke_mlfq_expiry_wins_over_wakeup_and_retains_debt(ops,state);
    invoke_mlfq_block_charge_reset_and_switch_tail(ops,state);
    invoke_mlfq_service_identity_and_queue_relink(ops,state);
    invoke_mlfq_service_smp_periodic_model(ops,state);
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
