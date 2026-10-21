# Native downstream audio staging experiment

Status: default off; original-buffer Quake diagnostics, not audio acceptance.
The application geometry, HDA write lead and scheduler defaults are unchanged.

## Ownership and accounting

The client FIFO and an accepted native device copy own different storage. A
bounded downstream allowance can release client capacity after an accepted copy
without claiming that those samples have played. For client capacity `C`,
software samples `S`, native in-flight source `I`, and downstream allowance `D`:

```
charged = S + max(I - D, 0)
available = C - charged
S <= C; charged <= C; S + I <= C + D
```

Invalid states fault the stream instead of underflowing into apparent writable
space. Rejected native submissions grant no credit; partial submissions grant
only accepted source credit. Completion, GETOPTR, GETODELAY and drain continue
to follow actual source receipts. Delay can therefore exceed the client FIFO
size. The native driver still validates safe physical writes; its 12-KiB lead
is unchanged. This does not add independent receipt epoch/overlap validation.

`audio staging on|off|1024|4096|status` controls future native capacity
negotiations. The boot default is off with a 1024-frame configured limit.
Numeric commands select the limit and enable the experiment; `on` enables the
current limit. Negotiated staging is capped by client capacity. Changes do not
shrink active streams. Reset preserves their geometry; empty renegotiation can
adopt a new limit. This is an explicit downstream frame budget, independent of
OSS fragment/readiness size.

The original 1024-frame-only prototype capped the allowance by fragment size.
The extended experiment makes the budget explicit; both definitions give
1024 frames for the Quake 4096/1024 configuration. Neither has been accepted
for Firefox. Its ALSA plugin derives logical availability from physical
completion and rejects a queue larger than its logical ring. Kernel capacity
alone cannot make that logical buffer reusable sooner.

## Hosted checks

Actual mixer/HDA bodies cover rejected/partial copies, exact finite drain,
pause/resume with another stream playing, reset during copy, close/reuse during
copy, ambiguous DMA sampling faults, and poll/epoll readiness thresholds when
outstanding source exceeds client capacity. The 4096-frame extension checks
8192 total source frames against a 4096 client FIFO, exact drain, immutable
active limits, invalid configuration, and small-capacity clamping.

The initial new sample-gap fixture failed because DMA had not started. Starting
the synthetic DMA before injecting the ambiguous interval exercises the real
fault path; the corrected audio suite passes. This was a fixture correction,
not a relaxation of discontinuity handling.

## First native off/on/off comparison

Inputs: original installed Quake OpenAL wrapper, 44100-Hz stereo S16,
4096-frame client capacity, 1024-frame fragment/readiness, native GL, eight-vCPU
MLFQ experiment, private 1-ms fence-poll QEMU, SDL physical audio with a requested
20-ms buffer, unchanged HDA safety lead. Three fresh tracked Quake launches use
one VM, with diagnostic probes off for the plain intervals. No application
configuration override or extra manual Quake instance is used.

| Downstream allowance | Plain source frames/s | Exact-zero spans in ~6-s PCM | HDA reservations in diagnostic interval | Reserved bytes |
| --- | ---: | ---: | ---: | ---: |
| Off, first | 33,493 | 1.429 s | 166 | 495,616 |
| 1024 frames | 39,925 | 0.549 s | 102 | 178,432 |
| Off, last | 33,917 | 1.428 s | 161 | 478,976 |

The source-counter brackets include bridge calls and are wider than their PCM
captures. Exact-zero spans are stereo zeros lasting at least 32 frames, not an
independent underrun detector. Content silence is possible; the separate HDA
reservation trace supports starvation without establishing exact sample-time
alignment. There are no physical-speaker timestamps or measured tempo claims.
The on-case diagnostic has zero unsafe/invalid delivery records, maximum mixer
sleep 11.89 ms, maximum service 7.14 ms, and producer gaps up to 54.97 ms. All
440 recorded writes accept a full 4096 bytes with no backpressure sleeps.

**The strict runtime gate rejects this cohort:** one scheduler service clamp
of 16,182 ns occurred on idle CPU2 (sample 2,568,213). Keep the audio metrics as
observations; do not promote them to clean performance acceptance. All three
original app groups and VM owner terminate successfully; heap integrity and
four offline filesystem checks pass. Those lifecycle results do not override
the runtime gate or remaining audible gaps.

Raw private evidence: ignored
`build/reviews/unattended-audio-stage-gpu-1ms-fixed-mlfq-a-20261022/`.
The analysis retains `strict_runtime_gate_pass=false` and exits unsuccessfully
for the rejected counter. Frozen binaries/source and hosted logs are under
`build/workloads/timing-scheduler-20261021/audio-native-stage-frozen/` and
`reboot-resume/audio-native-stage-*`.

