# Finite audio stream controls

The [mixed Firefox/Quake test](mixed-desktop-timing.md) slows both stream
completions near 0.62/0.54 nominal. A cheap native control removes application
rendering/decoding and coordinates two pregenerated OSS sources. It sustains
both near real time. A subsequent control with independent writers loses
progress without graphics, narrowing the investigation toward refill timing
and mixing. These controls do not identify a specific scheduler or mixer defect.

`scripts/workloads/audio_mix_timing.py` generates all source samples before
opening devices. A packet changes frequency every 100 ms using the existing
packet-code permutation. One stereo source contributes exclusively left; the
other exclusively right, at conservative amplitude. Sources share 44.1-kHz,
16-bit stereo format and negotiated four-by-4,096-byte fragment geometry.

One coordinator alternates nonblocking descriptor refill order every loop.
It attempts each descriptor, accounts partial writes/EAGAIN, sleeps one ms
only when neither accepted data, and records accepted-write and independent
completion brackets. The finite deadline is ninety guest seconds; external
observation timeouts must retain the same original job. A successful result
requires exact independent completion followed by explicit OSS sync and close.
Reported queued values are not a claim that Prepare saw both unconsumed queues.

For example, inside a private Brook VM without other audio streams:

```sh
python3 audio_mix_timing.py --seconds 12 --channels both --first 0 --output /tmp/mix-left-first.json
```

Repeat with left/right single-channel controls and reversed initial refill
order. Capture both PCM channels with independent host/guest clock brackets;
guest timestamps and complete bytes alone do not certify speaker timing.

## Native result

The fresh `unattended-audio-mix-mlfq-a-20261021` VM keeps kernel `b3ffe02c…`,
original thirteen modules/12-KiB HDA, MLFQ/blocking accounting, eight vCPUs/eight
GiB, normal host masks 0–15, physical PipeWire callback audio and requested
20-ms buffering. No games/browsers or CPU workers run. Owner/yield, idle
coalescing, runtime history and expensive timing recorders remain off. Delivery
diagnostics are enabled only for the same six-second steady capture per case.

| Case | Twelve-second source drain | Per-stream nominal completion | Fitted source seconds / stored PCM second |
| --- | ---: | ---: | ---: |
| Left only | 12.072 s | 0.9978 | 0.9985 |
| Right only | 12.149 s | 0.9987 | 0.9993 |
| Both, initial left first | 12.183 s | 0.9849 / 0.9849 | 0.9881 / 0.9874 |
| Both, initial right first | 12.477 s | 0.9616 / 0.9621 | 0.9675 / 0.9677 |

Each active descriptor submits and completes exactly **529,200 frames**.
Both channels independently decode with pitch fit near 1× and no observed
backwards/repeated symbol transitions in these captures. Inactive control
channels remain fully quiet. Active-channel stored quiet-bin fractions are
approximately 0.1% for singles, 1.0% for the first dual run, and 2.6%/1.8% for
the second. The variation does not establish a refill-order effect without
repeats. Fits are approximate, quantized and apply to stored PCM, not physical
speaker tempo. Partial steady captures cannot prove every source symbol played.

All capture brackets lie inside the finite sources. Independent clock brackets
contain 1×. Eight original source/report jobs finish with complete
identity/group/output/EOF receipts. Final audio drains, heap, policy/ledger,
ready count, IRQ IF/stale-ticket and runtime gates pass, with zero clamps in
this cohort. The original VM owner exits zero and all four primary filesystem
checks pass. These gates do not rehabilitate older failed application cohorts.

Raw evidence and checked per-channel decoding remain under that review's
`audio-mix-control/`, including `summary.json`. The new source's native code
and arguments are preserved per case in `guest-source.py` and original job
receipts.

## Independent writer control

The optional `--gate-url` waits for a finite HTTP barrier after source generation
and before device open. Its response must contain `released: true` and a positive
integer `release_host_ns`; the report retains that receipt and surrounding guest
timestamps. The socket timeout is 45 seconds. Without this option, the original
single-process coordinator behavior remains unchanged. A controller can launch
separate `--channels left` and `--channels right` original jobs, with unique
barrier URLs, and release both once their sources are prepared. Barrier release
does not guarantee simultaneous device configuration or start.

The fresh `unattended-independent-audio-mlfq-a-20261021` VM uses the same frozen
kernel, original modules, geometry, audio backend and policy as above, with no
graphics applications or CPU workers. Both cases submit and complete exactly
529,200 frames per source. Two six-second steady captures lie inside both
sources; the full source reports separately certify their exact drains.

| Job submission order | Left / right source drain | Per-stream nominal completion | Fitted source seconds / stored PCM second |
| --- | ---: | ---: | ---: |
| Left job first | 12.932 / 12.926 s | 0.9180 / 0.9176 | 0.9256 / 0.9272 |
| Right job first | 14.637 / 14.533 s | 0.8221 / 0.8221 | 0.8207 / 0.8180 |

Both-channel pitch fits remain near 1×. Stored PCM covers 99.997% and 99.976%
of the respective host capture brackets; omitted inactive-output time does not
explain this slowdown. Aligned approximately one-ms RMS bins (threshold 10 in
signed-16 sample units) show left-only/right-only quiet fractions of 5.55%/5.00%
and 13.56%/14.53%, with no bins where both channels are quiet. HDA delivery
diagnostics record zero silence reservations and zero unsafe, invalid,
contended or saturated records in both windows.

Complementary quiet intervals support missing mix contributions, but do not
prove that both software queues held samples when the mixer prepared a batch.
Cached stream totals include frames already in flight: the snapshots contain
0–448 software frames and 3,648–4,096 in-flight frames per stream. Accepted-write
gaps have a roughly 12-ms p95, with maxima of 28.72 ms in the first case and
83.65/33.02 ms in the second. Actual source starts differ by 2.2 microseconds
and 498 ms respectively. These phase differences, and the lack of repeats,
prevent attributing the variation to job submission order.

Eight original source/report jobs finish with complete identity/group/output/EOF
receipts, the original VM owner exits zero, and all four filesystem checks pass.
Runtime, policy, heap and IRQ gates pass with zero clamps in this cohort;
previous failed browser/game runtime gates remain failed. Checked results,
channel decoding and joint quiet-bin classification are retained under the
review's `independent-audio-control/` directory.

The [next experiment](audio-refill-coalescing.md) is a bounded, default-off opportunity to coalesce a refill
from a recently contributing empty stream, with explicit device headroom and
forced-resume limits. It needs actual mixer-body tests before native trials.
It must not wait indefinitely for every stream or alter driver publication
safety. The writer-immediate-flush hypothesis is ruled out by the current
source: writes enqueue; the mixer performs periodic submission.

The later [capacity control](audio-capacity-control.md) adds explicit four/eight
fragment selection and an optional barrier after device configuration. Its
four/eight/four sequence retains first-write phase variance, so larger buffers
are not accepted as a fix. It motivates accepted-prefix contribution accounting
rather than more empty-queue waiting.
