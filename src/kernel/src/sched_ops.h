#pragma once

// sched_ops.h — Scheduler policy ABI
//
// Defines the vtable interface between the kernel's scheduling mechanism
// (context switch, interrupts, per-CPU state) and a pluggable scheduling
// policy module (queue management, priority, timeslice calculation).
//
// This header has NO kernel dependencies — it is shared by:
//   - The kernel (scheduler.cpp)
//   - Scheduler modules (sched_rr.cpp, sched_mlfq.cpp)
//   - Host-side tests (dlopen the .so, call through the vtable)
//
// Each scheduler module exports:
//   extern "C" const brook::SchedOps* GetSchedOps();

#include <stdint.h>
#include <stddef.h>

namespace brook {

// Maximum PIDs the policy must support (must match kernel MAX_PROCESSES).
static constexpr uint32_t SCHED_MAX_PIDS = 1024;

// Shared kernel/module storage contract. Policy fixtures must fit the same
// storage used by the kernel, rather than allocating unbounded host memory.
static constexpr size_t SCHED_POLICY_STATE_CAPACITY = 65536;

// Sentinel value: no process (returned by PickNext when queue is empty).
static constexpr uint16_t SCHED_PID_NONE = 0xFFFF;

// BRO-176 HANG diagnostic snapshot of a policy's internal queue view for one pid.
// All fields are best-effort; a policy that can't supply one sets it to 0/NONE.
struct SchedDebugInfo {
    uint16_t queued;      // 1 if the pid is flagged in-queue, else 0
    uint16_t active;      // 1 if the pid has been InitProcess'd
    uint16_t head;        // current queue head pid (NONE if empty)
    uint16_t tail;        // current queue tail pid (NONE if empty)
    uint16_t nextPid;     // pid's forward link
    uint16_t prevPid;     // pid's backward link
    uint32_t readyCount;  // policy's readyCount
    uint32_t listLen;     // ACTUAL length of the queue by walking head->next
    bool hasServiceInfo = false;
    uint64_t serviceNs = 0, inactiveServiceNs = 0, rebases = 0, saturations = 0;
    uint64_t deferredRemovals = 0, workConservingSelections = 0;
};

// Scheduler policy vtable.
//
// All functions receive an opaque `state` pointer (allocated by the caller,
// sized by `stateSize`). Process handles are opaque `void*` — the module
// never dereferences them; it uses only the `pid` for internal bookkeeping.
//
// Thread safety: the caller (scheduler.cpp) holds a lock around all calls.
// Modules do NOT need internal locking.
static constexpr uint32_t SCHED_POLICY_ABI_VERSION = 2;

struct SchedOps {
    const char* name;        // Human-readable name, e.g. "rr", "mlfq"
    size_t      stateSize;   // Bytes needed for policy state
    uint32_t    abiVersion;  // Reject incompatible policy layouts before callbacks.
    uint32_t    opsSize;

    // Initialise policy state (zeroed memory of `stateSize` bytes).
    void (*Init)(void* state);

    // Register a new process with the policy. Called once per process.
    // `pid` is unique and < SCHED_MAX_PIDS.
    // `priority` is a hint (0=highest, 3=lowest); RR may ignore it.
    void (*InitProcess)(void* state, uint16_t pid, uint8_t priority);

    // Add a process to the ready queue.
    void (*Enqueue)(void* state, uint16_t pid);

    // Pick the highest-priority ready process. Returns pid, or SCHED_PID_NONE if empty.
    uint16_t (*PickNext)(void* state);

    // Remove a specific process from the ready queue (e.g. when blocking).
    // No-op if the process is not queued.
    void (*Remove)(void* state, uint16_t pid);

    // Notify that a process exhausted its timeslice (may demote priority).
    void (*TimesliceExpired)(void* state, uint16_t pid);

    // Notify that a process yielded voluntarily / blocked on I/O (may boost).
    void (*VoluntaryYield)(void* state, uint16_t pid);

    // Periodic tick — called once per timer interrupt.
    // `nowMs` is the current wall time in milliseconds.
    // Used for anti-starvation boosts, aging, etc.
    void (*Tick)(void* state, uint64_t nowMs);

    // Return the timeslice (in ms) for a process.
    uint64_t (*Timeslice)(void* state, uint16_t pid);

    // Return the number of processes currently in the ready queue.
    uint32_t (*ReadyCount)(void* state);

    // BRO-176 HANG diagnostic (optional; may be null): fill `out` with the
    // policy's internal view of `pid` and the queue, so the kernel can tell
    // whether a Ready-but-undispatched process is (a) flagged queued while
    // unlinked (logic desync) or (b) its state struct was corrupted by a wild
    // write (head/tail/count inconsistent with the per-pid links). Pure reads.
    void (*DebugDump)(void* state, uint16_t pid, SchedDebugInfo* out);
    // CPU service in nanoseconds, charged exactly once for each observed running
    // interval, including partial slices and CPU-clock-query flushes. Kernel/IRQ
    // ownership time is included; reported host steal is subtracted when present.
    void (*AccountService)(void* state, uint16_t pid, uint64_t serviceNs);

    // A running entry participates in fair-service eligibility even while it is
    // absent from the dispatchable queue. False also covers block/stop/exit.
    void (*SetRunnable)(void* state, uint16_t pid, bool runnable);

    // Called under the ready lock only at a preemptible user-mode timer boundary.
    // RR/MLFQ retain wall-slice semantics; service policies can retain unfinished
    // requests across early preemption. May update policy request state.
    bool (*ShouldPreempt)(void* state, uint16_t currentPid, uint64_t elapsedWallMs);

};

inline bool SchedOpsUsable(const SchedOps* ops) {
    return ops && ops->abiVersion == SCHED_POLICY_ABI_VERSION && ops->opsSize == sizeof(SchedOps) &&
           ops->name && ops->name[0] && ops->stateSize &&
           ops->stateSize <= SCHED_POLICY_STATE_CAPACITY &&
           ops->Init && ops->InitProcess && ops->Enqueue && ops->PickNext &&
           ops->Remove && ops->TimesliceExpired && ops->VoluntaryYield &&
           ops->Tick && ops->Timeslice && ops->ReadyCount &&
           ops->AccountService && ops->SetRunnable && ops->ShouldPreempt;
}

} // namespace brook

// Module entry point — each scheduler .mod / .so exports this symbol.
extern "C" const brook::SchedOps* GetSchedOps();
