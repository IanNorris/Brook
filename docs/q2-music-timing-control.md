# Quake II music and demo timing control

## Current MLFQ follow-up

A fresh private GL3 1280×720 music-only run uses the tested MLFQ wakeup
preemption module, default BSP wake scanning, the same frozen kernel as the
[matched browser comparisons](mlfq-wakeup-preemption.md), and SDL audio into
the private 44.1-kHz PipeWire null sink. Callback HDA and requested 20-ms output
buffering are checked. No added CPU workers run in this game smoke test.

The nominal 15.1-second segment has a guest-observed marker interval of
15.2415 seconds. That does not reproduce the large game-time acceleration.
Host marker polls lag log production and cannot be treated as precise bounds
on execution of the markers. The guest interval alone is not a host-referenced
clock calibration.

The eight-second capture contains 8.0196 seconds of PCM over 8.0070 host seconds,
with at most one silent 10-ms bin at RMS threshold 10 in s16 units. No sampled
stream discontinuity or native HDA fault occurs. **This does not establish
correct music progression:** whole-capture spectral alignment instead prefers
about 0.825× source tempo (similarity 0.796). The first 0–3 seconds, middle
3–6 seconds and final 5–8 seconds prefer 0.805×, 0.820× and 0.845× respectively,
with similarities 0.828–0.841. The varying fits are not proof of a uniform clock
rate or a pitch change. Repeated/delayed non-silent audio remains possible.

The initial 0.85–1.65 search landed at its lower edge, so the retained follow-up
widens the range to 0.50–1.65. Independently generated 1× and pitch-preserving
1.3× controls recover 1.000×/1.300× with similarities 0.948/0.913. Thus this is
a supported slower-source-progression lead, not a fitted historical speedup or
a reason to claim the music is fixed. Matched controls below do not attribute
it to MLFQ or to BSP-only wake scanning.

The game job exits zero with no live lineage tasks. Runtime/clock checks pass,
all eight pvclock/steal slots are active, policy service exactly matches the
kernel ledger, walked/reported queue counts agree, and heap checks pass.
Original launcher/router completion, four offline filesystems and saved-data
preservation pass. The VM and private services stop. The locally extracted OST
hash matches the user's original; all recordings, game data and analysis
artifacts remain ignored under `q2-mlfq-current`. `music-rate-check.json` retains
subwindow results, control results and input/script SHA-256 identities. The
initial host-Python NumPy import failure and successful isolated Nix analysis
are retained separately. No copyrighted asset is committed or put in a Nix
derivation.

### Matched module and wake-scan controls

Three more fresh private boots retain the same kernel, other twelve modules,
GL3 scene, music source, SDL/private null output and capture procedure. The old
MLFQ module is the exact preserved control used in the browser comparisons.
The shared-scanning run uses the candidate module and records 1,555 AP scans,
valid consistent counters, then restores BSP mode before shutdown.

| Run | MLFQ / wake scanning | Guest demo interval | Whole music fit | Similarity | Longest silent 10 ms bins |
| --- | --- | ---: | ---: | ---: | ---: |
| q2-mlfq-current | candidate / BSP | 15.2415 s | 0.825× | 0.796 | 1 |
| q2-mlfq-control | old / BSP | 15.2122 s | 0.830× | 0.815 | 3 |
| q2-mlfq-repeat | candidate / BSP | 15.2359 s | 0.830× | 0.800 | 3 |
| q2-mlfq-shared | candidate / shared | 15.2687 s | 0.825× | 0.804 | 3 |

This closely repeated slower-progression lead is independent of the tested
MLFQ change and survives AP wake-scan handoff. It does not establish which
shared component causes it. Every recording has approximately eight seconds
of PCM over eight host seconds; that output sample count alone cannot establish
source pacing. Sampled stream counters likewise progress below the nominal
44.1-kHz output rate, but sparse observations do not distinguish producer delay,
silence insertion, resampling or other causes.

