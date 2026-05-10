# Scheduler Documentation

## Overview

Brook's scheduler is preemptive with pluggable policy and SMP support. The
mechanism (context switch, per-CPU state, lock management) lives in
`scheduler.cpp`. The policy (queue management, priority, timeslice) is
abstracted behind the `SchedOps` vtable, with a built-in MLFQ implementation
in `sched_policy.cpp` and a loadable round-robin module in `sched_mlfq`.

## Files

| File | Lines | Purpose |
|------|-------|---------|
| `scheduler.h` | ~125 | Public scheduler API declarations |
| `scheduler.cpp` | ~1725 | Scheduler mechanism: context switch, per-CPU, locks |
| `sched_ops.h` | ~80 | SchedOps vtable interface (policy ABI) |
| `sched_policy.h` | ~108 | MLFQ policy declarations + SchedInfo/SchedProcess structs |
| `sched_policy.cpp` | ~147 | MLFQ policy implementation |

---

## Architecture

### Mechanism / Policy Separation

The kernel's scheduler mechanism handles:
- Per-CPU state tracking (current process, idle process, slice timing)
- Interrupt-safe locking (`SchedLock` — ticket lock with CLI/STI)
- Context switch orchestration (save/restore GPRs, FPU, CR3, FS base)
- Timed wakeups and blocked process management
- Process reaping (deferred until kernel stack is unused)

Policy modules implement the `SchedOps` vtable:
- `Init`, `InitProcess`, `Enqueue`, `Remove`, `PickNext`
- `TimesliceExpired`, `VoluntaryYield`, `Tick`, `Timeslice`, `ReadyCount`

Policies are registered via `SchedulerRegisterPolicy()` and can be switched
at runtime via `SchedulerSwitchPolicy()`.

### Per-CPU State

```
PerCpuSchedState {
    currentProcess    — Process running on this CPU
    idleProcess       — Per-CPU idle process (pid=0, halts)
    sliceStartTick    — When current timeslice started
    cpuEnv            — KernelCpuEnv for syscall stack updates
    pendingRequeue    — Set before context_switch, consumed after
    pendingRetire     — Terminated process to mark reapable after switch
}
```

Max CPUs: 64 (SCHED_MAX_CPUS).

### MLFQ Policy (Built-in)

4 priority levels:
| Level | Name | Timeslice |
|-------|------|-----------|
| 0 | Realtime | 5ms |
| 1 | High | 10ms |
| 2 | Normal (default) | 20ms |
| 3 | Low | 40ms |

- New processes start at Normal (2)
- Exhausting timeslice → demote (never demotes Realtime)
- Voluntary yield/block → boost one level
- Anti-starvation boost every 1000ms → all to High (1)
- Per-level FIFO queues (doubly-linked for O(1) remove)

---

## Key Functions

### Lifecycle

| Function | Description |
|----------|-------------|
| `SchedulerInit()` | Initialize policy, create BSP idle process |
| `SchedulerInitApIdle(cpu)` | Create idle process for AP |
| `SchedulerStart()` | Pick first process, enter user mode (BSP) |
| `SchedulerStartAp()` | Wait for BSP, pick process, enter user mode (AP) |
| `SchedulerAddProcess(proc)` | Register with policy + pid lookup, enqueue |
| `SchedulerRemoveProcess(proc)` | Dequeue + remove from all-process list, free PID |

### Scheduling

| Function | Description |
|----------|-------------|
| `SchedulerTimerTick(allowPreempt)` | Timer ISR handler. BSP: wakeups, alarms, reap, policy tick. All CPUs: idle dispatch, timeslice preemption (user-mode only) |
| `SchedulerYield()` | Voluntary yield — pick next, switch |
| `SchedulerBlock(proc)` | Mark Blocked, remove from queue, yield. Checks `pendingWakeup` to prevent lost wakeups |
| `SchedulerUnblock(proc)` | Mark Ready, insert into queue. Handles races with mid-context-switch processes |
| `SchedulerStop(proc)` | Like Block but sets Stopped (for SIGTSTP) |
| `SchedulerSleepMs(ms)` | Set wakeup tick, block. Falls back to busy-spin pre-scheduler |
| `SchedulerExitCurrentProcess(status)` | Terminate, SIGCHLD to parent, reparent children, switch away. `[[noreturn]]` |
| `SchedulerKillThreadGroup(tgid, caller, status)` | Mark all threads in group as Terminated |

### Context Switch

| Function | Description |
|----------|-------------|
| `DoSwitch(old, new, requeueOld)` | Core switch: validate pointers, double-schedule detection, update per-CPU state, call `context_switch()` asm, drain post-switch |
| `DrainPostSwitch(cpu)` | Mark retired process reapable, re-enqueue old process. Called from all trampolines |
| `ProcessTrampoline()` | First entry to user mode (new process) |
| `ForkChildTrampoline()` | Fork child: restore all registers, RAX=0, IRETQ |
| `KernelThreadTrampoline()` | Ring-0 thread: read fn/arg from stack, call fn |

### Process Lookup

| Function | Description |
|----------|-------------|
| `ProcessCurrent()` | Fast path via `gs:184` (migration-safe). Fallback to per-CPU array |
| `ProcessFindByPid(pid)` | Linear scan of g_allProcesses under lock |
| `SchedulerFindTerminatedChild(parent, pid)` | For wait4 |
| `SchedulerFindStoppedChild(parent, pid)` | For WUNTRACED |
| `SchedulerFindProcessByBaseName(name)` | For shell `run-once` |

