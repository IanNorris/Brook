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

## Firefox follow-up: source time versus HTTP receipt time

Firefox subsequently launched through its normal catalogue entry on both
kernels, alone in an eight-vCPU, 4 GiB software-Wayland guest. The same private
synthetic profile fixture and local 640×360, 24-fps VP9/48-kHz Opus clip were
used. Native Play/Pause actions bracketed ten-second PCM captures without
screenshots or profiling inside the capture intervals.

| Observation | Previous kernel | Epoch-anchor kernel |
| --- | ---: | ---: |
| Host PCM capture duration | 10.003 s | 10.006 s |
| Stereo PCM frames at 44.1 kHz | 441,300 | 441,056 |
| Silent 10-ms bins | 0 | 0 |
| Source sample interval | 12.653 s | 9.623 s |
| Media seconds per `performance.now` second | 1.002 | 1.001 |
| Browser total video frames per page second | 24.26 | 24.00 |
| Browser reported presented frames per page second | 3.24 | 8.00 |
| Out-of-order sample receipts | 14/26 samples | 8/20 samples |

Both pause checks returned zero PCM frames and an inactive stream. Each normal
Firefox close drained its full launch lineage; heap, checked shutdown,
authoritative QEMU exit, launcher exit zero, serial and offline FAT/ext2 checks
passed. Evidence is under `build/workloads/firefox-realtime-20261018/`.

**This is not an accepted video speedup measurement.** The control observer
first requested an unsupported 15-second capture, then resumed the same playing
page at the supported ten-second limit. That gave the control a longer warm-up.
A second observer assertion exposed delayed/out-of-order HTTP telemetry; the
existing PCM was retained and reanalyzed, without replaying Play or recapturing.
The candidate's interval followed the corrected path. One pair with unequal
warm-up and no host-load control cannot establish the effect of the clock fix
on presentation performance. Total/presented frame counts are browser reports,
not an independent count of visible compositor frames or proof of decode cost.

The earlier half-speed inference from receipt-time progress is withdrawn.
Sample requests can arrive seconds late and out of order. Sorting by source
`performance.now` gives media progress near one second per page second on both
kernels; numerous frames are still reported dropped. This does not prove
real-time host pacing for the earlier three-app run, whose old fixture lacked
source timestamps. Further work should separate decode, presentation and
scheduling costs with a controlled warm-up and loss-checked profiles.

`scripts/analyze_browser_media.py` now sorts source samples, unwraps real clip
loops, preserves smaller media backsteps, rejects counter resets and labels its
rates as page-clock measurements. Its optional PCM interval filter selects
**receipt** times; it explicitly does not claim sample/PCM alignment. Four
regressions cover reordered delivery, loop/backstep distinction, interval
scope, missing source spans and counter reset. For example:

```sh
python3 scripts/analyze_browser_media.py RUN/media-state.jsonl \
  --phase ff-candidate --capture RUN/candidate/audio.json
```

The [buffered-fixture presentation follow-up](firefox-presentation.md) separates
frame counters from frame callbacks, adds loss-checked profiles, and records
the remaining native/browser visibility discrepancy.
