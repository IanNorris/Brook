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

The enabled probe samples the stable KVM clock at six points:

1. Timer C-handler entry after its GS invariant guard, before EOI and TLB work.
2. Entry to the running scheduler's timer body.
3. After its initial ready-lock acquisition, runtime charge and release.
4. After post-switch draining and POSIX-timer handling, before blocked scanning.
5. After the blocked scan, candidate unblocking and reference release return.
6. After BSP alarm handling, reaping, device checks, policy tick and load sampling,
   before the later scheduling/preemption decision.

The event records all raw nanosecond stamps and their intervening durations:
`prefix_ns`, `runtime_ns`, `pre_scan_ns`, `scan_ns` and `tail_ns`. They include host
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

The entry wall sample is bracketed with two BSP steal reads and retried if that
record changes. `has_steal` identifies a valid available sample;
`has_steal_gap` additionally requires a preceding entry with a valid monotonic
steal sample in the same control epoch. Missing/failed pairs preserve the raw
timer observation using a separate clock read and increment
`missing_steal_entries`. Counter regression and a steal delta exceeding the
entry gap are reported separately, rather than silently clamped. The report
retains both raw steal values and their delta. Pair retries can move the chosen
wall sample later within entry collection; it is not an assembly-entry timestamp.
Stable individual records are not an atomic host-delay oracle. A gap minus its
reported steal delta is an estimate, not guest CPU time or proof of IRQ masking;
QEMU emulation or blocking outside runnable-host delay can remain unaccounted.

`tail_ns` ends after BSP housekeeping, before the later preemption decision or
context switch. It includes any blocking/host delay in that work. `completion_cpu`
is sampled near completion, and `migrated` counts non-BSP completion samples.
The probe observes such a case rather than changing the existing dispatch path.
It does not measure every later instruction until IRET.

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

## Initial native evidence

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

## Paired-steal and housekeeping controls

The extended probe passes all 65 hosted suites, the thirteen-module import audit
and the twelve supported compiled entry templates/56 mutation checks. Hosted
cases additionally cover absent/first/backwards steal samples, deltas exceeding
their wall gap, an unavailable final clock and migration metadata. Four entry
paths remain outside the structural audit. The new frozen kernel is
`1c411ad8e9d64be7645e8f2c929d7a991fd04af3fd68b18a6804ff49b0708f2c`;
all thirteen modules remain byte-identical to the initial probe capsule.

Two fresh private boots repeat the same browser/audio/CPU fixture:

| Run | Policy | Completed entries | Largest entry gap | Steal delta for that gap | Maximum measured housekeeping tail |
| --- | --- | ---: | ---: | ---: | ---: |
| ts-e1 | EEVDF | 31,541 | 115.853 ms | 23.007 us | 62.521 us |
| ts-m1 | MLFQ | 33,848 | 104.163 ms | 15.072 us | 35.579 us |

Every observed entry has a completed record. Neither probe reports a dropped
observation, invalid clock, missing/backwards steal pair, excessive steal delta,
migration or saturation. EEVDF retains all fourteen slow events; MLFQ retains
sixteen of twenty-seven, exposing eleven exclusions. The retained steal deltas
range from zero to 34.343 us for EEVDF and 3.269–85.704 us for MLFQ. Twelve EEVDF
and ten retained MLFQ episodes lie wholly inside the CPU workload interval;
other records are not silently labelled loaded performance results.

These reports make the measured housekeeping tail and reported host runnable
delay insufficient explanations for those retained gaps. They do not exclude
host emulation/blocking or IRQ-masked kernel paths outside the measured stages.
Some arrival RIPs symbolize to `SchedulerBlockImpl` just after STI, a Unix socket
readiness check and the virtio GPU completion-poll pause lambda. Others interrupt
the user CPU loop. Arrival context alone does not establish the delaying caller.
No change to those paths is justified solely by these sampled RIPs.

Both browser boots pass runtime-accounting gates, original launcher/router exit,
all four filesystem checks and saved-data preservation. No native HDA fault is
logged. Their task/mixer probes also have valid clocks and no saturation or
mixer contention drops. Ignored `timer-steal-cohorts` evidence includes frozen
source hashes, exact raw samples, gate receipts and a correlation verifier.

## CPU/audio isolation

A third fresh EEVDF boot uses that identical kernel and modules without starting
Firefox or Chromium. It plays thirty seconds of generated packet-coded audio,
first without CPU workers and then with twenty-four CPU workers for twelve
seconds. All probes are enabled around each audio run. The kernel compositor
and desktop services still run: this is a no-browser comparison, not a GPU-free
or otherwise identical application workload. Counter windows include command
setup/drain; the loaded eight-second capture is verified inside the CPU interval.

| CPU workers | Completed timer records | Maximum entry gap | Slow timer episodes | Full audio completion |
| --- | ---: | ---: | ---: | ---: |
| 0 | 41,431 | 11.761 ms | 0 | 29.9916 guest s |
| 24 | 34,738 | 10.934 ms | 0 | 30.0384 guest s |

Both probes have no invalid clocks, dropped/missing/regressing/inconsistent
steal observations, migration or saturation. All 1,323,000 application frames
complete per run. The eight-second coded captures have approximately 1.0000
pitch scale and 1.00014 source seconds per PCM second, with no missing packet
IDs between retained runs or reversed/repeated transitions. These sampled
windows do not prove continuity of every frame in the full thirty-second output.
The original launcher/router, offline result size/SHA verification, runtime,
filesystem and saved-data gates pass. No native HDA fault occurs. Evidence is
under ignored `timer-steal-packet-isolation`; all VMs and private services stop.

Generic CPU pressure alone does not reproduce the long browser-associated entry
gaps in this isolation boot. Next distinguish long IRQ-masked scheduler/kernel
sections from QEMU/device-emulation delays under browser activity. The current
QEMU binary exposes `kvm_run_exit` and virtio GPU command/fence trace events,
which may help attribute host-side delays without another guest policy change.
The scheduler fairness and historical playback-speedup investigations remain
open; no default or quantum change is made here.
