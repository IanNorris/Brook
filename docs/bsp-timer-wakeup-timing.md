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

## Host KVM/device trace checkpoint

Two further fresh EEVDF boots use the same frozen kernel and mixed workload,
with twelve seconds of QEMU KVM exit/device trace and approximately 20 ms host
thread samples. These are diagnostic runs, not unprofiled performance controls.
Audio remains callback-driven through the private PipeWire null sink. No kernel,
policy or quantum is changed.

The first attempted trace (`ht-e1`) held its QMP connection while sampling. QEMU
accepts one connection on this endpoint, so the workload's audio-capture greeting
timed out before `wavcapture`. This attempt is explicitly rejected as a workload
control. Its trace is retained; the same tracked CPU/browser jobs were drained,
probes saved and disabled, and the same VM normally shut down. The original
launcher/router, four filesystem checks and saved-data preservation passed.
The controller then failed because it assumed every workload failure had a
runtime-gate receipt; `recovery.py` records the actual disposition instead.

The corrected helper serializes short QMP connections with the bridge operation
lock and releases QMP during sampling. Both `ht-e2` and `ht-e3` complete the
original workload, including the eight-second audio capture inside its CPU-load
interval. Runtime gates, original launcher/router exit, all filesystem checks
and twelve-document/four-profile-tree preservation pass. No native HDA fault is
logged. Every enabled trace event is restored to its previously disabled state.
All VMs and private services are stopped.

`ht-e2` records three retained timer episodes wholly within the trace window;
the largest is 44.990 ms. Host intervals from CPU 0's successful KVM exit trace
to its next `KVM_RUN` trace reach 30.268 ms. `ht-e3` additionally traces memory
region accesses, exposing these CPU 0 intervals:

| Host exit-to-next-run interval | Exit | Access observed in that interval | CPU 0 host samples strictly inside it |
| --- | --- | --- | --- |
| 34.694 ms | port I/O | COM2 status read, `serial`, address `0x2fd` | two sleeping in `futex_wait_queue` |
| 32.828 ms | MMIO | HDA position read, `intel-hda`, address `0x80840104` | one sleeping in `futex_wait_queue` |
| 25.192 ms | port I/O | COM1 status read, `serial`, address `0x3fd` | one sleeping in `futex_wait_queue` |

The main QEMU thread is sampled running during those same waits. Corresponding
nearby guest timer gaps are 34.745, 32.928 and 43.988 ms; their arrival RIPs
symbolize to the COM2 lifecycle worker, the HDA module and `SerialPrintf`.
Reported steal for the two near-equal-duration episodes is tiny, and enclosing
CPU 0 host run-queue delay is zero or a few microseconds. Host sleeping/device
handling cannot be dismissed just because reported steal is small.

Clock alignment is deliberately bounded: guest RPC brackets leave 23.113 ms
offset uncertainty in `ht-e2` and 31.812 ms in `ht-e3`, assuming a 1x clock.
The observed realtime/monotonic envelopes add about 1.8–2.0 ms. These numbers
do **not** establish exact one-to-one causality for every nearby episode. The
COM2 interval still overlaps its nearby timer gap by at least 1.513 ms across
all recorded offset bounds; the COM1 interval overlaps by at least 10.777 ms.
The HDA pair does not have a positive minimum overlap under those bounds.
Other traced gaps reach 68.610 ms while individual nearby userspace handling
intervals are much shorter. The larger-gap cause remains open.

