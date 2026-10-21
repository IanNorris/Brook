# Timer ready-lock site attribution

The [direct OSS SMP controls](direct-oss-smp-pressure.md) motivate separating
initial IRQ accounting from idle dispatch. A matched recording-off/on pair
on the first labelled kernel retains poor audio progress. Its bounded trace
provides positive initial-accounting wait observations, **not evidence for
changing idle-dispatch admission**. No scheduling or audio defaults change.

Both private cohorts use kernel SHA-256
`34ac8f8a4b3bf1565455d2c69e5a15bde7ff5a240856282c7194704cf26d01ab`,
the same thirteen original modules, MLFQ/AP try, eight vCPUs, 8 GiB, physical
PipeWire callback audio with 20-ms buffering, original 12-KiB HDA lead and
four 4,096-byte OSS fragments. The forty-second packet-coded source generates
its samples before device startup, then warms up ten seconds. Eight host
workers run for twenty seconds on CPUs 0–7, sharing the vCPU masks; named
I/O-helper exceptions retain CPUs 0–15. History, CPU profiling and expensive
timer/ready/watch probes remain off. Only the second cohort enables the
existing bounded ready-lock trace around pressure.

| Cohort | Loaded nominal completion / guest second | Source outcome | Aggregate vCPU host execution over roughly 19.9 s |
| --- | ---: | --- | ---: |
| Recording off a | 0.3008 | EPIPE after 22.173 guest s; 723,862 frames submitted; final completion unknown | 5.41 CPU-s |
| Recording on a | 0.04435 | Exactly 1,764,000 frames drain in 59.012 guest s | 0.80 CPU-s |

Both complete audio-query host brackets and WAV captures lie inside every
worker's measured execution interval. The off cohort subsequently records
a native `sample-gap` fault across approximately 185.66 ms. Its original
writer exits 1, with identity, complete output and empty group verified. The
failure report is exported, no stream remains, and normal shutdown succeeds.
This failed source gate is preserved; successful cleanup is not acceptance.

The on cohort leaves no stream after exact drain and report export. Both final
cumulative runtime snapshots have zero invalid/backwards/clamped/stale and
saturation counters. Policy/ledger totals agree, IRQ IF/stale-ticket errors
are zero, and heaps pass. Both original VM owners exit zero and all eight
filesystem checks pass. The source outcomes and host-execution difference
show substantial run or observer sensitivity. One off/on pair cannot quantify
a causal instrumentation cost or establish equivalent timing behaviour.

## Retained trace

The on cohort stops recording before final export. Epoch 4 is unchanged,
active mask is zero, and 174,893 lifetime completions report zero lost or
clock-error counts. The final snapshot contains 24 unique completed records
after deduplicating current/max-wait/max-hold slots. All belong to the stopped
capture epoch and are consistent, with valid clocks, monotonic raw pairs and
no analysed wait/hold bracket whose steal increment exceeds elapsed time.
Caller strings resolve against the exact frozen ELF. Coverage is bounded;
these are not all acquisitions, and zero lost publications does not establish
complete event history.

Three initial-accounting waits lie wholly inside the guest-clock audio
interval whose complete host brackets are contained in pressure:

| CPU | Ticket | Observed wait | Reported steal increment | Elapsed minus reported steal |
| --- | ---: | ---: | ---: | ---: |
| 0 | 2,629,012 | 10.989760 ms | 10.943017 ms | 46.743 µs |
| 3 | 2,625,856 | 17.556712 ms | 17.514630 ms | 42.082 µs |
| 6 | 2,621,305 | 9.007822 ms | 8.978968 ms | 28.854 µs |

These are reservation-to-acquisition **observation brackets**, including
publication delay and delayed execution after ownership becomes available.
The subtraction is not exact guest CPU time. A long waiter does not establish
a long holder or identify which ticket delayed it. Most elapsed time in these
records is reported steal; this is not a multi-millisecond lock-work claim.

Seven retained records name initial blocking accounting; three overlap the
known loaded interval. Eight name `irq-idle-dispatch`, with maximum observed
wait 159 ns, but all eight are outside that interval. Their absence from
loaded maxima cannot exclude earlier dispatch waits or estimate exposure.
Other retained sites include yield, unblock and kernel-region boundaries with
observed waits up to approximately 19 ms. Two conditional ticket-neighbour
matches bound unlock-to-acquired observation at approximately 1 ms and
32–120 ns. Missing history and uint32 ticket cycles prevent treating those
matches as a complete ownership chain.

