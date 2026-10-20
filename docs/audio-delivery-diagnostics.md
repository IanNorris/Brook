# HDA silence reservation and copy diagnostics

The [small-queue controls](audio-small-queue-pacing.md) show repeated short silence
without a pitch change. Immediate producer delivery helps that reproduction but
fails to establish a consistent mixed desktop benefit. Before changing HDA's
conservative lead, this probe measures the intervals the driver actually reserves.
It changes no scheduling policy, queue capacity, publication check or completion
accounting. The existing module ABI remains 34; two new named kernel exports
support the diagnostic HDA module without changing `AudioDriver` or receipt layout.
New modules require those exports; use the matching frozen kernel/module capsule.

## Controls and interpretation

The normal debug command pipe accepts `audio delivery on`, `off`, `reset` and
`status`. It starts disabled with epoch zero. Reset clears interval counters and
recent events, preserves enabled state and advances the observation token. Enable
and disable also advance it, so an old in-flight copy cannot enter a newer epoch.
A lifetime contention counter is deliberately not reset.

Each accepted native HDA copy reports physical pending bytes before reservation,
reserved silence bytes, accepted bytes, copy clock brackets, the existing
post-copy device advance, and physical headroom. Startup copies are counted but
excluded from running-position/headroom statistics. Watermarks include unowned
silence and never represent application completion. An unsafe publication remains
rejected and its exposed receipt remains subject to the existing discontinuity
rules. The probe does not turn a failed sample into proven playback.

Enabled diagnostics add one pre-copy stable KVM clock read. Existing post-copy
clock and position samples supply the other endpoints; there are no extra MMIO
reads. Copy duration excludes reservation zeroing and device setup;
the copy-and-position bracket includes the existing position read. Device advance
starts at the driver's preceding position observation, so it spans reservation
work as well as copying. It is not a device-rate estimate from copy time alone.
Headroom is the receipt's lead over that preceding completion watermark minus
observed advance, clipped to zero if DMA caught the write.

Aggregate counters and a seven-bin copy-duration histogram cover the enabled
interval. Edges are 1 us, 10 us, 100 us, 1 ms, 10 ms and 100 ms; bins are lower
inclusive and upper exclusive, with the final bin unbounded. Sixteen recent events
are returned oldest first; they are not a full timeline or a worst-case sample
set. Separate `max_advance_event` and `min_headroom_event` objects retain the
actual samples responsible for those extrema, with their own clocks and lead.
Position validity also requires the existing publication sample clock order
and age checks, while a valid caught-write sample retains zero headroom.
Invalid samples, counter saturation or dropped records must be inspected
before inferring bounds. Absence of a large advance in a short run cannot justify
relaxing the publication guard.

The HDA playback lock serializes samples. Recording only tries the independent
diagnostic lock and drops contended records rather than waiting for a reader.
Snapshot/control operations never poll or lock the driver. The disabled path
checks an atomic token through the kernel hook and adds no timing/MMIO samples.
This is diagnostic instrumentation, not proof that disabled overhead is zero.
Software-queue-to-device residence time remains a separate measurement to add.

## Verification

The pure model tests disabled/stale tokens, reset, exact reservation totals,
histogram bins, invalid clock/position observations, caught-write headroom,
startup exclusion, recent-event wrap and saturating counters. The hosted test
using the production HDA implementation checks an exact reservation and device
advance during copy, then intentionally advances DMA past the receipt and verifies
that the publication guard still rejects it. Disabling recording leaves counters
unchanged. All 68 hosted suites pass. The diagnostic kernel and all 13 modules
build, with all 108 imported kernel symbols resolved. Supported entry-state and
mutation audits pass; the existing unsupported entries remain outside that audit.

## First native measurements

Fresh private disks, eight vCPUs, six GiB, MLFQ/default BSP wake scanning,
callback HDA, SDL into a private 44.1-kHz null sink and a 20-ms requested host
buffer reproduce the existing 30-s numbered-tone direct writer. There are no CPU
stress workers. Each run proves exactly 1,323,000 completed frames, 5,292,000
accepted physical application bytes and successful synchronization.

| Negotiated stream capacity | Completion | Silence reservations | Reserved bytes / 176,400 B/s | Remaining excess over source + reservations |
| --- | ---: | ---: | ---: | ---: |
| 4,096 frames | 34.023 s | 454 / 702,208 bytes | 3.98077 s | 42.36 ms |
| 8,192 frames | 30.034 s | 1 / 1,504 bytes | 8.53 ms | 25.88 ms |

Reserved silence accounts for almost all the smaller queue's extra duration.
The remaining interval includes startup/drain/observation effects; these counters
do not prove exact physical-speaker timing. This supports a driver reservation
interaction, rather than a change of pitch or an explanation of the historical
accelerated playback. Queue geometry is read back from the guest before timing.

Across these runs, maximum measured copy time is 25.687 / 49.795 us; maximum
copy-plus-position bracket is 2.172 / 2.529 ms. The greatest observed device
advance is 3,584 bytes in each run. Minimum remaining headroom is 10,752 / 12,288
bytes. Extrema may come from different writes: they must not be combined into an
imaginary single sample or treated as worst-case bounds under host contention.
A later refinement retains the exact maximum-advance and minimum-headroom events.
Both the 8,192-byte idle trailer and 12,288-byte publication lead stay unchanged.

All eight native control/fault cases pass. No invalid clock/position sample,
unsafe publication, diagnostic lock drop or counter saturation is recorded.
The probe starts disabled at epoch zero; histogram totals equal observation
counts and accepted-byte totals match the writer. Original launcher/router owners
exit normally; four filesystem checks, saved-data preservation, exported result
size/hash, heap integrity, runtime accounting and frozen identity checks pass.
The frozen initial capsule uses kernel
`d5d7be6433ddd93d9c403a31de125e34ac60ec1d9f9ae9e37b6a56a8fa8b7f96`
and HDA module
`81de6967b1c17f2fba1b7f8635174048760a81695a75ebf90a29d7ff4e943748`.
The other twelve modules match the accepted prefix-refill control exactly.

Ignored evidence is under `delivery-probe-*`. The first probe's raw position
validity bit checked range/overflow; the refinement additionally qualifies sample
clock order/age. All native observations above passed the full existing
publication predicate, so that distinction does not invalidate these samples.
Focused tests verify that stale samples cannot enter refined headroom extrema,
and that valid caught-write faults still retain zero headroom. The refined
capsule is `delivery-extrema-frozen`, kernel
`f94ba4c6bdafdd85c46bb60360b08d58d5f3b7757bbe5dd4680d462700cc68a8`
and HDA `2290b9900c2f2b19e3cd72cfe29d7ad2ca258024477f59fc918b1334e34a3dad`.

The refined capsule also completes the same native Quake GL3 generated-track
run. No invalid sample, unsafe publication, diagnostic contention or saturation
is recorded; the default-off status, exact extrema JSON and all existing
lifecycle/filesystem/accounting gates pass. There are 899 accepted submissions,
3,690,496 application bytes, and 340 reservations totaling 779,776 physical
bytes over the complete application run, including load/startup/shutdown phases.
That aggregate must not be divided by the demo interval as though it covered
only that interval.

Snapshots surrounding the roughly eight-second PCM capture differ by 120
reservations / 262,656 bytes, equivalent to 1.489 s of physical silence. These
are surrounding snapshots, not sample-exact capture boundaries. Reservations
can include future queued intervals and an already traversed unowned gap if
DMA has passed the old submission end. They do not map one-for-one onto quiet
samples in the captured window. The generated-track decoder reports source
progression 0.8306, pitch approximately 1.0000, and 15.67% quiet 1-ms bins,
with consecutive decoded source IDs.

The retained maximum-advance and minimum-headroom event is the same write:
13,056 bytes pending, no new reservation, 4,096 bytes accepted, 3,584 bytes of
observed advance and 9,472 bytes remaining headroom. Its copy bracket is
1.033 us and its pre-copy-to-post-position bracket is 95.398 us. Device advance
starts at an earlier position observation, whose timestamp is not exposed by
this version: **95.398 us is not the full advance interval** and must not be
used to derive an instantaneous device fetch rate. The maximum copy-and-position
bracket over the whole run is 7.479 ms, from a different event. The copy maximum
is 4.272 us. This is further evidence to measure MMIO/host delay separately
from copying and to preserve the exact publication predicate.

Astra's read-only review agrees with the reservation interpretation and recommends
matched off/on observer controls and representative contention before any lead experiment. Diagnostic
reads/recording may perturb timing; no reduction in safety margin follows from
these unloaded measurements alone. This instrumentation is not a playback fix.


## Matched observer controls

The refined frozen capsule runs four thirty-second direct-writer cases in one
fresh private VM, in off/on/on/off order, always with 4,096-frame capacity. Each
case drains, exports its guest size/hash receipt, and leaves the source closed
before the next case. No CPU stress runs during these comparisons.

| Diagnostic case | Completion | Source seconds / stored PCM second | Quiet 1-ms bins | Reserved physical silence |
| --- | ---: | ---: | ---: | ---: |
| off1 | 33.928 s | 0.8835 | 10.39% | not recorded |
| on1 | 34.039 s | 0.8839 | 10.50% | 4.014 s |
| on2 | 34.012 s | 0.8838 | 10.45% | 3.991 s |
| off2 | 34.037 s | 0.8816 | 10.59% | not recorded |

The pairs do not show a consistent observer slowdown. Mean completion differs by
approximately 43 ms (0.13%); two cases per state do not establish zero overhead.
The four-second stretching is present with recording disabled. Pitch remains
approximately 1.0000. Both enabled cases account for all 5,292,000 source bytes,
with no invalid samples, unsafe publications, saturation or contention drops.
Default-off state, exact kernel/module identities, eight native control cases,
job drains, original launcher/router exits, heap/runtime, four filesystem checks
and saved-data preservation pass. Evidence is under `delivery-observer-*`.

