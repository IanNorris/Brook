# Runtime service-clamp evidence

The MLFQ [targeted wakeup control](targeted-wakeup-timing.md) records four
clamped-service samples. Its wall and steal counters never move backwards, but
their difference can still fall below the ledger's previous service floor.
That run remains failed runtime acceptance. Four clamped samples do not
necessarily represent four separate host-delay events: several samples can
remain below one previously established floor while a correction catches up.

`sched runtime status` now exposes `first_clamp` and `max_clamp` objects. Each
records sample sequence, CPU, task PID/incarnation, the preceding and current
wall/steal pairs, the retained service floor and the positive deficit. `valid`
is false until a clamp is observed. The first record is immutable; the maximum
changes only for a strictly larger deficit, preserving the first equal maximum.
For a valid record:

```
deficit_ns = service_floor_ns - (wall_ns - steal_ns)
```

The preceding pair is the previous accepted raw sample on that CPU, not
necessarily the sample that established the service floor. A floor can span
several corrections and ownership changes. Identity is the ledger owner at the
clamped sample, not proof that this task caused the delay or owns all earlier
service represented by the floor. These records do not capture every clamp,
host scheduling event or wall/steal publication interleaving.

The implementation reuses the samples that triggered the existing clamp. It
adds no clock reads, logging, allocations or synchronization. It does not
change sampling retries, subtraction, completion accounting, the monotonic
floor or policy service charges. Formatting occurs after the runtime snapshot
has been copied under the existing ready lock. This is a normal debug-thread
report, not a panic-safe diagnostic. Driver ABI and scheduler defaults remain
unchanged.

Hosted tests check the first complete record, a later smaller correction that
must not replace it, a larger deficit on another CPU/task identity, and exact
catch-up without manufactured service. All 64 host suites pass. The native
build resolves thirteen modules against 106 kernel exports. The frozen capsule
changes only the kernel and its symbol-address table from the targeted-wakeup
capsule; all thirteen modules remain byte-identical. Native evidence is retained
separately from these functional checks.

## Native controls and remaining uncertainty

Three fresh private eight-vCPU/six-GiB boots repeat the generated Firefox
video/audio fixture with twenty-four guest CPU workers. Both mixer service and
targeted wakeup probes are enabled; CPU profiling and mapping counters remain
off. EEVDF keeps its 4-ms normal request. Callback HDA requests 20-ms host
buffering into an isolated PipeWire null sink. A fourth fresh MLFQ boot adds
eight bounded host CPU workers for thirty-five seconds after guest load
readiness, all sharing CPUs 0–13 with QEMU and leaving CPUs 14–15 available.
That fourth run is explicitly a host-contention diagnostic, not a comparable
unmodified policy-performance control.

| Run | Policy | Additional host workers | Runtime samples in observation interval | Reported steal in interval | Clamped samples |
| --- | --- | ---: | ---: | ---: | ---: |
| ce-m1 | MLFQ | 0 | 4,357,418 | 0.258 CPU-s | 0 |
| ce-e1 | EEVDF | 0 | 4,334,619 | 0.286 CPU-s | 0 |
| ce-m2 | MLFQ | 0 | 4,165,159 | 0.254 CPU-s | 0 |
| ch-m1 | MLFQ | 8 | 3,440,512 | 62.999 CPU-s | 0 |

These intervals run from the controller's initial runtime snapshot through its
post-browser-close snapshot, not exactly the thirty-second guest CPU workload
or the thirty-five-second host load. Steal totals combine all vCPUs and are
not elapsed wall time. The bounded host workers all exit zero after approximately
35.07 host seconds. The large steal increase establishes that the contention
control exercised host scheduling pressure, without reproducing service clamping.
It does not establish why the earlier four samples clamped or prove they were
benign. That earlier runtime gate remains failed.

All four boots report zero invalid/backwards/clamped/stale/saturated runtime
counters, with all eight paravirtual-clock and steal records active. Both new
evidence objects correctly report `valid:false` with no clamp. At this stage no native
positive-clamp record had been collected; the 21 October follow-up below
records the first positive native samples. Positive-record behavior is also
tested in the hosted accounting model. No native HDA fault is logged. Original launch
wrappers and audio routers exit zero, all four filesystem checks pass per boot,
and twelve-document/four-profile preservation checks pass. All test VMs, private
services and host load workers are stopped.

The frozen kernel SHA-256 is
`de6282384a7ebdb0e8709558146f8bae5502d6fcd00940a8713d4331ebc96823`.
Source hashes, the unchanged-module manifest, original job/launcher receipts,
host load ownership/exits and the result verifier are retained under the ignored
`build/workloads/timing-scheduler-20261020/clamp-evidence-*` and
`clamp-host-stress` directories. Keep this capture available for recurrence;
do not relax the accounting gate from a non-reproduction. The next latency
measurement should separate BSP timer arrival from time spent reaching and
performing the blocked-wakeup scan.


## First positive native record, physical-output RR browser control

