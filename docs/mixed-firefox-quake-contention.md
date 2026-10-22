# Live Firefox and Quake contention checkpoint

Status: unresolved. This observation follows the user-approved milestone pushed
to main at86eb6421. Investigation continues on
`wip/desktop-stability-20261022`; the live VM remains the exact frozen earlier
kernel/module set. No scheduler default or audio configuration changes occur
between these observations. The user manually starts Quake alongside Firefox
and reports renewed audio stutter and roughly halved Quake FPS.

## Scope and identities

Private review:
`build/reviews/live-firefox-downstream4096-gpu-1ms-fixed-mlfq-a-20261022/`.
Firefox is PID21/incarnation21, Quake PID614/incarnation614. Both window/client
identities remain unchanged across the short CPU/frame captures. GL context2 is
the kernel compositor; actual serial context activation maps257 to Firefox
process74 and258 to Quake614. The8-vCPU8-GiB VM uses the same MLFQ experiment,
SDL20-ms requested host audio, original12-KiB HDA safety lead, private1-ms QEMU
fence polling, enabled GPU completion waits and4096 downstream audio budget.
Audio client geometries remain4416/64 (Firefox) and4096/1024 (Quake).

These are distinct diagnostic windows, not a matched single/mixed/reversal
benchmark. CPU sampling, frame probes and host logging add overhead. User FPS
is not a retained SDL swap trace, and compositor cycles are not app frames.
No single-app comparison is manufactured by minimizing or closing either app.

## Reconciled compositor frames

The stopped short trace exports249 consecutive rows with no overwritten prefix,
invalid frames, clock failures or contended records. One in-flight cycle is
aborted on stop. The analyzer verifies clock origin/mode, epoch, nested-command
attribution and exclusive phase conservation against the exact runtime schema.

| Metric | Value |
| --- | ---: |
| Body median |18.00ms|
| Body p95 |54.15ms|
| Body maximum |88.41ms|
| Command-lock share of body |28.79%|
| GPU fence-wait share |58.96%|
| Used-ring acknowledgement share |8.68%|
| Scene preparation share |1.91%|
| Texture upload share |0.09%|
| Command queue setup share |1.30%|

Command-lock p95 is33.20ms; fence-wait p95 is27.82ms. These are guest elapsed
waits including scheduling and host delay, not exclusive GPU execution time.
This locates most compositor elapsed time in command serialization/completion,
not scene construction. It does not yet identify the queue owner or prove a
safe way to permit multiple independent requests in flight.

## CPU and audio

The initial10-s CPU profile drops19023 events; a2-s repeat drops92. Both are
preserved and explicitly lossy. A separate500-ms CPU-only capture contains399
samples and8265 context switches with zero drops and matching event counts.
337 samples are in idle;32 leaves are HDA MMIO reads. The inclusive stacks place
34 samples in AudioMixerRefresh, mostly reached through Firefox's audio thread
and ioctl. IRQ-masked work is not sampled uniformly; these fractions are not an
unbiased CPU-utilization measurement or proof that MMIO causes the audio gaps.

In the separate short mixed audio probe, the mixer has maximum sleep37.07ms and
service97.29ms. Two service cycles cross20ms. Producer gaps reach139.1ms, but
maximum completed write-body time is0.120ms, with no recorded backpressure or
write errors. There is one2728-byte HDA silence reservation and no invalid clock,
position or unsafe publication flag. Sampling windows include control/setup
work and cannot causally align every gap to GPU waits or captured PCM samples.

## Host fence and CPU observation

A later finite host trace retains1462 complete fenced control/response pairs,
with at most one fenced control pending and no interior unmatched response or
unfinished request. Boundary exclusions, full trace hash, original wall/monotonic
clock brackets and conservative guest offset bounds are retained. Logging has
no exposed drop counter, so complete pairing is not proof of an unbiased timeline.
All enabled trace events and audio delivery probes are restored off afterward.
The log sink remains available for subsequent QEMU warnings.

| Context | Paired submissions | Host fence median | Host fence p95 | Maximum |
| --- | ---: | ---: | ---: | ---: |
|2, compositor|959|2.080ms|15.294ms|48.820ms|
|257, Firefox process74|358|0.014ms|8.898ms|22.615ms|
|258, Quake614|145|0.022ms|11.497ms|33.796ms|

The trace's fence-control event occurs after command dispatch; response precedes
virtqueue publication. The intervals include host fence handling/polling and
GPU work, but do not directly measure shader execution, guest wakeup or scanout.
Application fenced submissions here contain zero command bytes; unfenced
application commands are also present. Do not infer negligible application GPU
cost from a fast empty-fence median. Compositor packets include0,152,240,324 and
868 bytes; no command packet is decoded here.

