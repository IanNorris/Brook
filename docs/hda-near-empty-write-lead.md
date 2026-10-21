# HDA near-empty write publication

**21 October affinity correction:** the first loaded-control scripts changed
only QEMU's main-thread CPU mask after launch. They did not constrain existing
vCPU/audio/renderer threads. A live QMP/host audit of the nominal eight-CPU
pressure run confirms all eight KVM vCPU threads still allowed CPUs 0–15.
Earlier host workers were bounded and pinned, but VM CPU reservations and
fully shared contention were not established. Keep their functional/audio
results; do not use them as proof of a pinned contention margin. A corrected
launch-time-affinity pair and per-thread verification are recorded below.


The [mixer timing baseline](audio-mixer-service-timing.md) reproduces a native
write-publication hazard. The device has consumed all but 744 bytes of the
submitted audio when a new 3,072-byte write begins. Its position then advances
1,024 bytes during an approximately 49-microsecond bracket. The unchanged
publication guard correctly refuses the write: the device has already passed
its beginning. Only publication check bit 64 fails. Clock reversal, stale
position sampling and half-ring ambiguity do not cause this particular fault.

Previously the driver reserved a conservative write lead only when no audio
remained pending. A short pending tail could therefore expose the next receipt
to a burst fetch. The candidate extends that reservation to any running stream
with less than the existing lead pending. It preserves the owned tail, clears
only the unowned interval after it, and places the new receipt beyond that gap.
The lead remains 12,288 bytes, approximately 69.66 ms at 44.1-kHz stereo s16.
No publication safety predicate, ambiguity limit, fault watermark or restart
rule is relaxed. Application completion is still reconciled by receipt ranges;
playing unowned silence cannot credit application frames.

This can insert silence and increase latency when a producer arrives near
empty. Safety tests alone cannot establish acceptable playback pacing or
continuous audio. It does not fix a producer or mixer that fails to run on time.

## Deterministic checks

Production-driver hosted tests recreate the exact 744-byte tail and 1,024-byte
fetch in a 49-microsecond bracket, including a write crossing the physical ring
boundary. They verify that the old tail remains intact, the gap is silence,
the new receipt is safely ahead, and proven completion remains unchanged by
reservation. A larger injected fetch still triggers the publication guard.
Additional checks cover invalid reservations, a faulted clock, full-ring
ambiguity, old/new stream epochs and lack of application credit for the gap.
All 64 host suites pass. The native driver build resolves all thirteen modules
against 106 kernel exports.

## Browser controls

The baseline and candidate use one byte-identical kernel and twelve common
modules. Only `intel_hda.mod` changes. Fresh private eight-vCPU/six-GiB images
run Firefox's generated local video/audio fixture plus twenty-four CPU workers
for thirty seconds. Audio uses callback HDA, requested 20-ms buffering and an
isolated PipeWire null sink. The mixer timing probe is enabled in all these
diagnostic runs; CPU profiling and mapping counters are off.

| Candidate run | Policy | Video callbacks/s | Periodic sleeper p95 | Maximum mixer late lower bound | Native HDA fault |
| --- | --- | ---: | ---: | ---: | --- |
| wl-e1 | EEVDF | 4.95 | 3.82 ms | 225.67 ms | sample-gap |
| wl-m1 | MLFQ | 6.88 | 21.15 ms | 116.23 ms | none |
| wl-e2 | EEVDF | 4.23 | 3.06 ms | 126.99 ms | none |

Video callbacks use the buffered page trace wholly within conservative load
bounds, rather than sparse HTTP observations. Media progression is only
0.806–0.838 source seconds per page second. These are diagnostic controls, not
a statistically established performance improvement or default-policy gate.
No candidate reproduces the short publication fault, but three runs cannot
establish its universal absence.

The `wl-e1` sample gap is 217.210 ms, beyond the unchanged approximately
185.760-ms observation limit. Its retained mixer event covers the fault:
deadline setup at 59.843303392 s, resume at 60.078971777 s, service end at
60.078972414 s. The mixer is approximately 225.668 ms late and then returns
after 637 ns. This establishes a delayed-resumption episode rather than a
long service bracket. It does not separate blocked timer delivery, ready queue
wait, post-dispatch return or host delay. Other application paths can also
sample HDA while the mixer sleeps. This run remains unaccepted as clean audio.

