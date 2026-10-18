# Timerfd clock-domain investigation — 2026-10-18

The permission-kernel review found Wayland dispatching about 60 times per
second while its frame timer ran about once per second. The optional
`waylandd --trace-frames` diagnostics distinguish dispatch, timer delivery,
frame requests, commits, completed callbacks and destroyed callbacks. They
are not a lossless surface census or a measurement of displayed video FPS.

On kernel `50fe3e571183c246225af8cbe724133394f8bdd3ce71a5afd7dbcdf997be368e`,
an eight-sample relative CLOCK_MONOTONIC/epoll probe requested 16 ms and
reported median 16.009 ms, maximum 17.978 ms. The corresponding absolute
probe timed out. Idle Wayland frame-timer gaps reached about 1.54 seconds.
The preserved evidence is under
`build/workloads/wayland-frame-trace-20261018/review`.

`timerfd_settime` stored absolute clock timestamps as LAPIC millisecond ticks,
while `clock_gettime` used the KVM nanosecond clock. The two clocks can drift;
absolute REALTIME also has an entirely different epoch. Poll additionally
slept for its safety interval without scheduling the next timer deadline.

The candidate keeps each timer's deadline and interval in selected-clock
nanoseconds. A shared, mutex-protected refresh operation accrues expirations
without consuming them. Read consumes only after a successful safe copy;
poll, epoll and gettime preserve pending counts. Periodic timers retain their
phase and submillisecond interval, and arithmetic saturates rather than wraps.
Only a remaining duration is translated into a monotonic scheduling deadline
and then a LAPIC wakeup tick. The final wakeup tick is assigned after all
potentially sleeping locks. A generation captured with each timer deadline is
checked again using atomic loads after the last lock, so rearming an earlier
timer while acquiring a later timer cannot lose its wake notification.

Each blocking read/poll/epoll owns a retained registration on its timer.
Rearming or disarming wakes all registered waiters. Registrations are removed
before releasing the retained timer; fd reuse does not redirect them. A
registration-allocation failure falls back to a bounded 5 ms readiness scan.
A disarmed blocking read waits for an arm or signal. Set/get also pin their
open description, validate flags and use fault-aware user copies.

Validation tools:

- `scripts/workloads/timerfd_latency.py`: relative/absolute MONOTONIC through
  epoll, with per-mode results emitted immediately; timings include user
  scheduling overhead and have no fixed performance pass threshold.
- `scripts/workloads/timerfd_regression.py`: both clocks and both modes through
  poll, select, epoll and nested epoll; past deadlines; periodic accounting;
  old/gettime, disarm, aliasing, invalid flags/descriptors/pointers, protected
  cross-page buffers, dup/fork sharing and rearming two blocked readers.
  `--expect-fault-retains-count` checks Brook's stronger all-or-error read copy.
  Linux can return a partial read or consume its count on a copy fault.
- `scripts/workloads/timerfd_rearm.py`: earlier rearm through each multiplexing
  API and submillisecond periodic delivery. Its loose bound catches the old
  two-second deadline; it does not deterministically inject mutex contention.
- The existing native clock suite exercises the actual timer accounting
  implementation, including repeated observation, precision and overflow.

