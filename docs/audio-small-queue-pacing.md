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

Next test submitting available prefixes of blocking writes while preserving
full-write expectations, stream/control epochs, format conversion and buffer
ownership across waits. Concurrent writers, reset/pause, signals, partial faults
and resampled inputs must remain correct. Compare the same queue geometry
before/after and keep the native publication/fault tests. An alternative lead
change needs explicit safety evidence, not merely better playback numbers.

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
