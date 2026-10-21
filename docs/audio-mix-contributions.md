# Accepted mix contribution accounting

The mixer now has a default-off diagnostic that distinguishes missing empty
queues from missing tails of short, positive queues. Native captures identify
short tails as the dominant missing-contribution category in two independent
writer windows. No mixing, waiting, scheduler or driver safety policy changes.

## Commands and exact units

`audio contributions on|off|reset|status` uses the ordinary guest debug channel.
Controls and snapshots use the existing mixer lock; JSON formatting occurs after
release. This is not panic-safe. Status copies approximately 3.4 KiB of bounded
state under that lock, so avoid frequent polling during performance captures.
There are no additional clock reads, driver polls or allocations. The disabled
path still has condition checks and a 256-byte temporary stack reservation;
its plain preparation fields are initialized only when recording.

For each included identity/stream epoch and validated nonzero native receipt:

```
accepted_output_frames = contributed_frames + missing_empty_frames + missing_short_frames
```

Contribution uses the actual prepared contribution length capped to the accepted
prefix. Empty/short classification and occupancy bins use the preparation-time
snapshot, not the queue after the driver returns. A late producer cannot change
PCM already copied. Close, reset, pause or slot reuse during copying retain the
old captured identity/epoch in this diagnostic, independently of whether that
old receipt can still credit an application's completion counters.

`output_frames` counts unique accepted mixer-output frames. Per-stream totals
are stream-frames and cannot be summed as a physical output duration. Missing
frames are also weighted into software occupancy bins (zero, 1–440, 441–1023,
1024+) and in-flight occupancy bins (zero, positive below half capacity, at least
half). Each histogram sum equals that row's empty-plus-short missing frames.

Enabled, nonfaulted streams that have submitted PCM in their current epoch are
included. Opened-never-written and disabled endpoints are excluded. A drained
but open producer remains observable: its missing frames do not prove the app
currently demands audio. In-flight occupancy means already committed samples,
not fresh software data available for the next preparation.

Thirty-two historical identity/epoch rows remain until reset. A full table does
not evict old rows; omissions are explicit in `omitted_observations` (omitted
stream observations) and `omitted_stream_frames`. Counters saturate with an
explicit flag. Invalid inputs, omissions, saturation or token exhaustion prevent
complete-accounting acceptance.

Controls advance a token. A preparation racing enable/reset/off is fenced out;
submissions retired by the existing delivery-epoch check are also excluded.
The counters therefore describe the validated accepted subset, not a complete
hardware timeline across controls or stop/restart. Accepted PCM may later fault;
this is attribution of exposed output, not proof of playback or speaker timing.
Keep the diagnostic epoch fixed during a measured interval.

## Verification

A new hosted model test checks partial/rejected input, conservation, bucket
boundaries, controls, historical epochs, table overflow/omissions, invalid input
and saturating arithmetic. The production mixer and HDA tests additionally cover:

- A positive short contribution and its actual DMA silent suffix.
- Drained-open, disabled and never-written endpoint eligibility.
- Partial native acceptance, full-ring rejection and subsequent remixed suffix.
- Producer refill, stream reset, close/reuse and pause during the copy.
- A diagnostic reset racing the copy, without changing the accepted PCM.

All 74 hosted suites pass. After removing implicit preparation-array
initialization from the disabled path, both affected suites pass again. Astra
reviews the actual implementation and finds no blocking correctness issue.
Native two-pass linking, independent symbol-table comparison, thirteen module
imports/109 exports and ABI-34 staging pass; original driver bytes are unchanged.

## Native classification

A fresh private eight-vCPU/eight-GiB MLFQ VM runs two independent finite sources,
one in each stereo channel, with four 4096-byte fragments, unchanged 4096-byte
write requests and configured HTTP barriers. Both use the original 12-KiB HDA,
physical callback PipeWire/20-ms buffering and host mask 0–15. No graphics or CPU
workers run. Diagnostics remain off except around the matching six-second PCM
captures. Sources are submitted left-first, then right-first; this single pair
cannot establish an order effect.

Each row below is one stream in the submission-counter interval. Its interval
encloses the PCM capture but is not sample-aligned with playback: submissions
lead hardware completion, and endpoint in-flight queues remain explicit.

| Trial / stream | Accepted output | Contributed | Missing empty | Missing short | Short share of missing |
| --- | ---: | ---: | ---: | ---: | ---: |
| Left-first / 17 | 288,576 | 265,856 | 896 | 21,824 | 96.1% |
| Left-first / 34 | 288,576 | 265,920 | 1,344 | 21,312 | 94.1% |
| Right-first / 49 | 288,144 | 274,480 | 496 | 13,168 | 96.4% |
| Right-first / 66 | 288,144 | 274,592 | 192 | 13,360 | 98.6% |

Each window has 639 accepted observations. All per-row conservation and both
histogram identities hold exactly, with fixed epochs and zero invalid records,
omissions, saturation or exhaustion. Every missing frame falls in the
in-flight-at-least-half-capacity bin; most short missing frames have only 1–440
software frames available at preparation. This directly identifies accepted
batches extending beyond short positive contributions, without explaining why
refill arrives late or predicting a real-app improvement.

Completion rates are approximately 0.921/0.921 and 0.950/0.950 nominal.
Independent packet decoding gives source/PCM rates 0.913/0.916 and 0.944/0.948,
with pitch near one. Stored PCM covers 100.041%/99.994% of host brackets, within
measurement precision. Actual source start spreads remain 1.94/498.23 ms; do
not attribute differences between these two trials solely to submission order.

Every source submits and completes exactly 529,200 frames. All eight original
source/report jobs retain successful collected exits, full identity/output/EOF
and empty groups. Controller and original VM owner exit zero; all four stopped
filesystem checks, runtime/debt, policy ledger/ready counts, heap and IRQ gates
pass. HDA delivery has zero silence reservations and no invalid, unsafe,
contended or saturated records. Earlier application failures remain separate.

Frozen kernel SHA-256:
`0d93dfcce6e8be4f095b37574a08ad51ad013201436483c8f1f28305ec5878ac`.
Checked source hashes, counters, prepared sources, decoded PCM, endpoint queues,
original receipts and lifecycle gates are retained under
`build/reviews/unattended-audio-contributions-mlfq-a-20261021/audio-contributions-control/summary.json`.

## Next bounded intervention

The result explains why the earlier empty-queue coalescing experiment misses
most loss. A separate default-off experiment can shorten a batch to the shared
positive prefix only when fresh native headroom covers the unchanged 12-KiB
publication lead, snapshot age and the next nominal service opportunity. Empty
queues add no restriction. Keep one submission per ordinary service invocation;
immediately submitting the unmatched remainder would recreate the same tail.

This must retain every driver guard and fall back to the normal batch with
inadequate/stale headroom. A nominal service interval is not a wall-time response
guarantee. Actual-body tests must cover completion-driven refill, tiny prefixes,
a producer that never refills, low/stale headroom, partial acceptance and exact
drains. A native off/on/off control must show reduced short-tail loss and improved
source progress without more inactive output, reservations, faults or drain
inflation. If substantial shortening exposure provides no benefit, stop that
batching approach rather than adding more grace conditions. No default change
is justified by the current diagnostic alone.

The [guarded shared-prefix trial](audio-shared-prefix.md) subsequently exercises
this intervention in actual-body tests and one native off/on/off cohort. It
reduces short-tail loss and improves finite-source progress, but remains off
pending real browser/game acceptance.
