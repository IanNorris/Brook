# Empty desktop frame breakdown

This is the first control for the GPU performance investigation. It measures
the shared compositor/virgl path, not a Vulkan application. No pacing,
buffering, fence, resource lifetime or scheduler-default behavior has changed.

## Host polling baseline

Two private empty-desktop runs use the same frozen kernel/modules, eight
vCPUs/8 GiB, optional MLFQ, GPU composition and 1920×1080 scanout. No app windows
exist at either endpoint. The render node is `/dev/dri/renderD128`, PCI
00:02.0, Intel UHD 630 (`8086:9bc5`); the separate NVIDIA card is not selected.
PipeWire/HDA remain configured but no application plays audio.
The initial device display-info reply advertises 1280×800; the driver then
creates and binds a 1920×1080 primary display and compositor render target.
The final serial inventory verifies the latter dimensions and active GPU DRAW
composition for every cohort; the initial reply is not the tested resolution.

Quiet eight-second host-cost windows, three-second CPU/CS profiles and separate
six-second host traces are kept distinct. Only the private QEMU fallback fence
poll changes between the two runs:

| Measurement | 10-ms host poll | 1-ms host poll |
| --- | ---: | ---: |
| Quiet compositor interval p50 / p95 | 27 / 33 ms | 19 / 20 ms |
| Quiet QEMU process CPU cores | 0.4983–0.4987 | 0.2760–0.2762 |
| Trace compositor fence-response p50 / p95 | 9.544 / 10.012 ms | 1.797 / 2.413 ms |
| Trace response → next submit p50 / p95 | 16.439 / 22.447 ms | 16.794 / 17.716 ms |

These are one ordered pair, not repeated balanced trials. Host fence response
is completion observation, not GPU execution. The source sleeps for 16 ms
**after** rendering; remaining frame work and wakeup latency also contribute
to response→next-submit. Do not add these medians into a synthetic frame.
The 10-ms CPU profile contains 121 compositor samples: 119 include
`SubmitCommand`, and 116 include its completion wait. Most samples on the
other cores are idle. IRQ sampling and profile-writer work prevent treating
those counts as unbiased CPU percentages. Module addresses are resolved using
the exact staged module hashes, recorded load addresses, loader section
placement and bounded function intervals.

Evidence is under `build/reviews/unattended-empty-desktop-gpu-{10,1}ms-fixed-mlfq-a-20261021/`.
All original controllers/VM owners exit zero and all four filesystems per run
check cleanly. This does not establish application performance or recommend a
normal QEMU default change.

## Reconciled native frame recorder

`gpu frame timing on|off|reset|status|frames` exposes an optional 256-row
recorder. The disabled path does not read clocks or acquire recorder locks.
Recording uses stable KVM elapsed time and try-locks; any contention delta,
clock failure or invalid frame rejects analysis. It does not promise a fixed
probe execution bound. Normal controls/exports take a lock and are not panic
inspection commands. Stop before exporting all rows in one response.

The frame starts before deferred captures and scene draining, and ends after
debug polling, immediately before blocking. Nested scopes are exclusive:
their time is removed from their parent, so every retained row must sum to
`end_ns - begin_ns`. The explicit `other` phase covers residual frame work.
Epoch, frame and LIFO tokens prevent stale/control-raced scopes from charging
a later frame. Stop may discard one incomplete frame; overwritten prefixes
remain visible. First/last fence IDs allow correlation with independent host
traces without assuming that response timestamps are actual scanout.

The corrected native review is
`unattended-frame-breakdown-gpu-1ms-fixed-mlfq-b-20261021`, frozen kernel
`3aa630e73001eb5d7edc294560ad236a6ff8f74b9a9aa884055c1daf4c5a38f0`.
It captures 352 complete cycles, retains the last 256 and reports 96 overwritten
rows, no aborted/invalid frames, clock failures or contended records. Every
retained row reconciles exactly. Times below are guest elapsed, including
preemption, host steal and recorder overhead; they are not CPU execution times.

