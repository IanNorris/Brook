# Targeted deadline and queue timing

`sched timing watch PID` selects one live task for additional diagnostics in
`sched timing status`. Selection binds its PID and incarnation under the ready
lock. PID zero clears selection; malformed, overflowing and absent PID values
are rejected. Selection clears that target's counters but does not enable
recording or reset aggregate ready-latency counters. The existing
`sched timing on|off|reset|status` controls remain authoritative. Reset clears
target counters and pending intervals while retaining the selected identity.
Recording remains disabled at boot; there is no scheduler policy/default change.

For each completed watched queue interval, the probe records insertion and
dispatch-bookkeeping timestamps from the existing stable-clock samples. A
blocked task with a nonzero `wakeupTick` additionally records that deadline and
the elapsed-millisecond sample taken after ready insertion, before releasing
the ready lock. The task may also wake for another reason before its deadline;
such early wakeups report zero lateness, not proof of a timer expiry.

`max_late_ms` measures positive deadline lateness at that latter sample. It
includes time before ready insertion, wakeup scans, lock acquisition and
insertion itself; it does not identify pure timer IRQ latency. `max_queue_ns`
measures insertion to dispatch bookkeeping. These brackets overlap slightly at
insertion and must not be summed as disjoint intervals. Neither includes the
subsequent context switch and return to the mixer. Compare raw timestamps with
the [mixer service brackets](audio-mixer-service-timing.md) to inspect that tail.
The selected task can also sleep inside driver operations: HDA's `PlayLock`
retries with `SchedulerSleepMs(1)`. Thus watched timed-wakeup counts need not
equal the mixer's ten-ms loop count. Correlate an event inside the loop's
pre-sleep-to-resume interval before classifying it as that loop sleep; events
inside its service bracket may instead describe driver-lock polling.
Deadline ticks and stable-clock nanoseconds have different origins; subtract
deadline/ready ticks only from each other, not directly from nanosecond stamps.

The report retains at most sixteen completed episodes with queue wait or
deadline lateness at least twenty milliseconds, sorted by their larger bracket.
`slow` and `excluded` expose truncation. It also reports all completed sample
counts, timed sample counts, maxima, invalid clocks, counter saturation and
whether a watched queue interval is still pending. Open/cancelled intervals,
entries already queued when recording or selection starts, stale incarnations
and intervals crossing reset do not manufacture completed timing samples.
The retained list is not a full timeline or proof of no starvation.
Pending state is captured diagnostic state; it is not updated while recording
is off and must not be interpreted as a live process-state query then.

All access is serialized under the existing scheduler ready lock, with bounded
storage and no retained process pointers. Collection performs no allocation,
logging or additional locking. The extra elapsed-time read occurs only when
recording is enabled, a selected incarnation is actually being unblocked, and
it has a deadline. Existing aggregate timestamps are reused for queue timing.
Debug output formats a copied snapshot after releasing the lock. This is a
normal debug-thread command, not panic safe. No process layout or module ABI
changes are required.

Hosted tests exercise separate deadline/queue delays, early and untimed wakes,
re-enqueue preservation, cancellation, PID reuse, selection while queued,
reset/on/off behavior, invalid clocks and largest-sixteen retention. All 64 host
suites pass. The native kernel build resolves thirteen modules against 106
kernel exports. Native workload evidence is recorded separately; these checks
alone do not establish the cause of the long audio sampling stall.

## Native mixer evidence

Two fresh private eight-vCPU/six-GiB boots select the audio worker's identity
before enabling counters. Both use the generated local Firefox video/audio
fixture with twenty-four CPU workers for thirty seconds, callback HDA with
requested 20-ms host buffering, and an isolated PipeWire null sink. CPU profiling
and mapping counters are off. EEVDF keeps its 4-ms normal request. The common
kernel SHA-256 is
`28170d463f39bbee45d06f2b5fa2c60aa8cb44f667b910b2c4787917dc976b19`.
Both boots reject malformed/overflowing PID commands, successfully clear the
target with zero, then bind PID 15/incarnation 15 with recording still disabled.

| Run | Policy | Watched dispatches | Maximum deadline lateness at ready | Maximum queue wait | Slow episodes | Excluded |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| rw-e1 | EEVDF | 3,013 | 69 ms | 29.141 ms | 18 | 2 |
| rw-m1 | MLFQ | 2,261 | 90 ms | 48.154 ms | 95 | 79 |

The largest retained EEVDF mixer-sleep episode resumes approximately 82.055 ms
late. Its watched event records 69 ms deadline lateness at ready insertion,
13.014 ms queue wait and 1.836 microseconds from dispatch bookkeeping to the
mixer's resume sample. The subsequent service bracket lasts 34.305 microseconds.
This episode contains substantial delay before ready insertion, not just queue
wait. The integer deadline sample slightly overlaps the queue bracket and has
millisecond resolution; it is not an exact additive decomposition.

