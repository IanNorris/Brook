# Independent audio capacity control

A four/eight/four-fragment control does not establish larger buffers as a fix.
Both descriptors are now configured before a common barrier release, but actual
source starts can still separate by almost half a second. That phase difference
confounds the apparent capacity effect and must remain visible.

## Probe changes

`scripts/workloads/audio_mix_timing.py` adds two options:

- `--fragments 4|8`: negotiate that many 4096-byte OSS fragments. Individual
  write requests remain 4096 bytes; source data and the retry loop are unchanged.
- `--gate-stage generated|configured`: choose whether the optional finite HTTP
  barrier precedes device open or follows configuration of all descriptors.
  The default remains `generated`, preserving earlier invocations. `configured`
  requires `--gate-url`; reports record the selected stage and actual geometry.

For independent writers, a controller supplies separate left/right URLs and
releases both after their barrier requests arrive. Configured readiness proves
both devices exist before release, not that the guest schedules their first
writes simultaneously. Each source still has a ninety-second guest deadline,
and the controller retains its separate host deadline and original job receipts.

## Native result

The fresh private MLFQ VM uses the same kernel and original thirteen ABI-34
modules as the [mixed-policy comparison](mixed-policy-comparison.md), eight
vCPUs/eight GiB, host mask 0–15, blocking AP accounting, original 12-KiB HDA,
physical PipeWire callback output and requested 20-ms buffering. No graphics
apps or CPU workers run. Coalescing, history and expensive timing probes are off;
HDA delivery diagnostics run only around the six-second captures.

Both independent sources contain twelve seconds of pregenerated packet-coded
PCM, one in each stereo channel. Both devices are configured before release.
All three cases use left-first job submission and the same source implementation;
only the negotiated fragment count changes.

| Case | Left / right nominal completion | Left / right whole-source drain | Actual start spread | Left / right quiet-bin fraction |
| --- | --- | --- | ---: | --- |
| Four A | 0.8490 / 0.8493 | 14.171 / 14.169 s | 1.91 ms | 11.15% / 11.02% |
| Eight B | 0.7330 / 0.7385 | 16.377 / 16.491 s | 497.21 ms | 22.11% / 23.10% |
| Four C | 0.9570 / 0.9568 | 12.449 / 12.444 s | 6.39 µs | 2.79% / 2.71% |

Nominal completion is completed frames divided by 44,100 and elapsed guest
seconds. Independent packet fits give source/PCM rates 0.843/0.847,
0.735/0.738 and 0.957/0.956 respectively, with pitch fits near one. Stored PCM
covers 99.996%, 100.045% and 99.991% of the host brackets; those small deviations
are bracket precision, not meaningful faster delivery. Quiet fractions use
approximately one-ms RMS bins and are not physical speaker measurements.
Guest/host clock bounds contain 1× in each case.

Each source submits and completes exactly **529,200 frames**. Both-channel
captures lie inside both finite sources. All twelve original source/report jobs
have successful collected exits, full identity/output/EOF and empty groups.
The original controller and VM owner exit zero; all four offline filesystem,
heap, runtime/debt, policy-ledger/ready-count and IRQ gates pass. HDA delivery
records zero silence reservations and zero unsafe/invalid/contended/saturated
records. Earlier failed application cohorts remain separate.

Endpoint snapshots show only 256–512 software frames even with eight fragments:
7,744–7,936 frames are already in flight against the 8,192-frame capacity.
Four-fragment endpoints have 296–512 software and 3,200–3,800 in-flight frames.
These snapshots distinguish fresh software data from total queue occupancy;
they do not reconstruct the queue seen by every preparation.

The larger capacity did not remove the slowdown in this sequence, and the
second four-fragment result differs materially from the first. With its large
start-phase difference and no matched repetitions, the eight-fragment case
cannot establish that larger buffers caused the degradation. Do not promote
larger buffers or another waiting policy from these results.

## Next discriminating measurement

Count accepted output frames per included stream identity/epoch, split into
contributed frames, missing frames from an empty preparation queue, and missing
tail frames from a positive but shorter queue. Retain preparation-time software
and in-flight occupancy; update only for the actual accepted prefix. The exact
identity is `accepted = contributed + missing-empty + missing-short-tail`.
Full preparation counts would be misleading when the driver rejects or accepts
only part of a batch. Disabled streams and epoch/lifetime changes need explicit
handling; endpoint occupancy cannot substitute for these counters.

A stronger guest-local release can separately reduce first-write phase variance.
Neither improvement should change mixer waiting, HDA safety or scheduler defaults.
The current result is retained in
`build/reviews/unattended-audio-capacity-mlfq-a-20261021/audio-capacity-control/summary.json`,
with exact executed sources, configured-barrier receipts, geometry, PCM/decodes,
write traces, original lifecycle evidence and filesystem checks.

The [accepted-contribution follow-up](audio-mix-contributions.md) implements
these counters and identifies positive short tails as the dominant category in
two native windows. It does not alter the capacity control or accept a fix.
