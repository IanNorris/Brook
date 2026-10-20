# Timing and scheduler audit — 20 October 2026

This is an active investigation. The baseline is main `7d005ae0`, kernel SHA-256
`f932600c7ef0ac9606057f8e976e47b3f397695aae69383dc965a6d342e2f1b3`.
The native review reported roughly 1.5× playback in Quake II and Firefox.
No repair has yet been shown to resolve the playback speedup, and no scheduler
default change has been made.

## Clocksource versus interrupt delivery

Brook enables KVM's `MSR_KVM_SYSTEM_TIME_NEW` (`0x4b564d01`) on the BSP and
registers one pvclock page. `KvmClockReadNs` converts TSC through that page;
`KvmClockReadStableNs` additionally checks the advertised stable-clock feature,
per-snapshot stable flag, arithmetic and ordered TSC sampling within the version
check. The vDSO uses the latter algorithm for MONOTONIC only. Other IDs fall
back to syscalls. Shared BSP-page reads on other CPUs require explicit stable
TSC/migration coverage; per-vCPU registration is not currently implemented.

The KVM clocksource is not an interrupt timer. Per-CPU periodic LAPIC timers
still generate vector 32. Brook calibrates their reload count against PIT
channel 2 and later changes that count using raw delivered BSP interrupts
versus coarse CMOS seconds. No TSC-deadline timer programming is implemented.
See the upstream [KVM clock ABI](https://docs.kernel.org/virt/kvm/x86/msr.html)
and [x86 timekeeping explanation](https://docs.kernel.org/virt/kvm/x86/timekeeping.html).

The reviewed boot logged `KvmClock: enabled`, stable flag `0x1`, and LAPIC
calibration 62,591 ticks/ms. Subsequent reload adjustments ranged from 55,705
to 62,591 in the inspected log; adjustment samples included 745 and 1,129
interrupts/second. Those observations are not proof of a 1.5× clocksource error.
A short host-bracketed audio-clock sample advanced 9.037 guest seconds in 9.033
host seconds. It does not exclude intermittent errors or another timing path.

## Source map

Paths below are relative to the repository. The selected source is the KVM path
unless explicitly marked as fallback.

| Consumer | Source / delivery | Code | Remaining concern |
| --- | --- | --- | --- |
| Syscall MONOTONIC, RAW, COARSE, BOOTTIME | `KvmClockReadNs`; adjustable LAPIC-ms fallback | `src/kernel/src/syscall.cpp`, `ClockNowNs` | IDs currently alias; validate unsupported IDs and RAW semantics separately |
| libc MONOTONIC fast path | read-only pvclock/TSC vDSO | `src/kernel/vdso/clock.c` | Compare forced syscall, libc, migration and version-update races |
| REALTIME, gettimeofday, RTC display | immutable CMOS epoch plus pvclock elapsed | `src/kernel/src/rtc.cpp`, `RtcNowNs` | CMOS precision is one second; physical/unstable-clock fallback unaccepted |
| timerfd expiry | selected nanosecond clock, lazy readiness refresh | `syscall.cpp`, `timerfd_state.h` | Delivery depends on readiness/wakeup paths; existing timerfd tests are not scheduler latency tests |
| POSIX timer expiry | pvclock; REALTIME recomposes `RtcNow()*1e9 + pvclock%1e9` | `src/kernel/src/posix_timer.cpp`, `Now` | Epoch/fraction mismatch repaired in this branch; host boundary regression passes, guest pending |
| Relative nanosleep | monotonic deadline rechecked after each wake | `syscall.cpp`, `sys_nanosleep` | Wake requests still expressed in adjustable LAPIC ticks |
| Absolute clock_nanosleep | selected-clock deadline, LAPIC wake hints | `syscall.cpp`, `sys_clock_nanosleep` | Recheck and request-overflow repair passes short guest regression; signal/load coverage remains |
| futex, poll/select/epoll waits | nanosecond deadlines plus LAPIC wake hints in several paths | `syscall.cpp`, deadline conversion and waiter loops | Audit each retry, clock selection, signal and overflow path individually |
| alarm and scheduler sleep | adjustable `g_lapicTickCount` | `scheduler.cpp`, `syscall.cpp` | CMOS rebases can move this counter in either direction |
| Scheduler slice expiry / policy aging | BSP apparent LAPIC-ms counter | `scheduler.cpp`, `SchedulerTimerTick` | Unsigned elapsed subtraction after a backward rebase; AP delivery differs from BSP time |
| CPU accounting | one user/sys/idle charge per delivered local IRQ | `scheduler.cpp`, `SchedulerTimerTick` | Counts interrupts, not actual elapsed CPU time; stolen time and masked IRQs bias results |
| Ready-queue latency diagnostic | stable KVM clock, incarnation-guarded episodes | `scheduler.cpp`, `ReadyLatencyNow` | Optional; does not measure time waiting for device completion |
| HDA completion | LPIB modulo ring position; stable pvclock bounds ambiguity | `intel_hda_mod.cpp`, `audio_dma_clock.h` | Check native 44.1-kHz progress against host audio capture, underruns and position discontinuities |
| Mixer stream completion | driver receipts attributed by identity/epoch | `audio.cpp`, `Reconcile` | Acceptance/completion are separate; stateless resampling remains a separate concern |
| QEMU host output | selected PipeWire/SDL/none backend | `scripts/run-qemu.sh` | Compare native review backend with silent controlled backend without changing game settings |

## Active scheduler and safety audit

The reviewed boot loads `rr` as the built-in policy: one global FIFO, 10 ms
slices, serialized through `g_readyLock`. `mlfq` registers as a module but is
not selected. It has four FIFOs with 5/10/20/40 ms slices, blocking/yield boosts,
demotion on exhaustion and a periodic boost to HIGH. The standalone
`sched_policy.cpp` implementation is not the selected `SchedOps` policy.

Mechanism ownership must be checked separately from policy queue tests:
`runningOnCpu` claims, pending requeue after leaving a kernel stack, retirement
references, blocked wake races, process stop/exit and the ready/all-process lock
order all live in `scheduler.cpp`. Kernel execution is deliberately not generally
timeslice-preemptible. A latency policy cannot safely change that assumption.

Confirmed module defect: MLFQ `InitProcess` reset a still-queued PID's priority
and links without unlinking its old membership. The RR implementation already
unlinked first. The new common regression replaces head, middle and tail PIDs
at all four new priorities, verifies both neighbours and counts, then requeues
and drains the replacement once. It fails MLFQ before the repair; afterwards
all 51 module checks pass, including the existing 100,000-operation eight-CPU
interleaving model. Those are host policy checks, not concurrent execution of
the kernel context-switch mechanism. No default-policy change follows.

The POSIX realtime timer selector now calls `RtcNowNs()` directly. A new host
regression includes the actual `posix_timer.cpp` and exercises its clock selector
and expiry code with independent monotonic/realtime fractional epochs, a
pvclock second rollover, a realtime rollover and non-KVM fallback. Before the
fix it reports five failures, including an absolute realtime timer expiring
before its deadline; after the fix it reports zero. Relative POSIX realtime
timers select a monotonic basis, so this defect chiefly affects absolute
realtime timers. This is not yet a reproduction of the playback-speed issue.

Other audit leads, not established safety failures:

- Runtime switching clears shared policy storage, then re-registers tasks under
  ready/all-process locks. Audit running, Ready-but-on-CPU, terminated and idle
  identification, held references and unlocked `Timeslice` reads before using
  hot switching as a benchmark technique.
- `Timeslice` is read outside the ready lock although the policy ABI requires
  serialized calls. Establish a safe snapshot together with the slice baseline.
- MLFQ priority zero is demoted on exhaustion in the module despite older docs
  claiming realtime is never demoted. Decide the contract before changing it.
- Yield/block promotion has no accumulated execution budget and can reward
  repeated short yields. Equal priority dispatch-count tests do not establish
  proportional CPU-time fairness, process-group fairness or a starvation bound.
- Tick-based CPU accounting is insufficient evidence for EEVDF virtual runtime.

## Verification and next gates

Ignored evidence is in `build/workloads/timing-scheduler-20261020/host/`:
`mlfq-before.log` (50 pass, one fail), `mlfq-after.log` (51 pass),
`posix-clock-before.log` (five failures), `posix-clock-after.log` (zero), and
`timing-paths-host.json`. `scripts/workloads/timing_paths.py` brackets libc
MONOTONIC with forced syscalls and samples relative and absolute MONOTONIC /
REALTIME sleeps for premature successful returns. Its host smoke passes. The initial live guest attempt found the session had
exited; subsequent tests use private clones, preserving the review originals.

The original baseline kernel's short idle-guest probe found 32/32 libc reads
inside forced-syscall brackets and no early relative sleeps. Four of twelve
MONOTONIC absolute sleeps and five of twelve REALTIME absolute sleeps returned
before the requested duration (largest shortfall 90,567 ns). The candidate now
rechecks the selected clock after every absolute wakeup and rejects arithmetic
overflow while constructing the deadline. It passes all 36 sleep observations
and 32 clock brackets, with no early returns. The candidate probe additionally
records the selected-clock finish against each actual absolute target. These
short idle samples do not reproduce the reported 50% playback speedup.

Candidate kernel SHA-256 is
`1da82d41b44f5ed19039d1059981259c988bce8839ab64a798c737225b3873b6`.
All 58 hosted suites and nine bootstrap-observer checks pass. The existing
compiled-entry supported-template audit and scheduler-start call-contract audit
also pass; neither establishes arbitrary handler-body or SMP scheduler safety.
Guest evidence is retained under
`build/workloads/timing-scheduler-20261020/{baseline,candidate}-observation/`.
The baseline acknowledged sync, exited normally and passed all four primary
offline filesystem checks. The candidate also acknowledged sync, its owned launcher exited zero and all
four primary filesystems passed offline checks. Full saved-profile fixture
verification and browser/game regression remain pending. No mixed-workload or browser gain is claimed.

1. Freeze a baseline and gather host/guest clock brackets, actual native audio
   completion/capture rates and browser media-time progression, without taking
   screenshots/profiles inside measurement intervals. Reproduce the speedup.
2. Correct confirmed clock-domain and early-wakeup defects with independent
   boundary/wakeup tests; compare the same workload and verify clean shutdown,
   filesystem checks and saved-user-data preservation.
3. Test policy queue and SMP mechanism invariants, including lifecycle churn,
   signals, wake races and controlled migration; repair policy switching before
   relying on it. Prefer boot-selected policy comparisons on private images.
4. Establish an eight-vCPU mixed workload: fixed CPU workers, periodic sleepers,
   input-like wakeups and local browser/audio playback. Report per-task service,
   wake/ready latency p50/p90/p95/max, underruns, throughput and scheduler cost.
   Separate warmup, startup and steady state; match visibility and host load.
5. Compare RR and repaired MLFQ. Consider EEVDF with explicit weights, virtual
   runtime, eligibility/deadlines, sleeper placement, lag, preemption, migration
   and overflow/lifetime contracts. Select implementation based on measured
   bottlenecks and safety gates, not only browser smoothness. Keep the default
   unchanged until evidence supports a change.


## First bounded RR saturation results

The `scheduler_mix.c` workload uses fixed CPU loops and four independent
16-ms absolute-deadline sleeper threads. CPU work is counted in identical
32,768-step batches. It skips missed periods rather than generating a catch-up
busy loop. Inputs are bounded to 64 CPU workers and 30 seconds; the initial
runs use eight or 24 CPU workers and ten timed seconds each. QEMU is confined
to host CPUs 0–13 on the 16-logical-CPU host. These runs contain no browser or
audio stream and do not establish mixed real-workload acceptance.

| RR CPU workers | Total batches | Largest/smallest worker count | Sleeper p95 lateness | Worst lateness | Missed periods per sleeper |
| --- | ---: | ---: | --- | ---: | --- |
| 8 | 1,921,931 | 1.0055 | 7.55–7.59 ms | 97.54 ms | 8 |
| 24 | 1,962,419 | 1.0175 | 94.26–95.59 ms | 165.51 ms | 492 |

Each worker made progress; no sleeper returned early. The service-count result
shows even distribution of this fixed CPU work, while periodic latency worsens
with runnable count. It is not per-app fairness or proof that RR alone causes
the latency. Ready-queue accounting spans setup/teardown as well as the timed
interval and must not be labelled a steady-state-only distribution. Coarse
host/RPC clock brackets under load are too wide to infer a small drift change.
The owned launcher exited zero after guest sync; all four primary filesystems
passed offline checks. Evidence is under `rr-load/` in the workload directory.

The first MLFQ comparison intentionally selects policy from INIT.RC before
`SchedulerStart`/AP activation, avoiding live hot switching. Its assertion
caught policy selection failing: registered MLFQ needs 8,240 bytes, but kernel
storage was 8,192. No MLFQ performance result was collected from that RR boot.
The rejected attempt was shut down cleanly and its four primary filesystems
pass. A shared 16-KiB storage contract, compile-time policy checks and hosted
size assertion now cover this mismatch. The assertion fails on the old bound
and all 51 module checks pass on the new bound. The rebuilt guest selects MLFQ successfully before scheduler/AP activation;
its boot log and timing-status reply agree. All 58 hosted suites and both
compiled audits pass again. Matched RR/MLFQ performance verification remains
pending; the first MLFQ run below uses the storage-corrected kernel.


## MLFQ pilot and procfs observation correction

On the storage-corrected kernel
`1471ce537a84563d409a21443acfe2f4abef6718c186af70bbd5589b80c0a48c`,
MLFQ finishes both bounded runs with all CPU workers progressing and no early
sleeper returns. At 24 workers, the pilot gives 2,043,281 batches, a maximum/minimum
worker-count ratio of 1.0246, sleeper p95 21.25–21.26 ms, maximum 63.94 ms and
75 skipped periods per sleeper. At eight workers it gives 1,984,538 batches,
ratio 1.016, p95 up to 20.36 ms, maximum 999.47 ms and 136 skipped periods.
These single trials, on different boots and kernel hashes from the first RR
pilot, do not justify a default switch or a causal performance claim. The
roughly one-second shared outlier still needs explanation. Retained evidence
is under `mlfq-storage-load/`.

The first `lapic_clock_observer.py` run parsed `/proc/uptime` as normal decimal
seconds and reported repeated backward jumps. Inspection found a separate
formatting defect: `GenUptime` prints integer centiseconds without zero padding,
so 226.09 becomes `226.9`. Those apparent jumps are not evidence of actual
counter reversal. Keep the failed diagnostic under `lapic-observation/`.

A second 24-second observation uses explicit `--brook-centiseconds` decoding
of the known legacy field, records raw strings and brackets reads with KVM
monotonic time. With a concurrent, bounded 24-worker/12-second load it takes
5,456 samples, sees zero backward counter steps and zero jumps exceeding the
read-window/quantization bound. Largest per-sample offset change is 24.36 ms;
maximum read window is 0.862 ms. Heap integrity passes afterwards. This does
not rule out early-boot or intermittent CMOS rebasing, and no playback speedup
is reproduced. Evidence is under `lapic-counter-observation/`.

The procfs source now emits both centisecond digits explicitly for uptime and
idle time (its formatter has no width modifiers). The rebuilt RR guest kernel
`51b528c4a0466b591b95e6b55a89b787d0f2874adb757fda31c0cb74463890cb`
passes a six-second, 1,900-sample default-decimal observation with
`--require-centisecond-format`: no malformed fractions, backward observations
or large jumps. The 32 clock brackets and 36 sleep checks pass again. Both
compiled audits pass for that kernel. This validates the procfs formatting
repair, not the reported application playback-speed issue.


## Matched policy comparison and blocking-audio-write defect

The repeated comparison freezes kernel
`51b528c4a0466b591b95e6b55a89b787d0f2874adb757fda31c0cb74463890cb`,
eight vCPUs, 8 GiB, host CPUs 0–13, Venus GPU composition and silent host audio.
Each policy gets private copies of the same checked data/Nix images, is
selected before scheduler/AP activation, and runs worker counts 8,24,24,8 for
ten timed seconds each. Both guests pass default-decimal procfs formatting,
clock brackets and sleep checks. No sleeper returns early, all CPU workers
progress, heap checks pass, launchers exit zero after guest sync and all four
primary filesystems pass offline checks.

| Policy / CPU workers | Sleeper p95 across two runs | Worst lateness across runs |
| --- | --- | ---: |
| RR / 8 | 7.20–7.79 ms | 83.37 ms |
| RR / 24 | 91.07–93.52 ms | 128.11 ms |
| MLFQ / 8 | 21.61–21.66 ms | 52.76 ms |
| MLFQ / 24 | 21.15–22.61 ms | 103.21 ms |

The loaded-case improvement comes with worse lighter-load latency. Do not
switch the default on these synthetic results. The earlier roughly one-second
MLFQ outlier is not reproduced by these matched runs and is not explained.
The retained individual rows and scope are under `matched/summary.json`.
The [policy proposal](scheduler-policy-next.md) sets out EEVDF motivation,
accounting/preemption prerequisites, module lifetime and validation gates.

An audio-only 30-second tone attempt on RR failed after roughly six seconds:
`os.write` returned EAGAIN on a blocking DSP description. Keep the failure
under `matched/rr/audio/w0/`; its sampled stream had no discontinuity flag.
A later traced retry delivered all 1,323,000 frames in 29.753 guest seconds.
Its warmed eight-second host capture contained 353,285 PCM frames, a frame/time
ratio of 1.0004 at 44.1 kHz and no silent 10-ms bins. The maximum four-second
client completion ratio was 1.0236. This does not reproduce a 1.5× playback
error and uses the silent backend rather than the user's PipeWire output.

The failure prompted a focused regression: pause a private stream, fill its
FIFO, clear O_NONBLOCK and start a writer; release space by reset after 1.2 s.
The baseline returned EAGAIN after 546 ms instead of waiting. The syscall had
an unconditional 500-ms LAPIC-tick deadline for blocking output. The candidate
removes that expiry while retaining nonblocking EAGAIN, signal interruption
and explicit device-discontinuity errors. On kernel
`05e4d6840be332129f8bc294dd392fa91f3bebb5f627ed5e2a529db1d5c61d86`,
the full nonblocking write still returns EAGAIN, the blocking writer remains
waiting until reset, and it then accepts exactly four bytes and exits cleanly.
Compiled entry/start audits pass. The idle and 24-worker reruns both deliver
all 1,323,000 frames, taking 29.928 and 29.922 guest seconds. Their warmed
8.006/8.083-second host captures contain 353,059/356,616 PCM frames: frame/time
ratios 1.00004/1.00041, with no silent 10-ms bins. Maximum four-second client
completion ratios are 1.0118/1.0241. Those short-window results do not reproduce
a 1.5× error. Existing DSP ownership and pointer/geometry/drain/signal probes
pass; all tracked lineages drain. A post-exit idle capture has zero frames,
heap integrity passes, the owned launcher exits zero after sync, and all four
primary filesystems pass offline checks.
Evidence is under `dsp-blocking-{before,after}/`. Do not claim this repair
explains the reported playback-speed issue.


## Silent PipeWire fixture for the remaining backend comparison

The current successful tone measurements use QEMU's `none` backend. To exercise
the user's PipeWire path without speakers or desktop-session changes, a
separate temporary PipeWire 1.6.6 server is prepared with only protocol/client
modules, a dummy driver and a null sink. It has its own 0700 runtime directory
and socket; no ALSA, device-discovery or microphone modules are loaded. A
half-second generated host tone is successfully configured and explicitly
linked into that private graph, and its process exits zero. The first unlinked
smoke attempt and metadata-module omission remain retained as fixture setup
failures. Private setup evidence is under `pipewire-null/`.

The same `05e4d684` kernel on RR completes the 30-second native tone in
27.413 s idle and 27.864 s with 24 CPU workers using the private PipeWire
backend. Four-second completion bounds reach 1.1268/1.1118 at the client.
QEMU's eight-second WAV captures still advance at roughly 1x host time, with
no silent 10-ms bins. The captured tone remains close to 440 Hz. These captures
are upstream of the sink and do not prove the sink plays every submitted frame.
Forcing the isolated graph from 48 kHz to 44.1 kHz reduces, but does not remove,
the discrepancy: completion takes 28.714/28.749 s. A further idle run without
WAV recording takes 28.463 s, so recording alone does not explain it.

In that last run, QEMU tracing records 772 HDA occupancy corrections and 33
codec buffer overruns. The corrections comprise 118 positive 1-ms, 258 negative
1-ms and 396 negative 4-ms changes to its timer origin. The net -1.724 s is
diagnostic only: an overrun discards the codec buffer and resets that origin.
The [QEMU 10.2.2 HDA source](https://github.com/qemu/qemu/blob/v10.2.2/hw/audio/hda-codec.c)
implements both behaviors. This is a concrete device/backend lead, not proof
that LAPIC calibration is wrong or that the user's 1.5x symptom is solved.

`BROOK_HDA_USE_TIMER=off` selects QEMU's alternate callback-paced codec for
controlled comparisons; `on` remains the default. It changes the emulated
device behavior, not Brook's clock source or scheduler. The first PipeWire
test VM completes guest sync and QEMU exits zero; its launcher wrapper exits
127 because its live shell source was edited before Bash reached EOF. That
harness error is retained rather than called a clean launcher acceptance.
Independent checks of all four primary filesystems pass. Subsequent launcher
edits must wait until the owning shell exits, as well as QEMU.

Evidence: `matched/rr/audio-pipewire{,-44100,-nocapture}/` and
`pipewire-null/hda-trace-summary.json`. Actual Quake/Firefox reproduction and
the alternate HDA mode comparison remain pending.
