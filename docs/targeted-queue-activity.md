# Targeted ready-queue activity

The [wake-scan handoff](wake-scan-handoff.md) reduced mixer deadline-to-ready
lateness, but dispatch waits remained tens of milliseconds. The existing
`sched timing watch PID` probe now retains per-CPU activity during that exact
PID/incarnation's queued interval:

- Delivered periodic timer observations: user mode, busy running kernel
  context, idle, or other/not-running context. Reschedule IPIs are excluded
  from these tick counters.
- Dispatch bookkeeping for other user/kernel tasks from all normal dispatch
  paths. The watched task's own dispatch and idle PID zero do not count.

Both hooks use the existing ready lock. There is no new clock read, policy
callback, allocation or collection lock. Collection remains disabled with the
normal ready-timing probe and inactive without a watched pending interval.
Each retained slow event has at most 64 CPU rows; zero rows are omitted.
Counters saturate explicitly. Invalid CPU/kind observations are reported as
`invalid_activity`, distinct from invalid clocks. Cancel/reset, selection,
incarnation changes and idempotent enqueue retries preserve interval identity.

These are observations, not CPU-time accounting. Masked IRQ time remains
unobserved; no ticks does not prove idle. A dispatch record precedes completion
of the physical context switch, so counts cannot be treated as an exact switch
timeline. The probe does not expose policy eligibility, virtual deadlines, or
CPU-ownership rejection reasons. The original top-sixteen retention limit still
applies; it is not a full queue-wait distribution.

Hosted regressions additionally cover per-CPU classification, own/idle dispatch
exclusion, retries, cancellation, selected/incarnation changes, disabled mode,
CPU 63, invalid CPU/kind and uint32 counter saturation. All 66 hosted suites
pass. The native kernel/module import audit resolves thirteen modules/106
exports. Twelve supported compiled entry templates and 56 mutations pass;
four paths remain unsupported. Scheduler policy ABI and module ABI remain
unchanged (2 and 34 respectively).

## Native observations

Two fresh private eight-vCPU/six-GiB boots use frozen kernel
`87592a287ce422005c48e18f13cef1e4735ce94b06aca525ceec7fea72b051cf`.
This includes the default nice-query encoding correction. Firefox completes the
same local generated video/audio fixture under both EEVDF and MLFQ, with
shared wake scanning explicitly enabled, 24 CPU workers and private null audio.
These are diagnostic cohorts, not comparisons against an uninstrumented kernel.

Only events wholly inside the CPU helper's recorded start/end timestamps are
used below. Watch and worker times share the verified guest stable clock; no
host RPC offset estimate is needed.

| Run/event | Queue wait | Deadline lateness | User IRQ observations | Busy kernel IRQ observations | Other user/kernel dispatch bookkeeping |
| --- | ---: | ---: | ---: | ---: | ---: |
| qa-es1, largest | 32.751 ms | 0 ms | 218 | 38 | 522 / 28 |
| qa-es1, next largest | 32.040 ms | 0 ms | 247 | 6 | 417 / 16 |
| qa-ms1, largest | 45.506 ms | 0 ms | 355 | 9 | 19 / 10 |
| qa-ms1, next largest | 41.069 ms | 0 ms | 287 | 40 | 21 / 7 |

During EEVDF's largest event CPU 0 has 32 busy-kernel observations and no
dispatch records, but the other seven CPUs have user observations and many
other dispatches. Its next event has user observations and other dispatches on
all eight CPUs. A system-wide period with no preemptible CPU opportunities is
insufficient to explain those retained waits. Eligibility/debt and ownership
need direct evidence before asserting a policy defect or changing weights.

MLFQ's largest event has user observations on all eight CPUs, with only one to
ten other dispatch records per CPU. Source review shows 5/10/20/40 ms slices;
`MlfqShouldPreempt` checks expiry only, without a higher-ready-priority wakeup
condition. The counts are consistent with long slices contributing to wakeup
latency, rather than all CPUs staying in kernel mode. Priority/queue ordering
is not captured, so the source mechanism still needs a controlled intervention.

EEVDF retains all six slow loaded episodes. MLFQ retains sixteen of 81, exposing
65 exclusions; all retained events are within the CPU interval. Both runs have
zero invalid-activity/clock/saturation flags in the watched data, pass runtime
and heap checks, drain original CPU/browser lineages, restore BSP wake mode,
and pass original launcher/router exit, four filesystem checks and saved-data
preservation. No native HDA fault occurs. All VMs/private services stop. Ignored
`queue-activity-cohorts` includes source hashes, raw per-CPU records, matched
mixer events and `verify-results.py`.

Next test MLFQ wakeup preemption with preserved service budgets and fairness
regressions. The kernel currently calls `TimesliceExpired` for a policy's
positive preemption decision and resets wall-slice start at dispatch; simply
adding a higher-priority check would misclassify early preemption or repeatedly
renew partial slices. Keep defaults unchanged. EEVDF eligibility and longer
host/device delays remain separate investigation paths; playback acceleration
is still not reproduced by this fixture.
