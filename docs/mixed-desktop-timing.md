# Mixed Firefox and Quake timing

The final reviewed kernel completes a real mixed MLFQ workload with Firefox,
Quake GL3 and eight background CPU workers. The applications and workers drain
normally, but playback is not accepted: concurrent audio progress falls, and the
final runtime gate records two service clamps. No scheduler default changes.

## Setup

`unattended-mixed-desktop-mlfq-b-20261021` uses kernel `b3ffe02c…`, the same
thirteen original modules and 12-KiB HDA, eight vCPUs/eight GiB, blocking AP
accounting, normal host masks 0–15 and physical PipeWire callback audio with
20-ms requested buffering. Owner/yield, idle coalescing, runtime history and
expensive timing recorders remain disabled. No host CPU workers run.

Firefox uses a fresh profile and the existing generated local media/telemetry
fixture. Quake uses fresh private homes and the ordinary GL3 wrapper with
1280×720, timedemo=0, cl_maxfps=60 and +d1 attract playback. It is not an
unconstrained timedemo. Two guest-clock reads per SDL swap are buffered until
exit; the trace measures application swap completion, not display scanout.
Licensed game data stays on private disks. No game data is included in Git.

The sequence is Firefox alone; both apps with Quake focused; both apps plus
thirty seconds of eight guest CPU workers; Firefox focused during that load;
close Quake and observe Firefox; reopen/close Quake while Firefox stays alive;
then close Firefox. Both loaded capture brackets lie inside every worker's
measured first/last work timestamps. All workers perform nonzero work.
Window snapshots record focus/geometry and possible occlusion; they do not
prove physical presentation or equal visible areas.

## Observations

Each row is one six-second capture; these are single-run observations, not
confidence intervals. Stream identities/epochs remain stable during each
bracket. Firefox's original stream is 33; a new stream appears with each Quake
launch and disappears on its close. This association is lifecycle evidence,
not a general per-stream process-identity API.

| Phase | Firefox completion / guest second | Quake-associated stream completion | Firefox callbacks / page second | Quake swaps / guest second |
| --- | ---: | ---: | ---: | ---: |
| Firefox alone | 0.9785 | — | 11.02 | — |
| Both, Quake focused | 0.6158 | 0.5354 | 5.85 | 8.14 |
| Both + CPU, Quake focused | 0.6116 | 0.5388 | 4.98 | 8.25 |
| Both + CPU, Firefox focused | 0.6192 | 0.5361 | 6.00 | 7.92 |
| Firefox after Quake closes | 0.9999 | — | 10.77 | — |
| Quake reopened | 0.6185 | 0.5410 | 5.67 | 7.98 |

CPU load changes this audio behavior little, while Quake close/reopen reverses
and reproduces it. That supports separating concurrent audio behavior from
application decoding/rendering and scheduling before another policy change.
Completion counters do not measure decoded music tempo or prove source
availability at mixing time. Both applications also compete for graphics work.

The current DSP writer only enqueues samples; it does not immediately flush
them. The mixer periodically prepares up to the maximum queued stream length,
capped at 1,764 frames, with absent contributions becoming silence. Accepted
hardware output cannot subsequently acquire a late contribution. There is no
common per-stream presentation timeline or bounded wait for another producer.
Those source properties permit asynchronous underfeed; the observations do not
establish that this policy caused it.

A cheap next discrimination uses pregenerated finite OSS packet streams, one
exclusively in each stereo channel, with one coordinator alternating nonblocking
refills. Single-channel controls, reversed initial refill order, independent
PCM decoding and exact drains separate basic concurrency from graphics load.
Positive total queuedFrames alone cannot prove data was available to Prepare:
it includes already-submitted inFlight frames.

## Gates and retained failures

All eight original guest jobs have collected exit0, full output/EOF and empty
original groups. The browser's 479-row buffered trace has zero drops and the
CPU overlap checks pass. Both Quake instances and Firefox close normally;
audio drains, heap integrity, policy/ledger balance and ready counts pass.
The original VM owner exits zero and all four primary filesystem checks pass.

The final runtime result has **two clamps**. Its first is CPU2/PID242 at
323.719961774 guest seconds: wall advances 26,635 ns, steal advances 32,221 ns,
and the retained floor deficit is 5,586 ns. Other raw-clock/lifetime error
counters stay zero. This remains a failed runtime gate under the original
contract; it is separate from the reproducible concurrent audio reduction.
The [independent debt reference](runtime-accounting-debt-reference.md) validates
how the ledger handles inputs, without identifying this native event's cause
or changing acceptance.

The initial …mlfq-a pilot requested forty seconds from SCHEDMIX, whose existing
limit is thirty. The original worker exit64 and complete identity/EOF evidence
are preserved. Its original applications were closed normally, the original VM
exited zero and all four filesystem checks passed before the fresh b run.
Its partial measurements are not pooled into the completed cohort. A separate
2-second pilot cleanup profile is lossy (24,975 dropped events/1,253 CPU samples)
and cannot establish causal CPU-time fractions or uniform IRQ-masked coverage.

Raw evidence remains under
`build/reviews/unattended-mixed-desktop-mlfq-{a,b}-20261021`.
The b cohort's `mixed-desktop-control/summary.json` reduces host-anchored browser
samples, windowed SDL swap traces, stream rates and independent lifecycle gates.
There is no matched RR/reversal result or full mixed-policy acceptance yet.


The [coordinated dual-source control](audio-mix-timing.md) subsequently sustains
both streams at 0.96–0.98 nominal, with independent channel decoding and exact
finite drains. That is materially better than this real-app pair, without
proving an application-specific cause. Independent writers without graphics
are the next comparison; neither the native runtime failure nor real-app
playback acceptance changes.
