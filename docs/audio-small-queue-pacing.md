# Small audio queues and refill pacing

The [current Quake II music controls](q2-music-timing-control.md) retain a
repeatable roughly 0.83× source-progression result with the old and new MLFQ
modules, and with BSP or shared wake scanning. Synthetic numbered tones now
reproduce the problem independently of commercial-music spectral alignment.
This is slower progression, separate from the historical accelerated playback.

## Signal and controls

A generated thirty-second `packet-code-v1` signal uses the existing 100-ms
frequency permutation from `audio_timing.py`. It is encoded as stereo 44.1-kHz
Vorbis, quality 8. Fresh private data-disk clones receive that synthetic track
at Quake's track02 path before boot. The golden disk and original OST are never
modified. The native GL3 1280×720 music-only demo then plays it through its
normal OGG/OpenAL path. Both tests use the frozen candidate MLFQ/kernel from
the browser comparisons and SDL audio into an isolated PipeWire null sink,
callback HDA and requested 20-ms host output buffering.

The original PCM and host-decoded OGG controls each recover all 300 symbols,
source progression approximately 1.0000 and pitch scale 1.0000, with no missing
or backwards symbols. The guest Quake captures recover consecutive symbols
but substantially slower progression. A separate native-format OSS writer
avoids Quake and GL rendering and changes only the per-stream queue geometry.
No CPU stress workers run in this series.

| Path / queue | Source seconds per stored PCM second | Pitch scale | Quiet roughly 1-ms bins | 30-s source completion in guest |
| --- | ---: | ---: | ---: | ---: |
| Quake synthetic music, BSP | 0.8354 | 1.0000 | 15.12% | not a drain test |
| Quake synthetic music, shared scan | 0.8349 | 1.0000 | 15.44% | not a drain test |
| Direct OSS, default 16,384-frame capacity | 1.0002 | 1.0000 | 0% | 30.030 s |
| Direct OSS, four 4,096-byte fragments / 4,096 frames | 0.5392 | 1.0000 | 45.67% | 55.460 s |
| Direct OSS, eight 4,096-byte fragments / 8,192 frames | 0.9389 | 1.0000 | 5.92% | 32.053 s |

All captures contain approximately eight seconds of PCM over eight host
seconds. This distinguishes stored-output pacing from source progression;
neither a full WAV nor an unchanged pitch proves timely application audio.
Decoded packet sequences have no backwards transitions or unobserved IDs.
Their timing is quantized by 100-ms symbols and 10-ms analysis hops; the
displayed decimals are diagnostic fits, not frequency certifications.

The small-queue writer negotiates exactly the requested geometry, confirmed by
`GETOSPACE` before timing. Thirty seconds means 1,323,000 submitted and proven
completed native frames, followed by successful `SYNC`. The direct default
control predates geometry metadata in the helper; its capacity is visible in
the retained mixer telemetry. The four/eight-fragment controls record actual
geometry in their exported result files. They use the same thirty-second
100-ms packet write requests, not Quake's own write sizes or timing.

## Why the previous silence summary missed this

Quake's generated captures have approximately 15% quiet 1-ms bins, with longest
quiet runs around 27–30 ms. Many shorter gaps cross the boundaries of the old
10-ms RMS bins, making those bins non-silent. Looking only at the longest
10-ms run concealed their cumulative cost. The packet decoder now reports
quiet fraction and longest run in fixed 44-frame bins at 44.1 kHz, RMS below
10 s16 units. Bin alignment still undercounts partial short gaps; it does not
recover an exact gap timeline or identify where silence was introduced.

A generated regression inserts twenty milliseconds of silence after each
100-ms source packet. It verifies slower progression near 1/1.2, unchanged
pitch, consecutive symbols, roughly one-sixth quiet bins and no large single
gap. All six decoder regressions pass, including independent tempo, pitch,
skip/replay and plain-tone rejection controls.

## Source mechanism and next intervention

The blocking `/dev/dsp` write path waits for enough room for the entire bounded
converted chunk before submitting it. With a 4,096-frame endpoint and a large
write, this requires draining nearly or completely empty. Separately, the
[HDA publication repair](hda-near-empty-write-lead.md) reserves a 12,288-byte
lead, about 69.66 ms, when pending physical bytes fall below that lead. The
gap is cleared to silence; it must not credit application frames.

The measured queue-size intervention and quiet fractions support a refill /
safety-lead interaction, beyond ordinary scheduler dispatch latency. They do
not yet measure individual reservations or establish that every quiet sample
comes from that site. Increasing the queue helps but does not fully fix the
eight-fragment case. It is not an accepted solution or a reason to weaken the
publication guard.

The prefix-refill intervention below addresses the whole-chunk admission
problem. Residual gaps require a separate delivery-path investigation. An
alternative lead change needs explicit safety evidence, not merely better
playback numbers: the codec tail-drain requirement and safe publication
distance have different purposes.

## Available-prefix refill

Blocking DSP writes now submit available output prefixes rather than waiting
for the entire bounded converted chunk to fit. They still complete that chunk
before returning normally. A local `DspWritePlan` keeps one conversion origin
across retries, avoiding new rounding drift when splitting resampled output.
Each attempt re-stages the bounded user input and converts its remaining
prefix under the existing description mutex. No pointer into shared staging
survives a wait, and no additional buffer or mutex is allocated per write.