All four runs pass job-lineage drain, original launcher/router completion,
clock/runtime checks, heap, four filesystems and preservation. Staged module
hashes verify the intended variants; candidate service/queue snapshots agree
with the kernel ledger and actual queue walk. No native HDA fault or sampled
stream discontinuity is retained. Ignored `q2-mlfq-music-comparison.json` records
the comparison, with raw evidence in the four named directories. To avoid
depending solely on commercial-music alignment, the next diagnostic sends the
existing synthetic packet-code-v1 signal through Quake's actual OGG music path
in fresh private clones. Original OST files and golden disks remain untouched.

## Earlier controls

Two fresh private Brook runs exercise GL3 at 1280×720 with a known local music
track. Both complete the bounded demo and shut down cleanly. Neither establishes
uninterrupted music acceptance: the first mixes sound effects into the capture;
the music-only repeat exposes a native audio discontinuity.

## Configuration and measurement

Both use kernel `a9627f6fb5fcb7fd1f976f5c03138a03c68396f6c8ede195bef8ad4ec372d7a1`,
all 13 matching ABI-34 modules, MLFQ, eight CPUs, six GiB and host affinity 0–13.
QEMU's HDA callback mode is checked live; SDL uses the private 44.1-kHz PipeWire
null sink with a requested 20-ms buffer. These are experimental settings, not
changes to the default scheduler, timer mode or host buffer.

`scripts/workloads/q2_demo_timing.py --music-track 2` explicitly restarts the
owned OGG track at the START marker and logs `ogg info`. `--music-only` also
sets effects gain to zero while retaining the separate music gain. Both logs
confirm OpenAL Soft 1.24.3, hardware virgl on Intel UHD 630, and track02 at zero
samples when START is emitted. The installed track's SHA256 matches the user's
local original. No game data or music belongs in git or a Nix derivation.

The shared probe retains its nominal 15.1-second demo segment. An eight-second
host WAV capture begins after observing START; screenshots are excluded from
the interval. Host marker observations include RPC, output delivery, log
visibility and polling delays. Their bounds are broad, not a tight calibration
of the instant the game executes either marker.

| Run | Guest-observed demo seconds | Host marker observation bounds | Active PCM / capture host seconds |
| --- | ---: | ---: | ---: |
| Music and sound effects | 15.1812 | 14.2688–15.6839 | 8.0007 / 8.0036 |
| Music only | 15.3347 | 13.0787–15.4346 | 2.0608 / 8.0062 |

Both host intervals contain 15.1 seconds. This supports normal demo pacing in
these trials, rather than proving exact guest clock rate or an uncapped graphics
throughput improvement.

## Music result and retained failure

The mixed capture's spectral match is weak (best similarity 0.299), at an
implausible reference offset around 202 seconds. Its fitted tempo is unaccepted.
The music-only capture matches the beginning of track02 near reference second
4.83, with best tempo 0.995 and similarity 0.761; the 1× candidate is almost as
strong (0.754). Known 1× and 1.3× FFmpeg controls recover their input rates.
The 0.005 search grid is not an error bound. This supports near-unit tempo for
the surviving PCM, not source progression through missing intervals.

The music-only capture is only 2.06 seconds of active samples across eight host
seconds. QEMU WAV capture omits inactive intervals; this ratio cannot be read as
the music's playback speed. Before START, cached audio telemetry already shows
stream identity 17/epoch 1 faulted: submitted 7,168 frames, completed 3,520,
software queue 3,072, no in-flight frames and `discontinuity=1`. Several later
observations retain those exact counters. After START the console records an
OpenAL shutdown/reinitialization, and telemetry eventually shows identity
33/epoch 4 progressing without the fault. The gap is retained as failed audio
continuity, not removed from the result or attributed solely to muted effects.

The native DMA clock intentionally stops on an ambiguous position sample,
device error or invalid elapsed-clock observation. Its half-ring sampling
window at 44.1 kHz is approximately 186 ms. Cached stream telemetry
does not distinguish these causes. The driver diagnostic below records these
causes without changing sampling limits. A native reproduction must identify
the reason and interval before blaming the scheduler. Accepting ambiguous ring wraps could manufacture
completion and lose application audio.

