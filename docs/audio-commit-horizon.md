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

The matrix has 156 configurations:

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
No new guest command, runtime flag, native state or default change is introduced.
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