On 21 October the isolated 8-KiB HDA experiment records one correction while
Firefox plays the generated local 640×360 video with eight bounded host CPU
workers and four sleepers. This is unchanged RR, eight vCPUs/eight GiB,
physical PipeWire output, historical timer HDA and 200-ms host buffering.
It is a different cohort from the earlier MLFQ/EEVDF null-sink controls.
The runtime gate fails; a small deficit is not grounds to silently waive it.

| Field | Captured value |
| --- | ---: |
| CPU / task PID / incarnation | 6 / 226 / 226 |
| Sample sequence | 3,477,285 |
| Previous wall / current wall (ns) | 107,095,032,872 / 107,095,055,241 |
| Previous steal / current steal (ns) | 199,972,129 / 199,994,778 |
| Previous service floor (ns) | 106,895,060,743 |
| Current wall minus steal (ns) | 106,895,060,463 |
| Deficit (ns) | 280 |

The wall increment is 22,369 ns and steal increment 22,649 ns. Neither raw
counter regresses. The first and maximum records are identical, and a later
snapshot still reports exactly one clamp. The floor equals the previous raw
wall-minus-steal value. This directly demonstrates an inter-sample correction;
it does not identify which host event or shared-record publication produced it.
There are no invalid, backwards, stale-owner or saturated runtime samples.

[KVM's steal-time ABI](https://www.kernel.org/doc/html/latest/virt/kvm/x86/msr.html)
allows implementation-dependent update intervals for its independently versioned
steal record. Brook brackets a stable pvclock read with two validated steal
reads and retries if their values differ. That prevents an observed change
inside the bracket, but does not guarantee that successive raw wall-minus-steal
values are monotonic when a previously delayed correction is published.
This is a plausible mechanism for the record, not a host-level causal trace.
The existing floor prevents cumulative service from moving backwards and
carries the correction forward; no sampling/accounting or acceptance rule is
changed on the strength of this observation.

The controller saves a complete 148-sample browser trace with zero trace drops
before stopping at the runtime gate. Idle and loaded audio completion relative
to guest elapsed time are approximately 0.99871 and 0.98909 respectively;
these are completion counters, not decoded source or speaker timing. Both
intervals retain the same stream epoch with zero discontinuity. The loaded
HDA probe records zero unsafe/invalid publications and 12,884 reserved bytes
in four reservations. Browser video still drops many frames, so near-real-time
audio completion is not a smooth-desktop acceptance result.

After saving additional clock/process/runtime evidence, the original Firefox
group closes normally with complete output and no remaining group tasks. The
original VM launcher exits zero and all four offline filesystem checks pass.
The planned second browser launch is not performed in this failed cohort.
Evidence remains ignored under
`build/reviews/unattended-hda-8k-browser-20261021/browser-lead-control`, including
`browser-lead-1/runtime-after.json` and `accounting-followup`. A later diagnostic
may complete cleanup/restart collection while retaining an explicit failed
runtime result; that must not turn this failed gate into a passing one.


The subsequent matched **original 12-KiB** browser control with today's callback
HDA/20-ms buffer also records one correction, already present before the loaded
interval. CPU 7/task 298/sample 1,301,550 advances wall from 48,059,413,840 to
48,059,433,002 ns and steal from 5,222,574 to 5,254,982 ns. The previous service
floor is 48,054,191,266 ns and the deficit is 13,246 ns. Other runtime quality
counters stay zero. Both fresh-profile browser groups and the original VM
exit normally, with clean heap and filesystem checks, but cumulative runtime
acceptance remains failed. The correction class is therefore observed with
both lead modules; this does not prove identical causes or rehabilitate either
failed cohort. The candidate's callback/20-ms two-launch browser run is
separately clean. Matched original evidence lives under
`build/reviews/unattended-hda-12k-browser-callback-20261021/browser-lead-control`.

## Normal-placement policy repeats, 21 October

The [fixed-HDA policy comparison](scheduler-normal-placement-comparison.md)
records two further failed EEVDF runtime gates during second Firefox launches.
The loaded phase initially has zero clamps; reopen then records two samples in
cohort a and three in cohort b. First/max deficits are 4,887 ns (CPU5, PID and
incarnation 498) and 17,115 ns (CPU4, idle owner 0). The raw increments are
53,248/58,135 ns wall/steal and 43,814/60,929 ns respectively. Each retained
floor equals the preceding accepted pair's difference. These first clamps
therefore capture new subtraction regressions, not merely repeated observations
below an older floor.

Neither raw counter moves backwards; policy/ledger totals still agree. Those
facts do not establish accurate attribution or excuse the failed gates.
First/max records cannot reconstruct ownership changes around a delayed steal
correction. A bounded history of accepted pairs, owner transitions, floor and
catch-up is the next diagnostic. Two RR and two MLFQ cohorts in the same study
remain runtime-clean; that does not identify EEVDF as the publication cause.
All original owners, heaps, audio drains and filesystem checks pass separately.
