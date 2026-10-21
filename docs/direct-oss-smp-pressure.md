# Direct OSS under host contention: eight/one/eight vCPUs

The [MLFQ Quake comparison](mlfq-host-pressure.md) freezes rendering despite
avoiding the native audio fault with AP try accounting. A subsequent direct
OSS writer also loses audio progress **without Quake running**. A one-vCPU
control improves playback substantially, and returning to eight vCPUs degrades
it again. This supports an SMP-dependent contribution; it does not identify
a particular lock or justify changing scheduler defaults.

The subsequent [timer lock-site capture](irq-ready-lock-sites.md) retains
initial-accounting waits during poor progress, but needs a finer context label
to distinguish AP idle from AP user execution. It does not justify changing
idle-dispatch admission.

## Conditions and checks

Three fresh private boots use the same frozen kernel and thirteen modules as
the Quake comparison: kernel SHA-256
`fc41f3dbf97797d1f349754303090fee0b6e14976f8350ae717e1636a104d8bb`,
original 12-KiB HDA lead, QEMU 10.2.2, physical PipeWire output, callback HDA
with a requested 20-ms host buffer, 8 GiB, MLFQ and explicit AP try mode.
There is no new kernel, history recorder, SDL observer or CPU profiler.
Mixer/delivery observations and the bounded host observer remain common.

The writer generates forty seconds of packet-coded 44.1-kHz stereo s16 audio
before opening the device, negotiates four 4,096-byte OSS fragments, and warms
up for ten seconds before host pressure. All vCPUs and eight finite host CPU
workers share CPUs 0–7; two named resetting I/O helpers may use 0–15. The host
workers run for twenty seconds. The desktop compositor remains present: this
is **without Quake rendering**, not a GPU-inactive experiment.

For every cohort, the entire six-second WAV capture and both complete
audio-query host brackets lie inside all workers' measured execution intervals.
The guest queries also lie inside the finite source's lifetime. These gates
use actual timestamps, rather than the worker's pre-start `READY` marker.

## Results

| Cohort, in execution order | Loaded nominal completion / guest second | Forty-second source drain time | Maximum mixer lateness | Stored PCM / capture host duration | Aggregate vCPU host execution over roughly 19.9 s |
| --- | ---: | ---: | ---: | ---: | ---: |
| Eight vCPUs a | 0 | 59.364 s | 179.00 ms | 0 / 6.006 s | 13.58 CPU-s |
| One vCPU a | 0.8898 | 43.780 s | 31.53 ms | 6.068 / 6.082 s | 5.96 CPU-s |
| Eight vCPUs b | 0.2302 | 57.516 s | 152.99 ms | 2.174 / 6.083 s | 6.74 CPU-s |

The completion ratios use native application-frame counters and separately
sampled guest clocks. The host/guest clock brackets are broad under pressure
and remain consistent with unit rate; they do not support a new calibration
or acceleration claim. Mixer maxima cover the recording interval from reset
before pressure through collection afterward. Host execution includes host
work on each QMP-identified vCPU thread, not only guest instructions.

Eight-vCPU a makes no completion progress during the measured 6.584 guest
seconds, with no stored WAV frames. Its source observations have consecutive
gaps of 7.914, 6.190 and 5.456 seconds around the largest delays. Those brackets
span partial writes, the subsequent progress query and scheduling; the existing
report retains only the last partial write, so they cannot be attributed to
one syscall. Each vCPU accumulates about 16.5–18.7 seconds of runnable host
wait during the roughly 19.9-second sampled load interval. The sum of sampled
host vCPU slices is approximately 2.15 million. These observations are not a
guest-instruction profile or proof of a particular host yield mechanism.

The empty first capture is rejected as useful playback and cannot be decoded.
QEMU WAV can omit inactive output, so its absence is not a measured frequency
or physical speaker-clock rate. The one-vCPU capture decodes at approximately
0.8939 source seconds per stored PCM second, with fitted pitch near 1.0000,
about 9.0% quiet 1-ms bins, and no detected backward/repeated transitions or
unobserved packet IDs between decoded runs. The eight-vCPU reversal decodes
at approximately 0.7504 source seconds per stored PCM second, with pitch near
1.0000 and about 31.1% quiet bins. It stores only 2.174 seconds of PCM during
the 6.083-second host capture. Packet estimates have 100-ms source
quantization, incomplete tone-window coverage and no physical-output timing.
They are not general continuity acceptance or a repair of historical speedup.

All sources eventually submit and complete exactly **1,764,000 frames** and
leave no stream behind. No native HDA fault or discontinuity is recorded.
All three cumulative runtime checks occur after source exit and report export;
invalid/backwards/clamped/stale and saturation counters are zero. Policy service
plus inactive service equals ledger service with no saturation. IRQ IF-error
and stale-ticket-wait counters, mixer/delivery quality and heap checks are clean.
All original source/export jobs satisfy identity, EOF and empty-group checks.
All three original VM owners exit zero, and all twelve filesystem checks pass.
These safety/lifecycle results do not turn poor loaded playback into a pass.

## Interpretation and next diagnostic

Quake rendering is unnecessary for the failure. Reducing vCPUs changes host
demand, timer distribution, dispatch opportunities and inter-CPU locking at
once. The eight/one/eight result therefore supports an SMP-dependent
contribution, **not specifically idle accounting or one ready-lock site**.
The eight-vCPU repeat remains poor but has some progress, retaining variability
rather than claiming an identical deterministic stall.

The timer source has at least two relevant ready-lock acquisitions: initial
runtime accounting and the later idle dispatch path. AP try currently excludes
idle initial accounting, and idle dispatch acquires the lock again. Widening
only accounting admission could simply move the convoy. Distinguish those
sites in the existing bounded lock trace before attempting an idle-dispatch
admission change, which would need separate retry/kick and ownership proofs.
Do not add HLT, change ticket-wait behaviour or silently broaden AP try here.

The next diagnostic build names these acquisitions `irq-accounting-blocking`
and `irq-idle-dispatch`; successful initial try already uses
`irq-accounting-try`. Previously both blocking sites defaulted to the function
name `SchedulerTimerTick`. Only caller labels change: no new clock read,
recorder, admission decision or lock operation is introduced. Recording stays
off by default. This kernel builds through both symbol-table link passes,
and its regenerated table matches the final ELF; all thirteen preserved modules
resolve against 109 kernel exports. The ignored `irq-sites-frozen` capsule
retains those modules and changes only kernel/symbol-table artifacts, with
kernel SHA-256 `34ac8f8a4b3bf1565455d2c69e5a15bde7ff5a240856282c7194704cf26d01ab`.
It also includes the earlier default-off history implementation, so reproduce
the direct-OSS failure with both recorders off on this capsule before using
its enabled lock trace for attribution. Native site attribution remains pending.

Ignored evidence is retained under
`build/reviews/unattended-mlfq-direct-oss-try-{a,b}-20261021` and
`build/reviews/unattended-mlfq-direct-oss-singlecpu-try-a-20261021`.
Each contains the executed controller, source, complete job evidence, PCM,
load containment, final gates and original-owner/filesystem receipts. The
aggregate is `build/workloads/timing-scheduler-20261021/direct-oss-pressure-summary.json`;
its checked analyzer is `reboot-resume/analyze-direct-oss-pressure.py` there.
This study leaves the [earlier accounting failures](runtime-clamp-evidence.md),
historical acceleration and broader scheduler/HDA acceptance open.
