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

## Reverse-order repetition

Four more fresh boots use the identical frozen kernel, modules and fixture.
This time shared mode precedes BSP mode for each policy. The raw records and
same strict verifier are under ignored `wake-handoff-repeat`.

| Run | Policy/mode | Max completed-scan gap | Mixer recorded deadline lateness | Mixer max ready-queue wait | Mixer max sleep excess over nominal 10 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| wr-es2 | EEVDF/shared | 3.005 ms | 2 ms | 35.092 ms | 35.076 ms |
| wr-eb2 | EEVDF/BSP | 133.118 ms | 131 ms | 43.209 ms | 172.340 ms |
| wr-ms2 | MLFQ/shared | 2.700 ms | 2 ms | 57.958 ms | 57.214 ms |
| wr-mb2 | MLFQ/BSP | 143.990 ms | 130 ms | 46.709 ms | 129.799 ms |

Both orders reproduce the targeted result: shared mode's completed-scan gap is
2.700–3.019 ms and the watched mixer's maximum deadline-to-ready lateness is
2 ms. BSP timer-entry gaps themselves remain; AP handoff does not repair timer
delivery, host device waits, or the BSP-only housekeeping paths. Both repeated
shared runs' worst retained mixer sleeps again include a watch event ready at
its deadline followed by a long queue wait.

| Repeated metric | EEVDF/shared | EEVDF/BSP | MLFQ/shared | MLFQ/BSP |
| --- | ---: | ---: | ---: | ---: |
| Periodic helper p95 lateness | 1.610 ms | 2.995 ms | 21.910 ms | 23.579 ms |
| Periodic helper maximum lateness | 13.269 ms | 152.479 ms | 48.001 ms | 133.777 ms |
| CPU batches | 6,007,817 | 6,109,865 | 5,951,861 | 5,970,441 |
| Buffered loaded browser callbacks/page second | 4.421 | 4.927 | 7.068 | 6.827 |

EEVDF/shared has lower browser callback rate in **both** pairs, by about 8–10%,
and CPU throughput is about 1.3% lower across those two pairs. MLFQ browser
results change direction between pairs. The measurements therefore support a
wakeup-availability improvement, not a general desktop speedup or promotion to
defaults. Two pairs are still too few to establish a stable performance
distribution. All eight original/repeated runs pass runtime/clock gates, heap,
tracked-lineage drain, original launcher/router, filesystems and saved-data
preservation, with no native HDA faults. The repeated VMs are stopped.

Source review identifies two remaining mechanisms to distinguish with targeted
queue evidence: EEVDF gives tasks equal initial weight (the priority hint sets
request length), and `SchedulerTimerTick` returns before arbitrary preemption
when the interrupted context is kernel mode. A ready-queue wait alone cannot
identify eligibility/debt, dispatch opportunities or long unpreemptible kernel
ownership. Do not tune weights or shorten requests from that wait alone.

The [targeted per-CPU queue-activity probe](targeted-queue-activity.md) now
distinguishes delivered user/kernel timer observations from other-task dispatch
bookkeeping during retained mixer waits. It supports investigating EEVDF
eligibility and MLFQ's expiry-only long slices without treating every wait as
system-wide non-preemptible kernel execution.