| Interval | p50 | p90 | p95 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Frame body | 2.558 ms | 2.990 ms | 3.470 ms | 10.890 ms |
| Frame start → next start | 18.005 ms | 19.006 ms | 20.009 ms | 27.006 ms |
| Frame end → next start | 15.533 ms | 16.017 ms | 16.159 ms | 16.983 ms |
| Installed remaining sleep budget | 15.113 ms | 15.172 ms | 15.840 ms | 15.999 ms |
| Resume minus installed deadline | 0.401 ms | 0.871 ms | 0.940 ms | 1.005 ms |

| Exclusive stage | p50 | p95 | Share of total retained body time |
| --- | ---: | ---: | ---: |
| Deferred captures | 0.0072 ms | 0.0091 ms | 0.26% |
| Scene draining | 0.0011 ms | 0.0015 ms | 0.04% |
| Scene/CPU preparation | 0.0685 ms | 0.0848 ms | 4.09% |
| Texture upload residual | 0.0033 ms | 0.0047 ms | 0.13% |
| Quad recording | 0.0014 ms | 0.0019 ms | 0.06% |
| Command-lock acquisition | 0.0009 ms | 0.0010 ms | 0.03% |
| Vertex upload residual | 0.0008 ms | 0.0010 ms | 0.03% |
| Command construction residual | 0.0014 ms | 0.0018 ms | 0.05% |
| Queue setup and response validation | 0.1752 ms | 0.3366 ms | 7.02% |
| Fenced completion wait | 1.9232 ms | 2.5166 ms | 68.55% |
| Ordinary acknowledgement wait | 0.3566 ms | 0.5685 ms | 19.67% |
| Display flush residual | 0.0004 ms | 0.0007 ms | 0.02% |
| Debug capture | 0.0007 ms | 0.0011 ms | 0.03% |
| Debug polling | 0.0003 ms | 0.0005 ms | 0.01% |
| Other/residual | 0.0008 ms | 0.0010 ms | 0.03% |

“Residual” matters: nested queue/wait time appears in separate exclusive rows.
An alternative attribution view groups complete control-call elapsed time by
the calling phase. Those groups reconcile exactly to queue setup + both wait
categories and must **not** be added to the stage table:

| Calling phase | Calls across 256 frames | Per-frame call elapsed p50 / p95 |
| --- | ---: | ---: |
| Texture uploads | 1024 | 0.371 / 0.630 ms |
| Vertex upload | 256 | 0.036 / 0.076 ms |
| Draw construction/submission | 256 | 1.931 / 2.525 ms |
| Display flush | 256 | 0.109 / 0.186 ms |

There are seven control requests per frame and one fenced request. Reported
request bytes are command packet bytes, not transferred pixel/vertex bytes.
The next ownership experiment can overlap independent CPU packet preparation
with the vertex transfer. Its immediate gain is bounded by the smaller of
transfer wait and CPU construction time; it is groundwork for broader overlap,
not a plausible explanation for all the missing presentation throughput.

## Clock-domain correction and verification

An earlier diagnostic build mislabeled `wakeupTick * 1,000,000` as KVM
nanoseconds. APIC elapsed ticks instead use their own `(origin_ms, origin_ns)`.
The source now exports raw deadline ticks. The analyzer requires stable matching
pvclock origins/mode and unchanged rejected-sample count at both endpoints,
then checks `origin_ns + (deadline_ticks - origin_ms) * 1,000,000` for underflow
and overflow. Early dirty/input wakes remain valid signed observations.

The earlier `…-a` review preserves its original export and an explicitly
hash-guarded recovery of raw ticks, with a separate checked origin conversion.
It is not evidence that the original field was correctly labeled. The final
`…-b` run exports raw ticks directly, requires no legacy recovery, and includes
the complete command attribution. The analyzer source is retained per run.

The final off/on/off quiet windows use the same kernel. Host process CPU cores
are respectively 0.2623–0.2625, 0.2847–0.2850 and 0.2823–0.2825. Before/after
millisecond compositor intervals have p50 19/18 ms and p95 20/20 ms. This small
single sequence bounds interpretation rather than proving zero recorder cost.
Original controller/VM owners, heap/runtime gates and all four filesystem
checks pass. No app workload or full rendering-correctness gate is claimed.

