# Shared initial mapping-validation experiment

Shared validation reduced measured mutex waits but did not establish better
browser presentation. Preserve this prototype as research, rather than adopting
it for the desktop. The working timing branch will retain the original exclusive
mutex implementation. The broad timing/audio/scheduler investigation remains open.

## Prototype and safety scope

The prototype replaces the AS mutation mutex with Brook's sleeping reader/writer
lock. All existing mutators, file-gate snapshots, invalidation, graphics copies
and CoW transactions acquire it exclusively. Only the initial
`UserMappingPageAccess` PTE/range check can share, selected by the experimental
`mapping reads shared|exclusive|status` debug command; sharing defaults off.
Mode is captured for each acquisition so a concurrent toggle cannot mismatch
its unlock. Initial validation releases its lock before CoW preparation, full
transaction acquisition and revalidation, preserving the existing upgrade path.

Concurrent cache fills require relaxed atomic accesses to positive range-lookup
slots. The AS lock still protects node lifetime and tree/range mutation;
invalidation remains exclusive. Page-table lookup retains its existing PTE lock.
The fatal exception diagnostic uses nonblocking read admission, respecting
queued writers and deferred readers. Existing diagnostic/cursor operations stay
exclusive to keep the experiment focused.

AS locking can nest beneath VFS reader/writer locks. The legacy Process held-lock
hints have one slot per mode; the prototype's nested-lock initializer suppresses
those hints for AS locks. It does not remove queue tracking, process references,
kernel-region cancellation protection, grant publication or wake references.
The legacy VFS hints are preserved. Tests exercise cancellation while nested and
queued, and verify that terminal publication waits until protected operations
unwind. These finite tests are not a proof of all kernel lock paths.

The AS and reader/writer-lock layouts change, requiring module ABI 35 and the
complete matched thirteen-module set. Older ABI-34 images/artifacts remain
intact. The prototype kernel SHA-256 is
`a0fe70fcb5eb267c719ad801ca089a8d5081be820e6f6d212e2c120dcf0ea12b`.
No scheduler policy/default or Linux userspace ABI is changed.

## Native controls

Four fresh private eight-vCPU/six-GiB EEVDF boots use one frozen kernel/module
set and the same generated video, fresh 800×600 Firefox profile, 24 CPU workers
for 30 seconds, input checks and null-output native PipeWire/callback HDA. No
CPU/CS profiler runs. Every boot proves sharing and timing counters initially
disabled; selected modes are recorded before Firefox starts.

| Run | Initial validation | Counters | Video callbacks/s | Media/page rate | Wake p95 (ms) | CPU batches |
| --- | --- | --- | --- | --- | --- | --- |
| sm-e0 | exclusive | on | 5.225 | 0.92798 | 3.757 | 6,039,211 |
| sm-e1 | shared | on | 4.438 | 0.95063 | 3.229 | 6,106,538 |
| sm-e2 | shared | off | 2.873 | 0.94281 | 3.301 | 6,034,228 |
| sm-e3 | exclusive | off | 4.086 | 0.90538 | 3.255 | 6,012,373 |

The instrumented pair's mean read wait falls from 104.157 to 50.349 µs, and
write-check wait from 106.880 to 77.956 µs. Typical checks remain very short;
hold p95 is bounded by 1 µs. Read-wait p95's histogram upper bound falls from
500 to 100 µs, while write-check p95 remains at most 500 µs. One shared check
has a 9.138-ms hold outlier; the bracket includes host/interrupt delay.

This is one run per mode/counter combination on a warm shared host, not a
statistical distribution. It neither proves sharing always regresses video nor
establishes its overhead. It does show that the lock-wait reduction alone is
insufficient evidence for the user's desktop/gaming objective. Keep the simpler
working implementation and retain the prototype for later use if another
workload or measured bottleneck justifies it. Other mapping holders, handoff
delay and GPU completion behavior remain unresolved.

## Retained audio failure

sm-e2 records a native HDA `sample-gap` failure. The preceding sample is at
69,428,433,107 ns, the next pre/post reads at 69,630,325,154 /
69,630,345,847 ns: about 201.913 ms across the sample bracket, beyond the
half-ring ambiguity limit of about 185.760 ms. Raw device position advances
14,336 to 49,664 (35,328 bytes, about 200.272 ms at the configured rate).

This is consistent with a delayed device observation, not evidence of an
accelerated/backwards clock. The log alone does not identify which task/lock/
host delay prevented servicing. The driver preserves its last proven completion,
stops/clears DMA and reports a discontinuity; do not relax that guard or credit
unproven frames. The run is failure evidence, excluded from clean audio
acceptance. It can also confound the presentation comparison. No native HDA
fault appears in the other three serial logs. All audio captures are proven
inside CPU load; that does not make sm-e2 continuous audio or speaker acceptance.

## Tests and limits

All 64 regular hosted suites pass on the prototype. They include:

- Real lock code with nested cancellation/handoffs, writer-respecting try-read
  admission, spurious wakes, delayed recipients and deferred reader cohorts.
- 160,000 simultaneous cache lookups with colliding slots against immutable
  range nodes, plus existing tree/edit/invalidation/ownership tests.
- The real VMM suite in exclusive and shared modes, including concurrent CoW,
  unmap/protect, file-gate ordering, retirement, failure and cleanup paths.
- Two validators held concurrently while a protection writer is pending,
  followed by permission/cache revalidation and a mode switch.

The native build resolves thirteen modules against 106 kernel exports. All
four native runs pass runtime clock/anomaly checks on eight CPUs, heap checks,
app/CPU exit-zero with no descendants, original wrapper/router exit-zero, four
filesystem checks and twelve saved documents/four original profile-tree
preservation checks. EEVDF service equals its runtime ledger and exposed queue
counts agree. Buffered traces pass with no lost page samples. These collection
gates do not override the HDA failure or prove complete application acceptance.

ThreadSanitizer passes the range-cache test and both VMM modes. Its lock stress
gate does **not** pass: one run reaches the 30-second alarm, and diagnostic runs
hit a five-second cohort completion assertion or the alarm. An isolated
pre-experiment d07bd8d3 lock/test copy also fails a five-second cohort admission
assertion under TSan. This prevents attributing the timeout solely to the
prototype, but does not resolve slow progress versus a lost wake. No claim of
sanitizer-clean lock stress or a new production deadlock is made. Keep this
limitation visible in the scheduler/lock safety audit.

All VMs and private services stopped. Ignored evidence under
`build/workloads/timing-scheduler-20261020`:

- `shared-mapping-cohorts` / `shared-mapping-final-control`: frozen controllers,
  mode/counter receipts, buffered traces, PCM, input, runtime and shutdown data.
- `shared-mapping-results.json`: histogram bounds, identities, collection checks
  and the explicitly retained native audio fault.
- `shared-mapping-frozen`: exact artifacts, source hashes/copies, validator and
  later stress-test diagnostics. The source prototype is retained separately.
- `shared-mapping-*host*`, `shared-mapping-*kernel*`: regular build/test logs.
- `shared-mapping-tsan*`, `rw-tsan-baseline`,
  `shared-mapping-sanitizer-status.json`: sanitizer successes, failures and the
  isolated baseline source/run. No failed run is replaced by a passing rerun.

Private review names are `timing-mix-sm-e{0,1,2,3}-20261020` beneath
`build/reviews`. No generated clip or game data is committed.
