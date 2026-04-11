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
static constexpr uint32_t SCHED_MAX_PIDS = 64;

// Sentinel value: no process (returned by PickNext when queue is empty).
static constexpr uint16_t SCHED_PID_NONE = 0xFFFF;

// Scheduler policy vtable.
//
// All functions receive an opaque `state` pointer (allocated by the caller,
// sized by `stateSize`). Process handles are opaque `void*` — the module
// never dereferences them; it uses only the `pid` for internal bookkeeping.
//
// Thread safety: the caller (scheduler.cpp) holds a lock around all calls.
// Modules do NOT need internal locking.
struct SchedOps {
    const char* name;        // Human-readable name, e.g. "rr", "mlfq"
    size_t      stateSize;   // Bytes needed for policy state

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
};

} // namespace brook

// Module entry point — each scheduler .mod / .so exports this symbol.
extern "C" const brook::SchedOps* GetSchedOps();
