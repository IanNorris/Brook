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
