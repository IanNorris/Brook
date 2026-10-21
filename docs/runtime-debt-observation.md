# Observing runtime correction debt

`sched runtime status` includes a `debt` object describing the correction
carried by the existing service floor. It uses the same samples and ownership
boundaries as the ledger. It does not read another clock, change a charge,
retry a sample, reset a baseline or alter scheduler/audio defaults.

| Field | Meaning |
| --- | --- |
| `known_ns` | Sum of debt on CPUs with a valid baseline at this snapshot. |
| `baseline_cpu_mask` | CPUs with a valid baseline; compare with the configured online CPU mask before interpreting zero debt. |
| `max_cpu_ns` | Largest observed debt on any individual CPU, not a simultaneous aggregate maximum. |
| `episodes` | Transitions from zero to positive debt within a valid epoch. |
| `catchups` | Positive debt becoming zero through a valid sample reaching or exceeding the retained floor. |
| `discarded_ns` | Known outstanding debt discarded when invalid/backwards input breaks an epoch. |
| `invalidated_epochs` | Valid epochs lost to invalid/backwards input, including epochs with zero debt. Repeated invalid samples do not count again. |
| `owner_transitions` | Changes of PID or incarnation while debt remains positive, including transitions to/from idle. |
| `saturated` | A diagnostic sum/counter overflowed; the snapshot is not exact. |

For a valid baseline, debt is `max(floor - (wall - steal), 0)`. The snapshot
sums cached per-CPU debt while holding the existing ready lock; charge paths
maintain only local debt and cumulative counters. Snapshot summation overflow
can cease on a later read after debt is discarded or repaid. Cumulative-counter
overflow remains sticky. Neither kind of overflow affects charge decisions.
CPU mask bits cover CPUs 0–63, matching the existing runtime mask interface.

A baseline reset is not catchup: its previous debt is explicitly discarded.
Unknown/uninitialized CPU baselines cannot be treated as known-zero debt.
Owner transitions measure exposure to attribution uncertainty, not its duration
or a number of incorrectly charged nanoseconds. The same debt may cross several
owners. Even complete catchup proves aggregate reconciliation only within a
valid epoch; it cannot undo earlier uncertainty about individual task charges.

## Hosted verification

The independent consumed-steal recurrence in `test_runtime_debt` checks the
new fields after every one of 30,000 four-CPU sample/owner events. It independently
tracks debt episodes, catchup, discarded epochs/debt, peak individual debt and
ownership crossings alongside existing thread/group/idle conservation checks.
Focused cases cover unchanged ownership, PID reuse, idle, partial recovery,
exact recovery, new baseline versus recovery, repeated invalid samples and
snapshot versus cumulative saturation. The history-enabled/disabled comparison
also checks every new field across its 40,000 events. All 73 hosted suites pass.

Astra reviewed the actual bookkeeping and found no blocking issue. Native export
and integration results are recorded separately. This is diagnostic coverage,
not new scheduler acceptance. All historical zero-clamp failures remain failed;
a positive retained native history and independent replay remain necessary
before considering a different prospective acceptance contract.

## Native integration result

A fresh eight-vCPU/eight-GiB MLFQ VM runs the existing clock/ownership probe
with one/eight-process quiet waves and an eight-process wave under finite host
CPU pressure. QEMU retains CPUs 0–15; eight host workers use 0–7 for twenty
seconds. The complete pressure guest-job bracket lies inside every worker's
measured active interval. Python's GIL serializes execution within each child;
its two threads exercise ownership and CPU clocks, not two independent burners.

| Phase | Probe threads | Read cycles | Sleeps | Runtime samples | Reported steal delta |
| --- | ---: | ---: | ---: | ---: | ---: |
| Quiet | 18 | 907,711 | 53,393 | 2,457,509 | 66.88 ms |
| Host pressure | 16 | 627,874 | 36,931 | 1,710,961 | 2.287 s |

Both reports pass. All eight baseline bits are present, with no debt episodes,
catchups, discarded debt, owner crossings, invalid/backwards inputs, stale
ownership or saturation. Two separate stopped history epochs retain 512 rows
each, including 33/35 owner events. Their retained tails replay through the
independent consumed-steal recurrence from the first known boundary. These are
negative captures, not positive native debt recovery or full-epoch replays.

The original four guest jobs, host workers, controller and VM owner exit zero
with complete output/EOF and empty original groups. Heap, ready-count/policy
ledger and all four offline filesystem checks pass. Original thirteen ABI-34
modules remain unchanged; runtime history is enabled explicitly only for this
diagnostic and stays off by default. Frozen kernel SHA-256 is
`112519e37cd0d542c984ae85af434676ce0df192ca6f54a5dd4d86fc37b1adff`.
Evidence and checked receipt are retained under
`build/reviews/unattended-runtime-debt-mlfq-a-20261021/runtime-debt-control/`.
These measurements are not a performance comparison against the prior kernel.

## Verification correction

The later shared-prefix experiment's fail-fast full build exposed a fixture
compile error in the final debt edge cases: the synthetic `at` helper required
two arguments, while new cases used its intended zero-steal shorthand. Earlier
multi-command build invocations continued to ctest after this failure, running
the preceding debt executable. Those aggregate green results did not prove the
final added edge cases compiled. The previously compiled 30,000-event diagnostic
comparison and the native integration evidence remain separate.

