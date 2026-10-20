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