Over a separate7.301-s bracket spanning setup and tracing, matched surviving
QEMU threads consume about1.855 host CPU cores in total. The busiest vCPUs use
0.479 and0.445 cores; the main thread uses0.215. New/exited threads are excluded.
This is not GPU utilization or total host load, but it does not establish full
host CPU saturation as the explanation for the observed waits.

## Next attribution and acceptance work

Measure unchanged single/mixed/reversed workloads with stable frame/PCM source
identities. Attribute long command-lock ownership and split host GPU fence
completion from guest notification/ready-list/return latency. Check whether
HDA MMIO progress reads encounter the same host stalls before treating GPU and
audio symptoms as one cause. A production change must preserve descriptor,
response, fence, backing/resource and timeout ownership; incomplete delayed
vertex recovery remains separately parked. Retain explicit residual elapsed
time, native rendering/input checks and original-job shutdown/filesystem gates.
No completed current-goal or scheduler-promotion claim follows from this capture.

## Controlled single/mixed/reversal baseline

The later private review
`build/reviews/matched-desktop-baseline-gpu-1ms-fixed-mlfq-b-20261022`
completed all original application jobs and its original VM wrapper successfully;
boot, data, Nix and temporary filesystems all checked cleanly. This cohort uses
the same frozen kernel/modules, MLFQ, 4096-frame downstream allowance, SDL host
audio and private QEMU. Firefox plays a generated local 720p30 H.264/AAC clip,
removing YouTube ads and network variation. Quake plays the same 600 original
demo frame records at normal demo speed. Its render caps are raised, timedemo
remains off, and the actual SDL getter confirms swap interval zero throughout
each trace. These are swap completions, not physical display scanout.

After clean shutdown, all three raw SDL traces were extracted from the private
temporary disk and checked against the byte counts and SHA-256 values collected
inside the guest. Each is complete with no dropped/incomplete trace records.
The comparison takes 50 seconds starting five seconds after the observed demo
start marker. Marker observations include log-reader scheduling latency, so
these are corresponding central demo windows, not frame-exact scene alignment.
Boundary-crossing intervals are retained explicitly in the analysis artifact.

| Quake condition | Swaps / guest second | Frame interval p50 | Frame interval p95 |
| --- | ---: | ---: | ---: |
| Alone, before Firefox | 58.10 | 16.99 ms | 23.01 ms |
| Alongside Firefox | 28.80 | 33.76 ms | 51.75 ms |
| Alone, after Firefox closes | 56.46 | 17.01 ms | 23.95 ms |

Separate approximately six-second plain audio observations preserve stream
identity and epoch across each counter subtraction. Quake source completion is
44.09k frames/s alone, 28.07k mixed, and 44.13k alone afterward. Firefox is
43.96k alone, 30.44k mixed and 44.18k afterward, against 44.1k native frames/s.
Both applications keep their original logical buffers: Quake 4096/1024 and
Firefox 4416/64 capacity/fragment frames. This reproduces the concurrent deficit
despite the downstream allowance; it does not establish decoded tempo or the
physical speaker rate from counters alone.

Firefox's separate page-state observations advance media by 13.12 seconds over
13.11 seconds alone, 10.73 over 15.56 seconds mixed, and 15.06 over 15.06 seconds
afterward. Browser drop counters rise by 60/393, 118/322 and 89/452 respectively.
Those page intervals overlap diagnostic work and are not the exact plain PCM
windows. The complete source and analyses remain in `matched-control/`, notably
`swap-comparison.json`, `media-comparison.json`, per-phase frame/clock/runtime
bundles and the original job/lifecycle evidence.

The next isolated candidate replaces an ordinary backing-detach acknowledgement
plus an empty fenced submit with a fenced detach command. It retains the prior
upload completion fence and conservative source quarantine on failure. Only the
GPU module changes in the native comparison; the baseline kernel and all other
modules remain byte-identical. Its release build and 87 hosted suites pass,
including actual control-response and upload bodies. Native benefit, rendering
correctness and timeout recovery acceptance remain unproven.

The first native fenced-detach cohort also shuts down cleanly and passes all
four filesystem checks. Hash-verified central-window Quake rates are
57.66 / 29.66 / 56.60 swaps/s alone/mixed/alone. Mixed p95 frame interval is
52.96 ms versus the baseline's 51.75 ms. Mixed audio source completion remains
deficient at 31.31k frames/s for Firefox and 28.92k for Quake. Both cohorts have
zero invalid/backwards/clamped runtime-accounting samples at shutdown.