The actual model tests cover exclusive nested scopes, owner filtering, ring
overwrite, stale epoch/frame tokens, wrong-order scope exit, control races,
clock failures and command identity. Ten Python tests reject corrupted
exports, lost rows/records, invalid origins, incomplete attribution and
double-counted time. Release build, entry/startup audits and all 78 hosted
suites pass. The two final model edge cases were rebuilt and rerun separately
in the Nix shell. A direct host rebuild failed to locate libc++; its test was
not run, and the failed invocation is retained rather than counted as a pass.
The broader timing/audio/scheduler findings remain open.

To analyze a bundle of acknowledged start/stop replies, clock snapshots and
stopped rows:

```sh
python3 scripts/workloads/analyze_frame_timing.py capture.json \
  --output breakdown.json --csv frame-breakdown.csv
python3 scripts/test_analyze_frame_timing.py
```

The final review's `frame-breakdown-control/` holds `raw-capture.json`,
`capture.json`, `reconciled-breakdown.json`, `frame-breakdown.csv`, off/on/off
cost brackets and the verification receipt. Artifacts remain private below
`build/`; no licensed application data is committed.

## Loaded recorder validation

The first Quake capture exposed two diagnostic problems. Foreign GPU callers
attempted the recorder lock before the model rejected their owner identity;
the stopped frame export also sent each scalar separately. Its export exceeded
60 seconds. That failed capture remains under
`build/reviews/unattended-preparation-overlap-gpu-1ms-fixed-mlfq-b-20261021`.
Frozen rows were recovered read-only after app exit, but the unpersisted original
start/stop acknowledgements prevent accepting it as a normal capture. A bulk
stdout trace download also failed its loss gate. The complete timestamp file
was subsequently recovered from the checked offline private disk; neither
original transport failure is counted as a pass.

Runtime probes now check an atomic owner hint before attempting the lock or
reading the clock, then retain the model's authoritative ownership check under
the lock. Stop/reset clear the hint. Actual-owner contention remains counted
and rejected by the analyzer. Each row is serialized through one bounded reply
buffer, preserving exact JSON while reducing socket writes. Hosted tests compile
the actual wrappers and row serializer, checking foreign admission, genuine
owner loss, reset/stale tokens, unavailable clocks and maximum-value chunking.
Release build, all 81 hosted suites and 11 analyzer tests pass.

Native review
`build/reviews/unattended-loaded-recorder-gpu-1ms-fixed-mlfq-a-20261021`
uses kernel
`f254dca9431761b3ed83befe56138118ae2d3612fd60fb6247208052fe10f0aa`
and unchanged overlap GPU module
`81ab428a1b8f19d2978231343241f4b972012cc2784b26b6f00ebd36d157379b`.
It retains eight vCPUs, 8 GiB, MLFQ, 1920×1080 GPU composition and the private
1 ms host fence poll. Quake GL3, Venus vkcube and vkmark run sequentially, each
with 256 reconciled compositor rows and no recorder contention delta.

| Workload | Actual client | Body p50 | Compositor interval p50 |
| --- | --- | --- | --- |
| Quake 2 GL3 | 1280×720 | 4.185 ms | 11.017 ms |
| Venus vkcube | 800×600 | 4.999 ms | 5.997 ms |
| Venus vkmark | 1280×720 | 6.015 ms | 6.043 ms |

These compositor intervals are not application frame times. Quake's buffered
SDL trace, hash-checked after offline extraction, completes 72 swaps within the
retained 3.225-second guest-clock window (22.33 swaps/s); complete inter-swap
interval p50 is 49.95 ms and p95 61.99 ms. Boundary-crossing gaps remain explicit.
Vkmark reports vertex 37 FPS / 27.027 ms and texture 38 FPS / 26.316 ms, both on
Virtio-GPU Venus backed by Intel UHD 630. Vkcube requested 1280×720 but negotiated
800×600, so it must not be presented as a 720p performance result.

Quake's stopped row export takes 1.42 seconds. Window minimize/restore replies
are separately timed (about 255/137 ms for Quake), including bridge overhead;
they are not input-to-photon measurements. Screenshots, CPU profiles and timing
windows are separate. All owned apps exit with complete output, no remaining
tasks or windows, clean heap/runtime checks, successful original VM owner exit
and four clean offline filesystem checks. This is one candidate cohort without
a matched performance control or comprehensive rendering-correctness verdict.
The outstanding GL/Vulkan comparisons and larger fence/pacing targets remain
open.