The initial analysis assertion requiring every coarse decoded ID failed on
`on1`: its 50-ms FFT decoder missed symbol 53 between 52 and 54. That failed
analysis/log is retained. A diagnostic report records the gap explicitly rather
than converting it into a continuity pass. An independent 20-ms raw-spectrum
check finds the expected unique 14,200-Hz tone in fourteen windows around that
interval. This establishes tone presence, not complete frame delivery; source
rate fits and consecutive coarse runs are not bit-exact continuity proofs.
There are no backwards decoded transitions in these four captures.

## Combined-load failure, not a headroom acceptance

A further private Quake run adds the existing `SCHEDMIX` workload: 24 CPU workers
and four periodic sleepers in the guest, plus twelve CPU workers and four
periodic sleepers on the host for thirty seconds. All seventeen host threads
(including their parent) are verified on CPUs 0–13 after workload readiness;
CPUs 14–15 remain outside that affinity. Original host and guest jobs finish
normally, and the capture interval is bracketed inside both timed load intervals.
The reported guest/host elapsed-rate bracket is 0.99938–1.00665, consistent with
1× elapsed KVM time. The demo observation interval grows to 44.355 s; it does
not demonstrate the historical accelerated playback.

This run **fails audio acceptance**. The original controller's local
`Q2_MUSIC_VM_ACCEPTED` completion message covered job/clock/heap/filesystem
checks, not all audio failure gates. The independent serial/stream/capture audit
supersedes it:

- HDA reports `reason=sample-gap` after an 800.636-ms interval from its preceding
  position-sample start. The unchanged half-ring bound is approximately
  185.760 ms. The last proven completion is preserved; no wrap is guessed.
- Cached stream diagnostics latch a discontinuity. There are no new accepted
  copies between the capture's surrounding delivery snapshots. The capture is
  inactive and contains **zero frames**, so it supplies no loaded headroom or
  source-rate measurement.
- Mixer service timing retains a 790.620-ms late wake at the fault, followed by
  2.463 s in the service bracket. Other retained wake delays reach 4.640 s.
  Those brackets include host delay and locks; they do not identify a scheduler
  queue wait or prove a particular lock owner.
- `unsafe == 0` in the copy probe does **not** exclude this fault: it counts
  post-copy publication rejection, whereas this failure occurs in progress
  sampling before another accepted copy. Serial faults, stream discontinuities,
  capture activity/frame count and clock/model validity are separate gates.

Across surrounding guest-clock observations at 29.992 and 61.524 s, accumulated
reported steal rises from 0.027 to 116.352 **CPU seconds** across eight vCPUs.
This is not 116 seconds of wall time, and it does not localize individual late
wakes. A post-run check finds no CPU quota or recorded throttling in the
launching process's cgroup hierarchy. Direct QEMU/worker membership and per-CPU
timer/ready-watch observations should be recorded in the isolated follow-ups.

The run is retained as an expected failure reproduction under
`q2-delivery-loaded-r2`, with an explicit false audio-acceptance artifact and a
verifier that checks the fault rather than hiding it. Original host/guest jobs
and VM/router owners exit normally; runtime accounting, heap integrity, four
filesystem checks, saved data and staged identities pass. The final extrema
are from before the stress period and cannot certify loaded headroom.

An earlier `q2-delivery-loaded` fixture fails a host-side affinity assertion
because it checks immediately after spawning `taskset`, before `taskset` applies
the mask. Its original Quake job and bounded host worker drain, and normal VM
shutdown/filesystem/preservation checks pass. The original host exit status is
not recovered, so this is not a performance control. Its retained serial log also
contains a sample-gap fault; guest CPU stress was never started. That is a lead
for a clean host-only repeat, not a substitute for one. The corrected fixture
checks all actual host thread affinities only after workload readiness.

No publication lead, FIFO trailer, sampling bound or scheduler default changes
follow from these results. Before a smaller-lead experiment, separate guest-only
from host-only load and enable the existing worker ready-watch/BSP/AP timing
probes to distinguish late wake publication, ready-queue delay and host stalls.

## Guest-only versus host-only load

Two fresh private runs use the same `delivery-extrema-frozen` kernel/modules,
MLFQ, BSP wake scanning, callback audio and 20-ms HDA output. Both enable the
existing mixer ready-watch, BSP timer and independent per-CPU arrival probes.
QEMU runs eight vCPUs with host affinity 0–13, normal nice 0 and SCHED_OTHER.
The host workers, when present, have the same affinity, priority and cgroup as
QEMU; their seventeen thread affinities are checked after READY. These are
single isolated runs, not a statistical policy comparison.

| Load for 30 seconds | Demo observation | Longest mixer ready queue | Audio result |
| --- | --- | --- | --- |
| Guest: 24 CPU workers, four periodic sleepers | 15.227 s | 18.767 ms | Active 7.999-s capture, no native fault or discontinuity |
| Host: twelve CPU workers, four periodic sleepers | 39.066 s | 1,018.979 ms | Sample-gap safety stop; only 2.139 s of captured samples |

The guest-only clock bracket is 0.99912–1.01013 and host-only bracket is
0.99923–1.00890. Neither reproduces accelerated elapsed time. The host-only
fault follows a 1,276.209-ms interval between position-sample starts, beyond
the unchanged half-ring bound. Its audio acceptance is explicitly false even
though the application job, runtime ledger, heap, normal shutdown, four
filesystem checks and saved-data checks pass. The guest-only run passes those
same structural gates and the active-publication/capture gates. A capture
passing those gates does not establish bit-exact audio continuity.

The longest host-only mixer queue interval is 49.126891542–50.145870620 s.
During that exact interval all eight CPUs record only KernelBusy periodic
contexts: 80 on BSP and 167–168 on each AP, with zero user/idle/other contexts
and zero other dispatch bookkeeping. The timer path returns before policy
`ShouldPreempt` for these busy kernel contexts. This is evidence of absent
observed preemptible execution, not evidence that MLFQ selected competitors
over the mixer. The largest BSP arrival gap is 96.813 ms and AP gaps are
17–28 ms; BSP wake publication alone does not explain the one-second queue.

Host vCPU schedstat deltas also show substantial runnable wait. These aggregate
host counters do not localize an individual guest stall. The retained peer
maximum-gap RIPs resolve in the frozen ELF to `SchedulerBlockImpl`, immediately
after its final `sti`; they are endpoint samples, not proof of the instruction
executing throughout a gap or during the longest watched queue interval.
Long audio service/wake brackets also include time after dispatch bookkeeping,
so queue delay must not be equated with full wake-to-resume latency.

Artifacts are `q2-delivery-guest-only`, `q2-delivery-host-only`,
`delivery-isolation-verified-results.json` and `delivery-isolation-sources` under
the ignored timing workload root. The independent verifier checks the two
different audio outcomes, staged identities, clocks, worker completion,
affinities, structural gates and the exact worst-queue CPU activity.

Next, retain bounded interrupted RIP/current-task observations while the mixer
is queued and per-CPU timer-handler occupancy, then correlate actual resume
with dispatch. Keep scheduler policy, kernel nonpreemption and HDA safety
guards unchanged until those observations distinguish host-stretched interrupt
handling, ordinary kernel work and task ownership restrictions.

The follow-up queue watch adds `context_samples`, `last_interrupted_rip`,
`last_current_pid`, `last_current_incarnation` and `last_context_kind` to each
retained event's CPU activity row. Kind values are User=0, KernelBusy=1,
Idle=2, Other=3; only interpret them when `context_samples` is nonzero. These
are the last observed endpoint on that CPU during the exact queued interval,
not an execution-time distribution. Periodic IRQs supply the existing saved
RIP; IPIs supply no sample. Recording uses the existing ready lock, adds no
clock reads or stack walks, stores no process pointers, and remains disabled
unless queue timing and a task watch are enabled. Cancellation/reset removes
the context with the queued interval. The snapshot has a compile-time 64-KiB
size bound. Hosted coverage checks last-sample replacement, PID incarnations,
CPU bounds, invalid kinds, enqueue retries, cancellation and reset.

The first enlarged diagnostic kernel faults at boot during policy-switch reset:
aggregate assignment materializes large snapshot temporaries on the boot
thread's stack. Its private image, original serial, eight vCPU register sets
and bounded stack evidence are retained as `q2-ready-context-host`. No workload
result is accepted from that attempt. The correction clears tracker state in
place and copies snapshots directly into caller storage, avoiding nested large
temporary snapshots. The corrected kernel passes all 68 hosted tests, the
bounded compiled-entry audit (12 supported wrappers, 56 rejected mutations;
four return paths remain unsupported), and native boot/policy/watch reset.

The corrected `q2-ready-context-r2-host` repeat again fails audio acceptance:
HDA sample-gap 1,827.343 ms, empty capture, and maximum mixer ready queue
2,339.980 ms. The exact worst interval has only KernelBusy periodic contexts
on every CPU, with a few other dispatches recorded. Its last interrupted
addresses resolve to `SchedulerBlockImpl` or `SchedulerKernelRegionLeave`,
immediately after interrupt re-enable. Other retained intervals also sample
`SchedulerKernelRegionEnter` just after `sti`. These are not spin-loop endpoints
and do not establish lock ownership or where the entire interval was spent.
They strengthen the case for measuring per-CPU handler occupancy and pending
interrupt delivery before modifying policy fairness. Host regression tests
were also run during VM setup; this diagnostic repeat is not a clean
before/after performance comparison. The original two isolation controls above
remain the load comparison.