The helper now defaults its steal argument to zero. A successful full build
followed by all 74 hosted suites passes, covering the current edge cases too.
Build/test commands now fail immediately on compilation failure. The original
failed build and corrected verification logs are preserved under
`build/workloads/timing-scheduler-20261021/shared-prefix/`, including
`final-host-build.log`, `verified-host-build.log` and `verified-host-tests.log`.
This corrects test evidence; production runtime accounting is unchanged.


## Positive counter episode during a mixed-app repeat

The later [shared-prefix real-app repeat](audio-shared-prefix.md#fresh-real-app-repeat)
records one clamp during the first Quake launch, before the experiment is enabled.
CPU 4's wall pair advances 28,567 ns and its steal pair 45,293 ns, leaving
16,726 ns of known debt. Counters subsequently record one catchup and one owner
transition; known debt is zero at the first mixed capture and final snapshot.
This demonstrates the new counters observing a native positive episode, not
correct attribution or a complete native history replay. History was enabled
only for cleanup, after the event, and remains negative for that later interval.
The runtime acceptance gate fails without a tolerance; the next targeted
collection must enable history before the first game launch.

The original evidence is
`build/reviews/unattended-mixed-prefix-mlfq-b-20261021/mixed-prefix-control/runtime-final.json`
and the phase snapshots beside it. Earlier clean runs do not supersede this
failure or earlier cross-policy clamps.


A separate diagnostic repeats the mixed workload with history enabled before
the first Quake launch and no later reset. It records 12,120,321 history events
without reproducing a clamp; 512 retained tail rows replay against the
consumed-steal reference. Original eight guest jobs, controller and VM owner
exit zero, all four filesystem checks and current runtime/heap/policy gates
pass. This negative, history-enabled cohort does not clear the preceding
positive failure and is not pooled with ordinary performance measurements.
Evidence is under
`build/reviews/unattended-mixed-prefix-history-mlfq-a-20261021/mixed-prefix-control/`,
including `debt-history-replay.json`. The private replay checker distinguishes
owner events from samples, validates shared export/trigger/catchup metadata,
and seeds only at the first retained boundary. A synthetic positive
clamp→owner change→catchup passes; ten mutations of floors, credits, sequences,
identities, raw trigger pairs and catchup claims are rejected. This verifies
the checker, not a positive native history that has still not been captured.


## First complete positive native history

The later worker/reservation timing cohort finally captures a positive episode
in cleanup, after all eight measured-phase snapshots still reported zero clamps.
The unchanged frozen kernel is
`4506f7bbf02588dcdb772affc1b835829fd301c7423c7b9f1a4f00e402fdd2b0`.
History epoch one triggers on CPU 3 at sequence 2,179,657 / runtime sample
14,509,269. All eight per-CPU buffers freeze without overflow or baseline loss;
512 retained rows pass independent consumed-steal replay from their recorded
initial boundaries. The raw trigger/preceding pair, identity, floor and deficit
match the cumulative runtime evidence exactly.

| Boundary | Owner | Wall advance | New published steal | Credit | Remaining debt |
| --- | ---: | ---: | ---: | ---: | ---: |
| First clamp | 315 | 14,640 ns | 32,530 ns | 0 | 17,890 ns |
| Owner changes to 149 | 149 | no new sample | — | 0 | 17,890 ns |
| Second clamp | 149 | 6,956 ns | 0 | 0 | 10,934 ns |
| Third clamp | 149 | 767 ns | 0 | 0 | 10,167 ns |
| Owner changes to 226 | 226 | no new sample | — | 0 | 10,167 ns |
| Catchup | 226 | 54,786 ns | 0 | 44,619 ns | 0 |

This is one debt episode spanning three clamped samples and two debt-bearing
owner transitions. Across the observed interval, 77,149 ns wall minus 32,530 ns
steal equals 44,619 ns total credit exactly. Catchup is retained at sequence
2,179,682; no invalid/backwards pair, discarded epoch, stale identity or
saturation explains the result. Brook carries correction across owners without
subtracting already credited task time, matching the independent reference.
This demonstrates the local mechanism; it does not identify which task actually
incurred the delayed host steal or prove true per-task attribution correct.

`nativeOwnershipEpisode` in `src/tests/test_runtime_debt/main.cpp` replays the
exact captured samples and owner changes through the actual ledger and the
independent consumed-steal reference. It asserts every intermediate debt,
three clamps, two owner transitions, final thread/group credit and conservation.
It seeds at the retained preceding boundary, not the original epoch origin;
self-groups are synthetic because native history does not contain group IDs.
A successful fail-fast rebuild and the affected runtime-debt suite pass. The
sample pairs and intermediate debts are also checked against raw native rows.
This is a regression fixture, not a production accounting change.

All nine original guest jobs, controller and VM owner exit zero with complete
identity/output/EOF and empty groups. Four filesystem checks, heap, policy
ledger/ready-count and full browser/media transfer gates pass. **Runtime
acceptance remains failed with three clamps**; the positive replay does not
waive older failures or introduce a tolerance. The missing positive-mechanism
evidence has now been obtained, so another broad recurrence hunt is unnecessary.
Evidence is under
`build/reviews/unattended-reservation-service-mlfq-a-20261021/mixed-prefix-control/`,
including `debt-history-replay.json`, `cleanup-history-cpu-3.json`,
`regression-fixture-check.json` and `regression-host-test.log`.
