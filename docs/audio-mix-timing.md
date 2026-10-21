# Coordinated finite audio streams

The [mixed Firefox/Quake test](mixed-desktop-timing.md) slows both stream
completions near 0.62/0.54 nominal. A cheap native control removes application
rendering/decoding and coordinates two pregenerated OSS sources. It sustains
both near real time, narrowing the next investigation toward independent
producer timing and application/graphics interaction. It does not identify a
specific scheduler or mixer defect.

`scripts/workloads/audio_mix_timing.py` generates all source samples before
opening devices. A packet changes frequency every 100 ms using the existing
packet-code permutation. One stereo source contributes exclusively left; the
other exclusively right, at conservative amplitude. Sources share 44.1-kHz,
16-bit stereo format and negotiated four-by-4,096-byte fragment geometry.

One coordinator alternates nonblocking descriptor refill order every loop.
It attempts each descriptor, accounts partial writes/EAGAIN, sleeps one ms
only when neither accepted data, and records accepted-write and independent
completion brackets. The finite deadline is ninety guest seconds; external
observation timeouts must retain the same original job. A successful result
requires exact independent completion followed by explicit OSS sync and close.
Reported queued values are not a claim that Prepare saw both unconsumed queues.

For example, inside a private Brook VM without other audio streams:

```sh
python3 audio_mix_timing.py --seconds 12 --channels both --first 0 --output /tmp/mix-left-first.json
```

Repeat with left/right single-channel controls and reversed initial refill
order. Capture both PCM channels with independent host/guest clock brackets;
guest timestamps and complete bytes alone do not certify speaker timing.

## Native result

The fresh `unattended-audio-mix-mlfq-a-20261021` VM keeps kernel `b3ffe02c…`,
original thirteen modules/12-KiB HDA, MLFQ/blocking accounting, eight vCPUs/eight
GiB, normal host masks 0–15, physical PipeWire callback audio and requested
20-ms buffering. No games/browsers or CPU workers run. Owner/yield, idle
coalescing, runtime history and expensive timing recorders remain off. Delivery
diagnostics are enabled only for the same six-second steady capture per case.

| Case | Twelve-second source drain | Per-stream nominal completion | Fitted source seconds / stored PCM second |
| --- | ---: | ---: | ---: |
| Left only | 12.072 s | 0.9978 | 0.9985 |
| Right only | 12.149 s | 0.9987 | 0.9993 |
| Both, initial left first | 12.183 s | 0.9849 / 0.9849 | 0.9881 / 0.9874 |
| Both, initial right first | 12.477 s | 0.9616 / 0.9621 | 0.9675 / 0.9677 |

Each active descriptor submits and completes exactly **529,200 frames**.
Both channels independently decode with pitch fit near 1× and no observed
backwards/repeated symbol transitions in these captures. Inactive control
channels remain fully quiet. Active-channel stored quiet-bin fractions are
approximately 0.1% for singles, 1.0% for the first dual run, and 2.6%/1.8% for
the second. The variation does not establish a refill-order effect without
repeats. Fits are approximate, quantized and apply to stored PCM, not physical
speaker tempo. Partial steady captures cannot prove every source symbol played.

All capture brackets lie inside the finite sources. Independent clock brackets
contain 1×. Eight original source/report jobs finish with complete
identity/group/output/EOF receipts. Final audio drains, heap, policy/ledger,
ready count, IRQ IF/stale-ticket and runtime gates pass, with zero clamps in
this cohort. The original VM owner exits zero and all four primary filesystem
checks pass. These gates do not rehabilitate older failed application cohorts.

Raw evidence and checked per-channel decoding remain under that review's
`audio-mix-control/`, including `summary.json`. The new source's native code
and arguments are preserved per case in `guest-source.py` and original job
receipts.

The next discrimination is two independent writers with the same pregenerated
sources, geometry and a finite start barrier. If that also sustains concurrency,
return to the real-app producer waits and graphics interaction. If it fails,
inspect refill scheduling/phase and actual unconsumed queue availability before
changing mixer lookahead. The writer-immediate-flush hypothesis is ruled out
by the current source: writes enqueue; the mixer performs periodic submission.
