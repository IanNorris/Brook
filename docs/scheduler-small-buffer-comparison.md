# Scheduler comparison with callback/20-ms audio

Six fresh private Firefox/CPU trials repeat the scheduler comparison after the
[audio default change](hda-timer-discard-control.md). EEVDF retains a large
periodic wakeup-latency advantage, but MLFQ presents more video and has smaller
observed input-to-DOM bounds in this workload. RR still exhibits severe browser
and audio-producer starvation. The default scheduler remains RR; these results
do not establish one policy as best across workloads.

## Matched setup

The order is RR, MLFQ, EEVDF, EEVDF, MLFQ, RR. Each run boots a fresh checked
private clone with a fresh Firefox profile, eight vCPUs, six GiB, host affinity
0–13, the same generated 24-fps clip, an 800×600 Firefox window and 24 bounded
CPU workers for 30 seconds. The browser controller records buffered media/input
telemetry, periodic sleepers, an eight-second silent PCM capture and host load.
No sampling profiler runs in these cohorts. They are warm-host-cache trials,
not isolated-host or cold-start measurements.

Kernel SHA256 remains
`a9627f6fb5fcb7fd1f976f5c03138a03c68396f6c8ede195bef8ad4ec372d7a1`;
the complete 13-module ABI-34 set includes the HDA diagnostics. GPU module is
`724914fd7298dfb50d89ce1912b3fbd5d8b916ead51541ba983c48a4daf9f4bb`,
and HDA module is
`20854ba073c3afd6441b0286b7c35a50b84ecc563e17604505f942e982242b91`.
The pinned QEMU/renderer/render node, image source and workload sources match.
CPU benchmark binaries are extracted read-only from all six stopped FAT images
and verified identical, in addition to controller/page/clip hash checks.

Audio is the native PipeWire backend into a private explicitly routed 44.1-kHz
null sink. Neither launcher timing override is set. Actual QEMU arguments
confirm a 20-ms requested buffer; live QOM checks confirm callback HDA. This is
not physical-speaker acceptance. Early routing precedes readiness in each boot;
the same observer remains live until QEMU exits.

## Results

Ranges cover two trials per policy. Loaded presentation/media values use the
buffered page trace; RR's delayed HTTP arrivals provide no causally aligned
loaded samples. Two repetitions do not establish statistical confidence or
worst-case bounds.

| Policy | Periodic p95 | Worst periodic delay | Video callbacks/s | Media seconds/page second | CPU work batches |
| --- | ---: | ---: | ---: | ---: | ---: |
| RR | 90.27–91.38 ms | 101.02–102.48 ms | 0 | 0.0061–0.0098 | 6.887–6.973 million |
| MLFQ | 21.98–23.43 ms | 123.33–172.54 ms | 6.79–6.80 | 0.9623–0.9645 | 5.646–5.721 million |
| EEVDF | 2.91–2.95 ms | 62.20–114.80 ms | 4.47–5.10 | 0.9233–0.9458 | 6.085–6.141 million |

Maximum buffered dispatch-to-DOM upper bounds are 21.646 seconds on RR,
239 ms on MLFQ and 312 ms on EEVDF. These include dispatch and clock-alignment
uncertainty, and measure synthetic input reaching DOM rather than physical
input-to-photon latency. Their clock-anchor models pass; all traces export with
zero dropped samples. No buffered loaded cohort reproduces accelerated media.
The clips remain playable and buffered well ahead, without sampled low-buffer
starvation. Low media progression under RR is retained as failed performance,
not interpreted as a successful benchmark merely because collection completes.

RR captures contain only 5,120–20,096 PCM frames (0.116–0.456 seconds) across
about eight host seconds. MLFQ captures contain 348,928–350,976 frames, and EEVDF
352,000–352,896. WAV storage omits inactive output; silence inside stored PCM
cannot account for the absent RR output. Neither higher coverage nor successful
capture establishes glitch-free playback. Longest stored silent runs are
70–90 ms on MLFQ and 10–100 ms on EEVDF; RR's stored silence does not
represent its much larger inactive-output gaps.

In the first RR capture, repeated cached audio snapshots have both software
and in-flight queues empty, submitted equal to completed, and little new source
progress. For example, observed guest seconds 48.000 and 55.702 have only 4,410
additional submitted frames between them, with empty queues at both endpoints.
No native HDA fault is logged. Together with the earlier successful RR native
coded-signal/CPU control, this supports a browser-side producer starvation lead,
rather than assuming another device-clock rate defect. Snapshots do not identify
which browser/dependency thread prevents production or certify every interval.

The first RR user ready-queue episode maximum is 31.3 ms, far shorter than its
DOM stalls. The ready diagnostic excludes blocked time, pre-enqueue wakeup delay
and time between dispatch bookkeeping and user execution. CPU/context-switch
profiling should distinguish these stages before attributing the entire stall
to FIFO queue length or assuming EEVDF alone will resolve it.

EEVDF therefore remains an optional experiment: low periodic p95 is useful,
but it does not imply better video presentation or smaller UI latency. MLFQ's
interactive bias provides a better presentation result here. RR's larger CPU
batch count accompanies a stalled browser and is not an overall-workload win.
The legacy summary field `cpu_service_ratio` is a batch-count ratio, not measured
CPU-time fairness. Per-app/group fairness, arbitrary starvation bounds, kernel
preemptibility and precise user/system/IRQ attribution remain separate limits.
The earlier Linux reference is still a useful target, but its compositor,
software-vsync workaround and earlier audio cohort prevent a direct attribution
of its performance gap to this scheduler choice.

## Verification and evidence

All six app/CPU lineages drain and final audio stream lists are empty. Runtime
invalid/backwards/clamped/stale/saturated counters remain zero, pvclock and steal
masks are 255, heap checks pass and no unexpected HDA fault record appears.
EEVDF's policy-observed service balances the runtime ledger exactly. Queue
ready/actual-ready agreement and saturation checks pass where the policy exposes
them; MLFQ does not expose that debug contract, so its zero-filled fields are
not treated as a successful native queue-invariant check.

Every original launcher and routing observer exits zero. All four primary
filesystems pass offline checks, and the twelve saved documents/four original
profile trees match the preservation source. The six VMs, fixture HTTP server
and private audio daemon are stopped. No licensed media is used by this generated
fixture.

Ignored evidence is under
`build/workloads/timing-scheduler-20261020/policy-small-repeats`, including frozen
controller/launcher/source hashes, per-run metadata, actual QEMU argv, host
samples, screenshots, buffered traces, original job identities, audio captures,
`validation.json` and `cpu-fixture-hashes.json`. Each private review is under
`build/reviews/timing-mix-ps-{r,m,e}{0,1}-20261020`. The fixture uses
`policy-small-http-live`; the earlier sandbox-denied HTTP bind created no server
and is not a workload trial.