This pair does not justify promotion: the small throughput difference is not
an established gain, and the candidate's separate mixed frame-capture bracket
uses 4.83 whole-QEMU cores versus 1.81 in the baseline. Solo Quake costs remain
near one core. The cause of that mixed CPU difference is not attributed, and
these brackets are not simultaneous with the plain PCM observations. Retained
scene command-count histograms remain dominated by three- and five-command
cycles in both cohorts; they do not prove the changed owned-upload path was
substantially exercised. Dedicated owned-upload native acceptance is still
needed. Evidence is
`matched-desktop-fenced-detach-gpu-1ms-fixed-mlfq-a-20261022/`.

A more direct next target is desktop-layer upload invalidation. The idle
baseline retains seven commands per compositor cycle: four texture-upload
commands, one vertex upload, one fenced compose and one display flush.
The taskbar is redrawn and marks the shared dirty row range each cycle;
`CompositorPresentGPU` then uses that same range to upload wallpaper, chrome
and overlay textures, and uploads the cursor unconditionally. Separate layer
invalidation or exact unchanged-content detection could remove redundant
commands. This is a source/trace finding, not a measured speedup or permission
to discard updates after failed/uncertain GPU completion.

## Independent layer upload experiment

The next candidate uses module ABI 35: `GpuCompositorOps::UpdateTexture` returns
whether the transfer command received a validated successful response. This
does not prove GPU execution completion or authorize freeing its persistent
backing. Composition fences and existing source ownership rules remain intact.
Failed transfers retain wallpaper/chrome/overlay damage and do not acknowledge
window content or update the cursor cache.

The optimization is default off. Passing
`-fw_cfg name=opt/gpu-layer-damage,string=1` enables independent pending row
bounds for wallpaper, chrome and taskbar/launcher overlay. Overlay clearing and
chrome repaint mark their own layer; structural full repaint and first texture
creation still force complete refresh. Client-only animation does not redraw
chrome. Cursor upload state compares exact
pixels and dimensions, independently of draw position/hotspot, and changes only
after an accepted upload. This adds a small fixed cursor cache, without full
desktop shadow copies or probabilistic content hashes.

The title setter now requests repaint and wakes the compositor; previously a
clean window could wait for the 500-ms forced repaint before its new title
appeared. This fix and the bool-return interface are present in both controls.
Hosted tests exercise actual driver transfer code with response failure,
poisoned queue, disjoint retry damage, layer isolation, full refresh, recreated
texture and overflowing rectangle sizes. Cursor tests cover same-image retry
after failure, pixel changes, shrink/grow, hotspot-only changes and recreation.
The complete release build and all 87 hosted tests pass. Native comparisons,
visual/input/GL/Vulkan gates and the larger timeout recovery work remain open.

The first ABI-35 off cohort is retained as partial evidence: its host harness
omitted the required restore target after minimizing Firefox. Recovery restores
and closes the original Firefox job, then shuts down cleanly with all four
filesystem checks passing. A second private VM is stopped cleanly before any
workload starts after finding that the off path accumulated window-sized chrome
damage. That changed its baseline upload extent. The corrected off path uses
the original shared bounds, and the enabled path redraws chrome only on full
repaint. These controls must use the new `layer-damage-matched-frozen` capsule;
neither discarded cohort is a passing optimization comparison. The corrected
build again passes all 87 hosted suites. Review directories retain the failed
controller, recovery, idle shutdown and exact artifact provenance separately.

### Matched enabled/disabled results

The corrected pair uses byte-identical ABI-35 artifacts from
`layer-damage-matched-frozen`, differing only in `opt/gpu-layer-damage=1`.
Both original controllers and VM owners exit zero; all four filesystems check
cleanly. Original SDL traces are extracted offline and verified against guest
byte counts and SHA-256 values. Actual swap interval remains zero.

| Central 50-second Quake window | Disabled swaps/s | Enabled swaps/s |
| --- | ---: | ---: |
| Alone before Firefox | 56.56 | 31.00 |
| Alongside Firefox | 23.50 | 14.38 |
| Alone after Firefox | 42.92 | 29.12 |

The disabled run itself does not return to its initial solo rate; preserve that
variance rather than claiming complete reversal recovery. Nevertheless this
pair provides no reason to promote the optimization. Mixed frame-interval p95
is 71.20 versus 146.05 ms. The enabled idle command histogram is four commands
for 245 retained cycles and six for eleven periodic full repaints, versus seven
for all 256 disabled cycles. Idle median compositor body remains approximately
2.7 ms and whole-QEMU CPU approximately 0.20 cores in both samples. Removing
uploads demonstrably removes commands, but does not establish a speedup.

