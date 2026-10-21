# Audio commitment and refill discriminator

The reduced-probe native IRQ comparisons leave simultaneous Firefox/Quake
completion near 0.61/0.53 nominal with ordinary batching. A deterministic hosted
experiment now reproduces two ways to lose source throughput without scheduler
contention. It does not establish that these are the only causes in the browser.

## Actual-body fixture

`test_audio_delivery` runs the production mixer and HDA queue/progress bodies
with synthetic read/write MMIO and independently advanced DMA. Each producer
owns a finite, pregenerated eight-second stereo source, isolated to one channel.
The test compares every nonzero played sample to its expected source sequence
and requires exact submitted, physically played and credited totals at drain.
The deterministic sample pattern is not a unique frame-ID encoding.

The original quantum-only matrix has 156 configurations (the later horizon
follow-up below expands it to 468):

- 4,096 or 8,192 frames of endpoint capacity: four/eight 4,096-byte fragments.
  Fragment size remains 1,024 frames; the HDA write lead stays 12 KiB.
- Ordinary batching, existing shared-prefix mode, or a **host-only** 441-frame
  commitment cap using the existing fresh-age/headroom/epoch conditions.
- Writers that accept available prefixes or wait for a whole fragment.
- Cached completion alone or explicit producer refresh before inspecting space.
- Five-ms producer polling, offsets 0/1/4 ms and both source orders independently.
- A fixed subset additionally leaves the second producer dormant after its
  initial finite buffer, or misses four service opportunities then resumes the
  original cadence. The dormant producer performs no further refreshes.

DMA advances by 44/45 frames per one-ms step, exactly 441 frames every ten ms.
Physical advancement does not reconcile mixer credits. There is exactly one
ordinary mixer service opportunity per ten ms, except the explicit missed
opportunities; no immediate extra catchup flush is inserted. Initial priming
uses enough actual submissions for the chosen capacity.

Rates and missing/reservation counters use the fixed central interval from
two to six seconds, with both regular sources still active. The physical and
credited data-frame deltas must agree. Final drain is an independent integrity
check, not evidence of real-time throughput. Cached pending is distinguished
from physical pending derived from independent DMA movement. Both extrema are
sampled after each millisecond's producer/service actions and can miss a lower
value immediately before replenishment.

## Results

Selected central intervals, first source polled at multiples of five ms and
second source at offset one ms, explicit producer refresh, fragment-sized
writers, ordinary service cadence:

| Capacity, frames | Mode | First / second source rate | Driver reservations | Missing short / empty frames |
| --- | --- | --- | ---: | ---: |
| 4,096 | Ordinary | 0.5002 / 0.4998 | 0 | 0 / 177,152 |
| 4,096 | Shared prefix | 0.5002 / 0.4998 | 0 | 0 / 177,152 |
| 4,096 | Host-only quantum cap | 0.7762 / 0.7746 | 0 | 35,591 / 43,659 |
| 8,192 | Ordinary | 0.6252 / 0.6269 | 0 | 1,457 / 130,473 |
| 8,192 | Host-only quantum cap | 0.9980 / 0.9866 | 0 | 2,531 / 0 |

Rates are actual source data frames per nominal interval frames, not a fabricated
credit for silence. Missing counts sum both stream rows and are distinct from
driver silence reservations. All listed values exclude startup and drain.

A different regular configuration, with four fragments and only cached
completion, gives about 0.846/0.846 for whole-fragment writers across all modes.
It has recurrent driver reservations. Four fragments leave 4,096−3,072 =
1,024 frames above the fixed safety lead: exactly one fragment. Waiting for
that much completion and discovering it at a later service/poll opportunity can
leave too little lead for the next write. Even the dormant-peer control's active
writer runs about 0.929 with eighty reservations in the central interval.

The offset case instead loses throughput with **zero central driver
reservations**. Output advances while sources occupy different parts of it.
`Prepare` can commit forty ms from whichever queue currently has the most
software data. An empty/short peer receives silence in that irrevocably committed
tail; later source data is appended rather than filling that earlier gap.
`Consume`/`Reconcile` correctly avoid crediting that silence as source progress.
The capacity and timing controls separate this from missing completion credit.

## Acceptance and limits

All 156 configurations pass exact drain, PCM comparison and ownership/integrity
checks. The host-only cap nevertheless fails the regular playback criterion in
**20 of 48** regular candidate cases: each active source within 0.99–1.01 nominal
and no central driver reservations. It is rejected as a general native fix.
At this first stage no guest command, runtime flag, native state or default
change was introduced. The later native follow-up below adds an opt-in control.
Preprocessing `audio.cpp` with its actual native compiler flags confirms all four
test-state/API names are absent. The guarded proposal and snapshot sampling are
compiled only with `BROOK_HOST_TEST`.

Focused actual-body cases also exercise stale progress, insufficient lead,
full-ring rejection, partial acceptance, and changing the hosted setting during
copy. A captured fully accepted shortened proposal clears its own remainder
budget even if the setting changes; rejected/partial requests retain ordinary
recovery capacity. These checks qualify the experiment, not a native feature.

Astra reviewed the source and identified the original dormant-source observer
error; the final fixture returns before refresh for exhausted producers.
All 75 hosted suites pass after rebuilding that correction. There is no native
scheduler, hypervisor timing, physical speaker or browser in this fixture.
Previously failed native accounting/audio gates remain failed.

Next investigate an explicit bounded global commitment horizon **and** sufficient
fragment-refill slack. Preserve real source credits, negotiated OSS geometry,
driver recovery and absent-peer progress. Do not add more grace periods or
promote a larger buffer/default from this fixture. Any replacement must first
pass the same steady-state matrix, then the private native mixed workload.

## Reproduction

