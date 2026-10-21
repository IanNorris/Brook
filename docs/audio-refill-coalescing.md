# Bounded audio refill coalescing experiment

The [independent writer control](audio-mix-timing.md) loses progress without
graphics while the coordinated writer stays closer to real time. Complementary
channel gaps suggest missed mix contributions; the captures do not prove that
both software queues were ready at preparation. This experiment gives a recent
contributor one opportunity to refill before an otherwise one-sided batch.

The feature is **off by default**. It does not change the HDA driver, its
publication predicate, the mixer batch limit, or the default scheduler.

## Contract

`audio coalesce on|off|reset|status` controls and observes this kernel-only
experiment. `on` and `off` clear pending grace; `reset` also clears counters.
Do not use these controls during a measured interval. After finite sources
finish, read status before disabling and check that every deferral has a forced
resume. The status operation reads cached state without polling the driver.

A stream qualifies only when it is enabled, not discontinuous, has an empty
software queue, and still has frames in flight. Another enabled stream must
have software samples ready. Never-written, paused, reset, closed and fully
drained peers cannot cause a delay. Each empty episode receives at most one
grace; new accepted source data or an actual enable-state transition rearms it.
An idempotent enable does not. A short but positive peer queue retains the
existing maximum-length mix and silent suffix; this experiment does not solve
that case.

There is at most one consecutive deferred flush opportunity across all streams.
The next flush proceeds even if the same peer remains empty, a different peer
becomes empty, or the invocation arrives late. Stream identity and epoch must
match before a refilled peer counts as recovered. Deferral does not consume
samples, create a device receipt or suppress the driver's periodic service.

Deferral additionally requires a running, non-faulted native `intel_hda`
snapshot from this flush, with the same hardware epoch. Its age starts before
the progress callback and must be at most ten ms. Physical submitted-minus-
completed bytes must cover **12,288 bytes plus the rounded frame cost of the
snapshot age and another ten ms**, at 44.1-kHz stereo S16. Missing, backwards,
stale or overflowing clock evidence bypasses the delay. The default-off path
does not add clock reads.

The driver name cannot certify its configured lead. Native tests must pin the
unchanged twelve-KiB HDA module, SHA-256
`f05fdc31f3ea5c1737dfe732c3acc2dc4ca6820217d8be9e4253d9bea83e1b3e`.
No generic driver latency guarantee is implied. Host preemption after the clock
read, mixer execution and late scheduling can exceed the nominal allowance.
Existing HDA fault detection remains authoritative: one opportunity is not a
ten-ms wall-time bound or a replacement publication safety argument.

Counters expose deferrals, forced resumes, late resumes, headroom bypasses,
invalid clocks, unsupported drivers and recovery of ready contributors.
`recovered_ready_contributors` means a matching queue became ready by the next
preparation; it does not mean the driver accepted or played those frames.
All counters saturate visibly rather than wrap.

## Hosted evidence

`test_audio_delivery` compiles the production mixer and HDA bodies with MMIO
and clock side effects. It stages two exclusively left/right contributors with
a sixteen-KiB owned DMA queue, then makes only the left producer ready. The
baseline accepts left-only PCM. With coalescing enabled, no PCM is accepted on
that invocation; a right refill before the next invocation produces both
channels at the original device watermark. Exact completion is checked for
both sources.

Additional cases cover a peer that never refills, idempotent enable, paused and
fully drained peers, reset and close/reuse during grace, a changing missing
participant, short positive queues, partial native acceptance and full-device
retry without duplicate credits, headroom threshold ± one frame, snapshot age,
unavailable clocks, and service continuing after deferral. All 73 hosted suites
passed; after the review correction, the affected actual-body suite passed
again. Native two-pass linking, independent symbol-table comparison and all
thirteen original module imports also pass.

## Native acceptance required