Mixed source audio completion remains approximately 30.21k/28.32k frames/s for
Firefox/Quake disabled and 30.90k/28.66k enabled. Solo Firefox has a stable
440-Hz tone and approximately native-rate source completion. The user hears
Quake stutter in the disabled mixed run while the Firefox tone sounds steady;
in the enabled run the tone stutters during Quake loading, followed by strong
Quake stutter and progressive visible hitches. Offline narrowband measurements
retain these as separate observations: game harmonics overlap the reference
tone, so they cannot isolate each stream or establish decoded tempo.

The enabled full SDL trace contains late 1.345-s and 3.947-s frame-completion
gaps. Their inside-swap portions are only 13.40 and 21.03 ms, with 1.332 and
3.926 seconds between swaps. That interval includes rendering/DRM calls,
application work and scheduling; it is not exclusive CPU work. Both frame
captures have 255/255 early resumes and sub-microsecond median between-cycle
gaps. Continuous compositor activity is therefore common to both arms, not by
itself an explanation of the enabled slowdown.

Vkcube and vkmark complete with retained plausible images; vkmark reports the
Venus Intel UHD Graphics 630 device and 43/39 FPS for its vertex/texture scenes.
The user also observes occasional vkcube hitches. Minimize/restore and original
job close checks complete. A separate real-pointer launcher test opens the
menu at 100%, changes scale to 150%, closes it and restores 100%. Its retained
352x780 background crop has zero mismatched bytes after either restoration.
These are rendering/input-state checks, not smooth-pacing acceptance.

Strict runtime accounting remains unresolved: the disabled cohort ends with
one service clamp (maximum deficit 10,633 ns), the enabled cohort with eleven
(maximum 18,523 ns). Neither is a passing zero-clamp acceptance result; those
small samples do not explain multi-second stalls by themselves. No scheduler
default changes follow.

Evidence is under `matched-desktop-layer2-{off,on}-gpu-1ms-fixed-mlfq-a-20261022`.
After this pair, source fixes add invalidation to successful focus/raise and
send-to-back mutations. Actual window-body tests verify quiet-window focus,
ordering, title invalidation and no invalidation on rejected/no-op focus. The
new release build and all 87 hosted tests pass; these fixes are frozen separately
in `layer-focus-frozen`, not retroactively attributed to the comparison above.

### Next completion-notification control

Keep layer mode and one original Quake process fixed, and switch GPU completion
notification on/off/on. Retain per-arm completion counts, frame stages, CPU,
PCM/source counters and actual SDL windows. Different demo scenes make this a
diagnostic reversal, not a definitive equal-scene throughput comparison.
Importantly, zero `safety_expiries` does not prove no timer-delayed completion:
the current counter increments only if the used index is still unchanged when
the waiter resumes. Compare notification/latency-sample coverage as well, and
correlate a common host fence response with guest return if ambiguity remains.

