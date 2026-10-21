# Audio mixer service timing

The optional mixer probe separates delayed resumption after a nominal sleep
from time spent inside `AudioMixerTick`. It is intended to help investigate
desktop/audio stalls; neither bracket alone identifies scheduler ready time.
The scheduler default and audio sleep interval are unchanged.

## Interface and measurement limits

The normal debug channel accepts `audio timing on|off|reset|status`. It is not
panic safe. Recording is disabled at boot. Each control advances an epoch;
reset clears interval counters while preserving enabled state and worker
PID/incarnation. Completed counters survive on/off until explicitly reset.
Intervals crossing a control change, or still incomplete, are omitted.

Enabled recording samples the stable clock before deadline setup, immediately
after setup, after `SchedulerBlock` returns, and after mixer service returns.
The report distinguishes setup, sleep, service and the tail from the previous
completed service to the next pre-sleep sample. `late_lower_ns` is the positive
excess over ten milliseconds after the deadline-setup sample. It includes
blocked wakeup, timer dispatch, scheduling and host descheduling; it is not a
measurement of time continuously runnable. Service includes locks, interrupts
and host delays, rather than only mixer CPU work.

Disabled operation adds an atomic epoch load and branch, without probe clock
reads or counter locking. Recording uses a separate bounded reducer with
try-lock acquisition: the mixer never waits for a diagnostic reader. The
lifetime `contended_records` counter exposes omitted observations and does not
reset with interval counters. No allocation, logging or clock read occurs under
the reducer lock. A report retains the largest sixteen episodes with a setup,
late, service or tail bracket of at least twenty milliseconds. `excluded_slow`
counts smaller slow episodes excluded or displaced from that set. This is not
a complete timeline. Invalid clocks and aggregate saturation are explicit.

## Baseline evidence

Two fresh private eight-vCPU/six-GiB boots use Firefox with generated local
video and audio plus twenty-four bounded CPU workers. Callback HDA requests a
20-ms host buffer into an isolated PipeWire null sink. Neither CPU profiling
nor mapping counters are enabled. Both runs use the same kernel and modules,
with EEVDF's unchanged 4-ms normal request or MLFQ selected at boot.

| Run | Policy | Completed cycles | Maximum late lower bound | Maximum service | Slow cycles | Excluded slow |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| as-e1 | EEVDF | 2,903 | 92.737 ms | 45.997 ms | 13 | 0 |
| as-m1 | MLFQ | 2,247 | 90.885 ms | 46.058 ms | 152 | 136 |

Both reports have zero invalid clocks, zero contended records and no aggregate
saturation. The largest EEVDF event resumes 92.737 ms late but then services
the mixer in 57.158 microseconds. Thus at least that episode is dominated by
delayed resumption, rather than lengthy mixer service. It does not establish
which portion was blocked, runnable or host-descheduled. MLFQ's retained set
omits 136 smaller slow episodes and cannot support absence-of-event claims.

The EEVDF baseline also reproduces `publish-unsafe`, check bit 64 only: 744
bytes remain between raw completion and the next receipt's beginning, while
the device advances 1,024 bytes during a roughly 49-microsecond bracket. This
is a short write-publication hazard, separately from the long sleep episodes;
there is no retained EEVDF slow event within 100 ms of this fault. The existing
guard stops playback and preserves the proven completion watermark. This run
is not accepted as clean audio. No native HDA fault occurs in the MLFQ run.

Both original launch wrappers and audio routers exit successfully; all four
filesystem checks pass, and twelve documents plus four profile trees pass
preservation checks. Those lifecycle gates do not erase the EEVDF audio fault.
The model tests cover epoch changes, clock ordering, setup/service/tail
classification, bounded retention and aggregate saturation. All 64 host suites
pass; the native build resolves thirteen modules against 106 kernel exports.

Evidence is retained under the ignored
`build/workloads/timing-scheduler-20261020/audio-service-cohorts` directory and
the corresponding `build/reviews/timing-mix-as-*-20261020` private reviews.
The frozen kernel SHA-256 is
`8937859141ca14387045eaf9e2ee1077b2b2bc72a69c1cd79b280935bb5aac63`.
These diagnostic runs do not quantify probe overhead or demonstrate a complete
fix for browser/audio stalls or the historical application speedup.


## Physical-output Quake lead diagnostic, 21 October

A matched RR pair with physical PipeWire output, callback HDA and a 20-ms
requested host buffer compares the unchanged 12-KiB publication lead with an
isolated 8-KiB module. Both play the normal GL3 menu attract sequence while
eight finite host CPU workers/four sleepers run for twenty seconds. Delivery
and service probes are enabled; CPU profiling remains off.

| Lead | Completed cycles | Max sleep | Max service | Slow cycles >=20 ms | Application completion / guest elapsed |
| --- | ---: | ---: | ---: | ---: | ---: |
| 12 KiB | 2100 | 11.553 ms | 6.420 ms | 0 | 0.84364 |
| 8 KiB | 2101 | 11.136 ms | 6.920 ms | 0 | 1.00007 |

