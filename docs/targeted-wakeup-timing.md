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
