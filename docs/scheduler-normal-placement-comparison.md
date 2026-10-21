# Normal-placement scheduler comparison, 21 October

Two fresh application cohorts per policy reproduce a substantial loaded-browser
advantage for MLFQ over RR with the current kernel and original HDA lead.
EEVDF also records responsive loaded playback, but **both EEVDF cohorts fail
final runtime accounting acceptance** during their second Firefox launches.
Neither scheduler nor audio defaults change. The EEVDF failures are retained,
not overridden by policy/ledger agreement or clean shutdowns.

## Controlled setup

All six VMs use eight vCPUs/eight GiB, original QEMU 10.2.2 and PAUSE behavior,
callback HDA, requested 20-ms PipeWire buffering and physical default output
routing. All current QEMU-thread masks are audited at endpoints as CPUs 0–15.
These are shared-host, warm-cache observations, not isolated-host benchmarks.
No host CPU workers, CPU profiler, timer/ready recorder or AP-accounting
intervention runs. AP accounting remains blocking. Delivery diagnostics are
active only around PCM captures.

The common kernel SHA is
`fc41f3dbf97797d1f349754303090fee0b6e14976f8350ae717e1636a104d8bb`.
Every VM uses the same complete thirteen-module capsule, including original
12-KiB-lead HDA `f05fdc31f3ea5c1737dfe732c3acc2dc4ca6820217d8be9e4253d9bea83e1b3e`.
Only the private boot-policy selection changes. RR initializes first; optional
policies are selected before the scheduler starts dispatching. A launcher
initially expected a different log line in the first MLFQ boot. Its failed
observation is preserved; the same VM's live policy, selection-before-dispatch,
frozen hashes and diagnostic/audio settings were verified without restarting
or changing policy after dispatch.

The order is RR, MLFQ, EEVDF, RR, EEVDF, MLFQ. Every VM launches a fresh Firefox
profile against the same generated local clip/page, captures quiet playback,
then runs eight finite guest CPU workers and four periodic sleepers for twenty
seconds. A second fresh browser launch tests restart/quiet playback. The first
RR uses the preceding lead-comparison controller; the later controller adds
policy-service observations outside capture windows. Measurement paths remain
unchanged. The guest benchmark binary SHA is checked before use; all workers
perform nonzero work, and each loaded capture lies inside every worker's
observed first/last work timestamps.

Buffered schema-2 traces retain matching phases/epochs, monotonic sample times
and zero dropped trace records. Independent HTTP clock anchors select samples
wholly inside capture host brackets with two-ms timestamp quantization
allowance. That mapping assumes a 1× clock; the separate clock brackets contain
that rate. Samples retain playback rate 1, audio present, unmuted and no media
error. Video callbacks are opportunities, not physical presentation frames.
One key per loaded run measures DOM handling and subsequent RAF opportunity;
these are individual bounded observations, not input-latency distributions.

## Observed loaded behavior

Ranges contain two runs, without statistical confidence or worst-case claims.
EEVDF's loaded snapshots precede its later accounting failures; its full cohorts
remain unaccepted.

| Policy | Audio completion / guest second | Video callbacks / page second | Periodic p95 | Input-to-DOM upper bound | CPU work batches |
| --- | ---: | ---: | ---: | ---: | ---: |
| RR | 0.147–0.182 | 0.131–0.132 | 7.23–7.44 ms | 2658–3522 ms | 4.517–4.537 million |
| MLFQ | 0.954–0.992 | 10.61–11.63 | 2.00–4.34 ms | 91–218 ms | 3.737–3.866 million |
| EEVDF, runtime gate failed | 0.952–0.998 | 10.23–11.94 | 1.83–1.98 ms | 92–114 ms | 3.865–3.941 million |

RR stores only 2.245/2.984 PCM seconds during approximately eight host seconds.
MLFQ stores essentially all eight seconds; EEVDF stores approximately 99.76–99.96%
of the host interval. WAV storage can omit inactive output, so quiet-bin
percentages alone cannot measure those much larger missing intervals. These
are QEMU PCM captures, not timestamped speaker recordings or listening acceptance.
Counter completion is not decoded music tempo.

MLFQ spends more capacity on the application while completing fewer CPU work
batches. Batch counts are useful work, not precise per-task CPU service or proof
of arbitrary fairness. Its worst periodic delays remain 112–149 ms, versus
26–27 ms on RR; EEVDF records 96–122 ms. Better p95 or browser playback does not
establish a realtime bound. Quiet/relaunch callback rates also vary, including
an EEVDF relaunch near 8.19/s; neither optional policy has established a universal
performance win or a complete smooth-desktop acceptance result.

## Runtime failures and lifecycle gates