Run the hosted `test_audio_delivery` binary and save its complete output, then:

```sh
python3 scripts/workloads/analyze_mixer_commit.py /absolute/path/to/log --output /absolute/path/to/report.json
```

The analyzer verifies the configuration matrix and distinguishes fixture
integrity from the failed playback gate. Local evidence is retained under
`build/workloads/timing-scheduler-20261021/reboot-resume/`:
`mixer-steady-reviewed.log`, `mixer-steady-reviewed-analysis.json`,
`mixer-steady-reviewed-all-host.log` and `mixer-native-preprocessed.json`.
Earlier exploratory logs are retained with their superseded model limitations.

## Guarded target horizon follow-up

A second host-only mode targets 4,096 frames of physically pending output,
rather than capping each individual submission at 441 frames. With a fresh
current-epoch HDA snapshot whose pending bytes cover the unchanged 12-KiB lead
plus snapshot age and one nominal service interval, it requests at most:

```
max(0, 4096 - (submitted_bytes - completed_bytes) / 4)
```

At the target it defers submission, consuming no source frames and creating no
receipt or retained retry budget. The next ordinary service still refreshes
progress. Missing, stale or insufficient headroom retains ordinary batching and
the existing driver guards. This is a **guarded target**, not a hard maximum:
startup and recovery can exceed it. A stalled device still requires existing
driver fault/progress handling. No producer waits, extra flush opportunities,
synthetic source credits or native runtime option are added.

The expanded fixture has 468 configurations. It adds 5,120/6,144-frame boundary
capacities and 2,048-frame fragment controls at 6,144/8,192 capacity. Producer
order, phase, prefix/whole-fragment writing and refresh cadence remain distinct.
Each row below comprises twenty-four regular cases (two orders × three phases
× two writer types × two refresh modes):

| Capacity / fragment, frames | Source-rate range | Cases failing regular playback gate | Central driver reservations | Above-target sampled instants, summed |
| --- | --- | ---: | ---: | ---: |
| 4,096 / 1,024 | 0.7871–1.0000 | 16 | 598 | 4,358 |
| 5,120 / 1,024 | 0.9677–1.0000 | 2 | 0 | 4,992 |
| 6,144 / 1,024 | 1.0000 | 0 | 0 | 0 |
| 6,144 / 2,048 | 0.9951–1.0000 | 0 | 0 | 8,196 |
| 8,192 / 1,024 | 1.0000 | 0 | 0 | 0 |
| 8,192 / 2,048 | 1.0000 | 0 | 0 | 0 |

Counts sum separate runs, not a combined interval. The 6,144/2,048 tuple meets
the 0.99–1.01 rate gate but repeatedly uses the recovery path above target; it
does not establish the same regular target behavior as the three exact-rate
tuples. These results support specific tested geometry/cadence/horizon tuples,
not a universal minimum capacity or arbitrary scheduling-latency guarantee.
The original quantum cap remains rejected; the analyzer labels its separate
`candidate_playback_accepted` field explicitly as the 441-frame quantum policy.

At 6,144/1,024 and 8,192/1,024, the dormant-peer case leaves the active source
at 1.0 with no central reservations. Missing four service opportunities causes
one driver reservation and a 2,960-byte maximum sampled target excursion.
Pending output returns to the target seventeen ms after ordinary service
resumes. Both source rates over the central interval are about 0.9933: a gap
still occurred. Exact drains and source PCM comparison remain intact. Recovery
does not retroactively fill the missed audio or erase that reservation.

Focused tests cover deferral without ownership, setting changes during copy,
stale/low-headroom recovery, and genuine rejected/partial receipts after the
driver fills between preparation and submission. They preserve the captured
proposal's full-acceptance budget clearing and ordinary recovery after a failed
or partial submission. Initial fixture mistakes (absolute versus pending byte
expectations and too-small synthetic filler storage) were corrected before the
final passing run; the failing diagnostic log remains retained.

All 468 integrity cases and all 75 hosted suites pass. ASan/UBSan also pass,
with explicit coverage limits: ASan global redzones are disabled because module
metadata otherwise retains unused PCI initialization symbols, and LeakSanitizer
is disabled because the sandbox's ptrace environment prevents it from running.
The original linker/LeakSanitizer failures remain retained; no leak/global-redzone
acceptance is claimed. At this hosted-only stage native preprocessing excluded
all six hosted experiment state/API names. That historical exclusion does not
apply to the production horizon implementation described below.

### Geometry compatibility before native testing