Both retain zero invalid service clocks, contended records or saturation,
clean runtime/heap/lifecycle/filesystem gates and no native HDA fault.
Reservations are 183,808 versus 256 bytes. The original lead's steady slowdown
therefore coexists with regular worker resumption, supporting the separate
reservation/pacing mechanism.

Neither run reproduces the earlier approximately 550-ms candidate sample-gap
fault. With no corresponding slow event, this pair cannot classify that fault
as late resumption or long service, or override its failed audio gate. The
production lead remains unchanged.

A later live affinity audit finds the controller changed only the QEMU process
leader's mask. The bounded host workers were pinned, but shared vCPU contention
and spare-CPU reservation were not established. A nominal eight-CPU pressure
repeat still permits all eight KVM vCPU threads on CPUs 0–15. Keep these as
functional/observational load results, not a verified pinned-pressure bound.
See the correction in [the lead evidence](hda-near-empty-write-lead.md).

For future review launches, `BROOK_REVIEW_CPUSET=0-7 scripts/run_review.sh REVIEW`
applies the mask before QEMU creates its vCPU threads. The focused host test
checks actual inheritance by a launcher, thread and child and preserves exit
status. Some QEMU I/O helpers explicitly reset their affinity after creation,
so launch inheritance alone does not certify every thread: use live
`query-cpus-fast` TIDs and `/proc/PID/task` masks before and after measurement,
and explicitly constrain resetting helpers before a fully pinned control.
The corrected diagnostic pair retains those receipts separately.

Original evidence is ignored under
`build/workloads/timing-scheduler-20261021/hda-q2-service-cohort` and
`build/reviews/unattended-hda-{12k,8k}-q2-service-callback-20261021`.


### Verified CPU-contention failures and fault alignment

The launch-time correction is tested separately from the earlier leader-only
runs. In the first corrected preflight all eight vCPUs correctly inherit CPUs
0–7, but two QEMU I/O helpers reset themselves to CPUs 0–15. The strict check
rejects this before any load starts; the original app and VM then close cleanly
with all filesystem checks passing. No performance result is claimed for that
rejected preflight.

Two subsequent matched pairs verify all eight QMP-identified vCPU thread masks
before and after load. One additionally pins every current QEMU helper to 0–7;
the other leaves the two resetting I/O helpers' 0–15 masks and priorities alone.
Eight finite host CPU workers compete on 0–7 for twenty seconds in both pairs.
All four runs fault with `sample-gap`; these are failed audio acceptance runs.

| Helper treatment | Lead | Max late-resume lower bound | Max service | Reported host steal delta, all vCPUs |
| --- | --- | ---: | ---: | ---: |
| All current QEMU threads pinned | 12 KiB | 9.205 s | 10.252 s | 145.444 CPU-s |
| All current QEMU threads pinned | 8 KiB | 13.468 s | 5.590 s | 150.601 CPU-s |
| Two resetting I/O helpers unrestricted | 12 KiB | 7.962 s | 8.419 s | 145.535 CPU-s |
| Two resetting I/O helpers unrestricted | 8 KiB | 8.862 s | 0.063 s | 145.808 CPU-s |

Each fault's prior DMA observation occurs just before the retained pre-sleep
sample. The next observation starts immediately after the retained worker-wake
sample. Thus the **initial stale observation follows delayed worker resumption**;
subsequent long service brackets do not explain away that earlier gap. For the
fully pinned pair, the prior observation is about 12–14 microseconds before
sleep setup, and the next observation begins about 1.4 microseconds after wake.
This does not split timer availability, ready-queue delay and host scheduling.

A read-only host observer runs only for the vCPU-focused 8-KiB diagnostic,
adding no guest requests or QMP operations. Across its 14.70-second sampled
span, vCPU threads accumulate roughly 8.1–10.5 seconds of host runnable-wait
and 0.50–2.76 seconds of host CPU execution each. Wait channels are usually
zero, with some `kvm_vcpu_block`/futex samples. The host is Linux 6.12.112 with
sched_ext disabled. The vCPU threads have ordinary host scheduling and nice
zero; some helper threads have batch scheduling/nice 19. These observations
identify host contention and a possible lock-holder/priority interaction,
not its exact cause. Leaving the two I/O helpers unrestricted did not remove
the failure, so their affinity restriction alone is insufficient to explain it.
This one-sided observer is a diagnostic, not a matched observer-cost control.

All four service probes and runtime ledgers have clean quality counters.
Original app groups, finite workers, VM owners and filesystem checks complete
successfully. None of those lifecycle checks override the native audio faults.
The earlier approximately 550-ms fault remains preserved as a separate failure;
these stronger reproductions do not prove its exact cause.

Frozen inputs, live pre/post thread masks, serial faults and aligned events are
indexed by the ignored `hda-verified-affinity-failures.json`. Raw cohorts are
`hda-q2-affinity2-cohort` and `hda-q2-vcpu-cohort`. The next attribution uses the
existing default-off ready-lock ownership probe alongside the service probe
and a bounded host observer. Scheduler policy and HDA lead defaults remain
unchanged.