The mutex is released while waiting. A reset, pause or format/stream epoch
change after acceptance ends the old transaction with its accepted input-byte
prefix. Signals, copy faults and device faults similarly preserve accepted
bytes. Before acceptance a control change can restart the plan. Concurrent
writers can interleave prefixes; this does not promise atomic PCM writes.
Partial input-byte returns still use the legacy rounding rule. Interrupted
blocking prefixes therefore share the existing nonblocking resampler limitation:
a later syscall cannot recover an incompletely emitted source frame's fractional
phase. This expands where that limitation can occur; it needs stateful stream
resampling. Uninterrupted splitting within this bounded transaction preserves
the original conversion exactly.

| Direct OSS queue | Before: 30-s source completion | Prefix refill | Prefix refill plus extra completion polling |
| --- | ---: | ---: | ---: |
| 4,096 frames | 55.460 s | 33.955 s | 33.169 s |
| 8,192 frames | 32.053 s | 30.020 s | 30.032 s |

The retained prefix-only candidate's four-fragment capture progresses at
0.8827 source seconds per stored PCM second, with 10.52% quiet 1-ms bins; the
eight-fragment capture is 1.0003 with no quiet bins. The additional-polling
variant refreshes completion before initial/retried writes outside the mutex.
It does not remove the small-queue problem; its single-run difference does not
establish a repeatable benefit worth the additional device I/O. It is preserved
as an experiment and removed from the working source.

Quake confirms the limit of the prefix-only repair. Synthetic music still
progresses at 0.8323 with 15.86% quiet bins, and the original OST whole-capture
fit is 0.835× (similarity 0.832). Guest demo intervals are 15.223/15.243 seconds.
This repairs a large-write refill defect, not Quake's remaining pacing issue
or the historical accelerated playback.

The next controlled intervention is prompt native mixer delivery after a
successful prefix, outside the description mutex, to avoid an additional mixer
cycle before hardware submission. A native-only bounded attempt must retain
the flush-owner guard and must not enter the potentially blocking legacy
`play` path. Keep the current publication predicates, fault/completion rules
and 8,192-byte stop trailer. Before reducing the lead, measure actual reserved
gaps, pre/post-copy device advance and publication headroom, including wraps
and host-load tails; nominal sample-rate arithmetic cannot bound burst fetches.

### Prefix safety coverage

The production write plan is tested across 4/8/44.1/48/96/192-kHz input, mono/
stereo and u8/s16. Independent whole-conversion references match split/retried
prefixes, including partial acceptance, output bounds, epoch/format identity,
invalid input and shared staging interleaving. All 67 hosted suites pass;
supported compiled entry templates/mutations pass, with unsupported paths
still explicitly outside that audit. Thirteen module imports resolve against
106 kernel exports.

`dsp_write_controls.py` exercises eight actual guest syscall cases: fault before
acceptance, signals before/after acceptance, reset, pause, format change before
acceptance, a protected source buffer after acceptance, and two writers with
reset. Both variants pass, including the observed format-change-wins outcome.
The test allows the legitimate opposing format/reset race without claiming
that it exercised a branch it did not observe. Test mappings, fds, threads and
signals belong solely to the bounded test process in the private null-audio VM.

Both variants and both Quake follow-ups pass original owner completion,
job-lineage drain, runtime/heap checks, four filesystems and saved-data
preservation. Native result exports match guest size/SHA-256 receipts. All
thirteen driver/policy modules, including HDA, are byte-identical to the
pre-intervention capsule. No native HDA fault is recorded. No default policy,
wake scanner, queue geometry or driver safety rule changes. All test VMs and
the private null service stop.

The prefix-only frozen kernel is
`e5e096412c772989d8ad85bcbca6c581682cdef6776f8eda8ca07ae50601c393`;
the unretained extra-polling kernel is
`d3af323c1cb460cd9d557d22cbbef9ebbe2e6e3ef620bc49f253091c812b97d1`.
Ignored `dsp-{prefix,refresh}-{frozen,sources,geometry-sdl}` retain exact source,
identities, original controllers, control output and captures. Quake evidence
is in `dsp-prefix-q2-pair` and `q2-prefix-{packet,music}`.
`dsp-prefix-verified-results.json` and the preserved verifier record the gates.

## Evidence and lifecycle

All completed Quake and native groups drain with exit zero. Original launcher
and audio-router owners complete; four offline filesystem checks and saved-data
preservation pass. Candidate ledger/queue snapshots agree where collected.
Runtime checks retain no invalid/backwards/clamped/stale/saturated samples;
no native HDA fault is present. Native exported results match guest size/SHA-256
receipts after the original owners exit. No default scheduler, wake scanner or
driver safety rule changes. All private services stop afterward.

Ignored evidence under `build/workloads/timing-scheduler-20261020` includes
`q2-packet-{fixture,pair,bsp,shared}`, `native-packet-current-sdl`,
`native-packet-geometry-sdl`, `q2-packet-comparison.json`,
`q2-packet-quiet-windows.json` and `audio-queue-geometry-comparison.json`.
Original controllers, generated source, staged identity manifests, raw WAVs,
decoder output and exported geometry/results remain there. Only synthetic
generator/helper code, analysis tests and documentation belong in Git.