The checked reducer classifies complete wait/hold intervals as inside,
overlapping or outside the native audio window, retaining clipped overlap
separately from full durations. Partial records would remain excluded from
completed-duration summaries. Neither site counts nor maxima measure total
exposure or prove that the observed waits explain the entire playback stall.

## Next classification

The initial blocking label still mixes BSP, AP idle, AP user and nonperiodic
entries. AP CPU numbers alone do not establish idle accounting: the source
thread can migrate. Refined labels now use the **same captured admission
inputs**—CPU, periodic flag, user-mode flag, current-process presence and idle
identity—without rereading process state:

- `irq-accounting-bsp`
- `irq-accounting-ap-idle`
- `irq-accounting-ap-user`
- `irq-accounting-ap-kernel`
- `irq-accounting-nonperiodic`
- Generic `irq-accounting-blocking` for missing/contradictory context.

This changes diagnostic names only. It adds no clock read, lock operation or
admission choice; idle dispatch and successful initial try retain their labels.
The refined kernel builds through both symbol-table passes, the independently
regenerated table matches the final ELF, and all thirteen preserved modules
resolve against 109 exports with ABI 34 unchanged. Its ignored
`irq-context-sites-frozen` capsule has kernel SHA-256
`0e1d8a35f305ac8e7c37365b71dc1404d39b0dc4d7f6cdda9ec494a4b2c63664`.
The subsequent native pair below supplies a positive AP-idle initial-accounting
wait. That supports auditing an idle-specific intervention; it does not justify
broadening idle-dispatch admission or moving samples without an ownership audit.

Ignored raw evidence lives under
`build/reviews/unattended-irq-sites-{off,on}-a-20261021`. Failed-source cleanup
and the original nonzero exit remain in the off cohort. The checked aggregate
is `build/workloads/timing-scheduler-20261021/irq-sites-summary.json`; raw
snapshots and their `.site-analysis.json` reductions retain epochs, tickets,
clocks and exclusions. `reboot-resume/analyze-irq-sites.py` resolves and reduces
the bounded records. Earlier clamp failures, historical acceleration and
broader scheduler/HDA acceptance remain open.

## Refined-context native pair

The refined `0e1d8a35…` kernel subsequently completes matched recording-off/on
direct-OSS cohorts with the same modules and workload. Both sources submit and
complete exactly 1,764,000 frames, leave no stream and pass their final runtime,
policy/ledger, IRQ IF/stale-ticket and heap gates. Both original VM owners exit
zero and all eight filesystem checks pass. History remains off.

| Cohort | Loaded nominal completion / guest second | Source drain time | Aggregate vCPU host execution over roughly 19.9 s |
| --- | ---: | ---: | ---: |
| Refined recording off a | 0.08879 | 58.888 s | 4.48 CPU-s |
| Refined recording on a | 0.07519 | 57.595 s | 2.15 CPU-s |

Both WAV captures and complete audio-query host brackets lie inside every
worker's observed execution interval. Playback remains poor in both runs.
Their variation and different host execution retain observer sensitivity; this
is not a measured causal overhead or general performance acceptance result.

The stopped on snapshot retains 24 complete unique records at epoch 4, with
208,803 lifetime completions, active mask zero and no lost/clock-error,
inconsistent, old-window, partial or invalid-clock rows. No analysed wait/hold
bracket has an excessive steal increment. Four retained records identify
periodic AP-idle initial accounting. One is wholly inside the native audio
interval **112.928720703–121.296464741 seconds**:

- CPU 6, ticket 3,807,726, local serial 4,838, epoch 4.
- Reserved 118.030635197 s; acquired 118.050616396 s.
- Observed wait **19.981199 ms**, reported steal **19.925516 ms**.
- Elapsed minus reported steal **55.683 µs**, not exact CPU time.
- Acquired-to-release-before hold bracket **584 ns**, with zero reported steal.

This is positive AP-idle initial-accounting wait evidence during poor source
progress. It does not establish that the lock holder spent twenty milliseconds
working or that this site explains the entire stall. The same snapshot retains
an AP-user wait of 17.998960 ms and a nonperiodic wait of 18.991843 ms inside
that interval. Five retained idle-dispatch records have a maximum wait of
1.506 µs, all outside it; their absence inside the interval cannot exclude
unretained dispatch delays.

Raw evidence lives under
`build/reviews/unattended-irq-context-sites-{off,on}-a-20261021`; the checked
aggregate is `build/workloads/timing-scheduler-20261021/irq-context-sites-summary.json`.
The matched ELF resolves the labels, and final stopped reductions remain beside
the snapshots. The earlier failed off source on `34ac8f8a…` remains failed.

## Narrow candidate contract