Both RR and both MLFQ cohorts have zero invalid, backwards, clamped, stale and
saturated runtime counters. Both EEVDF cohorts start their second Firefox launch
with zero clamps, then record two and three clamps respectively. All other
runtime error counters remain zero. The retained first/max events are:

| Cohort | CPU / ledger owner | Wall increment | Steal increment | Service-floor deficit |
| --- | --- | ---: | ---: | ---: |
| EEVDF a | CPU5, PID/incarnation 498 | 53,248 ns | 58,135 ns | 4,887 ns |
| EEVDF b | CPU4, idle owner 0 | 43,814 ns | 60,929 ns | 17,115 ns |

Each retained floor equals the preceding wall-minus-steal value. Neither raw
counter reverses, but their difference does. A stable wall read bracketed by
unchanged steal values does not establish monotonicity between independently
published samples. The records do not identify the cause, prove harmlessness,
or attribute the correction to EEVDF. Earlier positives under other policies
remain relevant. See [runtime-clamp evidence](runtime-clamp-evidence.md).

MLFQ and EEVDF policy-service totals equal their ledgers at both boundary
snapshots, with zero policy saturation. This checks forwarding of the same
accounting result; it does not clear EEVDF's clamping failures. RR deliberately
has no policy-service total, so that check is unsupported rather than passed.

All eighteen browser captures retain stream continuity and zero invalid
clock/position, unsafe publication, diagnostic contention or saturation. All
six final serial logs have no native HDA fault. After browser teardown, each
VM drains two finite ten-second packet-coded OSS streams: negotiated four/eight
fragments of 4096 bytes, exactly 441,000 submitted/completed frames, with capture
containment inside the stream lifetime. Decodes find no skipped or repeated
intermediate symbols and pitch near 1×. These finite drains occur after CPU load;
they do not validate loaded drain behavior.

All original browser/load/export jobs, fixture servers and VM owners exit with
their required identity/EOF gates; heaps and all twenty-four filesystem checks
pass. Those lifecycle checks do not override runtime failure or RR's failed
playback performance. The aggregate analyzer now prints final gates explicitly
and returns nonzero when a runtime/native fault gate fails, while preserving
the complete report and observed performance.

## Next work and evidence

MLFQ has two clean application cohorts and a substantial repeatable advantage
under this load. EEVDF needs a focused clock/accounting audit before acceptance:
bounded per-CPU history around a positive clamp should retain accepted pairs,
owner transitions, floor evolution and catch-up without changing decisions.
Do not retry until monotonic, reset floors or waive small deficits.

The next separate host-pressure comparison should use fixed-12-KiB MLFQ with
blocking versus AP-try accounting, retaining original QEMU/PAUSE behavior,
verified helper/vCPU placement and finite load. Require useful playback and
progress alongside fault avoidance; reverse a materially different result.
A passing MLFQ pressure run cannot clear EEVDF's runtime failures.

Historical accelerated playback, severe overcommit, the independent HDA-lead
candidate, kernel preemptibility and broader multi-app/gaming acceptance remain
open. This eight-worker cohort does not replace earlier 24-worker or host-load
failures. RR remains the production default.

The ignored derived index is
`build/workloads/timing-scheduler-20261021/policy-normal-cohort/policy-summary.json`.
Raw reviews are `unattended-hda-browser-normal-12k-r2-20261021` and
`unattended-policy-normal-{mlfq-a,eevdf-a,rr-a,eevdf-b,mlfq-b}-20261021`.
Each review preserves its exact executed controller, raw phase/final gates,
trace and PCM provenance, original owner receipts and filesystem results.

## Later signal-fix follow-up: MLFQ also exposes a clamp

The [signal/wait correctness follow-up](signal-wait-restart.md#browser-follow-up-correctness-pass-is-not-runtime-acceptance)
subsequently records two runtime clamps in a fresh MLFQ browser/relaunch/finite
OSS cohort on kernel `b3ffe02c…`. Its first deficit is 7,682 ns on CPU3/PID621,
after the second browser's zero-clamp pre-close snapshot and before the finite
OSS source begins. Its paired baseline has no clamps. All original job/VM and
filesystem gates pass, but that final arm fails runtime acceptance.

This later kernel comparison does not alter the earlier six-policy measurements
or establish a scheduler-specific cause. Earlier targeted-wakeup MLFQ and physical-output RR controls already exposed
clamps, as recorded in [the cumulative evidence](runtime-clamp-evidence.md).
This recurrence reinforces that cross-policy uncertainty. Keep both the old and new failed gates open until
causal evidence explains the wall/steal correction; do not promote MLFQ on the
basis of the older clean runs alone.