The timerfd API reference is the [Linux manual](https://man7.org/linux/man-pages/man2/timerfd_create.2.html).

Remaining limits: timerfd edge-triggered epoll delivery, epoll's numeric-fd
watch identity, wall-clock adjustment/cancellation and close-versus-wait
cancellation are not established by this work. Brook has no supported wall
clock setting API; future clock changes require relative REALTIME timers to
retain duration semantics. Unsupported CANCEL_ON_SET is rejected. Extremely
large durations outside unsigned nanoseconds are rejected rather than offering
Linux's full range behavior. The Debug compiled-entry audit rejects an
unmodeled TLB leaf stack allocation; the workload Release configuration passes
its supported-template audit. Neither audit proves arbitrary handler bodies.

## Accepted workload candidate

The final kernel is
`483dcaa0e4fc907219018af8c8d4e347be32ee38b375bb3c56a40aa29930709c`.
Evidence is under `build/workloads/timerfd-clock-20261018`, with the independent
`acceptance-audit.json` covering both candidate boots. Both used eight CPUs,
4 GiB, private host-backed disks and the same diagnostic Wayland package.

The final Release build passes its supported compiled-entry audit and all
40 native suites. The guest passes 105 ABI checks as UID 1000, earlier-rearm
checks through all four multiplexing paths, and 64 relative plus 64 absolute
16 ms samples. Median/p95/max observed milliseconds:

| Mode | Median | p95 | Maximum |
| --- | ---: | ---: | ---: |
| Relative | 17.004 | 17.061 | 18.003 |
| Absolute | 17.003 | 17.021 | 17.460 |

Idle Wayland timer delivery returned to approximately 60 Hz, with a maximum
24 ms gap during the initial diagnostic run, versus roughly once per second
before the repair. Browser measurements use the local 640×360, 24-fps VP9 /
48-kHz Opus clip. Firefox was alone; Chromium played with Firefox paused and
Codium open. Rates use each page's performance clock:

| Browser | Media seconds / page second | Video callbacks / page second | Non-dropped frames / page second |
| --- | ---: | ---: | ---: |
| Firefox | 0.995 | 22.87 | 23.84 |
| Chromium | 1.021 | 24.06 | 24.28 |

These are separate counters, and none independently measures physical display
refreshes. Previous permission-kernel callbacks were about 1.20 and 1.01 per
page second. The final ten-second PCM captures contained 441,327 and 441,019
stereo frames at 44.1 kHz, nonzero signal and no silent 10 ms bins. Pausing
Firefox closed its stream; Chromium retained an all-zero stream. This short
synthetic run does not establish glitch-free real-site audio.

Concurrent playback still slows down. In a 9.801-second overlapping guest
realtime interval, Firefox advanced 0.377 media seconds per page second and
reported 9.85 video callbacks; Chromium advanced 0.829 and reported 21.63.
The windows overlap and focus/visibility differ, so this is neither a matched
throughput benchmark nor proof of a scheduler-only bottleneck. The short
runtime profile has 392 CPU samples, 9,729 context switches and zero drops.

Both candidate boots drained every recorded application launch lineage and
tracked command. Audio was inactive after exit, heap checks passed, guest sync
completed, QEMU and launchers exited zero, the serial kernel/device/raw user
fault audits passed, and all three read-only filesystem checks passed. Five
existing documents retained their exact hashes and Brook ownership. The first
probe's fixed 35-count sleep assertion is retained as a failed diagnostic;
the corrected check bounds counts against actual selected-clock elapsed time
and observed the expected 34 counts. No production fix was made to satisfy
that mistaken assertion.

In a separate native-window/PipeWire review, the user reported almost perfect
YouTube video playback and a substantial improvement in page responsiveness.
Occasional audible stutters remain. This is subjective real-site feedback,
separate from the controlled counters above. The user also reported CSD shadows
repeatedly darkening then resetting. The CPU compositor blends partial window
updates onto its previous scene and restores wallpaper every 500 ms. The
subsequent CPU region-recomposition repair passed guest shadow-stability and
launcher pointer tests; see [signal and desktop follow-up](signal-desktop-followup.md).

### Live concurrent profile

The same native review produced a 500 ms capture with 399 CPU samples and
12,637 context switches, no producer drops and matched exported event counts.
327 samples were idle; the separate untruncated census found 382 Firefox
lineage tasks and 112 Chromium lineage tasks. These are tasks, including
threads, rather than 494 independent browser processes. Among outgoing switch
stacks, 1,164 passed through the epoll readiness graph mutex and 389 through
its waiter/deadline path. These counts suggest lock contention as a useful
follow-up; they do not quantify blocked wall time or establish the main cause.
IRQ-masked execution is not sampled uniformly. Resolving the exact loaded
module maps eighteen driver samples to HDA MMIO read paths and one to the
playback lock. These sample counts do not establish their elapsed-time cost.

A separate passive real-site PCM capture contained seven adjacent silent
10 ms bins. The source itself can contain silence, so this is not by itself
proof of an underrun. No screenshots or profiling ran during that audio
interval. The native review later exhausted physical memory after STK was
started alongside both browsers and panicked while writing a signal frame.
Its disks remain forensic; subsequent acceptance used fresh private copies of
the last checked image. The [follow-up](signal-desktop-followup.md) records the
contained signal-copy failures and desktop checks.