Use one frozen kernel and original modules for off/on/off, with identical
pregenerated independent sources and fragment geometry. Enable only once both
sources have started, then leave the setting unchanged until their exact drains.
Capture both channels during a common steady interval, retain independent
host/guest clock brackets, and reverse producer job submission order separately
from the feature setting. Actual starts can still differ; do not infer an
ordering effect from a single pair.

Require positive deferral and recovery exposure before claiming a useful test.
Compare source progress, complementary and joint quiet bins, PCM coverage,
headroom bypasses, late resumes, HDA reservations and faults. Improvements must
reverse when disabled without sacrificing exact completion or original job,
VM, filesystem, runtime, policy, IRQ and heap gates. Older failed browser/game
runtime gates remain failed. If eligibility is rare or progress does not
improve, stop this experiment rather than expanding the waiting policy without
new evidence.

## Native result: no consistent progress benefit

`unattended-audio-coalesce-mlfq-a-20261021` uses one frozen kernel,
`ef0bf43b5eb0c6bb3d65971253ef7920e1498244ed8fa2b384c2a21402c3c3c6`,
the original thirteen modules (including the pinned twelve-KiB HDA), MLFQ with
blocking accounting, eight vCPUs/eight GiB, normal host mask 0–15 and physical
PipeWire callback audio with requested twenty-ms buffering. Other timing/owner
probes remain off; HDA delivery diagnostics run during the six-second captures.
There are no graphics applications or CPU workers. Each phase tests both job
submission orders with separate twelve-second independent sources.

| Setting / submission order | Per-stream nominal completion | Left / right quiet bins | Capture deferrals / forced resumes / ready recoveries |
| --- | ---: | ---: | ---: |
| Off A / left first | 0.8347 / 0.8335 | 12.57% / 13.19% | 0 / 0 / 0 |
| Off A / right first | 0.8591 / 0.8597 | 11.38% / 10.13% | 0 / 0 / 0 |
| On B / left first | 0.8724 / 0.8741 | 9.55% / 9.52% | 7 / 7 / 7 |
| On B / right first | 0.8247 / 0.8279 | 13.76% / 13.02% | 16 / 16 / 16 |
| Off C / left first | 0.9314 / 0.9315 | 4.15% / 4.25% | 0 / 0 / 0 |
| Off C / right first | 0.9728 / 0.9728 | 1.43% / 1.16% | 0 / 0 / 0 |

Whole enabled-source intervals record 25/29 deferrals, matching forced resumes,
with 23/27 ready recoveries. The measured windows include six/eight late
resumes. No headroom, clock or unsupported-driver bypass occurs. Thus the
opportunity is exercised, but there is **no consistent reversible benefit**.
The second disabled pair improves further; this is not evidence that enabling
the feature causes a regression either. Source start spreads are about
498–502 ms for the first five cases and 2.85 ms for the last. Scheduling/refill
phase and run-to-run variation remain confounders.

Independent channel fits agree with the completion-rate pattern and retain
near-one pitch scale. Stored PCM covers 99.945–100.046% of host capture time,
within these capture brackets' precision. Both-channel quiet bins are zero
except the last case (0.083%). All six windows have zero HDA silence
reservations and zero unsafe, invalid, contended or saturated delivery records.
Every source submits/completes exactly 529,200 frames. All twenty-four original
source/report jobs finish with complete identity/group/output/EOF receipts;
the original controller and VM owner exit zero; four filesystem checks and
runtime, policy, IRQ and heap gates pass. This does not clear older failed
browser/game runtime gates or prove physical speaker tempo.

Checked decoding, counters and original receipts are under the review's
`audio-coalesce-control/`, including `summary.json` and per-case
`joint-quiet.json`. The candidate remains off and is not accepted as a playback
fix. Stop expanding the waiting policy on this evidence. A follow-up must
distinguish short positive queue prefixes and refill phase from empty-queue
opportunities, or return to the remaining timing/scheduler correctness audit.
