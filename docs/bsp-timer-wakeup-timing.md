# BSP timer and blocked-wakeup timing

`timer timing on|off|reset|status` controls an optional probe for BSP LAPIC timer
IRQs. Recording is disabled at boot. Controls advance an epoch; reset clears
interval counters while preserving enabled state. An in-flight interval crossing
a control change cannot publish into the new interval. On/off preserve completed
counters until reset. The probe does not change interrupt frequency, timer
ownership, wake deadlines, preemption rules or scheduler defaults.

Only the LAPIC timer handler supplies a stack-local probe token to
`SchedulerTimerTick`. Reschedule IPIs still call that function without a token;
they are not timer arrivals. The configured QEMU BSP uses APIC ID zero, following
the existing raw BSP IRQ counter's convention. This diagnostic does not establish
correct BSP identification on arbitrary physical hardware.

## Brackets

The enabled probe samples the stable KVM clock at five points:

1. Timer C-handler entry after its GS invariant guard, before EOI and TLB work.
2. Entry to the running scheduler's timer body.
3. After its initial ready-lock acquisition, runtime charge and release.
4. After post-switch draining and POSIX-timer handling, before blocked scanning.
5. After the blocked scan, candidate unblocking and reference release return.

The event records all raw nanosecond stamps and their four intervening durations:
`prefix_ns`, `runtime_ns`, `pre_scan_ns` and `scan_ns`. They include host
descheduling and lock waits inside each bracket, not just CPU instructions.
Candidate count includes snapshotted timed, pending and resume wake candidates;
it is not a count of successful ready transitions or expired timers.

The probe also records time between consecutive observed timer C-entries as
`gap_ns`, with both arrival stamps. A large gap is not pure hardware delivery
latency: it includes previous handler work after the measured scan, IRQ-masked
kernel work, host descheduling and interrupt delivery. Naked ISR entry and the
GS guard precede the first sample. Other BSP reschedule IPIs may scan wakeups
between these timer samples; this is not an inventory of every wake scan.
Interrupted RIP/CS describes the context at arrival, not proof of which earlier
instruction caused a delay.

`max_gap_ns` covers observed entries even if their stage record is incomplete.
Stage maxima and `completed` cover only complete valid records. The report
retains the largest sixteen completed events where an arrival gap or one stage
is at least twenty milliseconds. `slow` and `excluded` expose truncation; this
is not a full timeline. Match retained raw stamps against
[task wakeup events](targeted-wakeup-timing.md) and
[mixer brackets](audio-mixer-service-timing.md) rather than attributing unrelated
maxima to a single stall.

## Collection safety and verification

Disabled entry adds an epoch check without clock reads, APIC lookup or counter
locking. Enabled AP timer entries check the APIC ID and return without clocks or
records. BSP entry and completion use try-locks on a dedicated bounded reducer;
an interrupted diagnostic reader cannot make the IRQ spin on itself. No clock
read, allocation, logging or scheduler call occurs under that reducer lock.
Normal-thread controls/readers copy state under its lock and format replies
after release. Lifetime `contended_entries` and `contended_records` counters
expose skipped observations; they do not reset with interval counters. Nonzero
drops invalidate any claim that an observed gap proves no intervening timer
entry. Invalid clocks and counter saturation are explicit. This is a normal
debug-thread interface, not panic safe.

The hosted model tests cover arrival gaps versus individual stages, candidate
metadata, unavailable/backwards clocks, each bracket-order failure, stale epochs,
on/off/reset, nested stack-token completion order and largest-sixteen retention.
All 65 host suites pass. The native module import audit resolves thirteen modules
against 106 kernel exports. The compiled entry-state audit passes twelve modeled
templates and 56 mutation checks; its four previously unsupported paths remain
unsupported. No naked ISR wrapper was edited. Those checks do not prove complete
handler-body safety or desktop performance.

The browser workload runner accepts `--timer-wakeup-timing`, records mode in its
resume receipt, and saves idle/load snapshots with explicit intent/receipt
handling for controls. Native comparison uses a common frozen set of thirteen
modules; only the kernel and symbol-address table change from the preceding
clamp-evidence capsule. Recording overhead is not quantified by hosted tests.

