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

A fresh real Firefox/Quake/load cohort is the next gate. Horizon must be enabled
before endpoint configuration for prospective negotiation, with actual stream
capacity, fragment size and low-water threshold observed. Policy reversal on
those established endpoints must preserve geometry and settle prior accepted
credits. This is distinct from a comparison with default small geometry.
Real mixed-app acceptance and historical fast playback remain open. No audio
or scheduler default is promoted.