The repeat's original jobs/VM/router finish normally and independent runtime,
heap, filesystem, saved-data, identity, clock and affinity gates pass. Its exact
kernel is frozen under `ready-context-r2-frozen`, with the original thirteen
control modules. Address resolution and independent failure verification are
retained alongside the controller sources; unresolved user/module addresses
are explicitly left unresolved rather than assigned a kernel symbol.

## Timer-handler brackets and suspended interrupt frames

The first `q2-handler-bracket-host` experiment adds per-CPU wall brackets from
the existing post-GS arrival sample, through the point before
`SchedulerTimerTick`, to that call's return. The prefix excludes the scheduler;
the scheduler bracket includes locks, host delay and possible guest task
suspension. Naked-entry work, return-frame validation and the assembly epilogue
remain outside the measured C body. Default-off `timer peers` controls share
the existing epochs and independent per-CPU try-locks. No MMIO, stack walk or
policy change is added by the probe.

The run reproduces the audio sample-gap stop (3,220.231 ms), empty capture and a
5,339.889-ms mixer queue. Its longest returned handler brackets span that queue
on several CPUs, but their interrupted RIP resolves to `IdleLoop` immediately
after `sti; hlt`. Those frames dispatched useful work and returned much later:
their wall spans cannot be called continuous IRQ execution. Some per-CPU sums
exceed the entire observation span because suspended frames overlap. All
original jobs, shutdown, filesystem and saved-data checks complete; the audio
outcome remains explicitly failed.

The corrected probe adds a saturating per-CPU context-switch commit generation
at all three kernel `context_switch` call sites: normal switch, current-process
exit and first restore. This small counter is maintained even while timing is
off, with each CPU's counter on a separate cache line. The timer token samples
it at entry and return. Changed generations are
classified as switched; saturated/unavailable generations as unknown; migrated
returns are counted separately and excluded from duration aggregation.
`handler_events` now retains the largest sixteen **no-switch** completions of
at least 1 ms. Separate maxima preserve long brackets from all same-CPU returns
so switched frames remain visible rather than silently disappearing.

`adjacent_no_switch_pairs` requires consecutive arrival sequence numbers, a
completed previous frame, both frames without switches, unchanged generation
between them and ordered clocks. `near_returns` counts such gaps at most 100 us;
`sum_between_ns` and `max_between_ns` describe completion-to-next-entry wall
time. That gap includes wrapper/epilogue, intervening execution and host delay.
It is not proof of useful application progress. Clock failures, migration,
reset and out-of-order older returns break pairing. Hosted tests cover these
conditions, same-CPU suspension, unknown generations and bounded retention.
Sequence adjacency means consecutive **recorded** arrivals. A nonzero lifetime
`contended` count can hide intervening IRQ observations/returns, so those pairs
must not be described as proven consecutive hardware interrupts. The counter
is retained even across window reset; it is not silently cleared to certify
completeness.

The `q2-handler-generation-host` follow-up is **not an accepted performance
control**. A read-only runtime query times out; the controller exits, losing the
original host worker's wait status. The original guest job subsequently exits
nonzero because Quake returns zero without the required END marker. Recovery
polls that same operation identity, retains its failure and original host
RESULT, saves diagnostics from the same VM, then shuts it down normally. QEMU
and router exit evidence, filesystem and saved-data checks pass; they do not
erase the missing host exit or game acceptance failure. No replacement job or
VM is started as recovery.

The recovered switch-aware snapshots have no invalid clocks, unknown
generations, saturation or contention drops. The largest completed switch-free
handler wall bracket is 59.327 ms on BSP and 17.005–19.293 ms on APs; switched
frames reach seconds. The mixer queue reaches 8,344.656 ms. No retained largest
switch-free handler overlaps that worst queue, but retention is bounded and
outstanding/migrated frames remain outside those duration results. This does
not rule out cumulative timer work. Recovery considerably extends the window;
whole-window sums or adjacent-gap means must not be presented as loaded CPU
occupancy or a clean policy comparison.

The new diagnostics pass all 68 hosted tests and the bounded compiled-entry
audit (12 supported wrappers, 56 mutation rejections, four unsupported return
paths). These results localize future measurements; they are not a new
scheduler or audio performance improvement. The next useful split is time
through the scheduler's initial ready-lock/runtime-accounting section versus
its remaining work, with an uninterrupted load interval and retained original
worker exit status. Policy defaults and timer frequency remain unchanged.

A guest-only control on the switch-aware kernel completes the demo in
15.206 s with active audio, no native fault or sampled discontinuity, and clean
original jobs/shutdown/filesystem/saved-data gates. Per-CPU lifetime try-lock
drops are 6/2/1/3/1/1/4/3 and remain in its independent result; audio acceptance
is not a claim of complete IRQ observations. In the recovered host failure,
the surviving stream snapshots happen not to latch a discontinuity: the stream
is already removed when recovery samples it. The serial native sample-gap
fault and truncated 3.560-s capture remain independent failure evidence;
absence of a currently reported stream discontinuity does not override them.

The final cache-line-separated build is frozen as
`handler-generation-final-frozen` (kernel
`ee30b43b5b54bf8621a8d8fa046fb008045a108e95a922e88ff48c19e00d2a32`,
with the original thirteen control modules). Its fresh
`q2-handler-generation-final-guest` control completes in 15.222 s with an
active 7.999-s capture, no native fault/discontinuity, original guest job exit
zero and zero live tasks. Runtime/heap/clock/identity/affinity, original
VM/router exits, four filesystem checks and saved-data preservation pass.
Mixer queue maximum is 20.868 ms; this is a single diagnostic control, not a
latency improvement claim. Lifetime per-CPU probe drops are 2/1/1/1/1/0/0/1.
All 68 hosted tests and the bounded compiled-entry audit pass on the final
source. Scripts, final source patch/hashes and independent verifiers are
retained under `handler-generation-sources` in the ignored workload root.

## Initial ready-lock acquisition dominates AP timer spans

The `runtime-stages-frozen` diagnostic adds two default-off samples inside
`SchedulerTimerTick`: immediately after acquiring its initial ready lock and
after runtime/policy accounting plus the ready-watch observation, before
releasing that lock. The no-switch stages are:

- `runtime_acquire_ns`: scheduler-call entry through acquisition and the first
  clock sample, including flag save and ticket wait;
- `runtime_accounting_ns`: clock/runtime/policy/watch work and the second sample
  while the ready lock is held;
- `remaining_ns`: lock release through the rest of the scheduler call.

All are elapsed wall brackets, including host descheduling. Clock-order errors
reject the completed sample. Separate no-switch acquisition/accounting extrema
are retained alongside the existing bounded slow events. Switched frames keep
their timestamps but do not enter these aggregates. Sampling adds two clock
reads only when the peer timing probe supplies a stage pointer; IPIs and the
disabled path do not add those reads. No lock acquisition policy changes yet.

The larger response exposes a pre-existing limitation in `BridgeReply`: its
768-byte buffer silently discards further text/numeric digits. The shared
`BridgeReplyBuffer` now emits bounded chunks when full and continues the same
reply without allocation. A hosted regression runs the actual shared template
through multi-kilobyte fields, maximum decimal/hex values and buffer-crossing
numbers. Native peer responses parse with all stage/extrema fields present.

The private `q2-runtime-stages-host` repeat keeps the previous MLFQ, callback
audio, eight-vCPU and host-affinity settings, with twelve host CPU workers and
four periodic sleepers for thirty seconds. The controller records/retries
read-only observation failures, samples cached audio at most once per second
and retains original host/guest job owners through failure classification.
Guest acceptance is now explicit and separate from audio acceptance. It does
not retry unknown mutating commands or replace a job on observation timeout.

This run completes with original host worker exit zero, Quake START/END and
guest exit zero, no live guest tasks, normal VM/router exits, matching staged
identities, correct runtime/policy ledger totals, heap, clock brackets, four
filesystem checks and saved-data preservation. The independent verifier checks
those gates. Quake's demo observation is 43.387 s. Audio **fails**: native
sample-gap 1,605.318 ms, sampled discontinuity and zero captured frames. The
mixer's longest ready queue is 1,700.507 ms; every CPU's periodic observations
during that interval are KernelBusy. These outcomes remain failures despite
successful lifecycle completion.

Across the recorded no-switch scheduler brackets, the seven APs each accumulate
approximately 27.0–27.1 s in initial acquisition, versus only 5.3–6.0 ms in
accounting. Acquisition is 99.713–99.944% of each AP's measured scheduler span;
individual acquisition maxima are 19.1–24.0 ms. BSP splits approximately
13.51 s acquisition and 13.35 s remaining work, with 3.12 ms accounting.
There are no invalid clocks, unknown switch generations, saturation or
contention drops in these snapshots. The largest retained acquisition events
do not overlap the longest mixer queue; they must not be presented as its
individual lock-owner explanation. Wall-time sums are not CPU consumption.

The evidence supports an **opt-in experiment** deferring only initial
accounting/watch sampling on a contended AP periodic busy-kernel tick. It does
not justify skipping retirement/wake/termination handling or changing the
default. Required gates include genuine ticket-lock try-acquire semantics
(failure leaves no abandoned ticket), unchanged IF, exact elapsed-minus-steal
catch-up at later sample/dispatch/block/CPU-clock boundaries, explicit skipped
watch-observation counters, and matched native load trials with settled
ledger/policy agreement and actual audio/workload outcomes. A smaller measured
acquisition span alone is insufficient acceptance.