## Matched host controls and uncapped Quake benchmark

The loaded experiment repeats the same frozen guest and actual client sizes
with only the matched host fence-poll build changed. The 1 ms → 10 ms → 1 ms
sequence gives:

| Application measurement | 1 ms, A | 10 ms, A | 1 ms, B |
| --- | ---: | ---: | ---: |
| Quake GL3 observed swap completions/s | 22.33 | 11.59 | 21.33 |
| vkmark vertex FPS | 37 | 16 | 37 |
| vkmark texture FPS | 38 | 16 | 37 |

**These Quake runs had VSync enabled.** The saved config explicitly contains
`r_vsync "1"`; they remain application pacing measurements, not uncapped GPU
throughput. `cl_maxfps=60` is the packet limit with Yamagi's default async
rendering, not its render cap. Quake's recorded compositor body p50 is
4.185 ms at 1 ms polling versus 20.026 ms at 10 ms. Separate host traces show
compositor-context fence-response p50 2.049 versus 9.517 ms. That observation
includes host polling/scheduling and is not GPU execution duration. The
host trace analyzer excludes sequential-enable/disable transitions using a
known-enabled clock interval; boundary partials stay explicit and interior
identity/ordering failures remain errors.

At the user's request, a separate fixed-demo benchmark explicitly disables
VSync and bypasses engine frame pacing. The exact Yamagi source uses
`timedemo=1` to skip both packet and render pacing; `vid_maxfps=0` alone would
be clamped to 999. The new `--uncapped` option in
`scripts/workloads/q2_demo_timing.py` sets `r_vsync=0`, `timedemo=1`,
`cl_async=1`, `cl_maxfps=250` and `vid_maxfps=999`. Effective cvar queries and
saved config confirm these values in both runs. The extra queries use a
private config script because the engine's 50-argument limit rejected the
first attempt. That failed launch and a failed verification query remain
retained; the completed benchmark was not rerun to recover its evidence.

| Uncapped Quake measurement | 1 ms host poll | 10 ms host poll |
| --- | ---: | ---: |
| Built-in timedemo, 200 frames | 38.6 FPS | 13.5 FPS |
| Observed steady SDL completions/s | 43.09 | 13.43 |
| Complete inter-swap interval p50 / p95 | 19.04 / 47.32 ms | 75.08 / 100.05 ms |

The built-in numbers include the warmup section. The SDL window uses observed
START through the final available swap; END is observed about 20.58/10.49 ms
after that swap. Strict analysis rejects the original uncovered bounds, so
the adjusted bounds and uncovered tails are explicit. Marker timing is not
an exact GPU frame boundary. Timedemo is unpaced simulation and does not test
normal-speed demo/audio playback. This is one ordered uncapped pair, not a
balanced repeated benchmark or a claim that GPU work itself became 2.9× faster.

Evidence is in private reviews
`unattended-loaded-recorder-gpu-{1,10}ms-fixed-mlfq-{a,b}-20261021`
(only existing combinations), and
`unattended-uncapped-q2-gpu-{1,10}ms-fixed-mlfq-a-20261021`.
The successful 1 ms uncapped run is under `uncapped-q2-retry/`; the 10 ms run
uses `uncapped-q2/`. Both raw traces are hash-checked against guest metadata
after original owner exit and four clean offline filesystem checks. The
VSync-on reversal likewise passes its health, output and disk checks.

`tools/qemu-fence-poll-pkg/default.nix` makes the host controls reproducible.
With the repository's pinned package set, its 1 ms and 10 ms derivations and
realized outputs exactly match the tested binaries. It changes the guarded
single `virtio_gpu_fence_poll` timer site, preserving the shared patches and
configure options. The normal launcher selection remains unchanged. To opt
in before using an existing GL/Venus launch command:

```sh
export BROOK_GPU_QEMU="$(nix-build tools/qemu-fence-poll-pkg \
  --no-out-link --arg fencePollMs 1)"
```

The package is guarded to QEMU 10.2.2 and rejects a changed patch site. The
measured gain belongs to faster host completion observation; the independent
compose storage remains a correctness foundation with no demonstrated material
throughput gain on its own. Additive pacing, remaining app-side waits and
broader input/rendering acceptance still require investigation. Timing, audio
and scheduler findings remain open.