Do not silently enlarge a four-fragment maximum to six. Linux stores the upper
field of `SNDCTL_DSP_SETFRAGMENT` as `maxfrags` and applies it while selecting
period count; see [`pcm_oss.c`](https://raw.githubusercontent.com/torvalds/linux/master/sound/core/oss/pcm_oss.c),
`snd_pcm_oss_set_fragment1` and `snd_pcm_oss_period_size`. Buffer selection also
depends on device constraints, so a Linux comparison is a compatibility probe,
not a demand for identical hardware geometry.

The tested 8,192/2,048 tuple represents **four 8-KiB fragments**, preserving a
four-fragment count while changing the fragment size. This is a viable hosted
geometry control, not implemented OSS negotiation. Before a native mode:

1. Test explicit fragment requests and honest returned `GETBLKSIZE`/`GETOSPACE`
   geometry, byte/frame conversions, write acceptance and `POLLOUT` thresholds.
2. Preserve established queued/in-flight streams. Enabling a commit policy may
   let backlog settle, but must not resize, retract receipts or discard credits.
   Pause alone is not permission to change geometry.
3. Add a default-off native control only after those API checks, then run the
   private mixed Firefox/Quake/load gate with observed geometry, source drains,
   full media loop and all runtime/HDA/filesystem checks. Unsupported/smaller
   tuples must not inherit the successful tuple's acceptance claim.

Final local evidence: `mixer-horizon-final.log`,
`mixer-horizon-final-analysis.json`, `mixer-horizon-final-all-host.log`,
`mixer-horizon-native-exclusion.json` and `mixer-horizon-asan-ubsan-final.log`
under the same `reboot-resume` directory. Full timing/scheduler acceptance,
historical fast playback and earlier failed native gates remain open.

## Default-off native follow-up

The guarded target now has a production implementation exposed through
`audio horizon on|off|reset|status`. It starts disabled and is mutually exclusive
with shared-prefix and coalescing experiments. Unsupported drivers, stale clocks
and insufficient headroom retain ordinary recovery; 4,096 frames is a guarded
target, not a hard queue bound or a scheduling guarantee. Captured attempt kinds
and epochs prevent receipts from being attributed to another experiment or to
an earlier reset. Enabling the control never resizes existing streams or resets
queued data and credits.

New `SETFRAGMENT` requests may negotiate larger power-of-two fragment sizes
while respecting the upper field's maximum fragment count and the endpoint
capacity bound. Four 4-KiB fragments become four 8-KiB fragments; six 4-KiB
fragments already qualify. Valid requests that cannot qualify, such as a
two-fragment maximum, retain honest smaller geometry. The original invalid
requests remain invalid. `GETBLKSIZE`, `GETOSPACE`, writable thresholds and
diagnostics report actual geometry. Fragment size is recorded separately from
the writable threshold so changing low-water readiness does not falsify refill
qualification. Qualification counts all open endpoints, including paused ones;
it is not a count of active playback streams.

Geometry is committed only after the capacity operation succeeds. A mode toggle
is not a barrier against an already-started negotiation: the ioctl snapshots
the flag once. Tests serialize controls and inspect the returned geometry.
Queued streams still reject renegotiation until explicit reset; pausing is not
permission to discard data.

All 468 hosted integrity configurations now exercise the production horizon
body and pass; all 75 hosted suites pass. The expanded pure geometry helper
checks byte/frame sizes, request bounds, unchanged output on invalid requests
and count limits. Rebuilt ASan/UBSan audio and geometry suites also pass for
this production source, with the same global-redzone and leak-detector limits
described above. An initial missing libc++ link-path failure was corrected
before building and running the suites; no stale test binary was accepted.

A fresh private eight-CPU VM booted kernel
`b286425bd29487eb73f3b6da1782b41a9220c88e87f98d003de2ccea8b5ff36b`
with the same frozen thirteen ABI-34 modules. Four original guest jobs pass:
serialized geometry off/on/off and the existing endpoint pointer/drain/signal
regression. They verify full-buffer `EAGAIN`, `POLLOUT`, honest returned sizes,
queued-paused `EBUSY`, and geometry preserved across reset. All original job
identities, successful exits, complete output and empty process groups were
verified. The collection controller incorrectly parsed the endpoint's text
output as JSON and exited unsuccessfully; a separate offline verifier accepted
the original evidence without relaunching any guest job. This collection error
is retained rather than presented as a successful controller exit.

Evidence lives in
`build/reviews/unattended-horizon-geometry-fixed-mlfq-a-20261021/geometry-control/`.
These are ABI checks, not playback acceptance or a Linux hardware comparison.
The subsequent native control uses identical four-by-8-KiB geometry off/on/off,
two independent finite producers and six-second stereo WAV capture through
the physical PipeWire route. Write requests remain 4 KiB in all arms. Both
sources are generated and both endpoints configured before a shared release.
Each finite twelve-second source drains exactly; source-completion counters
and decoded packet progression provide separate measurements.

| Mode | Completion rates, two streams | Decoded source seconds / stored PCM second | Quiet 1-ms bins, two channels |
| --- | --- | --- | --- |
| Off A | 0.9578 / 0.9575 | 0.9492 / 0.9493 | 3.00% / 2.79% |
| On B | 1.0007 / 1.0007 | 1.0002 / 1.0002 | 0% / 0% |
| Off C | 0.9873 / 0.9873 | 0.9909 / 0.9909 | 0.75% / 0.75% |

Pitch fits 1.0 in all captures; stored PCM duration / host capture interval
is 1.00052, 0.99991 and 0.99964 respectively. Packet timing is approximate,
not a sample-level integrity proof or a direct speaker measurement. Quiet bins
use the existing roughly one-ms RMS threshold and can miss shorter gaps.
The second disabled arm's smaller loss demonstrates phase variability: this is
a positive controlled result, not a general improvement percentage. The enabled
run records 1,166 accepted shortened submissions, 44 deferrals, 24 recovery
bypasses and no invalid-clock/unsupported-driver events over the whole source;
the target still does not promise uninterrupted service under arbitrary load.

All twelve original producer/report-export jobs pass identity, collected exit,
complete output and empty-group checks. Current HDA/delivery, heap, policy
accounting, IRQ, runtime and shutdown gates pass, as do all four filesystem
checks. The ABI jobs are separate and are not counted in those twelve. Both
original playback controller and VM owner exited zero. Evidence is in the same
review's `horizon-control/summary.json`; production sanitizer results are in
`reboot-resume/horizon-production-sanitized.log`.

### Real Firefox/Quake/load follow-up

Fresh review `unattended-horizon-mixed-fixed-mlfq-b-20261021` enables horizon
before endpoint configuration. Firefox negotiates capacity/fragment/low-water
of 4,416/64/64 frames; Quake negotiates 8,192/2,048/2,048. Each identity/epoch
keeps that geometry through captures and policy reversals. Prior accepted
source frames drain across transitions before warmup/capture. This compares
policy on established enlarged buffers, not default small-buffer geometry.

| Phase | Firefox completion / nominal | Quake completion / nominal | Quake swaps / guest second |
| --- | ---: | ---: | ---: |
| Firefox alone, on | 1.0003 | — | — |
| Both, on A | 0.9183 | 0.9989 | 8.63 |
| Same endpoints, off B | 0.4566 | 0.7616 | 8.83 |
| Same endpoints, on C | 0.9308 | 1.0001 | 8.52 |
| Eight CPU workers, Quake focused, on | 0.9403 | 1.0012 | 8.00 |
| Eight CPU workers, Firefox focused, on | 0.8946 | 1.0001 | 8.27 |
| Firefox after Quake close, off | 0.9981 | — | — |
| Reopened Quake, negotiated on then captured off | 0.4607 | 0.7769 | 8.51 |

All eight browser windows are valid, with rate one, audio enabled and no media
error. These counters measure per-stream completion, not decoded music tempo;
swaps are application presentation calls, not scanout. The policy reversal
supports an audio-throughput improvement, not a graphics throughput claim.
Firefox still underfeeds with both apps active. Its observed 4,416/64 geometry
is not one of the earlier symmetric hosted tuples; an asymmetric actual-body
control is needed before attributing its remaining deficit or changing bounds.

All nine original jobs pass complete-output/identity/empty-group checks; both
bounded-load captures lie inside all workers' active intervals. The independent
guest media hash, every full sender transfer, reconciled request receipts and
an observed error-free browser loop pass. All eight heavy probes stay disabled
at capture endpoints; cached audio snapshots, WAV recording and buffered SDL
frame traces remain. Accepted-contribution categories were not recorded in
these performance windows and cannot be inferred from completion alone.
Current native audio, heap, policy, clean owner/server exits and all four
filesystem checks pass.

**Runtime acceptance fails:** two clamps form one debt episode, maximum
12,796 ns, one owner transition and one catchup; final known debt is zero.
There are no backwards clocks, invalid samples or stale identities. Catchup
does not waive the failed gate or earlier attribution limitations. Cleanup
history was enabled after the timed windows and did not capture this episode;
do not claim a new complete positive history from its later ring contents.

The first mixed attempt stopped at an empty-stream admission assertion before
launching apps. Its three original preflight jobs, clean shutdown and filesystem
checks are retained separately under replica A; they are not a workload cohort.
Replica B's `mixed-prefix-control/verification.json`, `geometry-transitions.json`
and `summary.json` retain the actual horizon results despite the inherited
directory/phase names. No audio or scheduler default is promoted. Full mixed
acceptance and historical fast playback remain open.

### Observed asymmetric geometry discriminator

The hosted fixture adds 144 cases alongside the unchanged 468-case matrix.
Shape 1 uses Firefox's observed 4,416/64 capacity/fragment frames against
Quake's 8,192/2,048. Shape 2 increases only Firefox capacity to 8,192 while
retaining its 64-frame fragment. Each producer opportunity greedily submits
whole fragments until space is exhausted; these are not measured application
write patterns. Firefox opportunities occur every 1/5/10 ms; Quake retains a
five-ms period with 0/1/4-ms phase offsets. Both source orders, cached completion
versus explicit refresh, and ordinary versus guarded batching are covered.

Selected central source rates (all orders/phases/cadences in each stated group):

| Geometry | Policy / completion observation | Firefox-like / Quake-like rate |
| --- | --- | --- |
| Observed asymmetric | Ordinary, cached | 0.4546 / 0.7854 |
| Observed asymmetric | Horizon, cached | 0.9635 / 1.0000 |
| Observed asymmetric | Horizon, refresh before refill | 1.0000 / 1.0000 |
| Larger Firefox capacity only | Horizon, cached or refreshed | 1.0000 / 1.0000 |

The observed asymmetric horizon passes 18 of 36 regular playback cases: every
refreshed case passes, every cached case misses Firefox's rate gate. The larger
Firefox-capacity horizon passes all 36. All 72 ordinary-batching cases fail
the playback gate; enlarging capacity without guarding commitment does not
solve the problem. All added cases have zero central driver reservations,
preserve exact PCM order and source drain totals, and match physical data to
cached completion over the central interval. Missing source throughput is not
automatically an HDA underrun or scheduler failure.

Initial priming still commits 8,192 output frames even when Firefox supplies
only 4,416 initially. The two-to-six-second measurement window excludes that
startup, and the second source cannot mask missing first-source PCM. Direct
`AudioMixerRefresh()` is an experimental variable, not a simulation of browser
ioctls, decode or thread timing. Brook's DSP ioctl entry already refreshes
completion before taking the per-open mutex, so the cached-only result does
not establish the cause of the native Firefox deficit. No production geometry
or policy bound is changed from this experiment.

`analyze_mixer_asymmetric.py` requires the exact 144-case matrix and rejects
missing/duplicate configurations, wrong geometry and physical/completion
mismatches. The original analyzer independently accepts the original 468 rows.
Evidence is `mixer-asymmetric-final.log` and `mixer-asymmetric-analysis.json`
under `reboot-resume`. All 75 hosted suites and the expanded audio fixture's
bounded ASan/UBSan run pass (same redzone/leak-detector limits as above).

The next discriminator is native finite synthetic sources with these exact
asymmetric geometries, horizon enabled, and real `GETOSPACE` before refill.
If these sustain nominal progress while Firefox does not, inspect actual
application production/readiness and accepted short/empty contributions before
changing capacity. If they also fall short, known source availability isolates
completion/refill scheduling more directly. The two runtime clamps and the
broader timing/scheduler acceptance remain separate open issues.

### Native finite asymmetric sources

Review `unattended-horizon-asymmetric-fixed-mlfq-a-20261021` keeps horizon on
and negotiates exactly 69 × 256-byte fragments (4,416/64 frames) for the left
source, four × 8-KiB fragments (8,192/2,048) for the right. Write requests are
256 and 8,192 bytes respectively. Each source is finite, pregenerated and
independently owned. All arms query `GETOPTR` every 100 ms. The refreshed arms
add real `GETOSPACE` before each refill and limit the write to returned space;
the middle arm retains nonblocking write/retry admission. It is not a pure
cached-only experiment, nor a replay of Firefox's write sequence.

| Arm | Decoded left / right source seconds per PCM second | Quiet bins, left / right |
| --- | --- | --- |
| Per-refill space A | 0.9828 / 0.9979 | 1.91% / 0.13% |
| Periodic pointer B | 0.9383 / 1.0002 | 4.90% / 0% |
| Per-refill space C | 0.9501 / 0.9993 | 4.19% / 0.07% |

Pitch fits 1.0 and stored PCM/host interval ratios remain 1.00090, 1.00002 and
0.99945. All twelve original producer/export jobs, exact source drains,
current delivery/HDA, runtime, heap/policy, clean controller/VM exits and four
filesystem checks pass. This verifies data integrity and lifecycle; the small
source fails the real-time playback gate even with real fresh-space queries.
Delivery/contribution recording is enabled for this discriminator, unlike the
reduced-probe real-app windows; do not pool the two performance cohorts.

The first capture records 4,676 missing-short frames and no missing-empty
frames for the small source; the large source contributes every accepted output
frame. A snapshot has 4,096 small-source frames in flight and only 320 in
software. At 44.1 kHz that software reserve lasts about 7.26 ms. Accepted-write
gaps in the fresh-space windows reach 20.88 and 29.01 ms (p99 about 7.87/7.90
ms); the periodic-pointer maximum is 31.92 ms. These intervals include query
time, bookkeeping, retries, sleeps and scheduling. They are not measured
dispatch latency or proof that all lost frames belong to those maximum gaps.

Fresh completion alone therefore does not guarantee this small geometry's
throughput under native refill timing. The current `target + 2 × fragment`
qualification provides less than one ten-ms service interval here. Before
changing prospective geometry or the target, add explicit bounded producer-gap
controls and justify a reserve against the unchanged HDA safety lead and
service interval. Existing queued endpoints must remain unchanged. Evidence:
`asymmetric-control/summary.json` and `write-gap-analysis.json`. No production
bound/default or scheduler policy is changed by these results.

### Explicit post-discovery refill budgets

A further 390 hosted cases keep the 4,096-frame horizon and fixed HDA lead.
After each one-ms physical DMA step, explicit refresh discovers completion.
When a whole fragment first becomes writable, producer 0 waits a declared
0/1/9/10/19/20/21/39/40/41/50/75 ms before greedily refilling. Producer 1 admits
immediately. Mixer submission retains a ten-ms cadence at phase 0/1/4 ms;
producer order varies independently. This separates fragment size from a
post-discovery execution delay. Discovery itself is idealized, and these
constant delays do not represent every bounded varying jitter sequence.

The proposed reserve formula is `C >= H + F + ceil(44100 × J / 1000)`, for
capacity C, target H=4096, actual fragment F and declared delay J in ms. Under
the original maximum of 69 fragments and a power-of-two size starting at 64
frames, the first viable size is 128 frames. Minimum counts are then:

| Declared delay | Capacity / fragment frames | Count | Reserve after fragment eligibility |
| --- | --- | ---: | ---: |
| 20 ms | 5,120 / 128 | 40 | 896 frames, about 20.32 ms |
| 40 ms | 6,016 / 128 | 47 | 1,792 frames, about 40.63 ms |

Both counts respect the original maximum; actual geometry must still be
reported to the caller. These are explicit test inputs, not implemented
prospective negotiation. Quake's 8,192/2,048 tuple supports either declared
budget. The test also delays the large-fragment producer against an immediate
small-fragment peer. It does not delay the small producer in both channel
positions, and should not be described as a complete permutation proof.

All 180 enumerated regular cases within the proposed budgets have exactly
176,400 contributed/physically played/completed frames per source over the
central four seconds, no short/empty contributions and no driver reservations.
All sixteen within-budget dormant/disabled-peer controls preserve exact active
progress without reservations. The old 4,416/64 geometry instead runs about
0.9796 at ten-ms delay and 0.9088 at twenty ms, while its supplying peer remains
nominal. The twenty-ms tuple first loses data at twenty-one ms; the forty-ms
tuple at forty-one ms. These small losses remain failures even when a looser
one-percent average-rate gate would pass. Beyond-budget outcomes stay in the
analysis rather than being discarded.

Missing four mixer service opportunities still causes one central reservation,
a sampled 2,960-byte target excursion and about 0.9933 source rates. Pending
returns within target seventeen ms after the first resumed service. Explicit
completion refresh continues during this scenario; it is not total observer
starvation. The recovery measurement now waits for that actual resumed service
(3,041 ms at phase one), rather than declaring recovery in the preceding
below-target gap. The older phase-zero measurements are unchanged.

Exact source PCM order/drains pass in all 390 cases; the separate 468 and 144
matrices remain intact. All 75 hosted suites pass. The expanded sanitizer run
exceeded the test's fifteen-second CTest limit; that timeout is retained
separately from the bounded direct-binary rerun. No sanitizer acceptance should
be inferred merely from the absence of a diagnostic before timeout. The same
instrumented binary subsequently completed successfully in 18.85 seconds under
an explicit ninety-second process budget, with the existing global-redzone and
leak-detector exclusions. This is a successful direct sanitizer run, not a
successful execution under the original CTest timeout.

`analyze_mixer_refill_budget.py` requires the exact matrix, verifies delay and
geometry, and rejects missing/duplicate rows or fabricated nominal results.
Its `budget_contract_accepted` means acceptance of this enumerated model only.
Evidence: `mixer-refill-budget-final.log` and `mixer-refill-budget-analysis.json`
under `reboot-resume`. A native original→20-ms→40-ms→original explicit-geometry
comparison is prepared, keeping per-refill queries and 256-byte writes constant.
Its write granularity differs from this model's whole-128-frame refills. The
production planner, horizon, HDA lead and scheduler defaults remain unchanged.

### Matched native reserve control

Fresh review `unattended-horizon-budget-fixed-mlfq-a-20261021` explicitly requests
original → twenty-ms → forty-ms → original geometry from the **unchanged**
kernel. The left source keeps 256-byte writes in every arm; the right keeps
8,192/2,048 geometry and 8-KiB writes. Both query real `GETOSPACE` before each
refill and `GETOPTR` periodically. All source data is pregenerated, both
endpoints are configured before release, and horizon stays on throughout.

| Left geometry, frames | Decoded left / right progression per PCM second | Quiet bins, left / right |
| --- | --- | --- |
| Original 4,416 / 64 | 0.9563 / 0.9991 | 4.22% / 0.07% |
| Twenty-ms 5,120 / 128 | 0.9818 / 1.0001 | 1.16% / 0.02% |
| Forty-ms 6,016 / 128 | 1.0002 / 1.0003 | 0% / 0% |
| Original again | 0.9679 / 0.9988 | 3.39% / 0.13% |

The forty-ms arm has no accepted short/empty contributions for either stream.
The left source loses 13,476, 3,968 and 9,620 short-contribution frames in the
other arms' counter windows respectively; the right contributes fully in those
windows. Counters enclose the WAV recording but are not sample-aligned to it.
Decoded pitch fits 1.0, and stored PCM/host interval ratios are all within about
0.1% of one. Decoder quantization and quiet-bin limits described above remain.

All sixteen original producer/report-export jobs pass identity, collected exit,
complete output and empty-group checks. Exact finite source drains, current
HDA/delivery, runtime, heap/policy, original controller/VM exits and all four
filesystem checks pass. This does not clear the two accounting clamps in the
earlier real-browser cohort. Accepted-write gaps remain descriptive intervals,
not measured eligibility-to-refill delay or a guarantee of forty-ms service.

Evidence is `budget-control/summary.json` and `write-gap-analysis.json` under
that review. Together with the enumerated hosted boundaries, the reversal
supports a prospective time-based reserve rather than fragment size alone.
No production negotiation, existing queue, HDA lead, horizon target or default
has yet changed. A production planner change still requires its own geometry,
source-integrity and real mixed-workload validation.

### Default-off prospective 40-ms planner

`audio_refill_budget.h` now defines the shared fixed-rate qualification:
`capacity >= 4096 + actual_fragment + 1764` frames. The 1,764 frames represent
a declared forty-ms refill allowance at 44.1 kHz, not a scheduler guarantee.
Planning and diagnostic qualification use the same helper; low-water readiness
remains independent. The hardware target and HDA publication lead are unchanged.

With horizon off, valid requests keep their original geometry. With it on,
already-qualified requests also remain unchanged. Otherwise the planner tries
supported power-of-two fragment sizes and selects the minimum qualifying count
at the first viable size, bounded by the requested nonzero maximum and physical
endpoint capacity. Required frames, bytes and ceiling counts use checked
64-bit arithmetic before multiplication/commit. A valid request that cannot
qualify keeps its honestly reported original tuple. No queued stream is resized
and mode changes retain their existing snapshot semantics.

Stereo16 examples (count × bytes):

| Request | Negotiated with horizon on |
| --- | --- |
| 69 × 256 | 47 × 512: 6,016/128 frames |
| 6 × 4,096 | 4 × 8,192 |
| 4 × 4,096 | 4 × 8,192 |
| 2 × 4,096 | 2 × 32,768: 16,384/8,192 frames |
| 1 × 4,096 | Original, unqualified |

Count reduction is valid negotiation under a maximum-count request. The
two-fragment tuple is a materially larger operating point: up to about
372 ms of source backlog, which is not bounded by the hardware horizon.
Tests cover that range explicitly rather than declaring it unsupported merely
because the previous formula did not qualify it.

The actual-body matrix now has 1,158 cases: the original 468, asymmetric 144
and expanded budget 546. Large-fragment cases include whole 8,192-frame
writers, exact PCM/drains, readiness-delay boundaries, dormant/disabled peers
and interrupted service. Software capacity can exceed immediately writable HDA
space; priming is bounded to the accepted driver amount (up to 8,820 frames),
then physical DMA advances normally. First-data/drain fixture timestamps are
relative to that prime, not VM startup. All 288 within-budget regular and 24
inactive-peer cases pass their exact-progress checks.

The actual write-plan test additionally converts a 204,800-byte request in
four bounded transactions with partial FIFO admissions and verifies exact source
ordering across the 64-KiB staging limit. This is not a substitute for native
user-copy/syscall-return validation. All 75 hosted suites and direct bounded
ASan/UBSan audio/geometry runs pass, with the previously stated exclusions.

Native kernel `bf37b8497ca3657e5d9651345b42491a99d3090475106a3cf84b33ea0b39266b`
passes two-pass symbol generation and all thirteen frozen ABI-34 module imports.
A fresh private VM passes four original jobs: seven geometry requests off/on/off
and the endpoint regression. Checks include two large fragments, `POLLOUT`
below the actual fragment threshold, prefix admission despite that threshold,
full-buffer `EAGAIN`, queued-paused `EBUSY` and reset preservation. Diagnostic
output reports the forty-ms assumption explicitly.

The same VM then completes twelve original finite-source/export jobs in three
arms: whole 32-KiB fragment writes, 128-KiB requests with whole-fragment space
admission, and the 6,016/128 versus 8,192/2,048 control. Both large-source arms
use the 16,384/8,192 tuple against a 6,016/128 peer. The oversized arm makes
63 partial returns, accepting at most 65,536 bytes per syscall; all source totals
drain exactly. Decoded source rates are approximately 0.9990/0.9980,
1.0001/1.0001 and 0.9994/0.9979. Current runtime, HDA safety, heap/policy,
original controller/VM exits and all four filesystem checks pass. These twelve
jobs are separate from the four ABI jobs.

**Uninterrupted playback acceptance fails:** delivery counters record 1,280,
1,280 and 1,024 reserved bytes across those three windows, with a few quiet bins
in the large-write captures. Safe reservation is still an inserted gap. Near
nominal source progress and clean driver fault guards do not waive it. The
separate `large-control/performance-gate.json` retains this failed gate while
`summary.json` records successful lifecycle and data checks.

Source-credit observations every 100 ms bound first observed completion at
about 0–102 ms after the first write. Tail completion after the last accepted
large-source write is bounded at 302–403 ms for whole fragments and 336–437 ms
for oversized requests. These are guest source-credit bounds, not physical
speaker onset or cold-boot latency; the polling uncertainty is included. They
make the larger software backlog visible rather than inferring latency from
the hardware target. Query duration/gap aggregates and admission details are
retained in `large-control/latency-and-admission.json` under review
`unattended-refill-production-fixed-mlfq-a-20261021`.

A fresh real Firefox/Quake/load cohort was then run as described below. No
default is promoted, and earlier accounting clamps remain failed evidence.

### Real Firefox geometry regression and matched control

The prospective planner fails real-application acceptance despite passing the
finite-source controls. Review `unattended-refill-mixed-fixed-mlfq-a-20261021`
uses the frozen kernel above, eight vCPUs, 8 GiB, the physical PipeWire route
with a 20-ms host buffer, HDA callback mode and the unchanged 12-KiB HDA lead.
Firefox negotiates physical capacity/fragment 6,016/128. Its first solo window
delivers about 0.685 nominal source completion, then stops at 643,374 frames
and media position 14.588979 seconds. All seven later windows have no Firefox
completion or video callbacks. Its stream is enabled, completely drained and
has no discontinuity. Quake continues near nominal. Full media transfer and
browser looping both fail. All nine original guest jobs, current runtime,
native HDA fault guards, heap/policy, owner exit and four filesystem checks
pass; these do not waive the failed application gate.

A matched control, `unattended-refill-original-fixed-mlfq-a-20261021`, keeps the
same kernel/modules/apps and host configuration. It configures Firefox while
the opt-in mode is off, verifies the original physical 4,416/64 geometry, then
enables the guarded horizon before the first capture. Quake still configures
with the mode on and obtains 8,192/2,048. The original Firefox endpoint remains
unchanged across the eight phases:

| Phase | Firefox completion / nominal | Quake completion / nominal |
| --- | ---: | ---: |
| Firefox alone, horizon on | 1.000 | — |
| Both, on | 0.904 | 1.000 |
| Both, off | 0.458 | 0.775 |
| Both, on again | 0.922 | 0.999 |
| Eight-worker load, Quake focused | 0.914 | 0.999 |
| Eight-worker load, Firefox focused | 0.935 | 1.002 |
| Firefox after closing Quake, off | 0.998 | — |
| Quake reopened, off | 0.458 | 0.780 |

All nine original jobs, full transfers, error-free browser loop, native audio,
heap/policy, owner exit and four filesystems pass in this control. Runtime
acceptance fails one 17.159-μs clamp with one owner transition and later
catch-up; final debt is zero. Neither the completion deficits nor that clamp
are waived. The comparison implicates the changed physical geometry in the
stall; it does not identify a specific bridge or browser defect.

The ALSA plugin retains a separate roughly 4,410-frame logical ring. Enlarging
physical OSS capacity does not by itself increase the producer's usable ALSA
backlog. With 6,016 physical frames, physical free space can exceed the native
128-frame writable threshold even when the logical ring is full. This is a
readiness compatibility lead. Permanent readiness alone does not explain why
production remains stopped after the queue drains.

The new `tools/alsa-oss-pkg/test_playback{,_plugin}.c` fixture exercises the
unchanged production plugin through real libasound conversion and sustained
`writei`/availability/delay/start/drain calls. Sixteen cases compare both
physical geometries with 22.05/44.1/48/96-kHz integer/float clients, delayed
completion, oversized/whole-buffer writes and partial returns. Accepted native
frames drain exactly with no logical-ring overflow. Both normal and optional
low-water package builds pass the fixture, existing callback checks and thirty
negotiation cases. This weakens the proposed logical-overfill mechanism;
it does not model a poll-driven browser backend or establish audible continuity.
The optional low-water patch retains its known partial-rate readiness failure
and remains disabled.

A Firefox-only enlarged-geometry repeat, review
`unattended-refill-mixed-fixed-mlfq-b-20261021`, stalls at 538,448 frames and
media position 12.209705 seconds. Its post-stall two-second profile has 1,362
samples, of which 1,058 belong to idle, and 16,818 dropped events. It does not
prove an ongoing hot loop or reconstruct the initiating failure. The diagnostic
controller attempted export during an asynchronous profiler-start race; the
same capture was recovered after it became idle. An oversized process report
also lost output; its original job nevertheless exited zero with an empty
group, and the failure is retained. A separate bounded log read completed.
Firefox closed through its original job, shutdown and four filesystem checks
passed. This diagnostic is not a full acceptance cohort. The next discriminator
is the actual backend's polling/error transition, with origin/pointer guards
preserved and no scheduler default change.

### Full nonblocking control-pipe writes

A positively bound callback recorder narrows the enlarged-geometry stall in
review `unattended-refill-callback-files-fixed-mlfq-a-20261021`. The first-write
marker identifies actual playback, not just a device probe. A hashed 206,150-byte
process log shows 2,970 write attempts and roughly 75,000 callback events before
refill stops at 646,302 accepted/completed native frames (14.655374 media
seconds). The recent writes succeed, with no non-`EAGAIN` callback errors.
The writer stops querying the pointer while another thread continues querying
zero delay. ALSA's last hardware pointer is 641,892, exactly its 4,410-frame
logical ring behind accepted application frames. Six original jobs, complete
log export, shutdown and four filesystems pass. The recorder perturbs execution
and retains sampled tails; it does not prove the writer's blocking call.

The [release-tagged Firefox 150.0.1 Cubeb ALSA backend](https://hg.mozilla.org/releases/mozilla-release/raw-file/FIREFOX_150_0_1_RELEASE/media/libcubeb/src/cubeb_alsa.c)
writes wake bytes for both processing and running state transitions and reads
one control byte per ready loop. Its control pipe is explicitly nonblocking.
An always-ready audio descriptor can fill that pipe. Brook's pipe-write branch
nevertheless ignored `O_NONBLOCK` on a full queue and registered a blocking
writer. A separate syscall watch started after the stall records no entries;
it is not a capture of the blocked call.

The bounded `scripts/workloads/pipe_nonblock.py` probe confirms the kernel bug
independently. It fills the declared capacity minus one, then attempts a
two-byte nonblocking write. A reader scheduled 100 ms later releases broken
kernels, so the probe leaves no orphan. Both `F_SETFL` and `pipe2` report flags
2049, yet the old native kernel waits about 100.8/100.6 ms and returns two bytes
only after that reader. Linux returns `EAGAIN` in about 12/10 μs before the
reader. Brook does not implement `F_GETPIPE_SZ`; the first probe's failure is
retained, and the corrected native command explicitly uses the kernel's
configured 65,536-byte capacity. This does not add a fictional capacity ioctl.

The actual production pipe-write branch is now shared with its hosted test
through `pipe_write.inc`. If a write accepts no bytes, it checks `O_NONBLOCK`
under the descriptor-table lock and returns `EAGAIN` before waiter registration.
`F_GETFL` and `F_SETFL` take that same lock. Successful/partial returns, empty
writes and broken-reader `SIGPIPE` behavior retain their existing behavior.
No lock spans the pipe copy or blocking wait. The actual-body test fails two
assertions before the guard and passes afterward. Additional cases cover the
socketpair write direction, preservation of an existing waiter, a bounded
two-writes/one-read self-pipe loop, and ordinary blocking writer progress.
Small-write `PIPE_BUF` atomicity, multiple blocking waiters, interrupted-writer
cleanup and shared open-file-description flag aliases remain separate limits.

The first candidate kernel
`f3d6df1a3a99e9de52b2e02ad2643b716647071bac3716d5c5cf65defd82e39e`
returns `EAGAIN` in about 10/12 μs for both native probe modes. Its fresh real
Firefox/Quake/load cohort retains the same enlarged Firefox 6,016/128 physical
tuple, original application packages, host audio configuration and driver
modules, with the optional callback recorder absent. Firefox completes and
loops the full 120-second clip. All eight windows have positive audio/video
progress; all nine original workload jobs, the separate pipe job, full media
transfers, native audio fault/discontinuity checks, heap/policy, shutdown and
four filesystems pass. This removes the geometry-triggered permanent freeze;
it is not complete audio/performance acceptance.

That candidate still fails runtime acceptance with three clamps, a 7.392-μs
maximum debt, one owner transition and later catch-up to zero final debt.
Firefox completion is about 0.935/0.937 with Quake about 0.996/0.992 in the two
guarded mixed windows. Under eight-worker load they fall to roughly 0.81/0.87.
These deficits and earlier failed gates remain open. The final matching
`F_GETFL`/`F_SETFL` locking companion passes all 76 hosted suites, stable two-pass
symbol generation and the thirteen ABI-34 module import checks. The default
scheduler and audio experiment flags are unchanged.

The final kernel
`44b64074d04aa396495b7e00c52787133b692fe4fb68e0366c5afc7666018020`
passes both native pipe modes before the delayed reader (about 9 μs and
395 μs observed elapsed time; this is an API test, not a pipe benchmark).
Review `unattended-pipe-nonblock-final-fixed-mlfq-a-20261021` repeats the full
mixed workload without the optional callback recorder. The independent pipe
job and all nine workload jobs complete with original identities, output and
empty groups. All media transfers, the error-free 120-second browser loop,
eight positive-progress windows, native audio fault/discontinuity checks,
heap/policy, original controller/VM exit and four filesystem checks pass.

Full timing acceptance still fails: eight clamps occur in one 13.473-μs maximum
debt episode with four owner transitions and later catch-up to zero final debt.
The two guarded mixed windows give Firefox about 0.854/0.864 and Quake
0.916/0.937 nominal completion. Loaded windows give about 0.890/0.872 and
0.927/0.930. These are separate cohorts with variable source completion;
they do not establish a throughput benefit or cost for the flag-lock companion.
The permanent freeze is absent in both fixed-kernel cohorts, while the runtime
and underfeed failures remain. The separate logical ALSA ring still has no
extra refill reserve merely because physical OSS capacity increased. That
negotiation/readiness problem and the broader clock/scheduler audit remain open.
