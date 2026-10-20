# HDA near-empty write publication

The [mixer timing baseline](audio-mixer-service-timing.md) reproduces a native
write-publication hazard. The device has consumed all but 744 bytes of the
submitted audio when a new 3,072-byte write begins. Its position then advances
1,024 bytes during an approximately 49-microsecond bracket. The unchanged
publication guard correctly refuses the write: the device has already passed
its beginning. Only publication check bit 64 fails. Clock reversal, stale
position sampling and half-ring ambiguity do not cause this particular fault.

Previously the driver reserved a conservative write lead only when no audio
remained pending. A short pending tail could therefore expose the next receipt
to a burst fetch. The candidate extends that reservation to any running stream
with less than the existing lead pending. It preserves the owned tail, clears
only the unowned interval after it, and places the new receipt beyond that gap.
The lead remains 12,288 bytes, approximately 69.66 ms at 44.1-kHz stereo s16.
No publication safety predicate, ambiguity limit, fault watermark or restart
rule is relaxed. Application completion is still reconciled by receipt ranges;
playing unowned silence cannot credit application frames.

This can insert silence and increase latency when a producer arrives near
empty. Safety tests alone cannot establish acceptable playback pacing or
continuous audio. It does not fix a producer or mixer that fails to run on time.

## Deterministic checks

Production-driver hosted tests recreate the exact 744-byte tail and 1,024-byte
fetch in a 49-microsecond bracket, including a write crossing the physical ring
boundary. They verify that the old tail remains intact, the gap is silence,
the new receipt is safely ahead, and proven completion remains unchanged by
reservation. A larger injected fetch still triggers the publication guard.
Additional checks cover invalid reservations, a faulted clock, full-ring
ambiguity, old/new stream epochs and lack of application credit for the gap.
All 64 host suites pass. The native driver build resolves all thirteen modules
against 106 kernel exports.

## Browser controls

The baseline and candidate use one byte-identical kernel and twelve common
modules. Only `intel_hda.mod` changes. Fresh private eight-vCPU/six-GiB images
run Firefox's generated local video/audio fixture plus twenty-four CPU workers
for thirty seconds. Audio uses callback HDA, requested 20-ms buffering and an
isolated PipeWire null sink. The mixer timing probe is enabled in all these
diagnostic runs; CPU profiling and mapping counters are off.

| Candidate run | Policy | Video callbacks/s | Periodic sleeper p95 | Maximum mixer late lower bound | Native HDA fault |
| --- | --- | ---: | ---: | ---: | --- |
| wl-e1 | EEVDF | 4.95 | 3.82 ms | 225.67 ms | sample-gap |
| wl-m1 | MLFQ | 6.88 | 21.15 ms | 116.23 ms | none |
| wl-e2 | EEVDF | 4.23 | 3.06 ms | 126.99 ms | none |

Video callbacks use the buffered page trace wholly within conservative load
bounds, rather than sparse HTTP observations. Media progression is only
0.806–0.838 source seconds per page second. These are diagnostic controls, not
a statistically established performance improvement or default-policy gate.
No candidate reproduces the short publication fault, but three runs cannot
establish its universal absence.

The `wl-e1` sample gap is 217.210 ms, beyond the unchanged approximately
185.760-ms observation limit. Its retained mixer event covers the fault:
deadline setup at 59.843303392 s, resume at 60.078971777 s, service end at
60.078972414 s. The mixer is approximately 225.668 ms late and then returns
after 637 ns. This establishes a delayed-resumption episode rather than a
long service bracket. It does not separate blocked timer delivery, ready queue
wait, post-dispatch return or host delay. Other application paths can also
sample HDA while the mixer sleeps. This run remains unaccepted as clean audio.

All three original launch wrappers and audio routers exit zero. Runtime
accounting has zero invalid/backwards/clamped/stale/saturated counters, with
all eight paravirtual-clock and steal records active. All four filesystem
checks and twelve-document/four-profile preservation checks pass per run.
Mixer probe reports have zero invalid clocks, zero contended records and no
aggregate saturation. Smaller slow episodes are omitted from the top sixteen
and explicitly counted, so the retained list is not a complete timeline.

Build isolation, controller receipts and verified results are retained under
the ignored `build/workloads/timing-scheduler-20261020/audio-write-lead-*`
directories. The common kernel SHA-256 is
`8937859141ca14387045eaf9e2ee1077b2b2bc72a69c1cd79b280935bb5aac63`;
the candidate HDA module SHA-256 is
`d2aa17f166d69e8cae49ca3e36ccf24ecbb47ee12ee071cf8884ef6b6f969a5d`.
Scheduler selection/defaults and the driver ABI remain unchanged.

## Packet-coded pacing controls

Two more fresh EEVDF boots compare the baseline and candidate with mixer timing
disabled. Each plays a generated thirty-second sequence of uniquely permuted
100-ms tones, first idle and then with twenty-four CPU workers. Completion is
drained explicitly; captures cover eight host seconds per condition.

| Driver | CPU workers | Full completion, guest seconds | Captured PCM / host time | Decoded source / PCM time |
| --- | ---: | ---: | ---: | ---: |
| baseline | 0 | 29.9963 | 0.99993 | 1.00018 |
| baseline | 24 | 30.0181 | 1.00017 | 1.00018 |
| candidate | 0 | 29.9934 | 0.99979 | 1.00014 |
| candidate | 24 | 30.0211 | 0.99979 | 1.00054 |

Every probe completes exactly 1,323,000 application frames. All four decoded
captures have pitch scale approximately 1.0000, no missing IDs between retained
symbol runs, no backwards/repeated transitions and no fully zero ten-ms bins.
Neither boot logs a native HDA fault. The loaded capture windows are wholly
inside conservative CPU-load bounds translated from live host/guest clock
brackets. That translation assumes the previously verified 1x stable clock;
the separate short capture brackets constrain its measured rate only to
approximately 0.944–1.041, not to audio-sample precision.

Both original launchers and audio routers exit zero, all runtime error counters
remain zero, idle output is silent, and all filesystem/preservation gates pass.
Large guest result files are extracted read-only after shutdown and checked
against their guest-reported sizes and SHA-256 hashes. Evidence and the
verification script are retained in the ignored `audio-write-lead-packet-controls`
directory.

These controls find no gross pacing regression in the candidate and support
retaining the targeted safety fix. The decoding rate is approximate because
symbols are quantized to 100 ms. An eight-second capture does not establish
continuity across every sample of the full thirty-second output or all browser
workloads. In particular, the browser sample-gap failure above remains open.

The next diagnostic should distinguish the audio worker's deadline-to-ready,
ready-to-dispatch and dispatch-to-resume intervals, alongside BSP timer arrival
and host steal. The current timer path checks blocked deadlines on the BSP
only. That is a source observation and a measurement target, not a proven cause
of the captured stall or justification for changing timer ownership without
SMP safety tests.