Source interpretation uses the exact QEMU 10.2.2
[KVM loop](https://raw.githubusercontent.com/qemu/qemu/v10.2.2/accel/kvm/kvm-all.c):
exit tracing precedes I/O dispatch, while the ioctl trace precedes reentry.
An exit-to-next-run interval therefore bounds host handling and other work,
not one device callback alone. KVM-run intervals also include host kernel time.
[Memory access preparation](https://raw.githubusercontent.com/qemu/qemu/v10.2.2/system/physmem.c)
acquires BQL for non-lockless regions.
[Read traces follow the callback; write traces precede it](https://raw.githubusercontent.com/qemu/qemu/v10.2.2/system/memory.c).
BQL/device contention is a supported next hypothesis, not an identified mutex
owner. No memory-slot-update event occurs in either trace. Requested generic
`cpu_exec_start/end` events produce no records, so they supply no timing proof.

The source also exposes a scheduler bottleneck independent of policy choice:
`SchedulerTimerTick` calls `CheckBlockedWakeups` only on CPU 0. A CPU 0 device
wait can delay all sleeping tasks even while other CPUs run. Next measure timer
availability on those other CPUs and audit a serialized wake-scan handoff or
deadline wake mechanism, preserving process references, SMP ownership and lock
order. Do not move reaping/static BSP housekeeping wholesale or treat extra
scans on every CPU as a free change. Attribute host lock waits separately and
retain the unexplained gap cases before claiming the audio problem resolved.

Ignored `host-kvm-trace`, `host-kvm-trace2` and `host-kvm-trace3` evidence retains
controllers, launchers, frozen source hashes, trace windows, raw host thread
samples, guest probe snapshots, tracked-job/lifecycle receipts and reproducible
`analyze-results.py` analysis with offset bounds. Scheduler default, safety gates
and the unresolved historical playback-speedup conclusion remain unchanged.

## Independent per-CPU timer arrivals

`timer peers on|off|reset|status` adds a separate default-off IRQ arrival probe.
Each CPU has its own cache-aligned model and try-lock; no global IRQ collection
lock or BSP-only completion path is needed. The hook runs after the GS guard in
the LAPIC timer handler, excluding reschedule IPIs. It reads stable pvclock and
records first/last arrival, count, adjacency count, largest gap/context, gaps at
least 20 ms, invalid clocks and lifetime contention drops. Failed/regressing
clock reads break adjacency rather than manufacturing a valid gap across an
unobserved interval.

`on` and `reset` clear the measured window; `off` preserves its evidence. A
monotonic epoch rejects delayed observations from before a control change;
exhaustion fails closed. Reports include each row's epoch and a final consistency
flag because rows are sampled separately. Counters do not claim simultaneous
all-CPU snapshots or distinguish the host/guest cause of a missed arrival.
Normal-thread controls may hold a slot lock; its IRQ observer drops rather than
spins, and that drop remains visible across resets. No scheduler behavior or
default changes.

The mixed workload's `--timer-irq-arrivals` flag retains idle and loaded
snapshots, disables the probe after load, and guards its mode on resume. New
hosted model cases cover independent CPUs, a delayed BSP with continuing AP
arrivals, stale/off epochs, preserving a disabled window, reset, unavailable and
regressing clocks, and equal timestamps. All 65 hosted suites pass. Native
module imports resolve for thirteen modules/106 exports; the compiled entry
audit passes twelve supported templates and 56 mutation checks, retaining four
unsupported paths. The frozen kernel hash is
`1fc8afd966ea45697fa45943ef5fd098f211e7281cd8e472b2fa398fe252eb29`.
Eleven modules are byte-identical to the prior capsule; the two scheduler modules
differ only in `.debug_info`/`.debug_line`, with all non-debug sections identical.

Two fresh private eight-CPU boots repeat the browser/audio/24-worker fixture:

| Run | Window | BSP largest gap | AP largest gaps, CPUs 1–7 | Retained BSP slow episodes with proven AP timer activity |
| --- | --- | --- | --- | --- |
| tp-e1, EEVDF | browser idle | 51.651 ms | 11.230, 15.547, 11.556, 9.097, 9.765, 9.053, 15.655 ms | 2/2 |
| tp-e1, EEVDF | CPU load | 103.650 ms | 33.547, 10.541, 14.478, 34.163, 18.602, 32.475, 43.132 ms | 16/16 |
| tp-m1, MLFQ | browser idle | 42.387 ms | 12.922, 11.183, 12.276, 12.666, 12.238, 12.004, 8.101 ms | 3/3 |
| tp-m1, MLFQ | CPU load | 116.687 ms | 25.203, 24.549, 27.640, 27.731, 10.895, 32.951, 31.396 ms | 16/16 |

Every per-CPU report has a consistent epoch, nonzero observations, exactly
`arrivals - 1` adjacent intervals, and zero invalid/drop/saturation counters.
For each availability claim, the entire BSP episode lies within the AP's
first/last observed arrivals and that AP's largest gap over its whole window is
shorter than the BSP episode. Therefore at least one AP timer arrived during
that BSP gap. This uses shared guest clock intervals directly, without the
host RPC offset uncertainty of the QEMU correlation. Loaded BSP lists exclude
six EEVDF and sixteen MLFQ episodes; no availability claim is made for those
unretained events. An AP having an IRQ does not prove it can acquire scheduler
locks promptly or complete an audio wakeup.

Both original workloads, runtime gates, launcher/router waits, four filesystem
checks and saved-data preservation pass. No native HDA fault occurs. All VMs
and private services stop. Evidence in ignored `timer-peers-cohorts` includes
source hashes, default-off checks on every CPU, raw snapshots, job receipts and
`verify-results.py`; module comparison and hosted/native/entry-audit results are
stored alongside it. These instrumented runs are not acceptance evidence of an
audio or frame-rate improvement, and do not reproduce the historical speedup.

The next candidate is a serialized wake-scan handoff: preserve BSP preference,
allow an AP to claim a scan when completion is overdue, and never let a CPU spin
waiting for the scan owner. Keep ownership across candidate gathering,
`SchedulerUnblock` and reference release. Merely serializing the registry scan
is insufficient: duplicate candidates could call `SchedulerUnblock` after a
peer has made the task Ready and publish an extra `pendingWakeup`. An active
owner cannot safely be revoked just because it is slow. Test this ownership
model and switch it on explicitly for native comparison before changing any
default. POSIX expiry, alarms and reaping remain separate BSP responsibilities;
the handoff alone would not repair every timeout path.
