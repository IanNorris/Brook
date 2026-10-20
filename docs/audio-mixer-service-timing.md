# Audio mixer service timing

The optional mixer probe separates delayed resumption after a nominal sleep
from time spent inside `AudioMixerTick`. It is intended to help investigate
desktop/audio stalls; neither bracket alone identifies scheduler ready time.
The scheduler default and audio sleep interval are unchanged.

## Interface and measurement limits

The normal debug channel accepts `audio timing on|off|reset|status`. It is not
panic safe. Recording is disabled at boot. Each control advances an epoch;
reset clears interval counters while preserving enabled state and worker
PID/incarnation. Completed counters survive on/off until explicitly reset.
Intervals crossing a control change, or still incomplete, are omitted.

Enabled recording samples the stable clock before deadline setup, immediately
after setup, after `SchedulerBlock` returns, and after mixer service returns.
The report distinguishes setup, sleep, service and the tail from the previous
completed service to the next pre-sleep sample. `late_lower_ns` is the positive
excess over ten milliseconds after the deadline-setup sample. It includes
blocked wakeup, timer dispatch, scheduling and host descheduling; it is not a
measurement of time continuously runnable. Service includes locks, interrupts
and host delays, rather than only mixer CPU work.

Disabled operation adds an atomic epoch load and branch, without probe clock
reads or counter locking. Recording uses a separate bounded reducer with
try-lock acquisition: the mixer never waits for a diagnostic reader. The
lifetime `contended_records` counter exposes omitted observations and does not
reset with interval counters. No allocation, logging or clock read occurs under
the reducer lock. A report retains the largest sixteen episodes with a setup,
late, service or tail bracket of at least twenty milliseconds. `excluded_slow`
counts smaller slow episodes excluded or displaced from that set. This is not
a complete timeline. Invalid clocks and aggregate saturation are explicit.

## Baseline evidence

Two fresh private eight-vCPU/six-GiB boots use Firefox with generated local
video and audio plus twenty-four bounded CPU workers. Callback HDA requests a
20-ms host buffer into an isolated PipeWire null sink. Neither CPU profiling
nor mapping counters are enabled. Both runs use the same kernel and modules,
with EEVDF's unchanged 4-ms normal request or MLFQ selected at boot.

| Run | Policy | Completed cycles | Maximum late lower bound | Maximum service | Slow cycles | Excluded slow |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| as-e1 | EEVDF | 2,903 | 92.737 ms | 45.997 ms | 13 | 0 |
| as-m1 | MLFQ | 2,247 | 90.885 ms | 46.058 ms | 152 | 136 |

Both reports have zero invalid clocks, zero contended records and no aggregate
saturation. The largest EEVDF event resumes 92.737 ms late but then services
the mixer in 57.158 microseconds. Thus at least that episode is dominated by
delayed resumption, rather than lengthy mixer service. It does not establish
which portion was blocked, runnable or host-descheduled. MLFQ's retained set
omits 136 smaller slow episodes and cannot support absence-of-event claims.

The EEVDF baseline also reproduces `publish-unsafe`, check bit 64 only: 744
bytes remain between raw completion and the next receipt's beginning, while
the device advances 1,024 bytes during a roughly 49-microsecond bracket. This
is a short write-publication hazard, separately from the long sleep episodes;
there is no retained EEVDF slow event within 100 ms of this fault. The existing
guard stops playback and preserves the proven completion watermark. This run
is not accepted as clean audio. No native HDA fault occurs in the MLFQ run.

Both original launch wrappers and audio routers exit successfully; all four
filesystem checks pass, and twelve documents plus four profile trees pass
preservation checks. Those lifecycle gates do not erase the EEVDF audio fault.
The model tests cover epoch changes, clock ordering, setup/service/tail
classification, bounded retention and aggregate saturation. All 64 host suites
pass; the native build resolves thirteen modules against 106 kernel exports.

Evidence is retained under the ignored
`build/workloads/timing-scheduler-20261020/audio-service-cohorts` directory and
the corresponding `build/reviews/timing-mix-as-*-20261020` private reviews.
The frozen kernel SHA-256 is
`8937859141ca14387045eaf9e2ee1077b2b2bc72a69c1cd79b280935bb5aac63`.
These diagnostic runs do not quantify probe overhead or demonstrate a complete
fix for browser/audio stalls or the historical application speedup.
