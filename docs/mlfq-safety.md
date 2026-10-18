# MLFQ safety audit — 2026-10-18

MLFQ is not yet accepted for a live policy switch or as the default. The native
browser review remained on RR; loading `sched_mlfq.mod` only registered it.
The subsequent STK panic occurred on RR after physical memory exhaustion.

## Coverage that exists

`test_sched_modules` loads the actual `src/modules/sched_rr.cpp` and
`src/modules/sched_mlfq.cpp` implementations as host shared libraries through
`SchedOps`. Originally it had 20 common tests per policy and six MLFQ-specific
checks for ordering, demotion, aging, timeslices, yield promotion and fairness.

All six policy-specific tests used standard `assert`, while the Release target
sets `NDEBUG`. Assertions, including side-effectful `PickNext` calls inside
them, were compiled out. A green Release run therefore did not establish those
properties. The updated harness changes these to unconditional tests and corrects
PASS counting after a failed check.

The separate `test_sched_policy` suite compiles the older pointer-based
`src/kernel/src/sched_policy.cpp`. It does not verify the loadable module;
priority-zero and yield semantics differ between those implementations.

The updated host harness passes 49 checks with `NDEBUG` explicitly enabled:
21 common cases for each policy and seven MLFQ-specific cases. Additions cover
all 1,024 PID slots, remove/reinitialize/requeue recycling, invalid PID no-ops,
and 100,000 deterministic model-based operations across eight logical CPU
slots and 96 tasks. The reference queues check priority, FIFO order, ready
counts, duplicate wakeups, blocking/promotion, expiry/demotion, aging and PID
recycling. Periodic copied-state drains detect duplicate or lost queue entries.

This is serialized callback interleaving, as required by the policy ABI. It
is not actual concurrent kernel execution, an SMP context-switch test, or an
unbounded fuzzing claim. The same 49 checks also pass with AddressSanitizer and UndefinedBehaviorSanitizer
instrumenting both shared modules and the test harness. Host results are retained
under `build/mlfq-safety-20261018`.

## Kernel integration blockers

- `SchedulerTimerTick` reads `g_schedOps->Timeslice` and shared policy state
  outside `g_readyLock`, violating the ABI's locking contract. Concurrent policy
  replacement reuses that state buffer. In an MLFQ-to-RR switch, a stale MLFQ
  lookup can interpret RR link bytes as a priority and index outside its
  four-element timeslice table. Locking only the pointer would not protect
  the state layout.
- `SchedulerAddProcess` initializes policy state under the ready lock, releases
  it, and later publishes/enqueues the newborn under ready/all-process locks.
  A switch in that gap does not see the unpublished process and wipes its
  registration. The replacement policy then ignores its enqueue as inactive.
- `SchedulerSwitchPolicy` filters names beginning with `idle`, rather than
  identifying actual idle tasks. A real program with that prefix is omitted
  from registration and can become unschedulable.
- Migration hardcodes priority 2 rather than preserving `schedPriority`. Its
  final ready-count diagnostic also calls the policy after releasing the lock.

These are source-identified races/behavior gaps, not reproduced SMP failures
in this audit. They need fixes and deterministic switch-versus-birth/timeslice
regressions before controlled guest switching, followed by eight-CPU process
creation/exit, signal, blocking/wakeup and workload stress on isolated disks.

## Policy contracts still to establish

The current kernel removes a task before blocking promotion and demotes an
unqueued running task on expiry. MLFQ's queue metadata relies on that ordering.
Changing priority while still queued can remove from the wrong priority queue;
reinitializing a queued PID also lacks RR's defensive unlink. These are missing
contracts/defensive coverage, not a demonstrated normal-callback corruption.
Decide and test their supported behavior explicitly.

Also cover clock reversal/wrap and exact boost boundaries, sustained mixed
CPU/I/O workloads, fairness/latency tails and starvation. Ordinary
`SchedulerYield` does not invoke `VoluntaryYield`; only blocking currently does,
so module tests alone do not establish the advertised yield behavior. MLFQ's
optional queue debug snapshot is absent, limiting incident diagnostics.

Do not interpret the current module test results as approval to change the
live review scheduler. A matched RR/MLFQ comparison remains future work after
these integration gates and the independent low-memory panic fix.
