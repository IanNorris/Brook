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
Native context classification remains pending. Seek a positive AP-idle
initial-accounting wait before an idle-specific intervention; do not broaden
idle dispatch or move samples relative to housekeeping on this evidence.

Ignored raw evidence lives under
`build/reviews/unattended-irq-sites-{off,on}-a-20261021`. Failed-source cleanup
and the original nonzero exit remain in the off cohort. The checked aggregate
is `build/workloads/timing-scheduler-20261021/irq-sites-summary.json`; raw
snapshots and their `.site-analysis.json` reductions retain epochs, tickets,
clocks and exclusions. `reboot-resume/analyze-irq-sites.py` resolves and reduces
the bounded records. Earlier clamp failures, historical acceleration and
broader scheduler/HDA acceptance remain open.
