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

## Next causal comparison

The [private QEMU build](../tools/qemu-fence-poll-experiment/README.md) prepares
matched 10-ms and 1-ms variants. Both retain all guest fences, ownership rules,
timeouts and validation. Compare the rebuilt 10-ms baseline before the 1-ms
candidate using identical private images and repeatable source/frame, audio,
accounting and lifecycle checks. A presentation improvement would implicate
host completion observation, while faster polling may add host wakeups/CPU
cost. No default promotion follows from the build or a single improved run.

Evidence is in the review's `mixed-prefix-control/post-timing-gpu-host-trace/`:
full log, exact clock brackets, original controls, retained PCM, event states,
source-preserving scope and analysis. The original controller/VM are terminal;
no VM remains. The matched host builds are tracked separately until completion.