The next experiment implements **default-off coalescing of periodic AP-idle
accounting**, leaving idle dispatch blocking. Host tests and the native build
pass. Native accounting/lifecycle checks pass; loaded audio acceptance fails. The purpose is to remove one redundant ready-lock acquisition
per eligible idle AP tick; it does not eliminate the later acquisition or every
possible convoy.

The source audit finds that AP `DrainPostSwitch` releases references/requeues
prior tasks without voluntarily switching; optional AP wake scanning unblocks
tasks without switching its caller. With incoming IF clear, these paths preserve
the CPU and idle owner. BSP expiry/reaping is excluded. A candidate must still
check CPU, current idle identity and interrupt state explicitly at the late
sample, with a counted ordinary-accounting fallback on any mismatch.

The candidate must satisfy the following before native acceptance:

- Eligibility requires a live AP, periodic entry, idle current process,
  non-user interruption and IF clear. BSP, user, nonperiodic, missing/changed
  owner and unsupported interrupt state retain the ordinary path.
- Skip only the initial idle accounting acquisition/sample. Preserve all
  housekeeping and the existing **blocking** idle-dispatch lock acquisition.
  Sample the same idle owner there **before `PickNextLocked`**, including the
  no-next-task outcome. Do not create retry-dependent dispatch liveness.
- Preserve `RuntimeDispatchLocked`'s outgoing sample before `SetOwner` when a
  task is selected. No idle service may become incoming task/group service or
  trigger an idle policy-service callback.
- Keep pending coalescing state local to the IRQ invocation. Runtime off must
  not discard an already pending late sample. Cumulative per-CPU entry,
  late-sample and fallback/error counts must expose bypassed paths; validate
  them after disabling new entries and allowing in-flight handlers to finish.
- Test clean monotonic task/group/idle conservation after boundary flushes,
  no-next/next and intervening-ready cases, steal changes, pending bookkeeping
  and shared wake scans. Test actual invalid/backwards/clamp handling and
  preserve every observed failed runtime gate.
- Moving the initial sample later can omit a transient invalid input or correction
  seen by eager sampling. The ordinary tick still takes a late sample and, when
  dispatching, the mandatory outgoing sample; fewer total samples are not promised. Do not promise identical diagnostics for different sample
  sequences, or treat fewer detected clamps as a repair. Preserve the ledger's
  semantics for every sample actually taken.
- Keep idle-watch tick coverage explicit if its observation moves to the late
  hold. An observer-count change is not less idle execution or useful progress.
- Compare eager/coalesced on the same kernel with recorders off, exact source
  and loaded progress gates, final runtime/ownership checks and original-owner
  filesystem gates. Retain failures and require reversal before promotion.

Neither this design nor a passing narrow control changes the scheduler default,
clears earlier clock-clamp failures or completes the broader timing goal.

## Candidate implementation and validation boundary

`sched irq-idle-accounting on|off|status` controls new admissions only; it is
off at boot. Each eligible IRQ owns a local pending token. Late CPU, idle-owner
and IF checks precede the existing blocking idle-dispatch hold; mismatches take
ordinary blocking accounting against the actual current owner and increment
fallback/error counters. Every fallback or IF/identity error fails acceptance.
Disabling the flag does not cancel an admitted token. Counters saturate and
are read independently: native validation must disable admission, drain work
and require stable reconciled rows, with positive AP exposure and zero BSP
coalescing. Flag-off alone does not prove in-flight handlers have retired.

The existing timer-stage `runtimeDeferred` observation now includes coalesced
idle accounting. Its early `accountingNs` stage excludes the moved work, which
falls in the later remainder; it cannot serve as an unchanged accounting-cost
comparison. Idle-watch observations also move to the late hold.
`late_watch_ticks` counts enabled watch calls, not accepted watched-task samples.

All 70 host suites pass, including exhaustive admission/token checks and actual
ledger conservation across idle/task boundaries, no-next/next outcomes, steal
and invalid/backwards/clamp cases. These tests do not execute the scheduler's
housekeeping wiring, pending requeues or retirement paths; native integration
and lifecycle checks remain necessary. The kernel links through both generated
symbol passes, the independently regenerated table matches, and all thirteen
preserved modules resolve against 109 exports at ABI 34. The ignored
`idle-accounting-frozen` capsule uses kernel SHA-256
`b215f9ace49f23ed577d114e7422d1b7259e8f62dc810427f335d45aea0bff9c`;
only the kernel and symbol table differ from the preserved module capsule.

