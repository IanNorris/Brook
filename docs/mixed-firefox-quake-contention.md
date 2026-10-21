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
