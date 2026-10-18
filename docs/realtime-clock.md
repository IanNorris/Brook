# Realtime epoch anchor — 2026-10-18

`clock_gettime(CLOCK_REALTIME)` and `gettimeofday` previously combined
RTC/LAPIC-derived whole seconds with an unrelated pvclock fractional second.
Their second boundaries differed: every pvclock wrap could make realtime move
backward by almost one second, even though monotonic time advanced normally.
Realtime futex/deadline users inherited the same composition through
`ClockNowNs(0)`.

## Correction

The BSP now initializes KVM pvclock before reading CMOS. `RtcInit` records one
Unix epoch and one elapsed-source sample, before AP/application readers start.
`RtcNowNs` advances that immutable anchor using pvclock when available.
`ClockNowNs(0)`, `clock_gettime(0)`, `gettimeofday`, `time`, and RTC display/file
timestamps now derive their seconds and fractions from the same value.
Timezone affects local display only.

CMOS precision remains one second; this is not nanosecond absolute UTC accuracy.
No runtime clock-setting API is implemented. Later physical CMOS changes do not
silently reanchor realtime. For non-KVM guests the elapsed source is raw
delivered LAPIC ticks, avoiding the apparent counter's backward rebases. Its
frequency still depends on calibration and interrupt delivery. Other legacy
scheduler/monotonic fallback consumers still use the adjustable apparent
counter and require a separate audit. Physical hardware, suspend/resume and
wall-clock adjustment semantics have not been accepted.

## Matched live check

Two isolated disk sets came from the same synthetic-profile three-app fixture.
Each boot used eight vCPUs and 4 GiB RAM with no browsers running. Eight Brook
UID-1000 processes sampled clocks for 15 seconds each: four used libc calls,
four direct x86-64 syscalls. Each checked monotonic/realtime/gettimeofday for
backward steps, bracketed realtime API agreement (including `time(2)`), and
realtime-minus-monotonic drift against the sampled call interval plus 5 ms.

| Result | Previous kernel | Epoch-anchor kernel |
| --- | ---: | ---: |
| Samples across eight processes | 119,940 | 119,836 |
| Realtime backward steps across workers | 120 (15 each) | 0 |
| Largest realtime backward step | 999,014,010 ns | 0 |
| Largest sampled realtime/monotonic offset change | 1,000,089,413 ns | 765,418 ns |
| API disagreements | 0 | 0 |
| Workers passing | 0/8 | 8/8 |

Previous kernel SHA-256:
`9a9a374601cd0d7d7fc55e90c63a676cc4eec1488f0d804a22fa3628f9a7494c`.
Candidate SHA-256:
`37cf304288e63f4525c9c50900a6b63c2de04c92e664483b8442dcc793b36a82`.

The control's expected failure was asserted inside a tracked parent command;
the parent itself returned zero only after all eight workers exited with the
expected probe result. Both tracked jobs drained all lineage tasks and both
output streams, then passed heap integrity, checked sync/stop, authoritative
QEMU exit, serial audit and offline FAT/ext2 checks. The resumed control
observer verified QEMU exit but could not waitpid the original orphaned launcher;
the candidate's launcher also returned zero directly.

The probe is `scripts/workloads/clock_consistency.py`. Its optional
`--coarse-time` switch allows Linux's coarse `time(2)` result to lag by one
second for host smoke tests; **Brook acceptance omitted that switch**. These
measurements establish the sampled API behavior, not absolute UTC accuracy,
all clock IDs, long-run drift or realtime timer wakeup latency.

## Regression checks and remaining work

All 39 hosted suites and the compiled entry/return audit passed. The existing
pvclock suite now exercises the real RTC driver with fake CMOS readings,
pvclock second boundaries, timezone isolation, a deliberate CMOS change through
recalibration, and raw-tick fallback boundaries. Temporarily restoring the
mixed-clock composition fails those checks; restored candidate passes.
The fallback is covered by host fixtures, not a non-KVM VM or physical run.

Ignored evidence: `build/workloads/realtime-anchor-20261018/{control,candidate}`
contains frozen artifacts, manifests, tracked command output, serial and disk
checks; `summary.json` records the eight-worker results. Build, final suites and
negative-control logs are adjacent under `build/workloads/`.

Firefox previously reported slow media progress. This clock defect could affect
its realtime waits, but browser playback has not yet been compared with this
kernel. Do not infer browser speed or audio improvements from the clock probe.
Full matching bootstrap acceptance still precedes this kernel revision.