The 1 ms uncapped VM also records one scheduler service clamp of 427 ns
(PID 14, CPU 1); its runtime acceptance gate fails. The debt is subsequently
caught up, but this is retained as an unresolved accounting finding, not
waived by the clean process exit, heap or filesystem checks. The 10 ms
uncapped VM has zero runtime error counters. These throughput observations
therefore do not establish full acceptance or a scheduler-default change.

For the 1 ms uncapped trace, complete frame intervals partition exactly into
41.16% inside SDL swap and 58.84% between calls. The respective p50/p95
durations are 4.05/40.10 ms and 14.77/18.66 ms. Between-call time includes
simulation, rendering, driver calls, kernel waits and scheduling; it is not
a vblank wait or pure CPU time. The detailed compositor stage percentages
above belong to the separate VSync-on capture; a matching uncapped capture
is needed before attributing this residual to framebuffer ownership.

## Uncapped compositor and submission diagnosis

A separate 600-frame-record diagnostic uses the same frozen guest and 1 ms
host poll, with effective VSync/pacing settings checked again. Longer
captures use `--measured-frames`; original embedded packets and frame-number
gaps are preserved and reported. An oversized request and two earlier
helper assumptions failed before launching the game; their jobs remain in
the private review. The successful capture has 601 server-frame deltas over
600 original frame records. It is not pooled with the short throughput pair.

The retained 256 compositor rows reconcile exclusive body time exactly:

| Exclusive stage | Share of body | p50 | p95 |
| --- | ---: | ---: | ---: |
| Fence completion waits | 72.85% | 3.308 ms | 5.068 ms |
| Acknowledgement waits | 9.39% | 0.372 ms | 0.518 ms |
| GPU command-lock acquisition | 6.97% | 0.002 ms | 3.446 ms |
| Queue setup | 5.72% | 0.272 ms | 0.451 ms |
| Scene preparation | 4.37% | 0.188 ms | 0.280 ms |
| Other exclusive stages combined | 0.70% | — | — |

Body p50/p95 is 4.556/8.995 ms; start-to-start p50/p95 is
5.077/13.647 ms. Between-frame gap p50/p95 is 0.000495/9.082 ms,
with 247 of 255 resumes before the installed fallback deadline. The
nominal additive 16 ms fallback is therefore not an unconditional sleep in
this loaded capture. Ring overwrite drops the earlier 213 rows; one
in-flight row is explicitly aborted when recording stops. Retained rows
pass the analyzer's conservation and contention gates.

A separate two-second sample records 1,593 CPU samples and 7,774 context
switches with no producer drops or count mismatch. Quake's main thread has
only 30 CPU samples: 17 include GPU completion waiting, of which 15 include
`AppPresentBarrier`. These are small, biased sample counts, not percentages
of frame wall time. Its 320 outgoing context-switch stacks include 215
`AppSubmit3D` command-lock waits; 247 include a DRM ioctl and mutex lock.
The compositor has 139 command-lock yield stacks. Categories overlap and
must not be added. The profiler writer itself has 58 samples, so this
diagnostic must not be substituted for an uninstrumented throughput run.

Source inspection confirms the mechanism: `GpuCmdGuard` serializes app and
compositor control submissions, and synchronous completion is retained
through the protected operation. `FENCE_FD_OUT` performs a real producer
fence before returning a completed sync file. This is necessary for current
cross-context correctness; removing that wait would falsely signal completion.
The next target is measured app-side lock/completion wall time and a design
for multiple owned in-flight submissions, not assuming extra framebuffers
alone remove these waits.

Evidence: private review
`unattended-uncapped-q2-gpu-1ms-fixed-mlfq-b-20261021/uncapped-diagnostic-d/`.
The original VM/controller exit successfully; heap, runtime counters and
four offline filesystems pass. The raw SDL trace is recovered after shutdown
and hash-checked. No new visual/input or physical-audio acceptance is claimed.