All three original launch wrappers and audio routers exit zero. Runtime
accounting has zero invalid/backwards/clamped/stale/saturated counters, with
all eight paravirtual-clock and steal records active. All four filesystem
checks and twelve-document/four-profile preservation checks pass per run.
Mixer probe reports have zero invalid clocks, zero contended records and no
aggregate saturation. Smaller slow episodes are omitted from the top sixteen
and explicitly counted, so the retained list is not a complete timeline.

Build isolation, controller receipts and verified results are retained under
the ignored `build/workloads/timing-scheduler-20261020/audio-write-lead-*`
directories. The common kernel SHA-256 is
`8937859141ca14387045eaf9e2ee1077b2b2bc72a69c1cd79b280935bb5aac63`;
the candidate HDA module SHA-256 is
`d2aa17f166d69e8cae49ca3e36ccf24ecbb47ee12ee071cf8884ef6b6f969a5d`.
Scheduler selection/defaults and the driver ABI remain unchanged.

## Packet-coded pacing controls

Two more fresh EEVDF boots compare the baseline and candidate with mixer timing
disabled. Each plays a generated thirty-second sequence of uniquely permuted
100-ms tones, first idle and then with twenty-four CPU workers. Completion is
drained explicitly; captures cover eight host seconds per condition.

| Driver | CPU workers | Full completion, guest seconds | Captured PCM / host time | Decoded source / PCM time |
| --- | ---: | ---: | ---: | ---: |
| baseline | 0 | 29.9963 | 0.99993 | 1.00018 |
| baseline | 24 | 30.0181 | 1.00017 | 1.00018 |
| candidate | 0 | 29.9934 | 0.99979 | 1.00014 |
| candidate | 24 | 30.0211 | 0.99979 | 1.00054 |

Every probe completes exactly 1,323,000 application frames. All four decoded
captures have pitch scale approximately 1.0000, no missing IDs between retained
symbol runs, no backwards/repeated transitions and no fully zero ten-ms bins.
Neither boot logs a native HDA fault. The loaded capture windows are wholly
inside conservative CPU-load bounds translated from live host/guest clock
brackets. That translation assumes the previously verified 1x stable clock;
the separate short capture brackets constrain its measured rate only to
approximately 0.944–1.041, not to audio-sample precision.

Both original launchers and audio routers exit zero, all runtime error counters
remain zero, idle output is silent, and all filesystem/preservation gates pass.
Large guest result files are extracted read-only after shutdown and checked
against their guest-reported sizes and SHA-256 hashes. Evidence and the
verification script are retained in the ignored `audio-write-lead-packet-controls`
directory.

These controls find no gross pacing regression in the candidate and support
retaining the targeted safety fix. The decoding rate is approximate because
symbols are quantized to 100 ms. An eight-second capture does not establish
continuity across every sample of the full thirty-second output or all browser
workloads. In particular, the browser sample-gap failure above remains open.

The next diagnostic should distinguish the audio worker's deadline-to-ready,
ready-to-dispatch and dispatch-to-resume intervals, alongside BSP timer arrival
and host steal. The current timer path checks blocked deadlines on the BSP
only. That is a source observation and a measurement target, not a proven cause
of the captured stall or justification for changing timer ownership without
SMP safety tests.


## Isolated write-distance experiment, 21 October

The postreboot physical-output Quake controls in
[scheduler attribution](scheduler-lock-attribution.md) reproduce roughly
0.84x application completion and measure about 0.95 seconds of reserved
silence alongside 0.94 seconds of missing application progression. Read
windows are not exactly aligned; outstanding application receipts and unplayed
reserved gaps also matter for exact conservation. This supports testing the
write distance separately from the codec's stop trailer.

An isolated module experiment names the write distance independently and
compares 12,288 with 8,192 bytes, retaining the 8,192-byte stop trailer. The
main branch's driver and default remain unchanged. Matched modules use the
same compiler/worktree, byte-identical kernel and other modules, private
images and physical speaker route. Alternating 12/8/8/12-KiB controls are
complete with the results below; bounded synthetic contention checks are also
complete, while broader application acceptance remains pending.

