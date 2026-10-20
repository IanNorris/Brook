# Optional wake-scan handoff

The [timer audit](bsp-timer-wakeup-timing.md) found BSP timer-entry gaps above
100 ms while AP timer delivery continued. Sleeping-task deadlines depended on
`CheckBlockedWakeups`, called only by CPU 0. The scan itself was usually below
0.2 ms. Changing scheduling policy or quantum cannot by itself make that scan
run while CPU 0 is delayed in emulated I/O.

`sched wakes shared` explicitly enables a candidate handoff. BSP-only scanning
remains the boot default. `sched wakes bsp` restores it; `sched wakes reset`
clears statistics/clock anchor while preserving the mode. `sched wakes status`
reports the mode, sequence, completed BSP/AP scans, candidates, maximum
completion gap, maximum scan bound and clock/saturation flags. Controls return
`applied: false` if a scan owns the guard; they do not wait or revoke it.

The production atomic model is `wake_scan_handoff.h`. An odd sequence owns the
scan, an even sequence is free; successful claims advance the sequence so stale
completion tokens cannot release a later owner. BSP attempts its normal scans.
An AP may attempt a single nonblocking claim only from a periodic timer IRQ,
after at least 2 ms without a valid completed scan. Eligibility is rechecked
after claiming, covering a concurrent completion or mode change. Reschedule
IPIs do not create extra AP opportunities. If stable clock sampling is
unavailable, BSP still scans and AP handoff waits for a valid completion anchor.
Maximum duration excludes unavailable start timestamps; invalid-clock counts
describe unavailable/regressing completion or completion before a valid start.

Ownership covers candidate gathering, wake publication and reference cleanup.
Only the existing wake scan moves: POSIX expiry, alarms, reaping, load average
and policy ticks stay on BSP. The scan releases the registry lock before taking
ready/futex paths and retains every selected process reference until cleanup.
The existing candidate filter and ordinary concurrent I/O-wakeup semantics are
unchanged; this guard is not a proof of all scheduler state transitions. An
owner delayed while acquiring another lock cannot be safely stolen, so the
fallback interval is an eligibility threshold, not a guaranteed wakeup bound.

The new hosted suite exercises BSP preference, the exact AP threshold, missing
clock anchors, stale completion tokens, busy control rejection, preservation and
reset of statistics, clock discontinuity and counter saturation. Eight real
concurrent claimants additionally check that the whole protected work region
has one owner, including a yielded cleanup tail. All 66 hosted suites pass.
The kernel/module import audit resolves thirteen modules/106 exports. The
compiled entry audit passes twelve supported templates and 56 mutation checks;
four paths remain unsupported. No module ABI change is required (ABI 34).

## First matched native comparison

Four fresh private eight-vCPU/six-GiB boots use the same frozen kernel:
`531113d815a2e61b80350c8b08d023a1726f3d0443f709efdb40bf56fbed3a90`.
GPU and HDA modules retain their previous hashes. The fixture uses Firefox,
generated local video/audio, 24 CPU workers and four periodic sleepers, with
callback HDA, requested host 20 ms buffering and private PipeWire null output.
All runs enable the same BSP/peer/mixer/targeted-ready probes. No host KVM trace
or CPU sampler is added. `--wake-scan-mode bsp|shared` records explicit controls,
idle/load snapshots and restoration; only explicit busy replies may be retried.
Uncertain delivery retains its intent and stops automatic mutation.

| Run | Policy/mode | Max completed-scan gap | Mixer recorded deadline lateness | Mixer max ready-queue wait | Mixer max sleep excess over nominal 10 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| wh-eb1 | EEVDF/BSP | 118.096 ms | 42 ms | 27.336 ms | 42.189 ms |
| wh-es1 | EEVDF/shared | 3.019 ms | 2 ms | 22.003 ms | 21.799 ms |
| wh-mb1 | MLFQ/BSP | 98.467 ms | 95 ms | 42.912 ms | 113.256 ms |
| wh-ms1 | MLFQ/shared | 2.875 ms | 2 ms | 47.947 ms | 47.915 ms |

Shared runs complete 1,955 EEVDF and 1,448 MLFQ AP scans; BSP controls complete
none. Probe windows include setup/drain, so those counts are not rates for the
thirty-second CPU interval. The mixer sleep-excess metric is a lower bound and
deadline lateness is sampled in elapsed milliseconds. Each shared run's worst
retained mixer sleep correlates with a watch event ready at its deadline
(`late_ms: 0`), followed by respectively 22.003 and 47.947 ms in the ready
queue. Dispatch-to-resume is about 1.9 us in the EEVDF event. Remaining delays
are not explained by the old BSP scan-availability bottleneck alone.

| Metric | EEVDF/BSP | EEVDF/shared | MLFQ/BSP | MLFQ/shared |
| --- | ---: | ---: | ---: | ---: |
| Periodic helper p95 lateness | 2.823 ms | 1.786 ms | 19.983 ms | 21.495 ms |
| Periodic helper maximum lateness | 36.109 ms | 5.593 ms | 113.797 ms | 46.002 ms |
| CPU batches in timed interval | 6,071,908 | 6,015,687 | 5,714,086 | 5,742,367 |
| Buffered loaded browser callbacks/page second | 4.198 | 3.873 | 6.466 | 6.364 |
| Longest silent 10 ms bins in eight-second capture | 10 | 7 | 5 | 5 |

This is one pair per policy, not repeated desktop acceptance. CPU throughput is
within about 1%, but browser presentation does not improve and periodic MLFQ
p95 is slightly worse. Browser callback rate is not physical display latency.
Capture frame rates are close to 1x; they do not prove source-content tempo or
resolve the historical Quake/YouTube speedup. No native HDA fault is logged.

All four original workloads drain their tracked CPU/browser lineages, restore
BSP mode and pass clock/runtime gates, heap integrity, original launcher/router
waits, four filesystem checks and twelve-document/four-profile-tree preservation.
All VMs and private services are stopped. Ignored `wake-handoff-cohorts` retains
frozen source hashes, default-mode checks, control attempts, raw snapshots,
audio captures, matched sleep/wakeup events and `verify-results.py`.

Keep the handoff optional. Next repeat the comparison to assess presentation
variance, then attribute long ready-queue waits and remaining device service
delays. Scheduler-default acceptance, broader timeout/ownership audits and the
historical playback-speedup reproduction remain open.
