# Guarded shared-prefix batching experiment

`audio shared-prefix on|off|reset|status` controls a default-off experiment for
short positive audio queues. It targets the accepted short-tail losses measured
by [contribution accounting](audio-mix-contributions.md). It introduces no sleep,
producer wait or immediate remainder-submission loop. Driver guards, mixer FIFO
ownership, format and scheduler defaults remain unchanged.

## Decision and headroom

Preparation first applies the ordinary maximum batch and pending budget limits.
With at least two enabled, nonfaulted positive software queues, it may shorten
the request to their shortest shared contribution. Empty queues add no limit.
Shortening requires the fixed native intel_hda experiment and fresh, running,
nonfaulted progress in the current hardware epoch. Physical outstanding bytes
must cover:

```
12288 + 4 * ceil((snapshot_age_ns + 10000000) * 44100 / 1000000000)
```

Age is bracketed from before the existing driver progress call through locked
preparation and must be no more than ten ms. Invalid clocks, stale progress,
insufficient lead and unsupported drivers retain ordinary batching. This is
headroom for one **nominal** service opportunity, not a guarantee of execution
within ten ms. Native publication guards remain authoritative after the decision.

The 12-KiB requirement is a contract of the unchanged driver used in this trial;
a driver-name match alone cannot establish another module's lead. Native evidence
must pin its exact hash. Coalescing stays off in this experiment.

Only the shortened request is prepared and submitted. Unsubmitted surplus stays
in the source FIFO. Rejection preserves the normal budget; partial acceptance
subtracts only accepted frames from it, allowing a subsequent low-headroom call
to restore ordinary batching. A fully accepted shortened request clears that
attempt's budget, so later refills cannot be constrained by an artificial tiny
tail. The captured attempt determines completion semantics even when debug
controls reset or disable the experiment during copying.

Counters distinguish candidates, shortened attempts, accepted shortened receipts
and accepted-prefix frames. `requested_tail_frames` is planned unsubmitted
surplus per accepted shortened receipt; it can count repeated plans and is not
counterfactual driver acceptance or proof of avoided loss. Headroom/clock/driver
bypasses remain visible. Tokens fence stale counter updates across controls;
overflow/exhaustion is explicit. No extra clocks or driver calls occur with both
batching experiments off, though disabled condition checks remain.

## Tests and review

Production mixer/HDA tests arrange the same completion-driven refill with the
feature off and on. Baseline uses 1,920 output frames for two 1,024-frame source
contributions, with a missing tail followed by an unmatched refill. The candidate
mixes both into 1,024 output frames. Both modes submit/complete exactly the same
source frames, including the priming data, and preserve DMA sample values.

Additional actual-body tests cover a producer that never refills, tiny prefixes
with low-headroom fallback, exact lead boundaries, stale progress, snapshot-age
budget, partial/rejected receipts and debug-control races. Review identified a
short-retry cap that prevented ordinary fallback; six assertions fail before the
budget correction and pass after it, with exact drains and zero reservations.
The concrete 512→448 accepted-prefix case verifies that new refills escape the
otherwise artificial 64-frame cap, including reset/off during copying.