The production-driver hosted tests now cover pending tail at lead minus four,
exactly lead, and lead plus four. They also sample raw DMA positions one to
three bytes beyond aligned completion, then inject fetches ending exactly at
the receipt and one byte beyond. Exact-boundary acceptance and beyond-boundary
fault/unchanged proven completion are checked. Existing near-empty wrapped
burst, larger fetch, ambiguous sample gap, ownership, epoch and tail-drain
cases remain. Diagnostic reservation/headroom expectations use the selected
lead minus four instead of hard-coding 12,284. Both lead settings pass these
actual-driver tests and the timing-model tests.

Astra's independent review supports this default-off experiment and emphasizes
that the post-copy guard is **detection and containment**: copied bytes may
already have escaped before detection. Preserving it retains conservative
completion and fault handling, not a universal absence-of-corruption proof.
Neither observed maximum DMA advance nor finite host-contention tests bound
future host preemption. No smaller default is accepted without the planned
repeated application, packet-code, queue-geometry, drain/restart and bounded
contention controls.


The four matched physical-output attract controls return to the slow state
when switching back to the original lead:

| Run | Nominal application completion / guest elapsed | Reserved silence bytes | Nominal silence seconds |
| --- | ---: | ---: | ---: |
| 12k-a | 0.83806 | 173248 | 0.98213 |
| 8k-b | 0.99838 | 248 | 0.00141 |
| 8k-c | 1.00001 | 0 | 0.00000 |
| 12k-d | 0.84325 | 184832 | 1.04780 |

All four have zero unsafe publications, invalid clock/position samples,
diagnostic contention or saturation. Their bounded profiles report zero drops.
Each original guest job exits zero with complete output and no group tasks;
its window closes gracefully, original VM launcher exits zero, and all four
offline filesystem checks pass. These short clock/counter controls support a
causal reservation effect in Quake; they are not independently decoded source
progression or a universal safety bound. No smaller production default is
accepted yet. Synthetic packet-code, finite drains, queue geometry and bounded
CPU contention are the next gates.

The frozen pair and complete evidence remain ignored under
`build/workloads/timing-scheduler-20261021/hda-lead-{12k,8k}-frozen` and
`hda-lead-cohort`. Main-branch changes at this stage are focused regression
tests and this evidence log; the experimental driver lives in an isolated
worktree and private capsules.


### Bounded packet-code load controls

Both frozen leads completed sequential four- and eight-fragment OSS writers
(4,096 bytes per fragment), each submitting and completing exactly 882,000
frames at 44.1 kHz. Each run overlapped eight host CPU workers and four sleepers
for twenty seconds on CPUs 0–13. Only the VM main thread was restricted
to those CPUs; existing vCPU threads were not pinned by that call. Thus no
VM-wide reservation or fully shared CPU contention was established. This
remains a finite functional load test, not a worst-case DMA guarantee.

| Lead | Fragments | Guest delivery seconds | Reserved bytes | Decoded source seconds / stored PCM second |
| --- | ---: | ---: | ---: | ---: |
| 8 KiB | 4 | 19.95581 | 0 | 1.00009 |
| 8 KiB | 8 | 19.99841 | 0 | 1.00009 |
| 12 KiB | 4 | 20.00258 | 8204 | 0.99894 |
| 12 KiB | 8 | 19.99542 | 496 | 1.00014 |

The eight-second PCM excerpts all decode with approximately unchanged pitch,
no missing packet IDs between decoded runs, and no backwards or repeated
transitions. The 8-KiB excerpts have no quiet approximately one-millisecond
bins. The original 12-KiB four-fragment excerpt has six quiet bins out of
8,026 (longest two consecutive); the eight-fragment excerpt has none. These
are stored PCM measurements, not direct physical speaker timing, and do not
cover the entire twenty-second writers.

All four controls have clean delivery diagnostics and runtime clock/accounting
quality counters, successful finite writer drains, successful original job and
host-load exits, and clean heap checks. Both original VM owners terminate zero
and all four filesystem checks per VM pass. The first 8-KiB report read exceeded
the bridge stdout retention window after the writer had completed; its failed
read receipt remains preserved. A read-only, size/hash-verified HTTP transfer
recovered the original report without rerunning that writer. Later reports use
the same bounded file-transfer method.

The synthetic writer does **not** reproduce Quake's large slowdown with the
original lead: both settings deliver approximately real time here. Its value
is an independent load/drain/queue-geometry check of the candidate, alongside
the alternating Quake reproduction. Broader browser and application restart
acceptance remains required before changing the production default. Full
receipts and decoded excerpts are indexed by the ignored
`build/workloads/timing-scheduler-20261021/hda-lead-loaded-summary.json`.


