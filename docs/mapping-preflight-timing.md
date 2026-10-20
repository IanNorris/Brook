# Mapping preflight mutex timings

The loss-free [scheduler profiles](hda-publish-and-scheduler-profiles.md) identify
repeated waits in `UserMappingPageAccess`. New optional counters separate its
initial mutex-acquisition wait from the following PTE/range check. Both EEVDF
and MLFQ spend much longer waiting than performing those checks. This supports
an audit of read/write locking and mutex handoff; it does not yet identify every
long-lived holder or prove the cause of a particular browser frame stall.

## Scope and interface

The debug channel accepts `mapping timing on`, `off`, `reset`, and `status`.
This is a normal debug-thread diagnostic, not a panic-safe command.
The counters are disabled at boot. `on` enables recording, `off` preserves totals
but cancels in-flight intervals, and `reset` clears totals while retaining the
current enabled state. Each control change advances an epoch; stale or disabled
tokens cannot publish into the new interval. Completed counts persist across
on/off until reset, so always reset explicitly for a new measurement window.

When enabled, the probe reads the stable KVM paravirtual clock before acquiring
`AddressSpaceState::mutation`, immediately after acquisition, and after the
initial `CheckPage`. It records after releasing that mutex. Collection uses a
separate bounded, non-allocating spin-locked reducer, without clock reads,
logging, scheduler calls or AS locks under its lock. Disabled operation adds an
atomic epoch load/branch; it performs no probe clock reads or counter locking.
No process/address-space layout or module ABI changes.

The JSON report includes inclusive histogram bounds, sample/invalid-clock
counts, wait/hold sums and maxima, saturation flags, and the PID/incarnation of
the task experiencing each maximum. These identify the waiter/check executor,
not the mutex holder. Read/write categories describe requested access; both
perform an initial metadata/PTE check. Callers include user-copy preflight and
CoW fault handling. The later exclusive CoW revalidation/resolution is excluded.

Important measurement limits:

- Wait includes mutex handoff, scheduling, host descheduling, guard contention
  and measurement overhead. It is not pure lock-owner CPU time.
- Hold covers the initial check through the final clock sample; it excludes
  unlock, later CoW and other mapping operations. It includes interrupts/host
  delay during that bracket, not only useful instructions.
- Totals combine all address spaces. Concurrent waits overlap; their sum is not
  lost wall time or CPU service. Active waits and intervals crossing a control
  boundary are omitted. A permanently blocked call never becomes a sample.
- Histograms provide upper bounds, not exact quantiles. The final bucket is
  unbounded above; exact observed maxima remain available. Invalid clocks and
  saturated totals must be checked before interpreting a report.

The browser runner's explicit `--mapping-timing` flag saves idle/reset/off
receipts and the pre-load process snapshot. Mutation commands retain intent
records and stop on uncertain delivery rather than silently resetting again.
A resumed cohort cannot change the diagnostic mode. These counter windows
include CPU setup/drain and controller latency around the timed load; they are
not exactly the CPU worker's 30-second interval.

## Native observations

One counter-off EEVDF control and fresh EEVDF/MLFQ diagnostic boots use the same
frozen kernel and thirteen modules, eight vCPUs, six GiB, fresh 800×600 Firefox
profiles, generated 24-fps video and 24 CPU workers for 30 seconds. No CPU/CS
profiler runs. Audio remains callback HDA/native PipeWire/private null output
with requested 20-ms buffering. EEVDF uses the unchanged 4-ms normal request.
All three boot snapshots prove disabled counters, epoch zero and zero samples.

Completed checks in the reset-to-off diagnostic windows:

| Policy / access | Samples | Mean wait (µs) | Mean check hold (µs) | Wait p95 upper bound (µs) | Hold p95 upper bound (µs) | Maximum wait (ms) |
| --- | --- | --- | --- | --- | --- | --- |
| EEVDF read | 369,223 | 87.692 | 0.230 | 500 | 1 | 92.253 |
| EEVDF write | 355,418 | 117.068 | 0.247 | 500 | 1 | 96.131 |
| MLFQ read | 470,669 | 67.566 | 0.250 | 500 | 1 | 68.546 |
| MLFQ write | 480,669 | 56.344 | 0.228 | 500 | 1 | 96.325 |

All four categories have wait p50 at most 0.1 µs. Thus many acquisitions are
fast, while the waiting tail is expensive. Loaded-window maximum check holds
range from 193 to 555 µs. An idle EEVDF read check has a 49.560-ms hold outlier;
do not claim that every critical section is sub-microsecond or infer pure
compute time from these wall-clock brackets. Native histograms sum to the
sample counts, their weighted bounds contain the reported time sums, and all
idle/load snapshots have zero invalid clocks and no saturation.

The source currently serializes even read-only `CheckPage` work through the
exclusive AS mutation mutex. Its subsequent CoW path already drops that lock,
acquires its full transaction and revalidates. A shared initial validation path
is worth investigating, but only after auditing every writer and existing lock
order. Preserve range permissions, PTE/CoW synchronization, cancellation and AS
lifetime, concurrent unmap/protect, file gates, TLB acknowledgements and fault
retry behavior. These results do not justify skipping preflight or weakening
memory isolation. Long other-mutator holds and grant-to-run delay remain
unmeasured. The subsequent [shared-validation experiment](shared-mapping-experiment.md)
reduces measured waiting but does not improve presentation in its controls;
its code is retained on a separate local branch and not adopted here.

For context, the counter-off EEVDF control presents 4.063 video callbacks/s,
counter-on EEVDF 4.379, and counter-on MLFQ 6.296. Their periodic wakeup p95s
are 3.063 / 3.679 / 20.797 ms. One run per condition does not establish probe
overhead or a policy improvement. The presentation gap to Linux remains.

## Validation and retained evidence

All 63 hosted suites pass, including the real VMM/CoW test with the probe enabled
and a new reducer/wrapper test. It checks histogram boundaries/overflow,
backwards/unavailable clocks, stale epoch rejection, independent categories,
maximum identities, saturated arithmetic and 4,000 concurrent records through
the actual locked wrapper. Native import checks pass for thirteen modules and
106 kernel exports.

All three native collections pass runtime clock/anomaly checks across eight
CPUs, heap integrity, tracked app/CPU exit-zero with no descendants, empty final
audio streams, original launcher/router exit-zero, four filesystem checks and
twelve saved documents/four original profile-tree preservation checks. EEVDF
service/ledger and exposed queue counters agree; MLFQ has no equivalent debug
contract. All buffered page clock models pass without lost trace samples, and
all audio windows are wholly inside conservative CPU-load bounds. No native HDA
fault or QEMU codec adjustment/overrun appears. That does not prove the prior
intermittent audio fault fixed or establish physical-speaker acceptance.

All VMs and both private services stopped normally. Defaults remain unchanged.
Ignored evidence under `build/workloads/timing-scheduler-20261020`:

- `mapping-timing-cohorts`: mt-e0 / mt-e1 / mt-m1 receipts, exact default/idle/load
  snapshots, generated media traces, captured PCM, input and lifecycle evidence.
- `mapping-timing-results.json`: independent histogram/time-sum checks,
  collection validation, derived quantile bounds and common artifact identity.
- `mapping-timing-frozen`: exact kernel/modules, source hashes/copies and validator.
- `mapping-timing-{host,all-host,kernel}-*` logs: focused/full tests and native build.

Private review directories are `timing-mix-{mt-e0,mt-e1,mt-m1}-20261020` beneath
`build/reviews`; they retain serial logs, original wrapper exits, filesystem and
saved-data preservation reports. No generated media or game assets are in Git.
