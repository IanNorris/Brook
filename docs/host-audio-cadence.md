# Host audio cadence control

The private review `unattended-audio-host-cadence-1ms-fixed-mlfq-a-20261021`
uses the frozen pipe-fixed guest, original modules/12-KiB HDA lead, optional
6174-frame logical ALSA buffer and guarded 4096-frame horizon, MLFQ, eight
vCPUs/8 GiB and the private 1-ms GPU-fence QEMU. The host audio configuration is
PipeWire, `out.buffer-length=20000,timer-period=5000`, callback HDA (`use-timer=off`).
No guest code or scheduler default changes.

## Different host buffering controls

In the pinned [QEMU PipeWire source](https://raw.githubusercontent.com/qemu/qemu/v10.2.2/audio/pwaudio.c),
output buffer length sizes the QEMU hardware voice, while `out.latency` controls
the PipeWire ring watermark (default 46440 microseconds). The backend also
derives a requested node latency from three quarters of the audio timer period.
At 44100 Hz the 5-ms and 1-ms periods request 165 and 33 frames respectively;
requests do not establish the actual server callback quantum. Thus changing the
period is a coupled host-timer/requested-latency experiment.

The [HDA callback path](https://raw.githubusercontent.com/qemu/qemu/v10.2.2/hw/audio/hda-codec.c)
fetches guest data before passing it into the QEMU voice. Backend writes
therefore do not timestamp guest DMA transfers. The
[audio timer](https://raw.githubusercontent.com/qemu/qemu/v10.2.2/audio/audio.c)
reports delayed intervals above 1.5 times its period, in QEMU virtual time,
rounded down to integer milliseconds. This is separate from host wall time.

`scripts/run-qemu.sh` now accepts `BROOK_AUDIO_TIMER_US=1000..1000000`, retaining
the normal 5000-microsecond default. This does not change codec `use-timer` or
requested output buffer length. Malformed/out-of-range values fail before QEMU
launch. Tests execute the actual option block, checking the unchanged default,
decimal boundary handling, malformed input and the no-audio backend.

## Bounded baseline trace

All eight ordinary source/frame/audio/CPU windows complete with heavy probes
disabled. After the full clip and browser loop, a separate diagnostic enables
`pw_write` and `audio_timer_delayed` alongside the existing host fence traces,
worker timing, HDA delivery and contribution counters. The common enabled
window uses the existing explicit RPC and host clock bounds. Events outside it
remain explicit transition exclusions. Extra logging can perturb this window.

The baseline contains 1735 PipeWire writes over a 7.036429-second first-to-last
write span. Positive-write gaps have median 5.029 ms, p95 5.149 ms and maximum
7.423 ms. No delayed-audio-timer event is recorded. The single-voice write
indices reconcile exactly, including partial-write clamping and ring occupancy
conservation. The watermark is 8192 bytes; observed pre-write fill is
6556–7996 bytes (about 37–45 ms nominal stereo S16 audio). The backend ring
therefore does not appear starved in this capture. No producer-drop counter is
available, so continuity does not certify an unbiased complete timer trace.

One 768-byte HDA reservation still occurs, matching a 192-frame/4.354-ms
all-channel zero run. Both sources contribute all 281920 frames of accepted
output with no missing tails or empty contributions. There are 622 recorded
worker cycles and no retained slow episode. The horizon interval has 616
decisions, 34 deferrals and 19 headroom bypasses. These counters enclose the
delivery recording but are not simultaneous or per-reservation source context.
The reservation pair again exhibits compressed observed position advance: at
least 4864 bytes (27.574 ms nominal audio) in at most 21.766 ms between position
reads. See the [reviewed derivation](gpu-fence-timing-control.md). Neither regular
backend writes nor this position bound prove sustained fast physical playback.

Ordinary WAV captures retain gaps too: Firefox alone has one 11.610-ms zero
run, mixed enabled A and C each have two runs up to 4.354 ms, and the
Quake-focused loaded window has one 1.451-ms run. Preserve this variation; the
separate diagnostic does not replace ordinary output acceptance. Browser
transfers/loop, nine original jobs, runtime quality, native fault/discontinuity,
heap/policy, original controller/owner exits and all four filesystems pass.
Full uninterrupted audio, earlier runtime failures, historical fast playback
and the wider clock/SMP/fairness audit remain unresolved.

## Private 1-ms candidate — not accepted

A fresh checked clone runs the identical controller/helper and frozen guest with
only QEMU `timer-period=1000` replacing `5000`. Actual QEMU arguments are checked
before starting its workloads. Requested buffer length stays 20 ms; the backend
watermark remains at its unmodified default. Compare PCM gaps, source/frame
progress and QEMU process CPU costs before interpreting a finer trace as an
improvement. This is one ordered control, not a default-policy recommendation.

The analyzer `scripts/workloads/analyze_qemu_audio_cadence.py` validates trace
hashes and enabled-state metadata before using the common window. It checks
write-index continuity, request/available clamping, fixed watermark and queue
conservation. Removed bytes left the backend ring, not necessarily the speakers.
Seven focused tests cover clamping, wrap, omitted/restarted writes, invalid fill,
malformed events and transition exclusion. Private raw artifacts are retained
under each review's `mixed-prefix-control/post-timing-gpu-host-trace/`.

The candidate completed the same lifecycle/media/native-fault/filesystem gates,
but **failed runtime acceptance with five clamps**. The first occurs at guest
time 20.318 s, before the first ordinary capture; maximum deficit is 17.875
microseconds. Two debt episodes catch up, known debt ends at zero, and three
owner transitions occur while debt exists. Cleanup-only history starts too late
to capture either episode. Zero final debt does not establish correct per-task
attribution. Earlier 5-ms cohorts also had clamps; this pair does not attribute
the accounting issue to the timer change. The failure is preserved.

The candidate logs 5309 PipeWire writes, a 1.047-ms median / 2.115-ms p95 /
3.791-ms maximum positive-write gap, and the same 8192-byte watermark. Pre-write
fill remains positive. It records 115 delayed-timer events, maximum reported
interval 3 ms. The delayed threshold also changed from 7.5 to 1.5 ms, so the
raw event counts are not comparable measures of worse latency. Index and queue
conservation checks pass in both traces.

| Ordinary window | Firefox callbacks/s, 5 → 1 ms | Quake swaps/s, 5 → 1 ms | QEMU CPU cores, 5 → 1 ms |
| --- | ---: | ---: | ---: |
| Firefox alone | 21.83 → 23.03 | — | 3.02 → 2.47 |
| Both, horizon on A | 18.56 → 15.65 | 15.58 → 16.33 | 2.64 → 3.86 |
| Both, horizon on C | 18.72 → 20.16 | 15.59 → 16.16 | 2.30 → 2.32 |
| Eight workers, Quake focused | 19.32 → 19.83 | 17.40 → 17.08 | 8.13 → 8.17 |
| Eight workers, Firefox focused | 18.24 → 19.88 | 16.95 → 16.72 | 8.12 → 8.14 |

CPU costs cover the QEMU process including KVM guest work, excluding separate
PipeWire, desktop and renderer/helper processes. These are one ordered pair
and variable windows, not a general performance improvement. Callbacks/swaps
are not physical scanout. First mixed Firefox audio completion is 0.9882 versus
the baseline's 0.9987; subsequent enabled/loaded windows are near nominal.

Some ordinary zero runs shrink or disappear: Firefox-alone maximum 11.610 →
1.451 ms, mixed A 4.354 → 2.902 ms, mixed C and Quake-focused load have no
runs of at least 1 ms in the candidate. The separate candidate diagnostic
nevertheless has three reservations of 256/512/256 bytes (1024 total), matching
three zero runs of 64/128/64 frames, versus the baseline's single 768-byte
reservation. All accepted batches again have complete contributions from both
sources, with 637 worker cycles and no slow episode. Finer backend cadence
does not eliminate the remaining physical-ring reservations or prove
uninterrupted/per-source audio. Keep the normal timer at 5 ms.

Both runs retain identical kernel/module/fixture/QEMU hashes, policy scripts,
executed controller and diagnostic helper sources. Normalized QEMU arguments
match after excluding only the verified timer field, review/session paths and
private ephemeral OVMF/ESP filenames. Every other difference is rejected. The
complete eight-window comparison and failed runtime gate are retained in
`build/workloads/timing-scheduler-20261021/logical-buffer-probe/host-audio-timer-comparison.json`.
Both original controllers and owners are terminal with exit zero and four
filesystem checks each pass; no VM remains.

Next work should preserve first-clamp/owner-boundary history from earlier in a
separate accounting diagnostic, with the established host settings fixed. The
short audio reservations and unsubmitted-attempt context remain open; neither
clock nor scheduler acceptance is inferred from this host-only experiment.