A fresh VM repeats the 600-record unpaced workload with only the existing
main-thread syscall watch enabled for approximately four seconds. Reported
dispatch totals are 2.224 s in 78,672 ioctls, 1.049 s in 108 futex calls,
and 0.698 s in 238 ppoll calls. Elapsed spans include preemption and host
scheduling. The fixed 16-kind recorder overflows 1,986 kind assignments;
therefore this capture **fails full syscall reconciliation** and the listed
totals are partial, not wall-time percentages or a replacement for the
SDL split. No continuity, clock or contention errors are reported.
Request-level GPU ioctl timing is the next diagnostic needed to separate
command-lock acquisition, producer fences and other waits. Evidence is
`unattended-uncapped-q2-gpu-1ms-fixed-mlfq-c-20261021/uncapped-syscalls/`;
original VM/controller exit and four offline filesystem checks pass.

## Request-level client timing and scene completion candidate

`tools/profiling/drm_ioctl_trace.c` is an optional libdrm preload probe. It
uses fixed-size per-thread/request/phase aggregates, logarithmic duration
histograms, explicit overflow/clock/in-flight quality counters and teardown
output. Optional hooks in the SDL2 swap probe classify the swap thread's
calls after 50 skipped swaps; other threads/startup remain unscoped. The
analyzer rejects lost/incomplete/inconsistent aggregates. Histogram percentile
outputs are bucket bounds. Elapsed time includes scheduling, not just CPU.
The eight actual-probe host tests cover phases, errno/return preservation,
concurrent threads, disabled output, overflow/corruption, in-flight teardown,
late RTLD_LOCAL libdrm lookup and actual SDL swap-interval metadata.

In private `unattended-drm-q2-gpu-1ms-fixed-mlfq-a-20261021/drm-q2-retry/`,
618 swaps after the skipped prefix take 17.782 s inside SDL, of which
2.530 s is inside libdrm; the explicit non-DRM residual is 15.252 s. There
are 617 complete between-swap intervals totaling 5.897 s. Outside-swap
libdrm aggregates include shutdown; conservatively bounding that 201.632 ms
tail gives 3.802–4.004 s within the intervals and a 1.893–2.095 s residual.
These are a longer diagnostic scene span, not the earlier short benchmark
or exact observed demo-marker bounds. Probes and different later demo scenes
preclude treating its throughput as a clean performance comparison.

Between-swap EXECBUFFER requests account for 3.367 s across 3,360 calls,
while 267,986 WAIT calls account for 0.531 s. Inside-swap EXECBUFFER calls
account for 2.529 s across 618 calls. Thus submission serialization is real,
but the large inside-swap residual requires investigation of the Wayland
server/callback path as well. The shared queue is not established as the sole
presentation bottleneck. The first launch failed because Mesa loads libdrm
locally; the probe's guarded loaded-library lookup fixes that case, with a
regression test. The failed original job remains retained.

A separate `unattended-drm-q2-gpu-1ms-fixed-mlfq-b-20261021/drm-q2/` capture
queries SDL2's getter: all 668 swaps report interval zero, with no missing
queries, dropped rows or incomplete swaps. This verifies the API state beyond
the saved cvar. Existing Wayland `--trace-frames` is enabled in this capture.
The pinned Mesa 26.0.6 source's `dri2_wl_swap_buffers_with_damage` explicitly
uses a display-sync callback even when the interval is zero, allowing the
server to process commits and buffer releases. This is not a vblank guarantee.
Both diagnostic VMs exit cleanly, with trace hashes checked after four clean
offline filesystem checks and zero runtime error counters.

The next candidate changes scene-job completion notification. Previously the
server's capture and dependent present calls each polled `WaitJob` with 1 ms
sleeps; the worker never woke the waiting caller directly. The candidate
registers a pinned caller under the scene lock, publishes completion under
that same lock, then takes a temporary reference and notifies outside the
lock. Scheduler pending wakes cover completion before block admission.
Notification precedes potentially expensive retirement of the old immutable
root; the worker retains its job reference through retirement. Absolute
timeout and finish-before-return ownership remain. Pending ordinary signals
and already-canceled running jobs retain the previous bounded sleep fallback.