The frozen kernel is
`a8de913a360fd140361a2d1db834bc82d53f0bb5aeade898c8b7404903986f53`,
with the same thirteen control modules. All 68 hosted tests and the bounded
compiled-entry audit pass. `runtime-stages-analysis.json` retains stage sums,
maxima and exact overlap checks. Controller/generator, analysis, independent
verifier, final source patch and hashes are archived under
`runtime-stages-sources` in the ignored workload root.

## Opt-in AP accounting deferral

`sched irq-accounting try` enables a narrow experiment; `blocking` restores
the default and `status` reports lifetime per-CPU counters. Only an AP periodic
timer interrupt in a non-idle kernel context may try the initial ready lock.
User-mode ticks, idle ticks, BSP ticks and non-periodic interrupts retain
blocking accounting. Failure reserves no ticket, changes no runtime baseline,
and skips only the initial accounting/watch sample. Existing wake, retirement,
termination and preemption handling continues. Later successful samples and
ownership boundaries charge elapsed-minus-steal service once.

A successful ticket reservation confirms the serving counter before entry.
If an unreserved reader was suspended across a complete 32-bit counter cycle,
it waits for its already-reserved ticket through the original bounded wait and
deadlock diagnostics. It never abandons, reacquires or prematurely releases
that ticket. Thus ordinary failure is nonblocking, but a rare stale successful
reservation may wait. A deterministic hosted test injects that state and checks
exclusion, one release and subsequent lock usability. Mixed blocking/try owners
also exercise exclusion and counter wrap using the production helper.

`attempts`, `successes`, `deferred`, `if_errors` and `stale_ticket_waits` are
saturating lifetime counters. Status rows are independently sampled; compare
settled counters after disabling the mode. `potential_watch_ticks_skipped`
equals deferred attempts, not the precise number inside an enabled ready-watch
interval. Missing observations cannot establish idle time or complete IRQ
coverage. Stage events explicitly mark deferred runtime sampling with zero
accounting duration. These remain elapsed wall brackets, including host
descheduling, rather than CPU consumption.

Two blocking and two try-mode trials, ordered A/B/B/A on the initial
`irq-try-frozen` build, keep eight vCPUs, MLFQ, callback audio and twelve host
CPU workers plus four sleepers for thirty seconds. Blocking mixer queue maxima
are 2,746.411 and 2,260.474 ms; try-mode maxima are 212.095 and 204.997 ms.
Mean observed demo intervals are 39.683 and 39.584 seconds respectively:
there is no demonstrated demo throughput improvement. All four trials fail
audio acceptance with native sample-gap faults; try-mode captures are also
shorter than the requested eight seconds. The queue improvement does not solve
loaded audio delivery.

Initial try-mode attempts/successes/deferred are 41,088/26,037/15,051 and
41,153/24,657/16,496, with zero IF errors. Blocking attempts remain zero.
Original guest and host jobs, CPU-clock API checks, settled runtime/policy
ledger equality, clock-rate brackets including 1x, heap, original VM/router
exits, four filesystem checks and saved-data preservation pass in all four.
A separate try-mode guest-only load with 24 CPU workers and four sleepers
finishes the demo in 15.304 seconds, with an eight-second active capture, no
native fault/discontinuity and a mixer queue maximum of 18.913 ms. There is no
matched same-build blocking guest-only control, so this is acceptance evidence,
not a throughput comparison. Probe contention drops are retained separately.

Remaining limitations include delayed remote CPU-time visibility between
samples, attribution across a concurrent policy switch, and conservative
discard of a larger unsampled gap after an invalid clock sample. General
per-core run queues and broader scheduler lock changes are not implemented.
The experiment remains default-off pending broader mixed browser/game tests.

The final counter-wrap-hardened build is frozen as `irq-safe-frozen`, kernel
`112db7da3ca48172b6283ab605b74827c2c6cbf9607c15f7e002f313dab046cc`,
with the same thirteen control modules. All 68 hosted tests and the compiled
entry audit pass. A fresh blocking/try pair completes original host/guest jobs,
CPU-clock API checks, settled accounting, heap, clock-rate brackets including
1x, normal VM/router shutdown, filesystem checks and saved-data preservation.
Blocking/try mixer maxima are 3,266.220/76.998 ms and observed demo intervals
42.813/40.370 seconds. This single pair does not establish a repeatable demo
throughput gain. AP average acquisition totals fall from 26.253 to 1.710
seconds; remaining scheduler wall time rises from 0.083 to 2.888 seconds.
Try counts are 39,558 attempts, 23,145 successes and 16,413 deferrals; both
IF errors and stale-ticket fallback waits are zero. The injected hosted test
covers the rare fallback path. Lifetime probe contention drops are
0/0/1/0/1/2/0/3 in the candidate and zero in the baseline.

Both final trials still fail audio acceptance. Blocking records a native
1,793.650-ms sample-gap fault and a 1.940-second capture. Try mode records no
native fault or sampled discontinuity, but captures only 59,136 frames
(1.341 seconds) during an 8.004-second host window, despite active guest
publication. The independent verifier originally assumed no native fault
implied a passing full capture; that assumption is removed. It recomputes the
full-window capture/publication gate and verifies that the short capture
remains a failure. Absence of a native fault cannot establish host playback
continuity or timing correctness.

`irq-safe-verified-results.json` retains structural checks separately from
audio outcomes; `irq-safe-comparison.json` retains timing and queue results.
Controllers, independent verifiers, comparison script, source patch and hashes
are archived in `irq-safe-sources` under the ignored workload root. The next
audio investigation must distinguish guest publication, QEMU callback
progress and host capture progress under load, without relaxing the HDA safety
guard or treating a shorter queue as a complete playback fix.

## Aligned run-state trace distinguishes stopped audio from missing callbacks

`q2-irq-trace-try-1` repeats the same final kernel/modules, MLFQ, callback
backend, private silent route and thirty-second bounded host load. It adds
realtime/monotonic anchor brackets and cached stream snapshots immediately
before/after capture. Original jobs, CPU-clock API, settled ledger, heap,
clock-rate, shutdown/router, filesystem and preservation gates pass. Audio
acceptance remains false, with no native fault or sampled discontinuity.

Across the 8.003526-second host capture window, QEMU's HDA run-state trace
records 3.671848 seconds active and 4.331678 seconds inactive. The WAV contains
3.680363 seconds of PCM, only about 8.515 ms more than recorded active time.
The observed realtime/monotonic offset envelope spans 1,023 ns; both endpoint
translations yield the same active total. This observed envelope does not
prove absence of an intermediate clock step. Inactive segments include
978 ms at capture start, 1,115 ms and 1,867 ms interior gaps, and 371 ms at
the end. Running state alone does not prove timely callbacks within active
segments.

The same stream identity/epoch survives across bracketing cached snapshots.
Both snapshots have zero software and in-flight frames, equal submitted and
completed counters, and no discontinuity. Submitted/completed deltas are
125,952 frames each. Driver accepted bytes increase by 503,808 (the same
125,952 stereo-s16 frames), over 120 accepted submissions; unowned reserved
silence increases by 103,680 bytes. Snapshot intervals enclose the capture
with transport uncertainty and are not exact capture-boundary measurements.