Both jobs exit zero with no remaining group tasks. Runtime counters report no
invalid/backwards/clamped/stale/saturated samples and all eight steal-clock
slots are active. Heap checks pass. Subsequent idle captures contain zero
frames. Original launchers and routing watchers exit zero; all four offline
filesystems and twelve-document/four-profile preservation checks pass. These
are structural/lifecycle results, not successful audio-continuity acceptance.

Ignored evidence is under
`build/workloads/timing-scheduler-20261020/q2-small-music{,-isolated}` and
`q2-small-music-comparison.json`, with private reviews
`build/reviews/timing-mix-q2-smusic{0,1}-20261020`. The original historical
approximately 1.3× acceleration remains a separate, unresolved application case.

## Fault diagnostics and bounded contention repeat

The HDA driver now retains the first unexpected fault record in each hardware
epoch and prints it after stopping DMA. The record includes a reason, previous
and sampled elapsed timestamps, previous/raw DMA positions, stream status and
submitted/proven-completed byte counts. Observation failures distinguish device
status, invalid position, unavailable/backwards clock, excessive sampling gap
and completion overflow. Submission/setup/start failures identify their site;
the publication safety check currently reports `publish-unsafe` rather than its
individual predicates. Explicit global stop is not reported as an unexpected
fault. Repeated polls preserve the first record; restart permits a new epoch's
record. There is no new module ABI or relaxation of completion rules.

Production-driver hosted regressions verify sample-gap evidence, device status,
unavailable/backwards clocks and first-record retention. All 62 hosted suites
pass, followed by a focused rerun after the serial format correction. Module
symbol checks and both kernel links pass. The kernel hash remains unchanged;
the diagnostic HDA module is
`20854ba073c3afd6441b0286b7c35a50b84ecc563e17604505f942e982242b91`.
Build evidence is in `build/q2-audio-fault-diagnostic-final.log`; the full
suite log is `build/q2-audio-fault-diagnostic-build.log`.

Two additional fresh private music-only controls use that module and the same
configuration. Neither reproduces the terminal fault. An intervening attempted
load run is rejected as a combined measurement, as described below.

| Diagnostic run | Guest-observed demo seconds | Active PCM / capture host seconds | Music match |
| --- | ---: | ---: | --- |
| No added CPU load | 15.3097 | 7.9993 / 8.0036 | First six seconds best 0.995×, similarity 0.761; whole capture best 0.980×, similarity 0.616 |
| 24 CPU workers, 30 s | 16.2344 | 8.0196 / 8.0033 | Whole capture best 0.995×, similarity 0.904; 1× similarity 0.899 |

The loaded run's worker interval is guest second 23.0097–53.0097; both demo
markers (33.3194 and 49.5537) lie inside it. Its first-six-second music match is
0.995× with similarity 0.939, almost identical to the 1× candidate. Known 1× and
1.3× matching controls pass in both runs. This is near-unit source tempo evidence,
not evidence for a precisely measured 0.5% slowdown. The weaker whole-capture
fit in the unloaded run remains visible rather than selecting only its stronger
window. Neither short capture establishes an absence of brief glitches.

Both job groups drain; sampled stream discontinuity and serial fault records
are absent. Runtime bad-sample counters remain zero, heap checks pass, idle
captures have zero frames, original wrappers/routing observers exit zero and
all four filesystems and preservation checks pass. All VMs and the private null
audio service are stopped. The default settings remain unchanged.

The rejected attempt requested 45 seconds from a load helper whose maximum is
30; it exited 64 without live tasks. Recovery retained the same Quake job, which
had already completed before capture, so both markers arrived in the same poll
and the capture contained zero frames. The separately completed corrected CPU
job cannot turn it into an overlapping workload result. Original requests,
recovery and rejection are retained under `q2-fault1`.

Diagnostic evidence is under
`build/workloads/timing-scheduler-20261020/q2-fault{0,1,2}` and
`q2-fault-diagnostic-results.json`, with matching private reviews. The change
improves future fault evidence; it does not claim the intermittent discontinuity
is fixed. Its precise native cause remains the next reproduction gate, alongside
the separately demonstrated timer-mode source acceleration.
