# EEVDF normal-request control — 20 October 2026

Shortening Brook's normal request from 4 ms to the Linux reference's measured
2.8 ms does not show a repeatable desktop benefit in this small comparison.
Keep 4 ms as the build default. Neither setting establishes acceptable loaded
video/input performance, and the selected scheduler default remains unchanged.
This isolates one parameter of Brook's global policy; it does not reproduce
Linux's per-CPU, group scheduling or kernel preemption behavior.

## Method and results

Four fresh private eight-vCPU/six-GiB boots run in order 4/2.8/2.8/4 ms. Each
uses a fresh 800×600 Firefox profile, generated 24-fps video, 24 CPU workers for
30 seconds, four periodic sleepers and four injected keys. Audio uses native
PipeWire into the private null sink, callback HDA and requested 20-ms buffering.
No profiler runs inside these controls. One frozen kernel and twelve modules
are byte-identical across all reviews; only the EEVDF module differs between
request settings. Other priority hints and initial weights are unchanged.

| Run | Request | Wake p95 / max (ms) | Video callbacks/s | Media/page rate | Largest DOM upper bound (ms) | CPU batches |
| --- | --- | --- | --- | --- | --- | --- |
| es-e4a | 4 ms | 2.510 / 91.531 | 4.167 | 0.92151 | 330.561 | 6,046,295 |
| es-e28a | 2.8 ms | 2.832 / 101.305 | 4.249 | 0.93579 | 1,369.431 | 6,025,005 |
| es-e28b | 2.8 ms | 3.267 / 63.729 | 3.736 | 0.91390 | 283.730 | 5,995,103 |
| es-e4b | 4 ms | 3.914 / 85.870 | 3.985 | 0.92408 | 323.663 | 6,049,153 |

All buffered page-clock models pass without lost trace samples; video remains
playable/buffered during the loaded selection. The DOM numbers are bounded
host-dispatch-to-handler estimates, not physical display latency. HTTP receipt
latency is retained separately and is not used as the DOM metric. A small
number of keys and two runs per setting cannot establish tail distributions
or prove equivalence. The long delay in the first 2.8-ms run does not repeat in
the second. Batch counts measure work throughput, not CPU-time fairness.

Video presentation remains approximately 4 callbacks/s across both settings;
this does not approach the approximately 22.7 callbacks/s Linux reference.
The requests are not the dominant explanation established by these trials.
Further work should localize dependencies, placement and long non-preemptible
kernel work using short, separately labelled profiles, keeping MLFQ as a
comparison. This is not a reason to continue RR tuning.

## Audio failures and capture boundary

The first 4-ms baseline records a native HDA `publish-unsafe` fault:

```text
reason=publish-unsafe epoch=1 previous_ns=58784623189 before_ns=0 after_ns=0 previous_position=14336 raw_position=0 status=0 submitted=6037112 completed=6029312
```

This is a post-copy safety rejection. The current diagnostic call omits the
post-copy timestamps/device position, so zero fields do not prove an unavailable
clock or zero hardware position. The exact failing predicate is unknown. The
run is retained as failure evidence and excluded from clean audio acceptance;
it cannot demonstrate glitch-free output. The other three serial logs contain
no native HDA fault. All four QEMU codec traces contain no timer adjustment or
overrun events. No HDA checks or recovery behavior were weakened.

The last 4-ms capture begins approximately 201 ms before the conservative
loaded-start bound. It is not proven wholly inside load and is excluded from
strict loaded-audio comparison. The CPU fixture announces READY one second
before its timed start; fast bridge delivery exposed the Brook controller's
missing wait. The controller now waits 1.1 seconds after READY, matching the
Linux controller, and writes `audio_capture_inside_load` in its summary. Final
clock brackets still decide acceptance. That harness correction was made after
these four runs and has syntax validation, not a new native acceptance run yet.

The four eight-second host captures contain 350,336 / 343,936 / 351,872 /
353,024 PCM frames respectively; each has a maximum 90-ms span of near-zero
samples by the stored 10-ms RMS-bin diagnostic. Captured duration/frames and
absence of faults alone do not establish continuous or correctly paced sound.
No speaker acceptance is claimed. Audio defects and the historical larger
application speedup remain open.

The [subsequent diagnostic/profile checkpoint](hda-publish-and-scheduler-profiles.md)
fills the missing publication samples and records three loss-free profiles.
It does not reproduce or fix this first baseline's intermittent HDA fault.

## Safety and evidence

All four runs pass the collection/lifecycle gates: zero clock/runtime anomaly
counters with all eight CPU clock/steal slots observed; policy service equals
the runtime ledger; policy queue counts agree; heap integrity is OK; tracked CPU
and browser jobs exit zero with no live descendants; final audio stream lists
are empty. Each original launcher and routing observer exits zero, four stopped
filesystems pass checks, and the twelve saved documents/four original profile
trees retain their checked contents/ownership. Those collection gates do not
override the audio exclusions above. All VMs and both private services stopped.

The 4-ms build passes all 62 hosted suites. Both request values pass the EEVDF
core/module tests, including weighted service, eight-owner SMP models and exact
nanosecond residual checks. Both native builds pass the 13-module/106-export
import audit. See [build configuration](desktop-scheduler-tuning.md#normal-request-control-build).

Ignored evidence under `build/workloads/timing-scheduler-20261020`:

- `ee-slice-cohorts` and `ee-slice-reverse`: original controllers, launchers,
  source hashes, buffered traces, captures, runtime and input receipts.
- `eevdf-slice-results.json`: separate collection, native-fault and audio-window
  flags; per-run results and exact kernel/module identities.
- `eevdf-slice-sources`, `eevdf-slice-build-comparison.json`,
  `eevdf-slice-alloc-sections.json`, `eevdf-slice-frozen-2800000`: source/build
  and common-artifact construction evidence.
- `eevdf-slice-host-*`, `eevdf-slice-kernel-*`: tests, configure/build logs and
  frozen native artifacts. Invalid cache value zero is explicitly rejected.

Private reviews are `build/reviews/timing-mix-es-{e4a,e28a,e28b,e4b}-20261020`.
They retain serial traces, original wrapper exit receipts, filesystem checks
and saved-data preservation reports. The generated media and all game assets
remain outside Git.