The worker-canceled path also now completes its source-ownership callback
before publishing Done, closing a pre-existing cancellation-ordering race.
Actual service tests cover completion before registration, between registration
and block, after blocking, signal/timer admission races, masked signals,
notifier/caller reference handoff, worker close, running cancellation/timeouts
and uncertain results. The final build passes all 81 hosted tests, eight
preload tests and 15 frame/swap analyzer tests. The first native results below do not demonstrate a performance gain;
the completion-wakeup path remains an opt-in experiment. No GPU fence or scheduler default
is removed or changed. The separately identified legacy AppBufferUpload
attachment/teardown lifetime hazard remains open before wider concurrency.

The always-on candidate's traced 600-record Quake interval is 24.656 s versus
23.564 s in the preceding traced control, with the same probes, driver modules
and actual SDL interval zero. Inside-swap non-DRM residual grows from
15.777 to 16.939 s; between-swap totals remain approximately 5.85 s. This
single ordered pair does not prove a regression, but it does not support a
speedup claim. Its separate loaded cohort reports Quake VSync-on observed
swap completions 20.80/s, vkmark vertex/texture 33/33 FPS, versus earlier
1 ms controls around 21–22/s and 37–38 FPS. Native heap/runtime/output,
original VM/controller exit and four offline filesystem checks pass.
Inspected Quake, vkcube and vkmark snapshots show plausible scenes without
obvious geometry/surface corruption; these are not frame-by-frame goldens.
Minimize/restore acknowledgements include bridge latency and do not establish
input-to-photon responsiveness.

Accordingly, `gpu scene wake on|off|status` controls a **default-off** runtime
experiment. Each WaitJob snapshots the setting; switching it off never
discards an existing registered waiter or its notification. The source
ownership-before-Done cancellation fix remains unconditional. A final build
with the control passes all 81 hosted tests and the same 23 profiling/analyzer
tests, including switching the control off during an active registration.
A same-kernel off/on/off diagnostic is required before reconsidering the
default. Broader concurrency, the legacy upload lifetime hazard, server-side
commit/sync attribution, physical-audio and scheduler acceptance remain open.

The same-kernel off/on/off run completes in 23.853 / 25.182 / 22.562
guest-clock seconds over the observed demo-marker span. All three runs use
the same hash-checked private demo and preload binaries, and all 668 swaps
report actual SDL interval zero. The on run is slower than both adjacent
off controls, chiefly in the inside-swap non-DRM residual (17.609 s versus
15.839 / 14.785 s), while inside-swap DRM is 2.470 s versus 2.544 / 2.526 s.
The native control switch works and remains off after the trial. Heap,
runtime counters, original controllers/VM and four offline filesystem checks
pass; raw traces are hash-checked. This is one ordered trial, not a statistical
regression proof. It gives no reason to promote completion wakes to default.
Evidence is `unattended-scene-wake-control-gpu-1ms-fixed-mlfq-a-20261021/`.

Next, instrument a bounded Wayland commit/scene-control/display-sync/output-
flush timeline with explicit client, surface and callback identity. A protocol
event queued and a flush attempt are not proof of delivery to the client.
Only after attributing this residual should we consider combining capture
and present into a transactional job or allowing more owned GPU submissions.

### Shared Unix-socket poll notification (2026-10-21)

The next experiment followed a more specific clue: the three same-kernel
completion-wake controls all had median SDL completion intervals near 50 ms,
with actual SDL swap interval zero. `PollWait` has a 50 ms fallback and stored
connected Unix-socket pollers in the ordinary single `readerWaiter` slot.
`scripts/workloads/poll_shared_socket.py` keeps a byte unread until all pollers
return, reverses thread launch order, and compares shared, separate and single
socket endpoints. Entry markers are observation bounds, not proof of kernel
registration. These are elapsed times including scheduling, not CPU costs.

On the checked baseline, one of two shared-socket pollers returned promptly
and the other returned about 22–23 ms after the write. With three pollers, two
were delayed about 17–22 ms. Single/separate endpoints returned promptly.
The Linux two-poller control woke both promptly. Sources, raw writer timestamp
bounds and results are retained under `unattended-poll-reader-control-gpu-1ms-
fixed-mlfq-a-20261021/poll-reader-probe/` and
`reboot-resume/poll-readers-linux-control.json` in the ignored experiment tree.