QEMU's
[10.2.2 HDA implementation](https://github.com/qemu/qemu/blob/v10.2.2/hw/audio/hda-codec.c)
emits `hda_audio_running` when codec run state changes and updates voice
activity. Brook's `ObserveNativeLocked` stops DMA after its submitted tail
and idle trailer drain; that ordinary path does not latch a native fault.
The measured inactive periods and empty bracketing queues support producer
starvation/draining as an explanation for this short capture. They do not
identify why the producer stalls, prove continuous publication between
snapshots, or exclude callback stalls during active periods.

`aligned-audio-trace.json` retains both clock translations, every intersecting
run-state segment, queue snapshots and publication deltas. The independent
structural verifier remains separate from this alignment analysis. Exact
controllers, analysis, verifier and relevant source snapshots are archived
in `irq-audio-trace-sources`. A subsequent bounded CPU profile targets the
producer during the same load; neither scheduler nor audio defaults change.

The follow-up `q2-irq-producer-try-1` adds a bounded 500-ms CPU/context-switch
profile. It retains the same structural/CPU-clock/lifecycle/filesystem gates,
with audio acceptance still false and no native fault/discontinuity. Its
8.005570-second capture contains 2.220408 seconds of PCM; HDA is active
2.203751 seconds and inactive 5.801819 seconds. Empty, same-epoch bracketing
stream snapshots again show equal submitted/completed deltas (67,584 frames).

The complete profile has 64 CPU samples, 166 context-switch records, zero
reported drops and matching event counts. Of the CPU samples, 21 land at
`SchedulerKernelRegionEnter+0x9c` and 21 at
`SchedulerKernelRegionLeave+0x9b`. Disassembly of the exact frozen kernel
places those addresses in the epilogues just after conditional `sti`; the
ticket-wait loops are at different addresses earlier in each function. This
is consistent with delayed IRQ sampling after interrupt-masked regions. It
cannot establish that those epilogues consume most CPU time, or identify
which protected operation caused an individual producer stall. Raw hit counts
must not be presented as unbiased CPU percentages.

The profile analysis resolves executable module symbols using actual boot
load addresses, the loader's section ordering/alignment and exact frozen
module hashes, with symbol-size bounds; user RIPs remain unresolved. Three
CPU samples resolve to virtio-GPU submission/wait helpers. Context-switch
stacks include the audio worker, network poller and timer dispatch, but this
short biased window does not establish a unique producer bottleneck.
`producer-profile-analysis.json` retains raw counts and outgoing stacks.
A useful next instrument is elapsed lock/wakeup observation for the audio
producer, retaining ownership and host-steal semantics, rather than treating
IRQ sample percentages as a substitute for those measurements.

## Watch the verified DSP writer independently from the mixer

`q2-irq-writer-try-2` identifies the producer before load with existing tools.
A privileged metadata job reads descriptor targets using `readlink`, and a
brief, half-second pre-load file-syscall trace observes twenty 4,096-byte
writes by PID 22 to its verified DSP descriptor 7. Other Quake threads share
that descriptor; ownership is established by observed writes, not descriptor
presence or generic thread names. Tracing is disabled before the bounded
host load. The scheduler watch then binds PID/incarnation 22/22. This remains
a diagnostic cohort, not a matched performance control. Privileged metadata
inspection is not an unprivileged procfs acceptance claim.

Across 2,850 ready-to-dispatch samples (2,482 with deadlines), the producer's
maximum queue wait is 155.245 ms and maximum deadline-to-ready publication
lateness is 190 ms. The largest retained publication delay has a subsequent
21.181-ms ready wait; the separate maximum ready wait has no deadline. These
extrema are different episodes and must not be added as one measured stall.
There are no invalid/saturated watch observations. Only sixteen largest
episodes are retained, with 89 excluded slow episodes, and experimental
accounting deferrals still omit potential periodic context observations.

The 8.005962-second capture contains 2.239274 seconds of PCM; aligned HDA
run-state totals are 2.248377 seconds active and 5.757585 seconds inactive.
Individual inactive periods include approximately 1.49–1.58 seconds. No
native fault or stream discontinuity is reported, but audio acceptance is
false. The observed ready/wake extrema do not by themselves identify the
cause of one continuous roughly 1.5-second producer gap. Time inside blocking
syscalls, waiting on application locks, and executing between write calls
remain unmeasured. Next diagnostics should bracket DSP write entry/return,
its description mutex and backpressure sleeps separately, and correlate those
with observed producer ready/wake intervals. They should preserve actual
return/error/partial-write behavior and label elapsed wall time and steal
separately rather than infer CPU occupation from IRQ sample percentages.

Original game, privileged metadata job, host workers and CPU-clock probe
complete with zero exits and drained guest tasks. The independent verifier
checks the observed writer against descriptor metadata, watch incarnation,
unchanged frozen kernel/thirteen modules, IF/ticket counters, settled ledgers,
clock brackets including 1x, heap, original VM/router exits, all filesystem
checks and twelve-document/four-profile preservation. An optional maps query
after shutdown is rejected during capability inspection; no upload/commit
or guest job occurs. Its local request and explicit unsubmitted disposition
are retained without retry. Source inspection also shows Brook's current
`/proc/PID/maps` only reports approximate heap and stack regions, so it cannot
resolve shared-library sample addresses.

The earlier `q2-irq-writer-try-1` discovery attempt fails before host load: an
unprivileged helper incorrectly tries to open descriptor targets as text and
receives EACCES. The failed job and original controller exit one are retained.
Recovery drains the same original game (exit zero), disables the experimental
mode, cleanly stops the same VM, and verifies filesystem/saved-data gates.
The original outer launcher wait status is unavailable and explicitly null;
no zero exit is invented. That run is excluded from performance acceptance.
The corrected repetition uses descriptor-link metadata instead of opening
the descriptors themselves. `irq-writer-sources` retains both controllers,
recovery, independent verifier, failure disposition and analysis evidence.

## Default-off completed DSP write brackets

`audio producer on|off|reset|status` adds a bounded diagnostic without changing
DSP return values, queue capacity or scheduling defaults. Each completed DSP
branch call records PID/incarnation/stream, requested and returned bytes,
entry/terminal KVM timestamps, description-mutex acquisition time and explicit
backpressure-sleep time. Those two phase sums are disjoint; the remainder
includes conversion/copy/other locks and measurement overhead. All are elapsed
wall time including scheduling and host descheduling, with no CPU/steal
apportionment. The probe makes no clock reads when its per-call token is off.

Per-writer gaps run from the previous recorded terminal measurement to the
next DSP branch entry, including recording overhead, prior syscall tail,
userspace and the next syscall's preflight/fd pinning. They do not prove an
application was executing throughout the gap. Controls invalidate crossing
calls and reset gap continuity; clock errors reject observations. Missing
terminal calls are excluded. Eight writer identities and sixteen largest
slow episodes are bounded explicitly. Writer overflow, saturation and
lifetime diagnostic try-lock drops remain visible; dropped records can inflate
gaps. The recorder never waits for a diagnostic reader.

Hosted tests exercise the actual RAII measurement code with deterministic
clocks: disabled clock-read exclusion, unchanged partial/error results,
disjoint phases, between-call gaps, reset-crossing rejection, missing terminal
results, backwards clocks even when later timestamps recover, independent
incarnations/streams, writer limits, top-event retention and saturation. All
68 hosted tests and the compiled entry audit pass. The frozen kernel is
`40f90fdcdaad0087a06d643ce11da6de3fe22f7afe8aa65c70320261704ef3c4`
with the same thirteen control modules.

The first host-loaded probe (`q2-dsp-spans-try-1`) is a **failed workload**,
not a passing performance control. Quake does not reach END within its
original 110-second limit, then fails to terminate within ten seconds. Its
Python leader exits one while seven descendants remain. The original
controller eventually exits one; original host-worker and outer-launcher
wait statuses are unavailable and explicitly null, despite the surviving host
RESULT. Recovery preserves all eight CPU register snapshots, probe/heap
state, explicitly kills the verified game PID and drains the original job.
The same VM/router then shut down normally and filesystem/saved-data gates
pass. One register snapshot lies in the GPU submission pause helper; it does
not establish a unique stuck command or prove the new probe unrelated to the
stall.

Completed-call data from that failed run have no invalid clocks, contention
drops, writer overflow or saturation. The maximum inter-record gap is
6,280.105 ms, maximum completed branch duration 2.468 ms, maximum description
mutex wait 0.289 ms and explicit backpressure sleep total zero. This locates
the observed long gap outside the measured DSP branch; write preflight/fd
pinning, other syscalls, application waits and userspace remain candidates.
The independent failure verifier preserves failure classification while
checking probe arithmetic, original job drain, settled ledgers and clean
inner VM/router/filesystem/preservation evidence. It does not invent missing
wait statuses or convert the timeout into acceptance.

A guest-only repetition (`q2-dsp-spans-guest-try-1`) completes the demo in
15.206 seconds, with 24 guest CPU workers/four periodic sleepers and clean
original jobs, CPU-clock, ledger, heap and lifecycle/preservation gates. Its
391 measured writes have a maximum branch duration of 0.996 ms, mutex wait
0.870 microseconds, gap 73.764 ms and no backpressure sleeps or probe losses.
The same kernel then passes all eight existing DSP control cases with the
probe on: fault-before, signal-before, signal-after, reset, pause,
format-before, fault-after and two-writers. The control diagnostic reaches
its eight-writer bound and reports seven overflow observations; it is not a
complete diagnostic trace of every control writer. Native return/control
acceptance remains independent of that deliberately bounded coverage.

That repetition's eight-second audio capture crosses normal demo shutdown: it
contains 7.561 seconds of PCM and the aligned HDA trace turns off only for the
final 456.812 ms, with no native fault/discontinuity. Full-window audio
acceptance correctly remains false. A six-second capture repeat on the same
frozen kernel addresses this fixture overlap; it does not relax the audio
safety or full-window capture criteria.

The six-second repeat (`q2-dsp-spans-guest-try-2`) completes the demo in
15.192 seconds and passes the full-window capture gate: 6.019773 seconds of
PCM during 6.005786 host seconds, with HDA active throughout, no native
fault/discontinuity, and no publication/clock safety failures. All original
guest/CPU-load/CPU-clock/control jobs, settled ledger/heap/clock gates, original
VM/router exits, filesystem checks and saved-data preservation pass. Its
341 observed writes have maximum branch/mutex/gap times of 0.910 ms,
0.606 microseconds and 71.721 ms, with no backpressure sleeps, invalid records,
writer overflow, saturation or contention drops. All eight DSP return/control
cases pass again; their separate bounded diagnostic still reports seven
writer-overflow observations. This validates diagnostic integration and
return semantics, not a host-loaded performance improvement or a fix for the
historical tempo symptom.

`dsp-producer-guest-verified-results.json` keeps both guest repetitions and
their distinct capture outcomes. `failure-verified.json` keeps the rejected
host-loaded workload separate. Exact source snapshots, patch, controllers,
recovery, verifiers, test/audit evidence and hashes are archived in
`dsp-producer-sources` in the ignored workload root. The next measurement
should distinguish the leading write preflight/fd-pinning interval from the
rest of the inter-branch gap and observe other blocking syscalls by the
verified producer, without attributing all that gap to userspace.


## Selected-thread syscall and return-phase brackets

`sched syscall watch PID INC` enables a default-off diagnostic for one thread
incarnation. `sched syscall status`, `reset` and `off` inspect or control it.
The common C dispatcher and `SyscallCheckSignals` have separate `dispatch`
and `return_check` phase identities. Counts represent measured phases, not
user syscall counts. Histogram keys include both raw syscall number and phase,
so even an invalid `UINT64_MAX` syscall remains distinguishable. Results are
raw C return values; `negative_results` does not claim errno semantics.

The recorder uses short IRQ-masked atomic-flag try sections with no ticket
reservation, waiting or callbacks while held. Stable-disabled/nonselected
entries do not read the clock. A selected actor is checked again at completion.
Epoch/sequence checks exclude stale completions after reset, replacement or
control changes. Failed record acquisitions have an explicit lifetime counter;
the next observation clears both preceding-end continuity and any active gap.
A reader/control busy reply explicitly reports no control effect, permitting
retry of that reply without assuming an unknown transport failure had no effect.
Asynchronous exit may leave an unmatched entry; this is not a liveness query.
There are at most sixteen histogram kinds and sixteen retained slow events,
with explicit overflow/exclusion/saturation counters.

Elapsed brackets use the KVM-backed audio clock and include scheduling and
host descheduling. Remaining gaps include recording/assembly overhead,
interrupts, scheduler and host delay, and application execution. Neither phase
nor gap is a CPU-time attribution. Return checks include the ordinary
`SchedulerKernelOperationBoundary()` and signal/restart processing; their raw
results may differ from common-dispatch results. No assembly entry/return
contract, scheduler default, quantum or audio guard changes here.

The initial dispatch-only run (`q2-syscall-spans-try-1`, frozen kernel
`70cbbffd2c0699e851e506afdf350d5680d906544cef0b9392e3cb2e44d5f338`)
completes the controlled demo in 41.993 seconds under the bounded host load.
Its selected DSP writer has 839 entries/838 completions and an unmatched normal
thread exit. The 418 poll calls total 14.512 seconds, with a 1.244-second
maximum; 418 writes total 98.110 ms, maximum 75.955 ms. Its 4.077-second maximum
gap is outside the common dispatcher, but the original measurement also
excluded return-side signal/scheduler checks. It therefore cannot identify
that gap as userspace time. Audio acceptance fails; workload, CPU-clock,
ledger/heap, original VM/router exits and filesystem/preservation checks pass.
The exact original source and verifier remain in `syscall-spans-dispatch-sources`.

The phase-separated run (`q2-syscall-return-try-1`, frozen kernel
`ef6f0c9799b0bdddb035c4cd37fbd3fd419fc70942cb844bc96b8b748b646d2f`)
uses the same thirteen driver modules and host-load fixture. It completes the
demo in 43.111 seconds, with 1,946 entries/1,945 completed phases and only the
normal unmatched exit. There are no invalid, incomplete, contention,
continuity-break, kind-overflow or saturated observations. Return checks have
a maximum duration of 11.461 ms, while the largest remaining inter-phase gap
is 5,749.708 ms, preceding a successful 4,096-byte DSP write. Thus return checks
do not account for that run's largest gap. Poll dispatches total 15.347 seconds
and reach 789.510 ms. Write dispatches reach 125.908 ms; their DSP branch
submeasurement reaches 62.415 ms with zero explicit backpressure sleep.
These distinct extrema are not additive parts of one identified episode.

The six-second host-loaded capture contains only 0.980 seconds of PCM. Its
aligned HDA trace is active for 0.966 seconds and inactive for 5.038 seconds;
audio acceptance remains false despite no native fault or stream
discontinuity. All original jobs, CPU-clock/ledger/heap checks, original
VM/router exits, filesystem checks and saved-data preservation pass. The
independent verifier retains the audio failure separately from those passes.
This is diagnostic evidence, not a host-load performance fix or a resolution
of the historical fast-tempo report. The next attribution needs to cover the
remaining return/entry assembly and runnable/descheduled/application intervals
without relabelling the entire unmeasured gap as user execution.


The same phase-separated kernel passes the guest-only repeat
(`q2-syscall-return-guest-try-1`) with 24 bounded CPU workers/four periodic
sleepers. The demo takes 15.195 seconds; 1,478 entries/1,477 completed phases
have only the normal unmatched exit and no invalid/incomplete/dropped,
continuity-break, kind-overflow or saturated observations. Maximum remaining
gap is 0.191 ms, maximum return check 1.041 microseconds and maximum poll
66.521 ms. Its six-second capture contains 5.979 seconds of PCM and passes the
unchanged full-window audio criteria. All eight DSP control cases, original
jobs, CPU clocks, ledgers/heap, original VM/router exits, filesystem checks and
saved-data preservation pass. The separate DSP-control diagnostic still has
its explicitly bounded writer coverage; it is not a complete trace of all
control writers.

The new model/wrapper regressions cover disabled/nonselected clock avoidance,
identity/epoch/sequence rejection, abandoned and backward records, result
preservation, reset crossing, distinct phases for arbitrary raw syscall
numbers, negative results, capacity/saturation and continuity invalidation
between or during records. All 68 hosted tests pass. Compiled entry auditing
passes its twelve supported templates and rejects all 56 injected mutations;
four unsupported templates remain outside that static audit's scope. Astra's
read-only review found no remaining blocking diagnostic issue after the phase,
negative-result naming and dropped-record continuity fixes.

`syscall-return-sources` archives exact sources, patch, controllers, independent
verifiers, frozen kernel/driver manifest and validation evidence. Results are
kept separately in `syscall-return-verified-results.json` and
`syscall-return-guest-verified-results.json`. Host contention remains an open
failure; neither the quiet control nor diagnostic coverage establishes broad
browser acceptance, fixes the historical speedup, or justifies changing the
default scheduler.


## Correlating remaining producer gaps with context switches

Two further private host-load runs retain the same phase-separated kernel and
thirteen driver modules, adding a five-second existing CPU/CS capture:
`q2-syscall-profile-try-1` and `q2-syscall-profile-try-2`. They complete the demo
in 41.734 and 42.594 seconds. All original workload/load/CPU-clock jobs,
ledger/heap/clock checks, VM/router exits, filesystem checks and saved-data
preservation pass; both full-window audio gates still fail without native
faults or stream discontinuities. The captures contain respectively 1,219
samples/5,311 CS events and 679 samples/3,365 CS events, with matching event
counts, zero reported drops and successful exact-kernel symbolization.

The verified DSP writer has six user samples in the first profile and sixteen
in the second, all at `0x10217922`. Kernel samples are mostly poll/block and
kernel-region enter/leave paths. Kernel-region samples at the post-STI
instructions remain a biased observation, not a measure of ticket spinning.
The first profile has twenty-two additional writer kernel samples; the second
has nineteen. These are delivered-IRQ counts, not percentages of CPU time.

Profile ticks are converted using the recorded pvclock origin and the
capture's global start tick. Correlation retains the full one-millisecond
quantization interval and excludes records touching a gap boundary. A sample
records an interrupted frame after interrupt entry; descheduling or IRQ-prefix
work can separate that frame from the record time. CS events mark scheduler
bookkeeping before the physical switch, not proof of completed dispatch.
These limitations apply even with zero reported ring drops.

In the second capture, an observed 1,321.415-ms inter-phase gap contains nine
same-address user samples and nineteen outgoing writer CS records, seventeen
with timer-handler stacks. Another 1,151.764-ms gap contains five same-address
user samples and twenty-five outgoing records, twenty-four from timer stacks.
The first capture independently has repeated timer switch-outs during its
1,994.408-ms and 581.444-ms gaps. This fits repeated preemption before useful
user progress; it does not establish the decision reason or attribute every
millisecond of a gap. The third capture below verifies the libc mapping independently, rather
than relying solely on the matching page offset of its syscall-return
instruction.

Astra's read-only source review rules out moving `sliceStartTick` as a sufficient
experiment: these are MLFQ runs, and its expiry uses retained `usedNs` after
service observations begin. Incoming runtime ownership precedes the physical
switch and post-switch drain; kernel/IRQ work is charged to the owner, minus
reported host steal. MLFQ may preempt either for exhausted service budget or a
queued higher-priority task, and it retains overrun debt. Consequently the
next bounded diagnostic should retain the actual decision reason, used service,
quantum/priority, dispatch generation and interrupted RIP, alongside service
and timestamps at dispatch and after post-switch draining. Only then can an
experiment separate debt/charged kernel work from higher-priority arrivals.
Renewing budgets at dispatch, dropping debt, moving accounting ownership
without preserving intervening service, or enabling arbitrary kernel
preemption would change semantics without proving the cause.


A third fresh run (`q2-syscall-profile-try-3`) preserves loader output in its own
file, independently of the game output. The second run's mixed output does not
retain the initial libc map and is not treated as a successful address
verification. In the third run, the loader explicitly reports libc at
`0x10171000`, needed by the actual Quake executable. The exact libc extracted
from that run's stopped disk has `__syscall_cancel_arch_end` at `0xa6922`:
`syscall` at `0xa6920`, then `ret` at `0xa6922`. Their sum is exactly
`0x10217922`, matching all eight writer user samples in that capture.
`user-address-resolution.json` retains the map, ELF hashes and scope. This is
loader/disk-image resolution, not a direct instruction-retirement measurement;
earlier same-address captures are consistent but lack their own preserved
startup libc maps.

The third demo takes 41.955 seconds. Its profile has 524 samples/1,159 CS events,
matching counts and zero reported drops. All original workload/load/CPU-clock,
ledger/heap/clock, VM/router, filesystem and saved-data checks pass again;
audio acceptance remains false without native faults/discontinuities. Thus
three profiles reproduce host-load failure on the unchanged frozen kernel.
The verified syscall-return location strengthens the repeated-preemption
hypothesis, but neither proves its decision reason nor resolves the historical
fast-tempo symptom. A bounded decision/service trace is the next experiment;
there is no new scheduler behavior or default change in this evidence update.

Exact controllers, analysis and verification scripts, kernel/driver identity,
profile/correlation/loader-resolution results and hashes are archived in
`syscall-profile-sources` under the ignored timing workload root. The three
outcomes are retained in `syscall-profile-verified-results.json`.


## MLFQ decision reasons and first post-switch drain

`sched decision watch PID INC | off | reset | status` controls a separate,
default-off selected-thread diagnostic. Native MLFQ reports positive
`ShouldPreempt` decisions through a matched kernel observation import. The
policy table/layout and module ABI remain unchanged; staging must pair the new
MLFQ module with a kernel exporting that import. Old kernels reject its missing
import rather than silently loading an incompatible diagnostic module. Hosted
module tests supply an exported observer stub and exercise the actual shared
library's service-expiry, legacy-wall and higher-priority reasons, including
expiry taking precedence when both conditions are true. No budget, priority,
queue selection or ledger mutation is added by the observer.

Decisions retain priority, quantum, total used service, reason, first queued
higher-priority level when applicable, interrupted RIP, periodic/IPI provenance,
CPU and commit generation. Per-CPU dispatch metadata is bound to epoch,
PID/incarnation and the destination CPU's next commit generation. Only the
first matching post-switch drain marks completion; later timer drains cannot
move that timestamp. Resumption on another CPU reads that CPU's current
metadata. Epoch changes invalidate old pairs. Raw wall/steal samples bracket
incoming ownership to drain completion without another ready-lock acquisition
or ledger charge. Their difference is not ledger-corrected service: delayed
steal updates and retained policy debt remain separate considerations.

The recorder uses short IRQ-masked atomic-flag try sections, no waiting,
allocation or logging while held, explicit contention and invalid/unpaired
counters, and the largest thirty-two total used-budget observations. It does
not favor largest post-dispatch overhead. Counts cover valid positive policy
decisions; selection, termination/state checks or a missing eligible peer may
prevent a physical switch afterward. A record is not proof of a completed
switch, user progress or causality for a particular audio fault.

The first host-loaded native run (`q2-decision-trace-try-1`, frozen kernel
`6f637473059b0ad81d3e24f9a3445a7ba72391f1cf39f220d446bad2adbc6706`)
changes only the kernel and MLFQ observation module; the other twelve driver
hashes remain identical to the preceding control. The demo completes in
37.404 seconds. All original workload/load/CPU-clock jobs, ledger/heap/clock
checks, VM/router exits, filesystem checks and saved-data preservation pass.
Audio acceptance fails with a native sample-gap fault and stream discontinuity;
the HDA position sample itself spans roughly 3.506 seconds. This remains a
failure, not an accepted performance improvement over the earlier runs.

The selected DSP writer has thirty-eight valid positive decisions: seventeen
service-budget expiries and twenty-one higher-priority requests, with no legacy
wall expiries, invalid records, drops, saturation or unmatched dispatch/drain
pairs. All thirty-two retained observations are paired. Thirty-one report RIP
`0x10217922`; one is an IPI with unknown interrupted RIP zero. The largest used
budget is 62.829 ms against a 40-ms quantum. Of fifteen retained higher-priority
requests, fourteen interrupt priority 3 for queued priority 1 work, and one for
priority 0. These are retained-event counts, not the priority breakdown of all
twenty-one requests.

Retained dispatch-to-first-drain wall brackets reach 10.044 ms, while the
largest total budget and the longest later delay are different observations.
They must not be added or interpreted as the budget consumed solely by that
dispatch. Both service expiry and higher-priority competition are now directly
observed; a wall-slice-start adjustment alone would not address either source.
The trace still does not establish how much of the long no-user-progress gap
is IRQ/lock activity versus host descheduling, nor which queued task wins each
higher-priority request. A bounded behavioral discriminator must preserve
termination/stop handling, accounting and retained debt if tried next.


The first guest-only control (`q2-decision-trace-guest-try-1`) completes in
15.278 seconds with zero positive writer decisions and zero trace errors/drops.
All eight DSP return/control cases and original workload/load/CPU-clock,
ledger/heap/clock, VM/router, filesystem and saved-data checks pass. Its
six-second capture crosses normal game exit: guest snapshots bracket
45.706–51.795 seconds while END is observed at 50.277 seconds. HDA becomes
inactive for the final 1.308 capture seconds, and only 4.701 seconds of PCM are
stored. The full-window audio gate remains false. A three-second capture repeat
on the same frozen kernel addresses this fixture overlap without relaxing the
full-window audio or native safety criteria; the original result stays retained.

Astra reviewed a possible next bounded behavioral discriminator: an opt-in,
MLFQ-only deferral of at most one positive periodic user-mode preemption decision
per actual dispatch generation. It must apply only to a still-Running task
under the existing ready lock, preserve all service, debt and priority, and
skip expiry handling on the deferred decision. IPIs, stopped/terminal tasks and
saturated generations must be excluded. Consumption must survive counter resets
and control toggles. Tests must cover expiry and higher-priority reasons,
negative decisions before the first positive, changed-reason recomputation,
migration/new commits and exact budget preservation. A first positive decision
can occur well after useful execution, so this would test sensitivity to
suppressing a decision, not by itself prove a return-without-progress loop.
Dispatch age, reason, RIP and prior user boundaries would be needed for that
interpretation. A faster demo alone would not resolve the independent multi-
second HDA sampling interruption. This behavior has not been implemented or
selected as a default by the diagnostic work.


The three-second repeat (`q2-decision-trace-guest-try-2`) completes the demo in
15.205 seconds and passes the unchanged full-window audio criteria: 3.000
seconds of PCM over 3.006 host seconds, no native fault/discontinuity, zero
positive writer preemption decisions and zero trace errors/drops. All eight
DSP control cases, original jobs, CPU-clock/ledger/heap checks, VM/router exits,
filesystem checks and saved-data preservation pass again. Both guest repeats
remain separately recorded; the earlier shutdown-overlapping capture remains
a failed full-window result. Quiet integration is not host-load acceptance.

All 68 hosted tests pass, including actual shared-module observer semantics
and the reducer's identity/epoch rejection, reason/threshold validation,
metadata clock order, missing pairs, capacity, reset and generation saturation
cases. The compiled entry audit passes twelve supported templates and all
56 mutation rejections, with four unsupported templates still outside scope.
Astra found no blocking guard, migration/pairing or accounting safety issue in
the observational patch. The exact source snapshot, patch, controllers,
independent verifiers, frozen manifest and native evidence are retained in
`decision-trace-sources`; host and guest outcomes remain separate in
`decision-trace-verified-results.json` and
`decision-trace-guest-verified-results.json`. No scheduler default or timing
safety limit changes are justified by this diagnostic acceptance.


## Bounded one-decision deferral experiment (withdrawn)

A private experiment used a default-off MLFQ-only allowance of at most one
positive periodic Running user decision per CPU commit generation. It skipped
expiry notification on that decision, retained all accounting/debt/priority,
and excluded IPIs, stopped/terminal tasks and saturated generations. Counter
resets and mode toggles could not renew the allowance. Decision records added
mode/deferral and prior periodic user-boundary flags; existing dispatch samples
provided age. Both pure-model exclusions and the actual shared MLFQ module's
unchanged budget/debt and changed-reason recomputation tests passed. All 68
hosted tests and the supported compiled entry audit passed; Astra found no
blocking preview/actual-deferral safety inconsistency.

Four fresh private host-load trials used the same frozen experimental kernel
`48b1b2c5d9a4cf4ebdabdf858b332cd15da451f26f3205cd0c199c30e9d30b91`,
all thirteen unchanged preceding driver modules, identical load/affinity and
silent audio, in off/on/on/off order:

| Trial | Accepted game workload | Observed demo | Audio accepted | Native sample-gap faults | Global deferrals |
| --- | --- | --- | --- | --- | --- |
| off-1 | no: missing END | unavailable | no | 0 | 0 |
| on-1 | yes | 41.802 s | no | 0 | 787 |
| on-2 | yes | 41.566 s | no | 0 | 943 |
| off-2 | yes | 38.325 s | no | 1 | 0 |

The first game engine exits zero, but its required END marker is absent and
the original Python workload exits one. It is a failed workload, not a timing
control. The original strict verifier rejects it; a separate classification
verifier checks that exact negative outcome while preserving all remaining
CPU-clock, trace, lifecycle, filesystem and saved-data gates. No END interval
is invented and the original gate is not relaxed. This limits the completeness
of the alternating comparison.

All original host-load/CPU-clock jobs drain successfully; all four original VM,
router, filesystem and saved-data checks pass. The enabled controls prove the
allowance was exercised: retained selected-writer events include ten and seven
deferrals, respectively; zero and one of these had a prior periodic user
boundary in that dispatch. These are retained-event counts, not a complete
writer distribution. They are compatible with deferring early-return
preemptions, but there is no full-window audio success and no demonstrated
workload speed improvement. Absence of a native fault in those two runs does
not prove timely host audio or a fix for the earlier multi-second interruption.

The unsuccessful behavior change was withdrawn from the working source after
archiving its exact patch/header, model/module tests, frozen image manifest,
controllers, independent verifiers and all outcomes in `preempt-grace-sources`.
The private immutable experimental capsule remains available for reproduction;
regular source and the live-review baseline retain committed scheduler behavior.
`preempt-grace-summary.json` and `preempt-grace-classified-results.json` preserve
the distinct negative and positive workload outcomes. This experiment does not
rule out preemption as a contributing mechanism, establish uniform IRQ/host
execution attribution, or finish the broader timing/scheduling goal.

## KVM lock waiting: source audit and next measurement

Source inspection on 2026-10-20, after withdrawing the deferral experiment,
separates three mechanisms that must not be treated as one feature:

| Mechanism | Brook implementation | Remaining evidence |
| --- | --- | --- |
| Elapsed time | Per-vCPU KVM clock registration/readers in `kvmclock.cpp` | Existing clock comparisons do not measure lock-holder scheduling |
| Host steal accounting | Per-vCPU MSR `0x4b564d03`, versioned bounded reads | Cumulative steal does not identify which holder or ticket successor delayed a lock |
| Virtualized lock waiting | `SchedLockWaitForTicket` uses `pause`, with IF cleared before reserving a ticket | No KVM wait/kick or directed-yield implementation found in kernel/drivers |

`SchedLock` contains only `next` and `serving`. It does not publish the owner
CPU or a ticket-to-waiter CPU mapping. `KvmStealRecord` includes the ABI's
`preempted` byte, but Brook does not read it. The shared ready, all-process and
PID locks use the same waiting primitive. The raw release before a context
switch deliberately leaves IF clear; that dispatch invariant must survive any
replacement. The spin limit is an iteration count, not a measured elapsed-time
deadline; its nearby cycle estimate is not an observed timeout guarantee.

KVM documents distinct feature bits for steal time, PV unhalt and PV scheduler
yield. Guest userspace configuration can mask them, so `-enable-kvm -cpu host`
in `run-qemu.sh` and successful clock registration do not prove that the latter
two are advertised. Record the actual guest feature leaf before enabling either.
See [KVM CPUID](https://docs.kernel.org/virt/kvm/x86/cpuid.html) and
[steal-time ABI](https://www.kernel.org/doc/html/latest/virt/kvm/x86/msr.html).

Upstream Linux pairs conditional lock waiting with a CPU kick through its PV
queued-spinlock protocol. Its KVM adapter uses APIC IDs for kicks and separately
uses directed yield after a call-function IPI to a preempted target. These are
different operations, not evidence that inserting a generic yield into Brook's
ticket loop is sufficient. See the [KVM adapter](https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/kernel/kvm.c),
[PV queued lock](https://raw.githubusercontent.com/torvalds/linux/master/kernel/locking/qspinlock_paravirt.h)
and [hypercall ABI](https://docs.kernel.org/virt/kvm/x86/hypercalls.html).

The next diagnostic should capture feature availability and sampled lock wait,
holder and next-ticket CPU identities without acquiring another scheduler lock.
Include holder release-to-successor-acquire gaps: a descheduled ticket successor
can stall progress even after the previous holder releases. Classify unavailable
or raced identity samples explicitly. Compare host wall time and per-vCPU steal
over matching intervals; retain the existing IRQ-accounting-try control. The
prior lock-wait profiles motivate this hypothesis but do not prove it.

Only after that attribution should a default-off wait/wake experiment be tried.
It needs a publication/check/sleep/kick protocol that cannot lose a wake, exact
APIC-ID mapping, feature-disabled fallback, ticket wrap handling, nested-lock
and IF-state tests, and the existing pick/claim/off-CPU ownership gates. A
reserved ticket cannot be abandoned. Simply replacing `pause` with `hlt` while
IF is clear has no established wake guarantee in Brook. Validate unchanged
fairness and accounting plus mixed-workload/audio latency under quiet and
bounded host contention; keep per-core queues and EEVDF as separate comparisons.
No lock algorithm, hypercall, scheduler default or live-review image is changed
by this source audit.

## Saved-profile check: interrupt delivery at unlock boundaries

An independent offline check of all three `q2-syscall-profile-try-*` captures
uses their exact frozen kernel (`ef6f0c9799b0bdddb035c4cd37fbd3fd419fc70942cb844bc96b8b748b646d2f`),
verifies exported sample/switch counts and zero drops, and decodes the original
ELF instruction bytes. Three frequent leaves are exactly the first possible
maskable interrupt boundary after an `sti` and its following instruction:

| Function | STI offset | Instruction in interrupt shadow | Sampled offset |
| --- | --- | --- | --- |
| `SchedulerKernelRegionEnter` | `+0x97` | `add $8,%rsp` | `+0x9c` |
| `SchedulerKernelRegionLeave` | `+0x96` | `add $8,%rsp` | `+0x9b` |
| `SchedulerBlockImpl` | `+0x287` | `test %r14b,%r14b` | `+0x28b` |

The [Intel instruction reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf)
describes the delayed interrupt recognition following STI. Brook samples the
interrupted RIP from its LAPIC ISR, so a timer held pending while IF is clear
can be observed at this later boundary. It cannot sample the preceding
IF-cleared ticket loop directly.

These three exact leaves account for 289/1219 (23.7%), 200/679 (29.5%) and
271/524 (51.7%) delivered samples. Each capture contains boundary samples on
all eight vCPUs. This establishes a widespread exposure to delivery-boundary
bias, not CPU occupancy in the named functions, the duration of preceding
lock waits, or proof that every such interrupt was pending during a lock.
Other leaves may also be biased. Do not subtract these counts and label the
remaining distribution an unbiased CPU profile.

The earlier timer-stage durations independently measure waiting; these profile
counts cannot replace or apportion them among lock holders and ticket
successors. Per-CPU samples alone therefore cannot answer the proposed
holder-descheduling question. `profile-irq-boundary-analysis.json` retains raw
profile hashes, per-CPU counts, exact STI/shadow/resume instruction receipts
and the scope limitation; `profile-irq-boundary-sources` preserves the checker.
No VM restart, kernel instrumentation or scheduler behavior change was needed.

## Read-only KVM capability and preemption-hint query

`sched kvm status` now reports raw KVM CPUID feature/hint words, separate
advertised steal-time/PV-unhalt/PV-yield booleans, the mask of registered steal
records, and independently sampled per-vCPU preemption hints. It also reports
that Brook implements neither PV lock waiting nor PV scheduler yield. This is
an on-demand debug-worker query: it acquires no scheduler lock, charges no
runtime ledger, issues no hypercall/MSR write, allocates no storage and never
clears the host-owned hint byte. It is not a new panic-safe query guarantee.

Only bit zero of the shared byte contributes to `preempted_hint_cpu_mask`;
other bits are reported separately and do not imply preemption. The byte's ABI
offset is asserted. Unlike cumulative steal time, this byte is read without a
versioned time snapshot, matching its independent hint semantics. Querying all
CPUs is not simultaneous. A zero bit is not proof a vCPU is running: reporting
can be unsupported or incomplete. These masks are evidence for a later lock
diagnostic, not authorization to skip tickets, halt, or alter fairness.

The hosted test drives the actual query with deterministic CPUID and registered
records. It checks feature/hint preservation, absence of KVM, missing records,
unregistered CPUs, zero CPU count, a clamped 64-CPU limit including CPU 63,
independence from an odd steal-time version, exclusion of other byte bits from
the preemption mask, no writes to those bytes, and a null output. This verifies
the diagnostic's handling, not what the current VM advertises; that requires a
new-kernel guest capture. The prepared live-review image remains unchanged and
does not include this command.

The final diagnostic kernel builds in both symbol-table passes; all thirteen
module imports resolve. All 68 hosted suites and the compiled entry audit pass
(twelve supported templates, 56 mutation rejections, four unsupported templates
still outside scope). Native feature availability remains unmeasured for this
kernel; build success does not establish PV-unhalt or PV-yield support.

One full-suite repeat exposed an existing schedule-dependent wake-scan test
assertion. With failure-only counters added, a one-host-CPU replay reproduces
`consistent=1 active=0 completed=10000 bsp=10001 ap=0 invalid=0`: every stress
claim won by the BSP, while all APs exhausted their bounded failed attempts.
The nonblocking primitive does not promise an AP winner within such a loop.
The corrected test gives each real worker a controlled completion before
releasing all workers into the original 10,000-attempt contention phase. It
retains overlap, stale/reentrant ownership, non-revocation, accounting and clock
checks, requires all eight workers to complete, and reports counters on failure.
This covers AP execution deterministically; it is not a fairness or starvation
guarantee. The corrected test passes 100 single-host-CPU and 100 normal repeats,
then the full suite passes. The original failed suite log, diagnostic failure
receipt and repeat results remain preserved alongside the new kernel evidence.

### Quiet native capability acceptance

A fresh six-disk private clone boots the diagnostic kernel
`090b153ef86aca1324710cfd158f44e59c08fb8c5bda2252ba2b8747dac08311`
with thirteen matched ABI-34 modules, eight vCPUs, 6 GiB RAM, host affinity
0–13, headless Venus display, `AUDIODEV=none`, MLFQ and the explicit
IRQ-accounting-try control. Its wrapper has a hard 300-second timeout; the
test requests an orderly shutdown immediately after the read-only queries.
It does not use or start either prepared persistent live-review image.

The guest reports feature word `0x01007efb`, hint word zero, advertised steal
time, PV-unhalt and PV scheduler yield. All eight clock and steal records are
registered (`0xff`); the clock source is pvclock with zero rejected samples.
Runtime diagnostics use stable pvclock and report no invalid, backwards,
clamped, stale or saturated samples. Both sampled preemption-byte masks are
zero in this quiet snapshot. This proves feature advertisement and functioning
query transport, not hint completeness, lock-holder descheduling, contention
performance or an audio fix. Brook still implements neither PV wait nor yield.

The heap check passes. Shutdown acknowledges the final guest sync and QEMU
exit zero; separately, the controller records the original `Popen.wait()` exit
zero. All four offline filesystem checks and the twelve saved-document/four
original-profile comparisons pass. Evidence is in
`build/reviews/kvm-status-native-try-2-20261020`.

The first attempt, `kvm-status-native-20261020`, is a launch failure, not a
guest boot: the assigned instance 582 produced host-forward port 69437 and
QEMU exited one. Its controller wait, launch log and untouched failure disks
are retained. The two not-yet-started live-review launchers had the same invalid
instance selection; they were corrected to 540/541 (ports 65237/65337), with
their originals retained. The successful diagnostic uses instance 539.

Both launcher entrypoints now reject invalid/noncanonical instances before
artifact preparation. The supported decimal range is 0–542, corresponding to
the forwarding rule `11237 + instance * 100`. Harmless launcher tests cover
out-of-range, empty explicit arguments, leading-zero, expression/text and
oversized inputs plus valid boundary values, without starting QEMU. This is a
launch preflight correction; the diagnostic kernel and scheduler are unchanged.
