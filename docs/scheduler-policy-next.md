# Scheduler policy direction after the timing audit

This is a proposal, not an implemented policy or a recommendation to change the
default. RR remains the default. Current results and safety limits are in
[timing-scheduler-audit.md](timing-scheduler-audit.md).

## What the measurements justify

The bounded CPU/sleeper workload shows that equal compute service does not imply
short wakeup latency. The first repeated RR results have roughly 7–8 ms sleeper
p95 at eight busy threads and 91–94 ms at 24 on eight vCPUs. The completed
same-kernel comparison gives MLFQ roughly 21–23 ms at both worker counts: better
under heavier load, worse under lighter load. Its earlier pilot includes a
roughly one-second shared outlier. Later browser pilots reproduce this
outlier and expose backwards CMOS rebases of the legacy scheduler clock;
the elapsed-clock candidate needs matched browser acceptance. Browser/audio/input workloads must validate any policy choice; a
synthetic sleeper is not an input-to-presentation test.

A missing prerequisite is observable runtime accounting. Brook currently
charges one CPU tick per delivered interrupt. The elapsed-clock candidate
now uses stable KVM pvclock for slices instead of the BSP's adjustable
apparent tick counter; this does not establish actual CPU-service accounting. Stable KVM elapsed time, delivered IRQ count,
actual CPU service and host stolen time are different quantities. Log which
one each statistic represents; do not use the existing `/proc` CPU ticks as
proof of precise EEVDF service accounting.

## Why consider EEVDF

The upstream [EEVDF description](https://docs.kernel.org/scheduler/sched-eevdf.html)
selects among tasks owed service (nonnegative lag), choosing the earliest
virtual deadline. Shorter service requests can improve latency while weighted
service accounting retains fairness. Its sleeper treatment prevents short
sleeps from erasing a task's service debt. These properties address both the
loaded FIFO latency and the risk of rewarding every short block in MLFQ.
This is a design motivation, not evidence that EEVDF will fix Brook's playback
speedup or kernel stalls.

## Mechanism prerequisites

1. Preserve serialization of every policy-state access. Timeslice reads and
   expiry decisions now hold `g_readyLock`, as do registry lookup and replacement.
   Rebuild holds ready/all-process locks, excludes terminated and still-on-CPU
   ready tasks, and preserves initial priorities. Task policy registration is
   atomic with birth, closing the initialize/publish gap. Native switch/churn
   coverage is recorded in the audit; module lifetime remains separate work.
2. Define task lifetime and queue ownership across Ready-but-on-CPU windows,
   post-switch requeue, exit, stop/continue, wake races and PID reuse. Preserve
   the existing off-CPU publication and lifetime holds. Never dispatch the same
   task on two CPUs.
3. Give policy registration a module-lifetime contract. `ModuleUnload` calls the
   module exit callback and frees its pages; `sched_mlfq` does not unregister
   its `SchedOps`. Both selected and merely registered policy pointers can
   outlive module memory. Do not unload scheduler modules during experiments.
   Pin registrations or implement synchronized unregister/rejection before
   treating unload as supported.
4. Add a runtime-accounting callback at dispatch/block/preemption boundaries.
   Timestamp per CPU with a validated monotonic source; account each running
   interval once, excluding blocked intervals and handling migration. State
   explicitly whether host stolen time and IRQ execution are included. Per-vCPU
   KVM steal accounting is separate work if needed for loaded-host fairness.
5. Add an explicit policy preemption decision. Ordering `PickNext` alone cannot
   preempt a low-priority 40-ms MLFQ slice promptly when a sleeper wakes. At a
   safe user-mode timer boundary, evaluate the decision and pick under the
   ready lock. Preserve kernel non-preemptibility until its locking contract is
   separately changed. An IPI can request reevaluation; it cannot justify an
   unsafe switch from arbitrary kernel code.

Changing `SchedOps` requires an explicit ABI/version transition and rebuilding
all affected modules. Registration must validate the required callbacks. The
16-KiB state bound now has compile-time and host checks; a larger EEVDF table
must get an explicit boot-time allocation/capacity design, never a per-tick
allocation or another unchecked fixed-size assumption.

## Proposed first EEVDF contract

- Start with equal weight 1024 per task; make weight/nice mapping explicit before
  exposing priorities. Process/app-group fairness is a separate hierarchy, not
  a consequence of thread fairness. Do not advertise realtime guarantees.
- Keep virtual runtime, virtual deadline, remaining request and bounded sleeper
  lag per task. Document fixed-point precision, saturation/rebasing and maximum
  lifetime. No unsigned-underflow interpretation of a regressing time source.
- Maintain the runnable weight/time model across both queued and currently
  running tasks on all CPUs. Select the earliest eligible deadline among tasks
  actually available to dispatch. Define progress when none are eligible;
  avoid unbounded queue scans or accidental all-CPU idling.
- Preserve service debt across brief sleeps, clamp exceptional sleeper credit,
  and test repeated block/yield patterns. Do not reset debt on wakeup.
- Charge partial service before a task blocks or is preempted. Carry remaining
  requests deliberately; changing request length must not create free service.
- Prefer the current global lock/queue initially so the new policy does not
  simultaneously change SMP ownership and balancing. Quantify lock/selection
  overhead before considering per-CPU queues.

## Required evidence before a default switch

Use an independent reference model for weighted service and eligibility, with
adversarial sleepers, sustained CPU load, unequal weights, all CPUs running,
empty queues, time updates, deadline overflow and lifecycle/PID churn. Verify
kernel mechanism races separately; serialized policy calls in a host model do
not prove actual SMP safety. Keep compiled entry audits' supported scope visible.

Run repeated, alternating-order private boots with equal kernel, image, host
CPU affinity, RAM, display visibility and audio backend. Include CPU throughput
and per-task service, periodic deadlines and queue latency tails, pipe/event
wakeups, real UI responsiveness, browser media progress and native audio output.
Retain startup and steady-state measurements separately. Add host contention
with fixed worker/time bounds and reserve control capacity. Verify normal app
exit, task drain, heap integrity, sync, authoritative QEMU/launcher exit and
filesystem/user-data preservation. Keep RR as the control and retain failures.