The same-kernel eager/coalesced pair keeps history and lock recording
off, original 12-KiB HDA lead, MLFQ/AP try, eight vCPUs and finite direct OSS
pressure. Exact drain, useful loaded progress, final runtime/policy/ownership
and filesystem checks are separate gates. The results below do not justify promotion, and
no defaults change.

## Native coalescing experiment

Three private eager/coalesced/eager boots use the same `b215f9ac…` kernel,
original thirteen modules and pressure protocol above. Both bounded lock and
runtime-history recorders stay off. All complete audio-query host brackets
and WAV captures are inside the actual execution interval of every host worker.

| Mode | Loaded nominal completion / guest second | Finite source | vCPU host execution over roughly 19.8 s |
| --- | ---: | --- | ---: |
| Eager a | Unavailable after stream failure | EPIPE at 16.948 guest s, 654,720 frames submitted | 7.19 CPU-s |
| Coalesced a | 0.09524 | Exactly 1,764,000 frames drain in 57.920 guest s | 8.98 CPU-s |
| Eager b | 0.03269 | Exactly 1,764,000 frames drain in 58.901 guest s | 11.24 CPU-s |

The first eager stream disappears between endpoint queries. Its missing rate
is **not zero measured progress**; the failed original job exits 1 with complete
output, empty group and original identity verified. Its report is preserved.
Both successful sources finish exact four-fragment drains and leave no stream.
All three final runtime, policy/ledger, heap and IRQ IF/stale-ticket checks pass.
All original VM owners exit zero and all twelve filesystem checks pass.

The coalesced run records **381,605** admitted idle tokens and exactly as many
late samples. Every AP has positive exposure; CPU 0 has none. Three stable
disabled snapshots reconcile with zero fallback, identity or IF errors. This
shows the intended path ran, but playback remains badly underfed. Its WAV stores
1.454 seconds of PCM across a 6.054-second host capture; approximately 38% of
stored millisecond bins are quiet. Packet decoding fits pitch near 1× and source
progression around 0.688 source seconds per stored PCM second. WAV omits some
inactive delivery intervals, so this fit is not physical-host playback tempo.
Same-symbol runs separated by gaps need not indicate replay.

The bounded sample supports **no promotion as an audio fix**. It does not
quantify a general improvement or causal overhead from three variable runs.
Removing an acquisition does not remove later blocking dispatch, AP-user or
nonperiodic waits, and it does not clear historical accelerated playback or
earlier EEVDF clamp failures. Raw evidence is under
`build/reviews/unattended-idle-accounting-{off-a,on-a,off-b}-20261021`;
`reboot-resume/summarize-idle-accounting.py` independently checks and reduces
original jobs, mode exposure, counter reconciliation, native gates, observed
host overlap and original VM/filesystem ownership into
`build/workloads/timing-scheduler-20261021/idle-accounting-summary.json`.

## Native branch and lifecycle coverage

A subsequent build adds only two cumulative branch counters: `late_no_next`
and `late_dispatch`, updated after `PickNextLocked` under the existing hold,
only when that IRQ consumed a coalesced token. Its separately frozen kernel is
`12eb6d3ac07f4359699eca0ea4260fab6f1ece0c7ad3be3707f2a8a9d558dfeb`;
this is **not** the kernel used for the pressure table. Both symbol passes and
the independent table/import checks pass, as does the affected host ledger suite.

One private native boot exercises quiet idle phases, bounded CPU-clock waves
with one/eight processes and two fresh threads each, and 24 CPU workers plus
four periodic sleepers for eight seconds. It toggles coalescing off/on while
work is live and runs optional shared wake scanning, then restores BSP mode.
Both original workload jobs finish with complete output, joined/reaped groups
and verified identity. Native snapshots record:

- **179,061** late samples with no next task and **3,929** with a task selected.
- **182,990** total tokens, equal to both late samples and the two outcomes' sum.
- Positive admission on every AP, none on CPU 0, with zero fallback/identity/IF
  errors and unsaturated counters after three stable disabled snapshots.
- **90** completed AP wake scans, demonstrating actual shared-mode execution.

Final runtime, policy/ledger, IRQ and heap gates pass after job/report drain.
The original VM owner exits zero and all four filesystems pass. This exercises
real housekeeping across wake, exit, join and retirement workloads. It does not
prove every pending-requeue/retirement interleaving, force mode-off at a specific
in-flight instruction, test enabled idle-watch coverage, establish general
starvation bounds or restore useful loaded audio. The feature remains off.
Evidence lives under
`build/reviews/unattended-idle-accounting-lifecycle-on-a-20261021/idle-lifecycle`,
with `build/workloads/timing-scheduler-20261021/idle-accounting-lifecycle-summary.json`
including original VM and filesystem gates.