### PID Management

| Function | Description |
|----------|-------------|
| `SchedulerAllocPid()` | Pop from free stack or bump g_nextPid. Panics if exhausted |
| `SchedulerFreePid(pid)` | Push onto free stack |

### Policy Management

| Function | Description |
|----------|-------------|
| `SchedulerRegisterPolicy(ops)` | Add to registry (max 8). Deduplicates by name |
| `SchedulerSwitchPolicy(name)` | Switch under lock: re-init, migrate all processes |
| `SchedulerPolicyName()` | Return current policy name |

---

## Locking Hierarchy

1. **g_pidLock** — PID allocation/freeing (leaf lock, very brief)
2. **g_readyLock** — Protects all policy module calls and ready queue
3. **g_allProcLock** — Protects g_allProcesses[] array

All three are interrupt-safe ticket locks (CLI on acquire, restore IF on release).

**Lock ordering:** Code that needs both `g_readyLock` and `g_allProcLock` must
acquire `g_readyLock` first (see `SchedulerSwitchPolicy`). However, most paths
acquire them independently. `ReapTerminated` drops `g_allProcLock` before calling
`ProcessDestroy` (which calls `SchedulerRemoveProcess` → `g_readyLock`), then
re-acquires `g_allProcLock`.

---

## Known Issues

### 1. Idle process naming for CPU > 9

`SchedulerInitApIdle` generates idle process names as `idle0` through `idle9`
using `name[4] = '0' + (cpuIndex % 10)`. For CPUs 10-63, the name wraps:
CPU 10 → `idle0` (same as CPU 0), CPU 11 → `idle1`, etc. This makes debugging
harder on > 10 CPU systems. Not a correctness bug.

### 2. SchedPolicyBoostAll doesn't update totalReady

When `SchedPolicyBoostAll` moves processes between queues (via `QueuePopFront`
+ `QueuePushBack`), the individual queue `count` fields change but
`state->totalReady` is correct because `QueuePopFront` decrements and
`QueuePushBack` increments `count` per-queue while `totalReady` isn't touched.
Actually, `PickNext` decrements `totalReady` when popping, but `BoostAll`
uses the raw `QueuePopFront` which decrements queue count but not `totalReady`.
Wait — `QueuePopFront` only modifies `q->count`, not `state->totalReady`.
And `BoostAll` calls `QueuePopFront` then `QueuePushBack`. Since `totalReady`
is only modified by `Enqueue` (+1), `Remove` (-1), and `PickNext` (-1),
`BoostAll` correctly doesn't touch it. The per-queue counts stay consistent.
**No bug here.**

### 3. g_schedStateStorage is fixed 4KB

The policy state storage is a static 4KB buffer. If a policy module requires
more (e.g., a complex scheduler with per-process extended metadata), init
panics. Adequate for current MLFQ (4 queues × 3 pointers + counters ≈ ~200
bytes) but could be a limitation for future policies.

### 4. SchedulerKillThreadGroup state write race

`SchedulerKillThreadGroup` sets `p->state = Terminated` and `p->exitStatus`
without holding any lock, while the thread may be running on another CPU.
The scheduler timer tick will eventually see the terminated state, but there's
a window where the thread runs user code while marked terminated. The
`reapable` flag is only set if `runningOnCpu < 0`, which prevents premature
reaping, but the state transition itself is racy.

### 5. ReapTerminated drop-and-reacquire pattern

`ReapTerminated` drops `g_allProcLock` to call `ProcessDestroy()`, then
reacquires it and restarts the scan from index 0. This is correct but
potentially quadratic: each reap restarts the full scan. With MAX_PROCESSES=256
this is bounded and fast enough.

### 6. CPU affinity pinning prevents load balancing

`PinUserAddressSpaceToCpu` uses CAS to pin a process to the first CPU that
runs it. Once pinned, the process can never migrate. This is intentional
(no SMP TLB shootdown) but means workloads can become unbalanced if many
processes are pinned to the same CPU.

### 7. No SMP TLB shootdown

The fundamental constraint behind CPU pinning, eager fork copies, and several
other design decisions. Documented in plan.md and throughout the codebase.
Future work.

---

## Context Switch Flow

1. `DoSwitch(old, new, requeueOld)` validates both process pointers
2. CAS `newProc->runningOnCpu` from -1 to current CPU (double-schedule check)
3. Update per-CPU state: `currentProcess`, `cpuEnv`, TSS RSP0, syscall stack
4. Pin new process to CPU if not already pinned
5. Set `pendingRequeue` if old should be re-enqueued
6. Call `context_switch()` assembly (saves GPRs, FPU/SSE, CR3, FS base; publishes `runningOnCpu = -1` for old process)
7. **Return point**: `DrainPostSwitch()` — marks retired process reapable, re-enqueues old process

## Timer Tick Flow (SchedulerTimerTick)

1. Guard: return if scheduler not running
2. BSP only: `CheckBlockedWakeups()`, alarm timers, `ReapTerminated()`, policy `Tick()`
3. All CPUs: charge tick to current process
4. Idle CPU: try to pick a ready process
5. Non-preemptible (kernel mode): return early
6. Check timeslice: if not expired, return
7. Stopped process: remove from queue, switch to next or idle
8. Running process with expired timeslice: `TimesliceExpired()`, pick next, `DoSwitch()`