Astra reviews both the algorithm and final retry/completion rules. All 74 hosted
suites pass after a successful fail-fast full build; final completion-budget
additions also pass the affected suite. The separate
[debt-test verification correction](runtime-debt-observation.md#verification-correction)
records why earlier aggregate green reports were insufficient for its final
fixture edge cases. Native two-pass linking and independent symbol-table checks
pass with thirteen unchanged ABI-34 modules/109 exports.

## Native acceptance rule

Use the same frozen kernel for independent-writer off/on/off trials, four
4096-byte fragments, unchanged write requests/retry loop, configured barriers,
source and physical audio backend. Require substantial shortening exposure,
reduced short-tail loss and improved per-stream source progress together, with
exact drains and no increased inactive output, HDA reservations or faults.
First-write phase variance and submission-versus-playback windows remain explicit.
If exposure is substantial without useful benefit, stop this batching approach
rather than adding further grace conditions. A passing finite-source test still
needs real browser/game acceptance before any default change.

## Native off/on/off result

One fresh private MLFQ VM uses the same frozen kernel throughout the three
cases: eight vCPUs/eight GiB, host mask 0–15, blocking AP accounting, physical
PipeWire callback audio with requested 20-ms buffering, four 4096-byte producer
fragments and configured HTTP barriers. Coalescing, history and expensive timing
probes remain off. Sources are pregenerated; source bytes, individual 4096-byte
writes, retry loop and left-first job submission stay fixed.

| Setting | Left / right nominal completion | Whole-source drain | Left / right decoded source/PCM rate | Left / right quiet bins |
| --- | --- | --- | --- | --- |
| Off A | 0.9118 / 0.9106 | 13.121 / 13.120 s | 0.8987 / 0.9019 | 6.76% / 6.46% |
| On B | 0.9489 / 0.9493 | 12.804 / 12.812 s | 0.9549 / 0.9572 | 3.57% / 3.04% |
| Off C | 0.8594 / 0.8580 | 14.439 / 14.449 s | 0.8634 / 0.8682 | 10.60% / 10.37% |

Nominal rates are completed frames per guest second divided by 44,100. Captured
packet decoding independently follows the progress pattern, with pitch near
one. Stored PCM covers 100.048%, 99.948% and 99.993% of host brackets. Those
near-full values have capture start/stop granularity; they do not prove exact
inactive-output equivalence or physical speaker timing. No chunk timestamps
are available from this capture backend.

The enabled capture exercises 58 accepted shortened receipts, 17,216 accepted
prefix frames and 9,664 planned tail frames. Of 256 candidates, 198 bypass for
headroom; there are no clock/driver bypasses. Whole-source totals are 161
accepted shortened receipts and 46,032 accepted prefix frames. This is actual
exposure, not merely a flag-enabled negative test.

Missing-short fractions of each stream's accepted output fall from
8.36%/8.62% to 4.72%/4.61%, then rise to 13.96%/13.57% after disabling. The
corresponding missing-empty fractions are 0.71%/0.43%, 0.07%/0.31% and
0.12%/0.33%. All accepted-contribution and histogram identities hold exactly
with fixed diagnostic epochs and no omissions, invalid records or saturation.
Submission counters enclose the PCM capture but lead its playback window;
these fractions are not sample-aligned explanations of every quiet bin.

Actual source start spreads are 0.91/498.23/497.23 ms. Similar large spreads in
the enabled and final disabled cases strengthen the narrow comparison, but
finer refill phase and run-to-run variance remain confounders. This is one
positive cohort, not a statistical estimate or real-app acceptance. Most
candidates still bypass on low lead; do not reduce the safety margin from this
result or claim a complete audio fix.

Every source submits and completes exactly 529,200 frames. All twelve original
source/report jobs retain collected exit zero, complete identity/output/EOF
and empty groups. The original controller and VM owner exit zero; all four
filesystem checks, heap, runtime/debt, policy-ledger/ready-count and IRQ gates
pass. HDA reservations are one/zero/zero (256/0/0 bytes), with no invalid,
unsafe, contended or saturated delivery records and no native audio faults.
Earlier failed application/runtime cohorts remain separate.

Frozen kernel SHA-256:
`dab582b6bcf322ad4a6ba5977e9affd46175bd1028a641c5264510b9306e8566`.
All thirteen original ABI-34 modules are unchanged. The pinned HDA module is
`f05fdc31f3ea5c1737dfe732c3acc2dc4ca6820217d8be9e4253d9bea83e1b3e`.
The initial provisional kernel is preserved as an unrun artifact; this final
kernel includes the reviewed retry and completed-short-request rules.

Checked evidence is retained under
`build/reviews/unattended-shared-prefix-mlfq-a-20261021/shared-prefix-control/summary.json`,
including exact sources, counters, endpoint queues, PCM/decodes, module identity,
original receipts and filesystem checks. The next gate is a controlled real
Firefox/Quake comparison. The experiment remains off by default.


## First real Firefox/Quake off/on/off cohort

A fresh private VM repeats the same kernel, thirteen modules, 12-KiB HDA lead,
MLFQ, eight vCPUs/eight GiB, host mask 0–15 and physical PipeWire callback path.
Firefox plays a generated 640×360 H.264/AAC clip; Quake uses GL3 at 1280×720,
attract mode, timedemo disabled and a 60-FPS cap. The same browser and first
Quake process stay alive through the three main captures. Their scenes advance;
this is a reversal test, not identical-frame replay. Buffered SDL swap tracing
and contribution diagnostics are identical in all three cases. The private
page only increases retained trace capacity from 720 to 1,440 samples; its
250-ms sampling and clock brackets are unchanged.

| Phase | Firefox / Quake nominal audio completion | Firefox media progress / page second | Firefox video callbacks / page second | Quake swaps / guest second |
| --- | --- | --- | --- | --- |
| Both, off A | 0.6286 / 0.5441 | 0.6348 | 6.41 | 7.80 |
| Both, on B | 0.8512 / 0.6994 | 0.8513 | 5.76 | 7.91 |
| Both, off C | 0.6164 / 0.5375 | 0.6171 | 5.13 | 8.93 |
| On, eight CPU workers, Quake focused | 0.8710 / 0.7010 | 0.8655 | 6.05 | 8.29 |
| On, same CPU workload, Firefox focused | 0.9229 / 0.7325 | 0.9218 | 5.67 | 8.28 |

Both four-second loaded capture brackets lie inside every worker's bounded
thirty-second work interval. These enabled-only phases have no matched disabled
load control, so they cannot establish a benefit under load. Main captures are six seconds. Completion counts
source frames, not necessarily their decoded musical tempo. Page media progress
independently follows Firefox's audio improvement; callbacks and swaps are not
scanout, and do not show a clear video/frame-time improvement. Quake p95 frame
times are 168.0/200.6/151.3 ms across the three main phases.

The enabled main capture accepts 287 shortened receipts and 119,764 prefix
frames; 25 of 312 candidates bypass for headroom. No clock or driver bypasses
occur. Firefox missing-short share falls from 28.28% to 2.19%, then returns to
28.19%; missing-empty share is 7.85%/10.34%/10.54%. Quake missing-empty share
falls from 45.27% to 27.47%, then returns to 46.24%; its missing-short share is
0.11%/0.98%/0.00%. Every contribution and histogram identity holds with fixed
epochs, no omitted rows, invalid records or saturation. These submission
windows enclose but precede the corresponding PCM playback windows.

This is **not full acceptance**. HDA reserved bytes are 43,480/34,640/0 across
the main off/on/off windows; reservation events are 5/10/0. Fewer enabled bytes
than off A but more events is not a consistent improvement. Improved audio
progress does not establish reservation-free delivery or uniformly reduced inactive output. Across all
eight captures there are 0–61,336 reserved bytes. PCM covers approximately
99.92–100.07% of host intervals, subject to capture start/stop granularity.
There are no native audio faults or invalid/unsafe/contended delivery records.
Firefox alone progresses at 0.9747 before Quake and 0.9570 after its closure.

During the final, disabled Quake-reopen capture Firefox reports
`SampleIterator::GetNext(): Sample data byte range beyond end of resource`,
first recorded around page time 143.261 seconds / media time 110.796 seconds.
The generated clip is 120.028 seconds long and decodes completely on the host
without errors. That establishes host-file decodability, not guest HTTP byte
identity or Firefox demux correctness. The analysis retains the bad samples
and marks this browser window invalid; its apparent rates cannot satisfy the
reopen acceptance gate. Earlier main and loaded windows have no media error.

All eight original guest jobs have collected exit zero, complete identities,
output/EOF and empty groups. The original controller and VM owner exit zero;
all four filesystem checks, heap, runtime/debt and policy ledger/ready-count
checks pass. Cleanup-only history records 1,991,421 events without a correction;
this is still negative evidence, not a captured positive debt episode. Historical
runtime failures remain open.

Evidence:
`build/reviews/unattended-mixed-prefix-mlfq-a-20261021/mixed-prefix-control/`,
including `summary.json`, `verification.json`, `contribution-metrics.json`, raw
browser trace, windowed swaps, PCM, original receipts and fixture identity. A
fresh identical repeat is required before drawing stronger conclusions. The
1.1-second transition warmup is fixed across modes, but this cohort does not
explicitly prove that all pre-switch PCM has drained. Astra recommends a further
on/off/on order control with measured transition draining and comparable media
positions, plus separate guest-byte/media-tail validation. Neither
the mixer experiment nor the scheduler default changes from this result.


## Fresh real-app repeat

A second fresh private VM repeats the identical source, kernel/modules,
backend, diagnostics, application settings and phase order. Main audio
completion is again Firefox 0.6145→0.8388→0.6148 and Quake
0.5379→0.6799→0.5362 across off/on/off. Firefox page media progress agrees
with its audio improvement. The enabled capture accepts 324 shortened receipts
and 130,070 prefix frames, with eleven headroom bypasses. Firefox missing-short
share is 28.53%→1.97%→28.28%. Main HDA reserved bytes/events are
0/0→5,608/1→0/0: reservations persist rather than improve consistently.
Video callbacks (5.82/5.30/5.77 per page second) and Quake swaps
(9.04/8.72/8.21 per guest second) still show no clear reversible improvement.

Enabled-only loaded audio completion is 0.9198/0.7380 with Quake focused and
0.9201/0.7356 with Firefox focused. Firefox alone recovers to 1.0003 after
Quake closes. All eight browser windows have valid sampled states; no media
error is recorded through page time 149.638 seconds / media time 113.399
seconds. The earlier read failure remains unexplained, and neither trial
establishes complete playback or looping of the 120-second resource.

The repeat **fails runtime acceptance**: one 16,726-ns service clamp occurs on
CPU 4, PID/incarnation 163, at guest wall time 88,834,069,969 ns, between the
Firefox-alone and first mixed snapshots, before the batching experiment is
enabled. Wall advance is 28,567 ns while reported steal advances 45,293 ns.
Debt diagnostics record one episode, one catchup and one owner transition,
with zero known debt at the following snapshots. Catchup does not prove correct
cross-owner attribution. Cleanup-only history starts later and does not capture
the event; it cannot explain the positive episode. No tolerance is introduced.

All eight original guest jobs, controller and VM owner exit zero, with complete
identity/output/EOF, empty groups, four clean filesystem checks, heap and
policy ledger/ready-count checks. Those clean lifecycle results do not clear
the clamp or the earlier browser error. The evidence lives under
`build/reviews/unattended-mixed-prefix-mlfq-b-20261021/mixed-prefix-control/`.
The two repetitions strengthen the narrow audio-progress finding, but full
acceptance and all default changes remain withheld. The next diagnostic enables
runtime history before the first Quake launch, separately from performance
comparisons, to seek the missing positive recurrence and ownership trace.


## Reverse order, observed drains and a complete browser loop

A further fresh VM reverses the order to on/off/on, keeping the frozen kernel,
all thirteen modules, policy, physical backend and application settings fixed.
This is a **separate fixture cohort**: it adds an independent guest HTTP
length/hash probe, explicit 60-second per-write media timeout and transfer
accounting. The historical server's ten-second write timeout remains the
command-line default. Source bytes and page sampling are unchanged. Guest
fetching before Firefox may warm host/source state, so this cohort is not pooled
with the earlier off/on/off measurements.

After each mode control, a source snapshot captures every current stream's
submitted-frame target. Later snapshots must show those targets completed in
the same identities/epochs, with no discontinuities, before the usual 1.1-second
warmup. The three main transitions observe completion in roughly 618–626 ms;
capture starts a further 1.75–1.78 seconds later. These are observed cached
source-credit boundaries, not physical speaker latency guarantees. They replace
an assumed drain with checked FIFO progress.

| Setting | Firefox / Quake nominal audio completion | Firefox media / page second | Firefox video callbacks / page second | Quake swaps / guest second |
| --- | --- | --- | --- | --- |
| On A | 0.8376 / 0.6858 | 0.8344 | 5.39 | 7.71 |
| Off B | 0.6279 / 0.5437 | 0.6255 | 5.22 | 7.70 |
| On C | 0.8421 / 0.6833 | 0.8393 | 5.20 | 8.22 |

Both enabled main captures exercise substantial shortening: 314/340 accepted
receipts, 127,820/136,570 accepted prefix frames and 19/18 headroom bypasses.
Firefox's missing-short shares are 3.07%/29.76%/2.06%. Quake's missing-empty
shares are 27.91%/43.98%/30.67%. All contribution and histogram identities hold
without omissions, invalid records, epoch changes or saturation. This reversed
response strengthens the mixer finding despite advancing attract/video scenes.
It is not proof of a frame-pacing improvement: Quake p95 times vary
239.4/192.0/164.6 ms, and video callbacks barely change.

HDA reservations remain a failed performance requirement: main reserved
bytes/events are 64,696/9, 9,856/4 and 6,984/3. There is no consistent
reservation-free or inactive-output improvement. Enabled-only loaded audio
completion is 0.9110/0.7286 and 0.8786/0.7064 with Quake/Firefox focus;
these still lack matched disabled load controls. Guards and lead margins stay
unchanged; this result does not justify adding waits or reducing headroom.

The independent guest download receives exactly 28,013,968 bytes with SHA-256
`0ad4cc105e1600a91c686d8517ca4f059411c2cfc2d6c907d0206e8197a47aed`.
Its HTTP 200/full-file response explicitly ignores the requested range. That
verifies the probe's bytes, not Firefox's separate channel/cache. Firefox's own
GET independently completes full-length matching server writes over about
75.44 host seconds. Both logged media GETs reconcile with two terminal
transfer records. The running server predates the later handler-joining fix;
this reconciliation is essential to exclude missing daemon-thread records.
Successful writes alone still do not prove guest receipt.

Firefox subsequently plays through the 120.028-second resource and loops,
with a retained position of 119.9995 seconds before wrap, no trace errors or
ambiguous gaps, audio enabled, playbackRate one and readyState four. All eight
measured browser windows are valid. Combined with the two earlier timeout/error
coincidences and the timeout-free repeat, this supports fixture truncation as
a cause to investigate rather than attributing the old demux errors to Brook.
It does not causally prove every earlier failure or certify general seeking,
all media formats or smooth playback.

All nine original guest jobs have exit zero, complete identities/output/EOF
and empty groups; original controller/VM owner exit zero, all four filesystem
checks, heap, current runtime/debt, policy ledger/ready-count and delivery
validity gates pass. Older positive runtime clamps remain failed. Evidence is
`build/reviews/unattended-mixed-prefix-order-mlfq-a-20261021/mixed-prefix-control/`,
including checked source drains, full-transfer reconciliation, the browser loop,
raw trace/PCM and original receipts. Full audio/mixed-desktop acceptance remains
incomplete, and both mixer and scheduler defaults are unchanged.