The 1024-frame allowance provides about 23.2 ms at44100 Hz; 4096 frames provide
about 92.9 ms. These are additional maximum outstanding source, not guaranteed
end-to-end latency. The next comparison holds application geometry constant
and selects 1024/4096/1024. Event-driven publication/readiness remains a
separate follow-up: current writes enqueue source, the mixer services on a
nominal 10-ms loop, and DSP poll/epoll inspect cached progress with a 5-ms
fallback. A durable producer wake must preserve wake-before-block and nested
wait ownership and must not let a greedy writer monopolize mixed publication.

## Full 4096-frame allowance and Firefox preparation

A second native cohort compares 1024/4096/1024 with the same client4096/1024
geometry and host configuration. The original app requests all exit0, the VM
owner exits0 and all four filesystem checks pass.

| Downstream frames | Plain source frames/s | Exact-zero spans in ~6-s PCM | Diagnostic reservations | Reserved bytes |
| --- | ---: | ---: | ---: | ---: |
| 1024, first | 40,201 | 0.557 s | 102 | 178,432 |
| 4096 | 44,155 | 0 | 0 | 0 |
| 1024, last | 40,183 | 0.556 s | 103 | 178,432 |

The4096 diagnostic interval averages44099.8 source frames/s; all three PCM
captures have no stereo exact-zero spans of32frames or longer. The user heard
no stutters in the second run and returning stutters in the third. This is
stronger Quake evidence than a start-only or synthetic test, but not general
mixed-workload acceptance. The strict runtime gate again rejects the cohort,
this time for two service clamps. They remain separate scheduler findings.
Private evidence is under
`build/reviews/unattended-audio-stage4096-gpu-1ms-fixed-mlfq-a-20261022/`.

Startup configuration is available through QEMU
`-fw_cfg name=opt/audio-staging-frames,string=4096` (0 disables;1–4096 accepted).
Malformed or truncated values leave the boot default off. Runtime changes
remain prospective. The frame budget is explicit; a default has not changed.

The optional ALSA package parameter `downstreamStaging = true` consumes
`BROOK_DSP_GETQUEUE`, a versioned56-byte native44.1-kHz stereoS16 snapshot in
[src/shared/audio_queue.h](../src/shared/audio_queue.h). It returns one coherent
epoch, submitted/completed source counts, client capacity, available capacity,
full queued source, and frozen downstream allowance. Other formats/rates return
unsupported; existing GETOPTR keeps its physical-completion meaning.

The plugin derives reusable logical occupancy from capacity-minus-available,
while delay reports all physically unfinished source. It validates native units,
geometry, epoch, counter conservation and downstream bounds. A software-only
origin reset requires real queued source to be zero, including downstream data.
This is a serialized private-descriptor contract, not detection of arbitrary
aliased writes or independent kernel-counter regression. The existing default
plugin path remains unchanged.

Both default and opt-in package builds pass. The opt-in uses the original64-frame
logical minimum, passes30 conversion negotiations and32 real-libasound sustained
playback cases, including logical4410/client4416/downstream4096, early reuse,
delay above the logical ring, downstream-only occupancy and exact physical drain.
The kernel build and86 suites pass. Hosted immediate-copy models do not replace
native acceptance; a separate interval with held software/unaccepted downstream
source is a follow-up to the existing actual-HDA rejected-copy tests.

A private Firefox live test is open using that module and startup4096 allowance,
with no logical-buffer enlargement or installed-config replacement. Evidence is
under `build/reviews/live-firefox-downstream4096-gpu-1ms-fixed-mlfq-a-20261022/`.
At handoff no media stream was open yet. Actual playback geometry, callback
progress, physical delay/drain and user feedback remain required before claiming
Firefox audio fixed. Keep the original guest request and VM owner until handback.

The live Firefox user subsequently reports smooth, correct tempo, then qualifies
that as occasional stutters with a substantial improvement. Its observed
client geometry remains4416/64 with4096 downstream frames. One initial capture
crosses stream49/epoch7 to stream65/epoch10 and is explicitly rejected for
source-rate subtraction. A later stable interval delivers43953.5 source
frames/s and has23.6ms of long exact-zero spans in6.06s of PCM. A separate
10.02-s diagnostic capture has five such spans totaling167.4ms, nine HDA silence
reservations (45,736bytes), no discontinuity/invalid/unsafe delivery flags, and
a producer gap up to115.9ms. Its wider counter bracket averages43374.9frames/s.
Writes have no backpressure; maximum write body1.02ms, mixer sleep15.58ms and
service15.10ms. These observations retain the remaining occasional gaps;
neither larger staging nor the new bridge is claimed to finish audio acceptance.
Probes are restored off and the user's original Firefox VM is left running.

On user request this milestone is being integrated into main. Scheduler defaults,
startup staging default(off) and default ALSA package selection remain unchanged;
the tested downstream bridge is an explicit package option until mixed-workload
and lifecycle acceptance is complete. The incomplete GPU delayed-vertex recovery
remains on its separate WIP branch, not part of the milestone integration.