The largest retained MLFQ mixer-sleep episode resumes 106.866 ms late. Its
watched event records 90 ms lateness at ready insertion, 17.120 ms queue wait
and 1.431 microseconds from dispatch bookkeeping to the resume sample. Service
then lasts 178.977 microseconds. Thirteen EEVDF and fifteen MLFQ retained mixer
sleep episodes have matching retained watch events. Neither retained list is
complete, so unmatched events are not treated as missing execution.

Both watch and mixer probes report zero invalid clocks and no saturation;
the mixer records no contended-observation drops. Neither boot logs a native
HDA fault. This pair does not reproduce the earlier 217-ms sampling failure.
The source still places blocked-deadline scanning on the BSP. These records do
not distinguish delayed BSP IRQ arrival, time before/in the scan, ready-lock
contention or host descheduling. Measure those before changing timer ownership
or attributing the pre-ready delay to scheduler policy.

The EEVDF workload passes its runtime-accounting gate. The MLFQ workload does
**not**: `clamped_service` rises from zero to four, with all other error counters
zero and all eight clock/steal records active. The ledger refuses negative
service when absolute wall-minus-steal falls below its previous service floor.
Existing diagnostics omit the offending wall/steal values and deficit, so these
four records cannot yet be classified as small sampling skew or a larger
accounting error. The runtime gate remains failed; no reset, override or clean
performance acceptance is substituted. The observed watch data above is
diagnostic evidence from that failed control, not a policy promotion result.
This does not establish that the new probe caused the accounting anomaly.

The original controller exits one after preserving that failure. The same VM's
current clock/runtime/heap/audio state is saved, then normal shutdown is
requested once. Both original launch wrappers and routers exit zero, all four
filesystem checks pass per boot, and twelve-document/four-profile preservation
passes. The failed gate and separate cleanup receipts remain distinct. All VMs
and private services are stopped. Evidence, frozen source hashes, verifier and
failure cleanup script are retained under the ignored
`build/workloads/timing-scheduler-20261020/ready-watch-*` directories.

Next capture the wall/steal/floor values and deficit for service clamping,
alongside BSP timer-arrival and wake-scan intervals. No scheduler default or
timer ownership change is justified by these diagnostic controls.

## Quake host-pressure post-dispatch attribution, 21 October

A fresh eight-vCPU RR control uses the unchanged current kernel
`fc41f3dbf97797d1f349754303090fee0b6e14976f8350ae717e1636a104d8bb`,
original 12-KiB HDA lead, physical callback audio with 20-ms buffering and
normal GL3 menu/attract playback. Eight finite host CPU workers run for twenty
seconds on CPUs 0–7, sharing the verified vCPU masks. Two resetting I/O helpers
may use CPUs 0–15. Mixer, targeted ready/deadline and BSP timer probes are
enabled alongside a bounded read-only host observer; CPU profiling is off.

One retained mixer sleep and one selected PID-15/incarnation-15 watch event
correlate by identity and timestamps within the same sleep interval:

| Checkpoint | Raw stable-clock time |
| --- | ---: |
| Mixer before sleep | 49.276404442 s |
| Deadline, translated from elapsed milliseconds | 49.285570777 s |
| Ready insertion | 49.335547741 s |
| Dispatch bookkeeping | 49.341019827 s |
| Mixer resumes after `SchedulerBlock` | 69.216196148 s |
| Audio service completes | 69.236994611 s |

The deadline is approximately 49.977 ms before ready insertion, with the
documented millisecond quantization. The watched queue interval is 5.472 ms;
dispatch bookkeeping precedes the mixer resume by **19.875176321 seconds**.
The dominant observed interval is therefore after dispatch bookkeeping. This
does not identify physical switch completion, post-switch drain completion,
interrupt activity or host scheduling as its cause. Subsequent service lasts
20.798 ms and is separate from that sleep delay.

The BSP records 5,302 complete timer observations with no clock failures,
contention drops, missing/backwards/excessive steal pairs or migration. Its
largest arrival gap is 96.298 ms, not the multi-second mixer interval. Of 394
slow timer events, only sixteen are retained; exclusions remain explicit.
The watch has 386 complete timed episodes, one retained slow event, no invalid
observations, saturation or exclusions. Runtime accounting is clean. The HDA
stream nevertheless faults with a native sample-gap discontinuity; frame ratios
crossing it are not valid playback-tempo measurements.

Kernel post-STI RIP samples in that watched event belong to other PIDs during
the 5.472-ms queued interval. They do not locate the audio worker during its
following 19.875-second delay. Some consecutive BSP events have less than
two microseconds between housekeeping completion and next entry, but that
checkpoint precedes full C-handler return and does not prove no intervening
switch. The next discriminator is the existing per-CPU timer-peer probe, whose
completion/generation/adjacency checks can distinguish no-switch handler chains.
It still lacks task identity and requires timestamp correlation.