QEMU's actual render descriptors point to i915 at PCI 0000:00:02.0. Linux DRM
fdinfo execution counters are available for future paired sampling. Its four
descriptor aliases share client ID 901 and must be counted once, as specified
by the [kernel DRM accounting documentation](https://docs.kernel.org/gpu/drm-usage-stats.html).
The separate unlabelled post-GL three-second sample shows about 6.25% render
execution for that client. It is not the mixed benchmark window or whole-GPU
utilization, and cannot establish whether GPU saturation caused the reported
stalls. Original fdinfo, frequency readings and identity/time bounds are saved.

The first notification control completes with original controller/VM exit zero
and four clean filesystem checks. It uses the separate `layer-focus-frozen`
build, with layer mode held enabled and the new focus invalidation fixes. One
original Quake demo stays running across all three notification arms.

| Metric | Wake on A | Wake off B | Wake on C |
| --- | ---: | ---: | ---: |
| SDL swaps/s in retained frame window | 39.42 | 37.99 | 30.06 |
| Compositor body median | 9.26 ms | 6.08 ms | 6.14 ms |
| Whole-QEMU CPU cores | 0.75 | 1.56 | 0.80 |
| Audio source frames/s | 44.14k | 44.12k | 44.14k |
| Completion waits | 10,226 | 0 | 8,118 |
| Notification samples | 9,012 | 0 | 6,932 |
| Recorded incomplete safety expiries | 198 | 0 | 161 |
| Mean notified-return latency | 11.55 us | unavailable | 11.42 us |

Completion counter deltas span each PCM/frame observation and its control RPCs;
the SDL windows use the validated retained compositor timestamp bounds. Trace
hashes are checked offline. Different scenes and short, unequal retained windows
prevent treating these as an equal-scene throughput result. Disabling interrupt
waits does not show a clear reversal and increases CPU cost; successfully
notified returns are usually quick. Unnotified/timer-delayed waits remain open,
particularly given the counter caveat above. One invalid latency sample is
retained in each enabled arm. This is not proof that all notification paths are
correct, nor an audio-under-contention test: Quake is alone here.

Evidence is `notification-layer2-on-gpu-1ms-fixed-mlfq-a-20261022`, notably
`notification-control/analysis.json`, raw per-arm completion snapshots, separate
PCM/frame windows and the single original application trace. The next diagnostic
should attribute the long between-swap intervals to the main thread's syscalls,
GPU lock waits and scheduled/runnable time, retaining host response versus guest
return timing where needed. No discarded upload or notification experiment
changes the default configuration.

### GPU timestamp capability and cube instrumentation

The slower layer-damage experiment is parked on
`wip/layer-damage-experiment-20261022` (`88875cf2`). The independent quiet-window
invalidation fix is retained on the baseline (`8de68d6c`). Rebuilding that ABI 34
baseline passes all 86 hosted suites. Native timestamp tests use the separate
`timestamp-baseline-frozen` capsule, kernel
`3d1c279d005174f4ffe0fe23837b75578570f1686ef15473f43821a73167f818`.

The opt-in `BROOK_VULKAN_TIMESTAMPS` correctness probe now retains GPU timestamps
around upload, compute and readback, CPU submit/completion brackets and optional
calibrated device/guest-monotonic samples with advertised maximum deviation.
Query reads follow the existing completion wait, use availability flags and add
no blocking query wait. Timestamp writes introduce pipeline dependencies, so the
measurements are instrumented intervals, not exclusive execution costs.

Both host Intel and native Venus return 36 valid timestamp bits with an
83.3333359 ns period and support calibrated guest monotonic timestamps. Native
default, timestamp and timestamp-plus-sync-fd modes all pass the existing
transfer/compute/readback and offscreen triangle correctness checks. The three
single-dispatch GPU intervals are 0.087–0.092 ms, while CPU submit-to-completion
return spans 0.765–8.324 ms. This establishes a working measurement mechanism,
not the cause of Quake/cube hitches. The first native calibration advertises
0.911 ms maximum deviation; its uncertainty must remain attached to estimates.
The sync-fd mode intentionally uses 256 dispatches rather than one, so it is
not a throughput comparison against ordinary mode.

Evidence is in `timestamp-capability-off-gpu-1ms-fixed-mlfq-a-20261022` under
`timestamp-control-b`. The original launcher and corrected controller exit zero
and four offline filesystem checks are clean; runtime sampling ends with zero
clamps/invalid samples/backwards clocks. The first `/bin/VKTIME` request failed
before creating a process; a retained path diagnostic identified
`/boot/BIN/VKTIME`, then separate tracked commands exercised the corrected path.

The separate `tools/vulkan-cube-timing-pkg` instruments the pinned Khronos cube
with a bounded two-slot query pool reused only after its existing frame fence.
It records CPU fence/acquire/prepare/submit/present intervals and GPU start/end
ticks without per-frame allocation/output or an additional blocking wait. Final
queries drain after the cube's existing idle wait. The trace fails analysis on
query errors, unavailable results, missing frames, overflow or reversed clocks.
Cube GPU values are currently durations only; the capability probe's clock
calibration is not automatically transferred to another process or device.

The first cube capture completes with 600/600 submitted, available query pairs,
zero errors and zero overflow. Both original control and instrumented processes,
the controller and VM launcher exit zero; all four offline filesystem checks
are clean. Raw CSV size and SHA-256 match guest metadata after extraction. The
runtime census ends with zero clamps, invalid samples or backwards clocks.

| Cube interval | Median | p95 | Maximum |
| --- | ---: | ---: | ---: |
| GPU start to end | 1.54 ms | 6.39 ms | 15.92 ms |
| CPU acquire call | 15.74 ms | 26.48 ms | 45.91 ms |
| CPU submit call | 0.029 ms | 12.08 ms | 31.91 ms |
| Present-return interval | 17.45 ms | 29.79 ms | 46.10 ms |

At the longest present-return interval (frame 105), acquire takes 45.91 ms while
the same frame's GPU interval is 0.76 ms. This directs the next investigation to
swapchain-image availability, Wayland callback/release timing and scheduling
delays, rather than assuming shader cost dominates that hitch. The cube uses
its default FIFO presentation: approximately one refresh of acquire waiting is
expected, so eliminating all acquire waits is not a correctness target. CPU
stages include descheduling, and GPU/CPU intervals overlap. Wayland's existing
aggregate trace records a 46 ms maximum callback request/commit delay on this
cube surface, but per-event correlation is still required for causal attribution.

Evidence is `cube-timestamps-off-gpu-1ms-fixed-mlfq-a-20261022` under
`cube-control/timestamps/{cube-timestamps.csv,analysis.json}`. The same separate
diagnostic binary runs once without timestamps and once with them; whole-app
host durations are 16.83 and 16.35 seconds, including startup/shutdown and
observer RPCs. These are not frame throughput measurements or a complete
instrumentation-overhead assessment. No scheduler defaults or buffer-release
ownership rules change.

### Bounded follow-up and NVIDIA side test

The user limits the next phase to completing GPU-side profiling and two focused
optimization attempts, preserving monthly budget for Steam. Remaining clock,
scheduler, mixed audio and ownership-recovery requirements stay open; this
budget boundary does not turn partial results into acceptance.

A bounded NVIDIA trial identifies a Quadro K1200 with 4 GB VRAM and driver
580.178.04 on `/dev/dri/renderD129`. Host hardware Vulkan correctness/timestamps
pass. Diskless QEMU EGL initialization first fails with implicit platform
discovery, then succeeds using explicit NVIDIA EGL/GBM manifests. A fresh private
Brook VM boots on that node, confirmed by actual QEMU descriptors and driver
mappings. Basic virgl self-tests pass, but the combined owned-capture/upload/scene
self-test fails and correctly prevents publishing 3D operations. The guest
Venus probe terminates with `VK_ERROR_INITIALIZATION_FAILED` at enumeration;
no cube or throughput comparison runs after that failure.

The original VM is shut down cleanly, and all four offline filesystem checks
pass. Its initial setup verifier also rejected the Nix wrapper executable's
name; recovery verifies the actual `.qemu-system-x86_64-wrapped` ELF inside the
exact intended package and retains the original VM, without relaunching. The
guest's completion-wake provider is unregistered because 3D was disabled. This
is a boot/compatibility finding, not a passing accelerated cohort. Evidence is
`nvidia-cube-off-gpu-1ms-fixed-mlfq-a-20261022`; further NVIDIA work is deferred
under RUN-03 in the backlog. Host configuration is unchanged.

### Linux solo/mixed reference, 22 October

A new private Linux cohort fills the missing Quake rendering-throughput
measurement. It uses the same 600 original demo frame records, normal demo
playback (`timedemo=0`), render caps lifted and actual SDL swap interval zero.
Each central window spans 50 guest seconds beginning five seconds after the
observed START marker. Both complete source traces pass quality checks; sizes
and SHA-256 match guest metadata. Boundary-crossing stalls remain explicit.

| Quake GL3, 1280×720 | Solo | With Firefox |
| --- | ---: | ---: |
| SDL swap completions/s | 121.06 | 98.70 |
| Frame interval median | 7.34 ms | 8.64 ms |
| Frame interval p95 | 17.99 ms | 22.83 ms |
| Maximum complete frame interval | 47.32 ms | 97.51 ms |

The Linux guest uses eight vCPUs/eight GiB, host affinity 0–15, the private
1-ms-fence-poll QEMU, Intel render node, 1-GiB hostmem and SDL/PipeWire private
44.1-kHz null output with a requested 20-ms callback buffer. The checked old
Linux disk is cloned, with its pinned Linux 6.18.26, Mesa 26.0.6 and Yamagi 8.60.
The diagnostic SDL trace has the same code with its fixed capacity enlarged
from 8,192 to 65,536 records; actual solo/mixed traces retain 8,143/6,599 swaps
without drops, incomplete records or invalid clocks. Demo elapsed time remains
separate from FPS; the solo observed demo interval is 63.62 seconds versus
nominal 60.2 seconds, and this test does not close the clock/pacing question.

Firefox decodes the identical generated 1280×720/30-fps H264/AAC clip. Its
software-vsync workaround under Weston remains explicit. Media/host progression
bounds are 0.99892–0.99970 before Quake, 0.99870–0.99947 during the observed mixed
window and 1.00162–1.00204 after Quake. Corresponding video callback opportunity
rates are about 24.44–24.46, 24.33–24.35 and 24.50–24.51/s. No sampled paused,
unplayable or ambiguous media-gap states occur in those windows. A separately
retained six-second HDA capture starts after Quake exits: all 600 complete
10-ms bins are nonzero. Its 0.727-core QEMU observation is also Firefox-only,
not a mixed CPU/audio result, despite the observer directory's original name.

This is a useful Linux target, not an exact Brook speed ratio. The fixture
displays video at CSS width 640 versus 1,280 in the Brook cohort, so page hashes
differ even though the decoded media is identical. Linux uses headless Weston
versus the current Brook native SDL window, and its guest compositor, audio,
filesystem, Mesa and scheduler differ. No Quake OST is staged. Before claiming
a fully matched mixed-GPU comparison, align the page display size and host
display path, repeat the run and retain mixed CPU/audio inside actual overlap.

Evidence is `build/linux-workload-reference-20261022/summary.json` and
`workload/{solo,mixed,firefox-*-media.json}`. Both original game processes and
Firefox exit zero. The original controller, VM, fixture and private audio
owners terminate; serial powerdown and read-only private ext4 checks pass.

### Phase-preserving Wayland timer experiment, first pair

The opt-in experiment on `wip/phase-frame-clock-experiment-20261022`
(`4d8612bb`) retains the existing 16-ms period and callback eligibility. It
advances late deadlines to the next future phase, skipping missed periods
without emitting catch-up callbacks. Both arms use the same rebuilt compositor;
only `--phase-frame-clock` differs. The scheduler, client audio geometry,
4,096-frame downstream allowance, kernel/modules and private QEMU are unchanged.

| Central 50-second Quake window | Timer off | Timer on |
| --- | ---: | ---: |
| First solo swaps/s | 22.82 | 44.46 |
| Mixed swaps/s | 13.74 | 25.64 |
| Final solo swaps/s | 25.90 | 46.30 |
| Mixed frame interval p95 | 136.53 ms | 60.66 ms |
| Mixed maximum complete frame interval | 534.93 ms | 610.04 ms |
| Mixed observation whole-QEMU CPU cores | 1.582–1.585 | 3.176–3.181 |

The six-second mixed audio observations still show approximately 33k Firefox
and 30–31k Quake completed source frames per guest second in both arms, versus
44.1k nominal. This pair does not establish an audio fix or better CPU efficiency.
The 440-Hz reference tone remains measurable at approximately 440.01 Hz; that
alone does not prove correct media tempo or absence of gaps in either stream.

Both original controllers and VM owners terminate successfully, all four
offline filesystem checks pass per arm, and extracted swap trace sizes/hashes
match guest metadata. Evidence is in
`build/reviews/phase-clock-{off,on}-off-gpu-1ms-fixed-mlfq-a-20261022/phase-clock-control`.
The actual original compositor binary matches Nix package
`6b0cwb49r4wrcqb6hql8qmd1hfvg28f6-waylandd-brook-0.1-brook`; derivation source
comparison finds only this experiment's changes. However, the new off arm is
substantially slower than the earlier 58-FPS baseline. A repeat off arm is
required before attributing the pair's difference to the timer. The experiment
is not promoted, and desktop/GL/Vulkan/input acceptance remains incomplete.

The subsequent unchanged off repeat (`phase-clock-off-off-gpu-1ms-fixed-mlfq-b-20261022`)
returns **43.76 / 25.06 / 44.52 swaps/s** for solo/mixed/solo. Its mixed p95
is 62.76 ms, close to the enabled arm's 60.66 ms. Both original owners exit
zero, all four filesystem checks pass, and the extracted traces match guest
sizes/hashes. Thus the apparent near-doubling in the first pair does **not**
survive the control repeat and cannot be credited to phase preservation. Stop
timer tuning here; retain the opt-in branch as an unproven experiment.

The repeat's mixed source audio rates are 30.65k Firefox / 28.14k Quake frames/s
and whole-QEMU CPU is 2.171–2.174 cores in the short observation. Mixed audio
and the 60-solo/40-mixed acceptance targets remain unmet. The next bounded
diagnostic uses existing QEMU `virtio_gpu_fence_ctrl` / `virtio_gpu_fence_resp`
trace events and separate guest CPU captures to distinguish host fence dwell
from guest completion-return delays. These are instrumented diagnostic windows,
not another throughput control or a direct measure of exclusive GPU busy time.

### Host fence diagnostic, 22 October

One separate instrumented cohort records existing QEMU fence events in short
solo and mixed windows, then captures guest CPU samples separately. Host logs
are size/hash checked, fence IDs are unique, paired spans do not overlap and
unpaired commands are confined to the enable/disable edges. QMP enables the two
events separately, so an edge can contain a submission without its response.

| Post-dispatch host fence span | Quake alone | With Firefox |
| --- | ---: | ---: |
| Measured trace span | 3.680 s | 3.478 s |
| Complete fence pairs | 1,110 | 1,179 |
| Fence span median | 1.416 ms | 1.211 ms |
| Fence span p95 | 7.969 ms | 7.126 ms |
| Fence span maximum | 20.157 ms | 20.225 ms |
| Summed paired span / observed trace span | 65.8% | 64.1% |

All paired commands are `SUBMIT_3D` (`0x207`). Source inspection matters here:
`virtio_gpu_fence_ctrl` is emitted **after** command dispatch and immediately
before fence insertion; `virtio_gpu_fence_resp` is emitted before publishing
the guest response. These spans exclude initial dispatch and guest wake/return,
and include GPU dependencies, QEMU polling and host scheduling. They are not
exclusive GPU execution times. Frame records retain only first/last fence IDs
and a count; other clients can interleave, so assigning every fence in that
numeric range to the compositor would overcount its work.

The solo CPU capture contains 2,370 samples and 20,113 context switches with
no reported producer drops; many CPU leaf samples are idle. The mixed capture
reports **39,367 dropped events** despite successful export/conversion. Retain
it as lossy evidence, not a precise CPU or scheduler breakdown. The exact QEMU
SDL source already requests `SDL_GL_SetSwapInterval(0)`; merely requesting zero
again is not a supported explanation for the variance.

Evidence is `gpu-host-fence-off-off-gpu-1ms-fixed-mlfq-a-20261022/gpu-host-fence-control`.
Both original application runs, Firefox, controller and VM terminate, and all
four offline filesystem checks pass. No synchronization/default changes result
from this diagnostic. A final small experiment can test finer host fence-poll
cadence against the existing 1-ms control, retaining guest ownership and timeout
rules and explicitly measuring additional host CPU cost.

### Final bounded attempt: finer host fence polling

The opt-in package on `wip/fine-fence-poll-experiment-20261022` (`c3b29529`)
uses a nanosecond-unit timer and a 250-us rearm interval. Legacy arguments still
evaluate to the exact existing 1-ms package. The candidate builds successfully;
disassembly confirms `qemu_clock_get_ns` followed by a 250,000-ns deadline
increment. This compares finer polling **and** deadline quantization with the
legacy millisecond-unit timer; it is not an isolated interval experiment.

| Central 50-second Quake window | Latest 1-ms control | 250-us candidate |
| --- | ---: | ---: |
| First solo swaps/s | 43.76 | 32.90 |
| Mixed swaps/s | 25.06 | 15.64 |
| Final solo swaps/s | 44.52 | 32.48 |
| Mixed frame interval p95 | 62.76 ms | 116.64 ms |
| Mixed maximum complete frame interval | 391.14 ms | 515.86 ms |
| Mixed observation QEMU CPU cores | 2.171–2.174 | 2.072–2.075 |

The candidate's mixed source audio rates remain 33.43k Firefox / 30.97k Quake
frames/s, short of nominal 44.1k. The lower short-window CPU observation is not
an efficiency win while presenting fewer frames. These are individual ordered
cohorts with established run variance; they provide no evidence for promoting
finer polling and do not prove it caused the entire difference. Stop here
rather than fitting further intervals to noisy results.

Post-benchmark Venus default and timestamp/sync-fd correctness probes pass with
driver ID 22. A separate cube completes 1,800 frames with no trace errors or
overflow; extracted CSV size/hash match guest metadata. Its retained completed
frame shows the textured cube and desktop without visible corruption. That
screenshot briefly pauses the VM, so this cube run is a correctness/visual
check, not a comparable pacing benchmark or input-responsiveness test. Final
heap integrity passes; runtime counters show zero invalid/backwards samples,
service clamps and stale identities. All original jobs/controller/VM terminate
and all four offline filesystem checks pass.

Evidence is `fine-fence-off-off-gpu-250us-fixed-mlfq-a-20261022/fine-fence-control`.
Neither bounded experiment is promoted. The 60-FPS solo / 40-FPS mixed targets,
mixed audio continuity, broader desktop/GL/Vulkan/input acceptance and unresolved
clock/scheduler findings remain open. Further work needs stronger attribution
of interleaved client commands and guest wait/run delays before choosing a
larger submission/completion redesign; the present evidence does not justify
dropping a fence or changing ownership/timeout rules.