The candidate gives each poll operation exact subscriptions, separate from
ordinary blocking readers/writers and the existing epoll slot. Registrations
retain the process and buffer storage without incrementing open endpoint
counts, so they preserve EOF. Allocation failure unwinds exact registrations
before legacy hooks are installed. Notification snapshots monotonically
ordered subscription identities and takes process references across unlocked
wakeups; no registration pointer survives unlock. A notifier also retains
storage when the last subscriber unregisters during its wake. Cleanup uses
saved backend identity rather than looking up a reused numeric descriptor.
The 50 ms fallback is unchanged. Listening sockets, pipe-backed epoll and
other legacy descriptor hooks are outside this patch's scope.

The first candidate private VM removes the shared-endpoint delay: all two-
and three-poller observations return within about 1.05 ms after the write.
The same 600-record uncapped Quake diagnostic takes 13.557 s between observed
markers, versus 22.562–25.182 s in the recent controls. After the same first
50 swaps are excluded, SDL completion rate is 44.393/s, median interval
22.037 ms and p95 32.933 ms, versus about 24–27/s and a median near 50 ms.
These are guest-clock swap observations, not physical scanout or normal-speed
audio acceptance, and this initial before/after is not an independent host-
load-controlled statistical trial.

The reconciled candidate interval includes 6.581 s inside SDL swap: 2.643 s
inside DRM and 3.938 s residual. Between swaps contributes 7.321 s, of which
DRM is bounded at 5.086–5.307 s after preserving a 0.222 s shutdown-tail bound.
The reduction is chiefly the inside-swap non-DRM residual, previously about
15–18 s. All 668 actual SDL interval queries return zero. Probe quality,
original VM/controller exits, heap, runtime counters and four offline disk
checks pass; raw traces are checked against hashes recorded before shutdown.
Evidence: `unattended-poll-reader-fixed-gpu-1ms-fixed-mlfq-a-20261021/`.

The candidate passes all 82 hosted suites. Its subscription tests execute the
actual registration, notification and destruction implementation under real
competing threads, including repeated/overlapping notification, exact removal,
merged interests, backend replacement, allocation failure, identity exhaustion
and last-subscriber destruction during notification. ASan/UBSan passes. These
hosted tests do not execute the complete `PollWait` admission body; the native
socket reproduction exercises that integration. Desktop/GL/Vulkan validation
and repetition remain required before a broader performance claim.

The subsequent fresh-image desktop/GL/Vulkan cohort also finishes cleanly
(`unattended-poll-reader-loaded-gpu-1ms-fixed-mlfq-a-20261021/`). Representative
Quake, cube and vkmark horse captures show no obvious corruption; this is not
pixel-golden or frame-by-frame validation. Quake's separately controlled normal
demo reaches 35.535 SDL completions/s in the retained compositor-trace window,
versus the previous roughly 21–22/s controls. Its compositor body has median
5.992 ms and p95 9.070 ms. Summed exclusive body shares are about 61.0% fence
wait, 21.0% command-lock wait, 8.4% acknowledgement wait and 5.4% queue setup;
command construction itself is about 0.03%. More frequent work now increases
whole-QEMU CPU use to approximately 0.991 cores (previously about 0.77), so the
throughput gain does not mean lower absolute CPU use.

Vkcube's compositor body median/p95 is 5.071/8.011 ms, with approximately
0.724 cores of whole-QEMU cost. Vkmark reports 38 FPS (vertex, 26.316 ms) and
39 FPS (texture, 25.641 ms), approximately the earlier 37–38 FPS baseline
rather than the Quake-sized improvement. Its compositor body median/p95 is
6.034/10.958 ms, whole-QEMU cost about 0.871 cores. Empty-desktop body median
is 2.591 ms with an 18.147 ms start-to-start median; this trial does not show
an empty-desktop speedup. Quake minimize/restore applied-state acknowledgements
are 170/130 ms and vkcube 334/132 ms; these include bridge round trips and
are not input-to-photon measurements. All app exits, runtime counters, heap
checks and four offline filesystem checks pass. Quake raw swap data is
hash-checked after shutdown. An additional fresh baseline/candidate comparison
remains useful given historical host-environment variability. Scheduler
promotion and unresolved timing/audio findings remain outside these passes.
