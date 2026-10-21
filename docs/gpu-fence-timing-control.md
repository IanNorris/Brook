# Host GPU fence timing control

The private review `unattended-gpu-host-trace-fixed-mlfq-b-20261021` retains the
pipe-fixed kernel `44b64074d04aa396495b7e00c52787133b692fe4fb68e0366c5afc7666018020`,
original thirteen modules, optional 6174-frame logical ALSA buffer, MLFQ,
eight vCPUs/8 GiB, host mask 0–15, physical PipeWire 20-ms callback audio and
12-KiB HDA lead. No guest synchronization or scheduler default changed.

## Collection and interpretation

A separate six-second diagnostic follows all eight normal measurement windows
and the complete browser loop. It enables only QEMU's fence-control,
fence-response and context-submit events plus the existing HDA delivery probe.
The host logs have UTC timestamps through `-msg timestamp=on`; a tiny no-disk
QEMU calibration confirmed that default traces lack timestamps and that this
option supplies them. An earlier review was stopped cleanly before any app
workload when that requirement was discovered. Its setup-only evidence and
four filesystem checks remain separate from performance results.

The trace event locations are checked against the exact
[QEMU 10.2.2 source](https://raw.githubusercontent.com/qemu/qemu/v10.2.2/hw/display/virtio-gpu-virgl.c):
context-submit precedes renderer dispatch; fence-control follows dispatch and
precedes fence creation; fence-response precedes response publication. The
fallback fence-poll timer rearms at 10 ms. These brackets exclude earlier guest
notification and later guest completion detection. They measure elapsed host
intervals, not physical GPU execution time.

The trace contains 1600 context-submit events, 1112 fence controls and 1112
responses. There are 1111 complete fence pairs, one leading response whose
control preceded recording, and one trailing control whose response followed
recording. The observed guest path owns one outstanding control command.
The analyzer verifies unique fence IDs across the entire capture, immediate
raw-line adjacency for submit/control association, one outstanding fence, and
boundary-only unmatched events. Negative controls reject reused IDs and an
interior missing control; an intervening unknown line retains the fence pair
but removes its context association. All 1112 observed controls are type 0x207
and immediately follow a context-submit event, so the stricter checks preserve
the measured context distributions.
The logger exposes no producer-drop count; reconciled pairs do not establish
complete or unbiased capture. Trace logging and the extra HDA clock bracket
may perturb this separate diagnostic.

| Context | Complete pairs | Dispatch p50 / p95 | Fence-response p50 / p95 | Maximum fence interval |
| --- | ---: | --- | --- | --- |
| 2 (compositor) | 707 | 55 / 94 µs | 9.502 / 10.095 ms | 11.006 ms |
| 257 | 218 | 14 / 39 µs | 12 µs / 9.624 ms | 10.049 ms |
| 258 | 186 | 16 / 37 µs | 12 µs / 9.916 ms | 19.440 ms |

Of the compositor's 707 paired submits, 186 have zero command bytes; the other
sizes are 152 (140), 240 (61), 324 (79) and 952 (241) bytes. All 218 context-257
and 186 context-258 paired submits are empty barriers. Response to the next
observed context-submit has a 132-µs median and 2.593-ms p95, including guest
observation, scheduling, construction and untraced commands; this is not a
scheduler-latency measurement.

Compositor fence intervals sum to 4.875 seconds. This is a strong lead for
completion-observation cadence in the serial path, not GPU utilization or proof
that any guest fence is redundant. In particular, source ownership barriers
must survive any subsequent batching experiment.

## Audio and timing failures remain visible

The six-second WAV contains exactly three all-channel zero runs: 320, 384 and
64 frames. The HDA delivery probe independently records three silence
reservations of 1280, 1536 and 256 bytes, in the same order and with the same
3072-byte total. It has zero invalid clocks/positions, unsafe publications,
contention, saturation or overwritten reservation entries. Gaps between the
previous accepted submission and the reserving copy are about 25.0, 35.0 and
21.1 ms.

This count/order/size correspondence strengthens the reservation explanation.
It is not a claim of exact time alignment or cause: the conservative guest/host
constant-offset interval is 28.819 ms wide and the WAV lacks chunk-arrival
timestamps. The clock brackets admit a constant unit-rate mapping; they do not
prove the rate remains constant between observations. Recorded GPU-interval
overlaps therefore remain possible overlaps, not causality.

The preceding eight-window run completes the browser transfer and loop and all
nine original jobs, heap/policy, native fault/discontinuity, original owner exit
and four filesystem checks. Its first Firefox-only capture is poor (0.207
nominal audio completion, initially empty source queues); enabled mixed/loaded
captures subsequently return about 0.999–1.001. This startup variation is
retained, not pooled away. Runtime acceptance fails four clamps. Earlier clean
cohorts do not waive this failure, and the post-timing trace is not substituted
for the ordinary measurement windows.

## Matched polling comparison, 21 October

The [private QEMU build](../tools/qemu-fence-poll-experiment/README.md) completed
both source-built variants. Version, no-disk QMP lifecycle, timestamped trace
and disassembly checks passed. Both retain all guest fences, ownership rules,
timeouts and validation. The only source substitution is the rearm constant.
The rebuilt 10-ms baseline ran before the 1-ms candidate, with identical frozen
kernel/modules, ALSA module, fixture and executed controller/helper sources.
These are one ordered pair, not repeated or balanced trials.

| Phase | Firefox callbacks/s, 10 → 1 ms | Quake swaps/s, 10 → 1 ms | QEMU CPU cores, 10 → 1 ms |
| --- | ---: | ---: | ---: |
| Firefox alone | 10.16 → 22.14 | — | 2.21 → 2.24 |
| Both, horizon on A | 6.00 → 17.72 | 8.99 → 16.45 | 1.73 → 3.05 |
| Both, horizon off B | 5.22 → 14.35 | 8.52 → 17.98 | 3.62 → 1.67 |
| Both, horizon on C | 5.39 → 18.34 | 8.17 → 16.62 | 2.11 → 5.10 |
| Eight CPU workers, Quake focused | 5.27 → 20.66 | 7.99 → 16.96 | 8.07 → 8.12 |
| Eight CPU workers, Firefox focused | 5.79 → 19.92 | 8.23 → 17.68 | 8.07 → 8.17 |
| Firefox after Quake closes | 10.69 → 22.74 | — | 2.56 → 2.52 |
| Reopened Quake, horizon off | 6.13 → 12.69 | 8.34 → 17.18 | 2.31 → 2.43 |

The browser uses the local 640×360 fixture; Quake uses GL3 1280×720 menu
attract mode, timedemo off and maxfps 60. Callbacks and swaps are not physical
scanout. CPU values are process CPU seconds divided by host monotonic elapsed
brackets, including KVM guest CPU, excluding separate PipeWire/renderer/helper
processes. Context switches are recorded separately and are not wakeup counts.
Costs vary substantially between normal windows; this is not a free speedup
or an established efficiency result. One DOM input probe has overlapping timing
bounds, so no input-latency improvement is claimed.

Both runs complete nine original jobs, the full 120-second clip and loop,
complete HTTP transfers, heap/policy checks, native audio fault/discontinuity
checks, original controller and VM exits, and all four filesystem checks.
Current runtime quality counters are zero in both. That does not resolve the
prior trace run's four clamps or the larger accounting audit.

Separate post-timing host traces reduce compositor fence-response median from
9.526 to 2.472 ms and p95 from 10.057 to 3.343 ms. Dispatch medians remain
49 and 54 microseconds. There are respectively 731 and 1865 complete compositor
pairs. Maximum fence intervals remain 10.637 and 10.873 ms. These observations
and the frame progress support completion-observation cadence as a material
bottleneck in this workload; no guest ownership barrier was removed.

### Common enabled trace window

QMP enables event types sequentially. The 10-ms raw log begins with three
controls before response tracing is enabled; the initial strict parser rejection
is retained. Analysis now verifies all three enabled states and slices to the
common enabled interval using explicit persistent-debug RPC monotonic brackets,
converted through conservative host realtime/monotonic offset bounds with a
one-microsecond timestamp margin. It does not use filesystem modification times.
The first RPC begins after enabled-state confirmation and the last finishes
before disabling starts. Three prefix/one suffix records are excluded for the
baseline; three prefix/three suffix for the candidate. Exact excluded records
and full logs are retained. Unmatched events are permitted only at boundaries;
whole-capture ID uniqueness, raw adjacency and single-pending checks remain.
Negative controls still reject reused IDs and interior missing controls and
remove context attribution across an unknown intervening line.

### Audio gaps and next diagnostic

Enabled/loaded audio completion remains about 0.998–1.001 nominal seconds per
guest second in both runs, while mixed horizon-off phases remain around 0.54
for Firefox and 0.67 for Quake. Completion averages do not establish source tempo
or uninterrupted output. The baseline diagnostic WAV has one 192-frame zero
run matching one 768-byte reservation; the candidate has two 64-frame runs
matching two 256-byte reservations. Short silence gaps remain despite the
presentation gain. Conservative clock brackets cannot align these millisecond
gaps precisely with host fences.

The next diagnostic combines existing worker sleep/service timing with delivery
reservations while retaining 12-KiB HDA lead and the fixed logical ALSA geometry.
The worker probe retains only slow episodes, not every service cycle. Absence
of a retained event cannot prove timely refill or enough source data. The
nominal (4096−3072)/44100 = 23.22-ms travel from a full horizon to the reservation
threshold is not a guaranteed worker-service budget: the horizon is a ceiling,
not a maintained minimum, and DMA may advance in bursts. Lowering the HDA lead
or raising the horizon would confound this attribution and is not accepted here.

Evidence is retained in
`build/workloads/timing-scheduler-20261021/logical-buffer-probe/qemu-poll-pair-comparison.json`
and the private reviews `unattended-qemu-poll-{10,1}ms-fixed-mlfq-a-20261021`.
Actual executable SHA-256 values are
`7dec38596c5425e2e0a5a002a62f2276cd3629adbc37b57f9aa6cbb4db257653`
(10 ms) and `4bc15b9ca1d6d079814be9d4c62f3c963d36e9403aa7bb24d8e98b291ad60646`
(1 ms). All original build/controller/VM handles are terminal with exit zero.
No defaults or scheduler policy were promoted. Historical fast playback,
uninterrupted audio, prior accounting failures and the full clock/SMP/fairness
audit remain open.