### Physical-route Firefox playback and restart controls

Fresh private eight-vCPU/eight-GiB VMs compare the same two frozen lead modules
with current callback HDA and 20-ms requested PipeWire buffering. Each uses
the same generated local 120-second 640×360 H.264/AAC fixture, a fresh Firefox
profile, an idle eight-second audio capture, a twenty-second host load with
eight CPU workers/four sleepers and an overlapping eight-second capture, then
a graceful browser exit and second fresh-profile launch/idle capture.
No CPU profiler or screenshots run during timing; delivery diagnostics are on.

| Lead | Phase | Completion / guest elapsed | Quiet approximately 1-ms bins | Reserved bytes |
| --- | --- | ---: | ---: | ---: |
| 8 KiB | First idle | 0.99772 | 19 / 8026 | 3608 |
| 8 KiB | Host load | 0.99145 | 51 / 8013 | 9544 |
| 8 KiB | Relaunch idle | 0.99785 | 15 / 8026 | 2840 |
| 12 KiB | First idle | 0.98163 | 148 / 8021 | 27864 |
| 12 KiB | Host load | 0.96595 | 256 / 8013 | 49624 |
| 12 KiB | Relaunch idle | 0.97596 | 178 / 8023 | 46736 |

All six captures retain stable application stream epochs and zero discontinuity,
invalid clock/position, unsafe publication, diagnostic contention or saturation.
Quiet-bin counts come from stored PCM RMS, with a threshold of 10 in signed
16-bit samples, not speaker listening or proof of no missing delivery intervals.
Capture host duration and stored frame counts remain available separately.
These results show substantially fewer short quiet gaps; they are one matched
pair, not statistical confidence or proof of all browser audio behavior.

After the first three media seconds, buffered trace video callbacks average
11.23/10.94 per page second across the candidate's two launches versus
10.64/10.03 for the original. Those whole-trace intervals mix idle/load/recovery
and many frames are still dropped. They are not physical presentation rates
or a smooth-browser acceptance claim. Broader graphics scheduling work remains.

The candidate callback run has clean cumulative runtime/accounting counters.
The original callback run records a 13,246-ns wall-minus-steal correction during
startup, so its cumulative runtime gate remains failed. A separate candidate
historical timer/200-ms cohort records a 280-ns correction and stops before
relaunch. Preserve those failures as described in
[runtime clamp evidence](runtime-clamp-evidence.md); do not infer either cause
or benignity from their size. The completed callback cohorts retain successful
original browser-group exits, heap checks, original VM-owner exits and all
four filesystem checks. These lifecycle gates do not override failed runtime
acceptance.

The ignored `hda-lead-browser-summary.json` indexes the full trace/capture
receipts under `unattended-hda-{8k,12k}-browser-callback-20261021`. A final
matched Quake attract pair with current audio defaults and bounded host load
combines the actual small-queue failure trigger with contention before a
production lead decision.


### Loaded Quake gate fails: production default retained

The final current-callback/20-ms pair runs the default GL3 attract launcher with
eight host CPU workers and four sleepers for twenty seconds. The original
12-KiB module again completes application audio at 0.84356 of guest elapsed
with 183,552 reserved bytes in 149 reservations. Runtime and HDA diagnostic
quality counters are clean, with no stream discontinuity.

The 8-KiB candidate instead records a native `sample-gap` fault and stream
discontinuity. Last observation is 64,662,401,701 ns and the next read bracket
is 65,212,274,036–65,212,302,082 ns: approximately 549.900 ms. This exceeds the
185.76-ms observation limit and even the roughly 371.52-ms full-ring duration.
The guard correctly refuses ambiguous completion. Its measured 0.90434
completion ratio crosses that discontinuity and is **not** an accepted tempo
comparison. There are no silence reservations or recorded unsafe/invalid
publications; the guard can fail before copy diagnostics are recorded.
Observed maximum advance is 1,024 bytes and minimum publication headroom
8,192 bytes. Those values do not explain or bound the missed observation.

