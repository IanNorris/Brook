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