## Native evidence

Fresh private eight-vCPU/six-GiB EEVDF and MLFQ boots use the generated local
Firefox video/audio fixture and twenty-four CPU workers for thirty seconds.
The mixer and selected-task probes are enabled alongside the BSP probe; CPU
profiling and mapping counters are off. EEVDF retains its 4-ms normal request.
Callback HDA requests 20-ms buffering into an isolated PipeWire null sink. The
frozen kernel SHA-256 is
`8ff7f61c075f14a10100ecebac5f00025ba1867794ecbe3a9bd4df15281caee4`.

| Run | Policy | Timer entries / complete records | Maximum arrival gap | Maximum prefix | Maximum runtime bracket | Maximum pre-scan bracket | Maximum scan |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| tw-e1 | EEVDF | 30,830 / 30,830 | 107.907 ms | 21.534 us | 31.288 us | 20.575 us | 151.143 us |
| tw-m1 | MLFQ | 32,600 / 32,600 | 82.772 ms | 29.250 us | 30.932 us | 22.481 us | 147.545 us |

Neither run reports invalid probe clocks, contention drops or saturation. All
nine EEVDF and fifteen MLFQ slow timer episodes fit in the retained sixteen
slots; none are excluded, and all fall wholly inside the CPU workload's recorded
start/end bounds. Counter collection starts before the workload and stops after
it drains, so aggregate counts and stage maxima cover a slightly wider interval.
The EEVDF idle interval also has a 74.549-ms maximum
entry gap, despite measured stages below 0.3 ms. These are observed timer-entry
gaps, not a measurement of CPU execution continuously spent with interrupts off.

Four retained watched deadlines in each policy fall inside retained timer-entry
gaps, with the task's ready timestamp inside the following scan bracket. The
deadline translation uses the recorded unchanged `clock status` elapsed-clock
origin; deadline ticks and raw nanoseconds are not directly subtracted.

For example, EEVDF records:

- Previous timer entry: 72.941592330 s; next entry: 73.049499006 s.
- Watched mixer deadline translated to the raw clock: 72.951782056 s.
- Scan: 73.049500763–73.049556109 s, lasting 55.346 us.
- Mixer ready insertion: 73.049519599 s, within that scan and approximately
  97.738 ms after its deadline.
- Subsequent queue wait: 17.193 ms; the mixer reports 114.697 ms late resumption.

The MLFQ example similarly has an 82.772-ms entry gap, a watched deadline
approximately 81.223 ms before ready insertion, a 29.996-us following scan and
17.259-ms subsequent queue wait. These observed correlations locate a large
part of each stall before timer entry. They do not prove why the timer was late
or that every task wakeup has the same cause. Queue and deadline probes have
slightly overlapping boundaries and differing timestamp granularity.

Both runs pass runtime-accounting gates with zero invalid/backwards/clamped/
stale/saturated counters and all eight pvclock/steal records active. Task/mixer
probes also report no invalid clocks, saturation or mixer observation drops.
Neither boot logs a native HDA fault. Both original launch wrappers and audio
routers exit zero, all four filesystem checks pass per boot, and twelve-document/
four-profile preservation passes. All test VMs and private services are stopped.
Frozen source hashes, controls, original receipts and the correlation verifier
remain under the ignored
`build/workloads/timing-scheduler-20261020/timer-wake-*` directories.

## Next attribution step

The measured scan and its initial scheduling stages are short in these two
cohorts. Measure paired BSP steal counters at consecutive timer entries and
later handler work before considering a timer-ownership or quantum change.
Steal subtraction requires its own validity checks: stable individual records
do not guarantee atomic wall/steal publication, as the
[runtime-clamp investigation](runtime-clamp-evidence.md) explains. Host QEMU
emulation delays and vCPU-idle handling are also distinct from guest work with
interrupts masked. The present probe does not settle those alternatives or
establish a scheduler performance win.
