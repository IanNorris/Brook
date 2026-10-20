# Quake II music and demo timing control

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
window at 44.1 kHz is approximately 186 ms. Current cached stream telemetry
does not distinguish these causes. The next diagnostic must identify the fault
reason and the interval surrounding it before changing sampling limits or
blaming the scheduler. Simply accepting ambiguous ring wraps could manufacture
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