Original app/controller/VM owners exit successfully, heap checks pass and all
four filesystems check cleanly. Evidence and the checked identity/time
correlation are retained in
`build/reviews/unattended-q2-wake-attribution-20261021/wake-correlation.json`
and its sibling `attract-audio-control` directory. No policy or timer default
changes follow from this single diagnostic run.

### Per-CPU handler follow-up

The next unchanged-kernel boot additionally enables `timer peers`. It again
faults audio but shows several different placements of the delay. Five mixer
sleep/watch episodes correlate. One spends 7.089 seconds queued, with 5,987
observed kernel-busy ticks and zero user-mode ticks across all CPUs during
that queue interval. Its dispatch-to-resume tail is only 2.934 microseconds.
Four others have queue waits of approximately 5–11 ms but dispatch-to-resume
tails of 0.972–6.193 seconds. A single queue-only or post-dispatch-only
explanation therefore does not cover every episode.

The per-CPU timer snapshots span approximately 23.7 seconds. Each AP reports
about 19.3 seconds summed over no-switch C-handler brackets, almost all in its
initial ready-lock-acquisition stage. These are wall brackets including host
descheduling and instrumentation, not CPU execution time or continuous spinning.
APs report 2,112–2,132 adjacent no-switch
handler pairs with at most 100 microseconds between C completion and next entry.
Retained events overlapping mixer stalls include sub-two-microsecond gaps,
unchanged switch generations and repeated kernel interrupted RIPs. This
supports repeated timer-handler chains under pressure, without identifying the
audio worker in a particular chain: peer events contain CPU but no PID.

All peer rows share the same epoch, have zero contention/invalid-clock/unknown-
generation counters and no saturation. CPU 0 and CPU 2 report six and one
migrated handler returns respectively; the other APs report none. Migrated and
switched handlers are excluded from no-switch attribution. Retained slow
events are heavily truncated, with explicit exclusions, so they do not provide
a complete stall timeline. Normal app/VM cleanup and all filesystem checks
pass. The source-defined AP accounting `try` mode is the next sensitivity
control; this result does not justify changing the timer frequency or default
accounting behavior.

The raw peer, mixer, watch and runtime snapshots plus derived timestamp matches
are retained under `build/reviews/unattended-q2-irq-attribution-20261021`,
including `irq-correlation.json`.

### Existing AP accounting-try sensitivity control

The same frozen capsule and verified contention setup, explicitly selecting
`sched irq-accounting try`, reduces the maximum mixer late-resume observation
from 7.136 seconds to 176.002 ms in one pair. Every AP exercises the intended
path: 890–1,386 failed accounting tries per AP defer without reserving a ticket;
all IF-error and stale-ticket-wait counters remain zero. AP summed no-switch
initial acquisition brackets fall from roughly 19.3 seconds each to 1.04–1.58
seconds; near-return pair counts fall from roughly 2,100 each to 49–97.
These are measured wall brackets, not a claim about uninterrupted CPU cost.

The selected worker's maximum observed queue interval is 37.000 ms. Fewer
watched ticks cannot demonstrate improvement because failed accounting tries
intentionally skip those observations. Two AP peer slots record one contention
drop each; numerous migrated handler returns are excluded. This run cannot
support complete handler-adjacency coverage. Runtime accounting remains clean,
and there is no native HDA fault or stream discontinuity in the retained run.
All original owners exit successfully and all four filesystems check cleanly.

Application delivery nevertheless completes only approximately 0.090 nominal
audio seconds per guest-clock second during the loaded capture. Eliminating
the multi-second observed worker gaps is not full audio/application acceptance.
Preserve both results. A fresh reversal to blocking admission is required before
attributing the changed long-stall behavior to this mode, and production
defaults remain unchanged. Evidence is under
`build/reviews/unattended-q2-irq-try-attribution-20261021`; the derived pair is
`build/workloads/timing-scheduler-20261021/irq-accounting-pressure-summary.json`.

The fresh blocking reversal restores 19.78–19.81 seconds of summed AP initial
acquisition-stage wall brackets, a 9.969-second maximum mixer late resume and a
native HDA fault/stream discontinuity. Runtime accounting, original app/VM
owners, heap and all filesystem checks remain clean. This blocking/try/blocking
sequence strengthens the causal link between repeated interrupt-side accounting
admission and the long-stall failure under this verified host pressure. It is
one reversal, with only one try run, and does not establish general desktop
acceptance or independence from probe overhead. The next comparison should
disable expensive timer/peer/watch recording while retaining mixer timing,
mode counters and the same bounded workload. Reversal evidence is under
`build/reviews/unattended-q2-irq-blocking-reversal-20261021` and in the same
derived comparison JSON. Defaults remain unchanged.