Actual host worker timestamps overlap 5.662 seconds of each approximately
6.007-second PCM capture. `READY` precedes the workers' scheduled start, so
the beginning of each capture predates CPU work. This is overlapping bounded
load, not an entirely stressed capture window. The exact timings and full
worker progress are saved. Both runtime ledgers, heaps, original app groups,
host loads, original VM owners and all filesystem checks pass; they do not
override the candidate's failed audio gate.

The lead only changes reservation threshold/target; it does not change the
mixer's nominal ten-millisecond deadline, unconditional driver service,
idle trailer or observation limit. A timing/load feedback effect remains
possible. One passing original and one failed candidate do not establish
which mechanism caused the missed observation. Astra's follow-up recommends
one matched diagnostic pair with the existing audio-service timing probe,
aligning sleep/deadline/wake/service-end events to the fault interval. No
sampling or publication guard is weakened. The production lead remains
12,288 bytes pending that diagnosis.

Full original receipts, serial fault and load timestamps remain under
`unattended-hda-{12k,8k}-q2-loaded-callback-20261021`; the ignored
`hda-lead-q2-loaded-summary.json` explicitly records the failed candidate gate.

### Normal placement, guest-load control

A further matched pair separates the write-lead effect from the severe host
oversubscription experiments. Both private VMs use the original QEMU 10.2.2,
physical PipeWire output, callback HDA with requested 20-ms buffering, eight
vCPUs/eight GiB, production RR and blocking AP accounting. Launch-time and
endpoint audits allow all QEMU threads on logical CPUs 0–15. No host CPU
workers, private PAUSE-exit modification, CPU profiler, timer/ready recorder or
AP accounting intervention is used. The delivery probe runs around captures.

The candidate derives from the current GPU capsule and replaces **only HDA**.
The older 8-KiB capsule also contains a different GPU module, so copying that
whole capsule would confound the comparison. The common kernel is
`fc41f3dbf97797d1f349754303090fee0b6e14976f8350ae717e1636a104d8bb`;
HDA hashes are `f05fdc31f3ea5c1737dfe732c3acc2dc4ca6820217d8be9e4253d9bea83e1b3e`
(12 KiB) and `69289ccc84462a4158b59ed98e3942787fcff9d66eb64134c3ced68652b19c71`
(8 KiB). The idle trailer and publication/fault guards remain unchanged.

Each VM launches GL3 Quake in menu/attract mode, captures six seconds after
warming, runs eight finite guest CPU workers plus four periodic sleepers for
twenty seconds, then closes and reopens Quake for a second quiet capture. The
loaded capture lies between every worker's observed first and last work
samples; all eight perform nonzero work. Those endpoints do not assert that
workers execute continuously. The guest benchmark SHA is checked before use.

| Lead | Condition | App completion / guest second | Quiet PCM bins | Reserved silence bytes |
| --- | --- | ---: | ---: | ---: |
| 12 KiB | Quiet | 0.84345 | 13.725% | 182784 |
| 8 KiB | Quiet | 0.99927 | 0.033% | 512 |
| 12 KiB | Guest CPU load | 0.83213 | 15.321% | 201728 |
| 8 KiB | Guest CPU load | 0.95512 | 4.117% | 50176 |
| 12 KiB | Close/reopen quiet | 0.84254 | 13.659% | 184576 |
| 8 KiB | Close/reopen quiet | 0.99899 | 0.033% | 768 |

Quiet bins use approximately one-ms RMS windows below 10 in signed 16-bit
samples; the soundtrack can contain natural silence. Application completion
is not decoded music tempo, and QEMU PCM files do not timestamp speaker
playback. Both streams retain their epochs with zero discontinuities. All
captures have zero invalid clock/position, unsafe publication, diagnostic
contention and saturation. Both cumulative runtime ledgers, original app-group
exits, empty audio-stream teardown, heaps, original VM-owner exits and all four
filesystem checks pass. Observed headroom and advance remain empirical samples,
not worst-case safety bounds.

This pair reproduces the independent pacing improvement under ordinary host
placement, including application restart, but does not establish a complete
audio fix. Some silence remains under guest load. The earlier heavy host-load
faults remain failed gates; these results do not narrow the supported workload
or override them. Fresh Firefox, finite exact stream-drain checks and reversal
of the decisive contrast remain required before a production lead decision.
RR and the production HDA lead remain unchanged.

Full receipts and the derived summary remain in ignored
`build/workloads/timing-scheduler-20261021/hda-normal-cohort/q2-summary.json` and
`build/reviews/unattended-hda-normal-{12k,8k}-20261021`.

