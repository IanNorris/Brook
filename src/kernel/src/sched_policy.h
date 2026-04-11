#pragma once

// SchedPolicy — testable scheduling policy layer
//
// This module implements the scheduling POLICY (which process to pick,
// priority calculations, queue management) separate from the MECHANISM
// (context switches, interrupts, per-CPU state).
//
// The policy is testable on the host without booting a kernel.
//
// Current policy: Multi-level feedback queue (MLFQ)
//   - 4 priority levels (0=highest, 3=lowest)
//   - New processes start at priority 1 (normal)
//   - Processes that yield voluntarily (I/O) get boosted
//   - Processes that exhaust their timeslice get demoted
//   - Periodic boost prevents starvation (all→level 1)
//   - Per-priority FIFO queues

#include <stdint.h>

namespace brook {

// Priority levels
static constexpr uint8_t SCHED_PRIORITY_REALTIME = 0; // Reserved for kernel tasks
static constexpr uint8_t SCHED_PRIORITY_HIGH     = 1; // I/O-bound / interactive
static constexpr uint8_t SCHED_PRIORITY_NORMAL   = 2; // Default for new processes
static constexpr uint8_t SCHED_PRIORITY_LOW      = 3; // CPU-bound / background
static constexpr uint8_t SCHED_NUM_PRIORITIES    = 4;

// Timeslice per priority (in ms). Lower priority = longer slice.
static constexpr uint64_t SCHED_TIMESLICE[SCHED_NUM_PRIORITIES] = {
    5,   // Realtime: short slices, high responsiveness
    10,  // High: interactive
    20,  // Normal: balanced
    40,  // Low: CPU-bound, less preemption
};

// Boost interval: every N ms, boost all processes to prevent starvation.
static constexpr uint64_t SCHED_BOOST_INTERVAL_MS = 1000;

// Process scheduling metadata (embedded in each process).
struct SchedInfo {
    uint8_t  priority;        // Current priority level (0-3)
    uint8_t  basePriority;    // Base priority (set at creation)
    uint64_t cpuTimeMs;       // Total CPU time consumed
    uint64_t lastScheduledMs; // Tick when last scheduled
    uint32_t yieldCount;      // Voluntary yields (indicates I/O-bound)
    uint32_t preemptCount;    // Forced preemptions (indicates CPU-bound)
};

// Opaque handle to a scheduled process (process-agnostic for testing).
struct SchedProcess {
    uint16_t   pid;
    SchedInfo  sched;
    SchedProcess* next; // Queue linkage
    SchedProcess* prev; // Queue linkage (doubly-linked for O(1) remove)
};

// Per-priority FIFO queue.
struct SchedQueue {
    SchedProcess* head;
    SchedProcess* tail;
    uint32_t      count;
};

// The scheduler policy state.
struct SchedPolicyState {
    SchedQueue  queues[SCHED_NUM_PRIORITIES];
    uint64_t    lastBoostMs;
    uint32_t    totalReady;
};

// ---------------------------------------------------------------------------
// Policy API (all are pure functions on the state — no side effects)
// ---------------------------------------------------------------------------

// Initialise policy state.
void SchedPolicyInit(SchedPolicyState* state);

// Initialise a process's scheduling info.
void SchedPolicyInitProcess(SchedProcess* proc, uint8_t basePriority);

// Insert a process into the appropriate priority queue.
void SchedPolicyEnqueue(SchedPolicyState* state, SchedProcess* proc);

// Remove a specific process from its queue (e.g., when blocking).
void SchedPolicyRemove(SchedPolicyState* state, SchedProcess* proc);

// Pick the highest-priority ready process. Returns nullptr if all queues empty.
SchedProcess* SchedPolicyPickNext(SchedPolicyState* state);

// Notify that a process exhausted its timeslice (demote priority).
void SchedPolicyTimesliceExpired(SchedProcess* proc);

// Notify that a process yielded voluntarily (boost priority).
void SchedPolicyVoluntaryYield(SchedProcess* proc);

// Periodic anti-starvation boost. Call once per SCHED_BOOST_INTERVAL_MS.
// Moves all processes to priority SCHED_PRIORITY_HIGH.
void SchedPolicyBoostAll(SchedPolicyState* state, uint64_t nowMs);

// Get the timeslice (in ms) for a process based on its priority.
uint64_t SchedPolicyTimeslice(const SchedProcess* proc);

// Return the number of ready processes.
uint32_t SchedPolicyReadyCount(const SchedPolicyState* state);

} // namespace brook
