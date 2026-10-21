# MLFQ under restricted host CPU contention

Three fresh private boots on 21 October retain the original kernel/module
capsule and HDA lead while comparing blocking and try admission for AP initial
IRQ accounting. **MLFQ does not restore useful Quake playback under this load.**
Try admission avoids the observed native audio discontinuity in its cohort,
but the renderer still freezes for approximately twenty seconds. There is no
scheduler, IRQ-accounting or HDA default change.

## Fixed conditions

- Eight vCPUs, 8 GiB, QEMU 10.2.2, physical PipeWire output, callback HDA with a
  requested 20-ms host buffer and the original 12-KiB publication lead.
- Kernel SHA-256
  `fc41f3dbf97797d1f349754303090fee0b6e14976f8350ae717e1636a104d8bb`
  and all thirteen modules match `gpu-poll-pause-frozen/manifest.json`.
  This intentionally excludes the later runtime-history kernel.
- MLFQ is selected before initial dispatch. AP mode is explicitly set and
  verified through the debug interface before the measured workload. The boot
  script's `sched` command selects policies; it does not interpret
  `sched irq-accounting try` as the debug interface does. The first try setup
  caught this mismatch, retained its original VM, and recovered before running
  the workload. Its original setup failure and recovery receipts are retained.
- All vCPUs share CPUs 0–7 with eight finite host CPU workers for twenty
  seconds. Endpoint and sampled thread masks retain the two named I/O-helper
  exceptions allowed on 0–15; this is not full-thread CPU reservation.
- Normal GL3 menu/attract launch, ten-second warmup, mixer-service and delivery
  observations, bounded read-only host observer and KVM counter endpoints.
  CPU profiling and expensive timer/ready/watch recording remain off.
- Both modes use the same buffered SDL swap observer, SHA-256
  `e970d3d2bd080e184a4be75791a98feb008930e2c1d9a0b800b6ab4e2af7ecba`.
  It makes two guest clock reads per swap and writes at exit. Its native
  overhead remains unquantified; this study is not uninstrumented acceptance.

## Results and measurement gates

| Cohort | Maximum mixer lateness | Maximum mixer service | Largest swap-completion gap | Stored PCM / capture host duration | Observed aggregate vCPU host execution |
| --- | ---: | ---: | ---: | ---: | ---: |
| Blocking a, pilot | 8.436 s | 23.95 ms | 20.063 s | 0 / 6.010 s | 13.31 CPU-s |
| Try a | 188.64 ms | 592.40 ms | 19.922 s | 0.266 / 6.008 s | 2.69 CPU-s |
| Blocking b, reversal | 4.992 s | 4.695 s | 19.941 s | 0 / 6.007 s | 4.68 CPU-s |

Mixer maxima cover reset before pressure through collection afterward, rather
than an exact cropped load interval. Host execution sums use all eight QMP
vCPU thread identities across read-only observer samples inside verified host
worker intervals, spanning approximately 19.9 seconds. They include host
execution by vCPU threads, not solely guest instruction execution. In try a,
each vCPU accumulates roughly 18.7–19.7 seconds of runnable host wait over
that sampled span. These counters support severe host execution deprivation;
they do not identify the guest spin-loop owner or a specific host cause.

All three WAV captures lie inside every worker's observed execution interval;
the controller waits beyond the pre-start `READY` marker and checks actual
first/last timestamps afterward. Try a and blocking b also contain both full
audio-query host brackets inside those intervals. Blocking a's query brackets
extend beyond pressure, so its audio-rate estimate and guest-query frame
window are **not wholly loaded measurements**.

Both valid query windows contain **zero completed swaps**, enclosed by the
approximately twenty-second completion gaps above. The try window lasts
6.512 guest seconds; the blocking reversal window lasts 15.796 guest seconds.
There are no fully contained frame intervals from which to report a loaded
p50/p95. The analyzer retains boundary-crossing gaps explicitly: cropping
them away would hide the freeze. SDL completion is not physical scanout.
The whole-application percentile includes warmup and cannot describe this
loaded interval.

Both blocking cohorts record a native `sample-gap` fault and discontinuity
counter 1. Try a retains counter 0, but completes only about 0.0214 nominal
audio seconds per guest second during its bracket. QEMU WAV output can omit
inactive intervals; stored PCM length is not a source-tempo or speaker-clock
measurement. Avoiding a device fault does not establish useful audio delivery.

## Safety evidence and limits

The try and blocking-reversal cumulative runtime gates are checked **after
normal app close and complete trace export**: invalid/backwards/clamped/stale
and saturation counters remain zero. The pilot checked runtime before close,
so final-phase runtime acceptance remains unproven for that cohort. Its
earlier clean gate is retained without broadening its scope.

Policy service plus inactive service equals the ledger total in each cohort,
with zero policy saturation. IRQ IF-error and stale-ticket-wait counters are
zero. Try a reports 8,038 deferred attempts; these are skipped initial
accounting/watch opportunities, not idle execution or a count of saved work.
Mixer and delivery quality checks are clean independently of the native
sampling faults. All original app/export jobs pass complete-output and group
exit checks. All three original VM owners exit zero and all twelve offline
filesystem checks pass. The corrected cohorts verify the uploaded probe's
guest SHA-256; the pilot only checked its byte count.

Four analyzer regression cases verify a stall spanning an entire crop,
partially crossing boundaries, exact boundaries, missing coverage and dropped
source rows. The complete trace must pass quality validation before cropping.
No runtime history is enabled here, and this study does not clear the earlier
[accounting-clamp failures](runtime-clamp-evidence.md).

Ignored evidence lives under
`build/reviews/unattended-mlfq-pressure-{blocking-a,try-a,blocking-b}-20261021`.
Exact executed controller copies, original-owner receipts, raw swap traces,
load containment and filesystem logs are retained. The checked aggregate is
`build/workloads/timing-scheduler-20261021/mlfq-pressure-summary.json`;
its analyzer is `reboot-resume/analyze-mlfq-pressure.py` in that directory.

## Next discriminator

Compare a finite packet-coded OSS writer **without Quake rendering** against
this MLFQ/try condition, keeping the original kernel, HDA, QEMU, affinities,
host workers and light diagnostics fixed. The desktop may still submit GPU
commands; absence of Quake is not proof of a GPU-inactive VM. Require source
progress during verified pressure, exact eventual completion/drain, final
runtime checks and original-owner cleanup.

If direct OSS also stalls with negligible vCPU execution, Quake rendering is
unnecessary for the failure. If it progresses normally, graphics-associated
activity becomes a useful discriminator, without yet identifying completion
polling, mutex ownership, compositor interactions or host backend dependencies.
An ad-hoc HLT or ticket-lock change is not justified by these observations.