#### Fresh Firefox and exact finite drains

The next matched pair uses the same normal-placement settings and HDA-only
contrast. Each VM launches a fresh Firefox profile twice against the generated
local video/audio fixture. The first launch includes eight guest CPU workers
for twenty seconds. Buffered schema-2 page traces retain the expected phase
and epoch, monotonic samples and zero dropped trace records. Host/page clock
anchors select samples wholly inside each audio capture's host brackets,
allowing two milliseconds of timestamp quantization. These calculations assume
a 1× clock; independent capture clock brackets contain that rate. They are
callback opportunities, not physical display presentation.

| Lead | Condition | App audio / guest second | Quiet PCM bins | Video callbacks / page second |
| --- | --- | ---: | ---: | ---: |
| 12 KiB | Quiet | 0.96935 | 2.919% | 11.686 |
| 8 KiB | Quiet | 0.98774 | 0.387% | 11.459 |
| 12 KiB | Guest CPU load | 0.14670 | 47.556% | 0.132 |
| 8 KiB | Guest CPU load | 0.16246 | 42.398% | 0.381 |
| 12 KiB | Fresh relaunch quiet | 0.99143 | 0.748% | 11.579 |
| 8 KiB | Fresh relaunch quiet | 0.98779 | 1.097% | 10.665 |

Neither loaded condition is acceptable playback. The smaller lead does not
repair browser contention. A single key dispatched after the loaded capture
reaches the DOM in approximately 3.219–3.522 seconds for 12 KiB and
4.183–4.348 seconds for 8 KiB, followed by 844/733 ms until its RAF callback.
These are individual observations, not a statistically established input
regression or proof that a particular scheduler policy caused the delay.
Quiet relaunch variability also prevents a general browser performance claim.

After both browser groups exit and their audio streams disappear, each VM
plays two finite ten-second packet-coded streams. Requested and actual OSS
geometry agree: four/eight fragments of 4,096 bytes. Every stream submits and
completes exactly 441,000 frames, synchronizes and tears down; each six-second
capture lies inside the observed stream lifetime. Packet decoding finds pitch
near 1×, no backwards/repeated symbol transitions or unobserved intermediate
symbols, with approximate source/PCM rates 0.99586/1.00016 for the original
four/eight-fragment cases and 1.00016/1.00033 for the candidate. This finite
stream check runs after CPU load ends; it does not establish loaded drain
behavior. Stored PCM is not timestamped physical speaker output.

Both cohorts retain zero stream discontinuities, invalid clock/position,
unsafe publication, diagnostic contention and saturation. Runtime ledgers,
heaps, original browser/load/export jobs, the original fixture servers and VM
owners, and all four filesystem checks pass. The original invalid-instance
launch attempt is separately preserved: QEMU never started, its original
wrapper exited 2, and all four private filesystem checks passed before retry.
No failed attempt is relabeled successful.

The complete results are indexed by ignored
`build/workloads/timing-scheduler-20261021/hda-normal-cohort/browser-summary.json`.
Review directories are `unattended-hda-browser-normal-12k-r2-20261021` and
`unattended-hda-browser-normal-8k-20261021`. The controller is copied verbatim
into each review. These controls separate a reproducible small-queue lead
problem from the larger loaded-browser scheduling/service problem; neither
the historical acceleration nor severe host-overcommit failure is closed.

#### Original-lead reversal

A final fresh Quake VM restores only the original 12-KiB HDA module under the
same normal-placement settings. Completion returns to 0.84256 quiet, 0.82257
under guest load and 0.84407 after close/reopen; quiet PCM bins return to
13.908%, 15.828% and 13.863%, respectively. Every loaded capture lies within
all eight workers' observed work lifetimes. Runtime, publication diagnostics,
stream continuity, original app exits, teardown, heaps, original VM-owner exit
and all four filesystem checks pass again. The result strengthens the causal
write-lead contrast without attributing the separate loaded-browser failure to
that lead. The summary above now includes `12k-reversal` and its raw review
`unattended-hda-normal-12k-reversal-20261021`.

The production lead and scheduler remain unchanged. The next policy comparison
should keep HDA and the host placement fixed, use the same buffered browser
fixture and finite guest workload, and preserve the failed host-overcommit and
historical-acceleration cases as separate open gates.
