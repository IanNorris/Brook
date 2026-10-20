# HDA timer-paced source loss and callback control

QEMU's timer-paced HDA codec discards source audio in a repeatable Brook native
control. The equivalent callback-paced run preserves source progression. The
launcher now defaults to callback pacing with a 20-ms requested host buffer;
RR remains the default scheduler. Explicit audio overrides remain available.

## Matched traced experiment

Fresh private images use the unchanged per-vCPU-clock kernel
`a9627f6fb5fcb7fd1f976f5c03138a03c68396f6c8ede195bef8ad4ec372d7a1`,
13 ABI-34 modules including the HDA fault diagnostics, eight vCPUs, six GiB,
MLFQ and host affinity 0–13. The frozen launcher pins QEMU 10.2.2. Both runs use
SDL's PipeWire driver into the same private 44.1-kHz null graph, a requested
20-ms host buffer and the same 30-second coded OSS signal. No physical output
or host desktop capture is involved.

The private QEMU launch enables timestamped output-codec running, adjustment and
overrun trace events. Live QOM checks verify the requested HDA mode. An
eight-second PCM capture is analyzed with the existing independently pitch-fit
packet-code decoder. Paired host realtime/monotonic anchors bracket each
capture; their offset changes are less than a microsecond in these runs.

| HDA mode | Guest completion of 30-second source | Source seconds / PCM second | Capture overruns | Nominal source discarded in capture | Coded timing residual std, before → after discard accounting |
| --- | ---: | ---: | ---: | ---: | ---: |
| Timer | 26.8661 s | 1.10451 | 19 | 0.88236 s | 239.0 → 10.1 ms |
| Callback | 29.9589 s | 1.00023 | 0 | 0 | 2.78 → 2.78 ms |

Fitted pitch is 1× in both. Timer capture contains 8.0200 PCM seconds across
8.0054 host seconds; callback contains 8.0007 across 8.0030. The decoder observes
four unobserved symbol IDs in the timer case and none in callback, with no
backwards transitions. An unobserved ID alone is not proof of a complete packet
drop. The traced discard path provides independent evidence beyond decoding.

The [QEMU 10.2.2 source](https://github.com/qemu/qemu/blob/v10.2.2/hw/audio/hda-codec.c)
uses an 8,192-byte output codec FIFO. On a full-buffer overrun it discards that
buffer and resets its timer origin. Occupancy corrections also shift the timer
origin. Nineteen full buffers at stereo s16/44.1 kHz correspond to 0.88236 source
seconds. Their removal predicts about 1.1100 source seconds per stored PCM
second over this capture, close to the independently decoded 1.1045. Aligning
decoded symbol centers with the traced discard events reduces the residual
standard deviation from 239 to 10 ms. This strongly identifies this emulated
codec discard path as the source of acceleration in this control, rather than
requiring a fast elapsed guest clock or a pitch change.

The whole timer trace contains 66 overruns and 1,050 occupancy adjustments,
with summed origin changes of −3,291 ms. Overruns reset the origin, so this sum
must not be interpreted as one accumulated final clock error. Callback has no
adjustment or overrun events. Tracing is an observer; the earlier untraced
[packet controls](audio-packet-code-controls.md) reproduce similar rates.

## Analysis and limitations

`scripts/workloads/analyze_hda_trace.py` parses these version-specific events,
separates output from input, and optionally aligns them with decoded symbols.
It rejects unknown HDA payloads, inconsistent clock anchors and captures whose
PCM duration differs from host duration by over 50 ms. This filter does not
prove absence of brief gaps. Timestamp/PCM alignment remains approximate;
100-ms source symbols, capture-edge buffering and inactive output limit it.
Four fixture/rejection tests pass in
`scripts/test_hda_trace_analysis.py`, including a generated source timeline
with known deleted intervals. No rate is forced to match the trace prediction.

Both native probes complete exactly 1,323,000 source frames with no reported
failure. Groups drain, runtime bad-sample counters remain zero, heap checks pass
and idle captures have zero frames. Original launchers and route watchers exit
zero; all four offline filesystems and twelve-document/four-profile preservation
checks pass. This accepts experiment collection/lifecycle, not timer audio
correctness. The original historical roughly 1.3× application recording and
intermittent native stream discontinuity remain separate unresolved cases.

Ignored evidence is in
`build/workloads/timing-scheduler-20261020/coded-hda-traces`, with private reviews
`build/reviews/timing-mix-ct-{t,c}0-20261020`. Each review retains its timestamped
`hda-trace.log`; each cohort retains clock anchors, frozen source/launchers,
small metadata receipts, verified offline guest reports, PCM and decode/trace
analysis. Licensed game data is not part of these synthetic controls.

## Launcher disposition

`BROOK_HDA_USE_TIMER` now defaults to `off`, and `BROOK_AUDIO_BUFFER_US` to
`20000`. The timer path remains selectable explicitly for diagnosis. A small
buffer accompanies callback pacing because the previous SDL/200-ms callback
cohort ran slowly, independently reproduced on Linux; see the
[buffer controls](linux-audio-buffer-control.md). Callback/20-ms controls include
Firefox/generated media, native coded audio with and without CPU load, and
[Quake II GL3/music](q2-music-timing-control.md). These do not certify every host
backend or physical listening configuration. The Linux benchmark launcher still
requires explicit `--audio-buffer-us 20000` for this matched cohort; its earlier
200-ms controls remain historical evidence.

A fresh RR/PipeWire control then launches without `BROOK_HDA_USE_TIMER` or
`BROOK_AUDIO_BUFFER_US` set. Actual QEMU arguments confirm
`out.buffer-length=20000`; live QOM confirms `use-timer=false`. Thirty seconds
of source complete in 29.9858 seconds idle and 30.0283 seconds with a separate
24-worker/12-second CPU load overlapping the capture. Decoded progression is
1.00018× and 1.00054×, with unchanged fitted pitch, no unobserved IDs or backwards
transitions, and no adjustment/overrun trace events. Captures cover about eight
PCM seconds across eight host seconds. Native fault records are absent; both
reports contain exactly 1,323,000 completed frames and no failure. Normal job
drain, runtime/heap, original wrapper/routing exits, offline filesystems and
preservation gates pass. This validates the audio defaults with the unchanged
RR policy; it is not a default-policy browser-performance acceptance.

Evidence is under `build/workloads/timing-scheduler-20261020/default-audio-control`
and `hda-discard-and-default-validation.json`. The three VMs and their private
audio daemon have exited. `BROOK_HDA_USE_TIMER=on BROOK_AUDIO_BUFFER_US=200000`
selects the previous launcher settings for explicit diagnostic comparisons.
