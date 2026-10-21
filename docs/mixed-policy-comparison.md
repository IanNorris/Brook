# MLFQ and EEVDF with Firefox and Quake

A matched pair on the current kernel reproduces concurrent audio slowdown with
both optional policies. Neither fixes it in this workload. Closing Quake restores
Firefox audio progress; reopening it reproduces the reduction. Accounting and
lifecycle checks pass in these two runs, without clearing earlier failures.

## Controlled setup

Both fresh private VMs use the same kernel and original thirteen ABI-34 modules,
eight vCPUs/eight GiB, host CPU mask 0–15, blocking AP accounting, original
12-KiB HDA publication lead, physical PipeWire callback output and requested
20-ms buffering. Only the boot-selected policy changes. No host CPU workers run.

Firefox uses the same generated 640×360 media fixture and fresh profile. Quake
uses GL3 at 1280×720, `timedemo=0`, `cl_maxfps=60` and `+d1` attract playback,
with a fresh private home for each launch. The identical buffered SDL probe
reads clocks twice per swap and writes its trace only on exit. No licensed
assets enter Git. The eight guest CPU workers run for thirty seconds; both
loaded captures lie inside every worker's measured work interval.

Timing probes and runtime history stay off during the timed phases. HDA delivery
diagnostics run only around each identical six-second capture. Runtime snapshots
follow captures. History is enabled only after the final performance capture,
covering final Quake/browser teardown, then stopped before export. Its observer
cost therefore cannot explain the recorded performance difference between phases.
This does not make the other instrumentation free.

Kernel SHA-256:
`112519e37cd0d542c984ae85af434676ce0df192ca6f54a5dd4d86fc37b1adff`.
Generated clip SHA-256:
`0ad4cc105e1600a91c686d8517ca4f059411c2cfc2d6c907d0206e8197a47aed`.

## Audio progress

Values are completed source frames divided by 44,100 and elapsed guest seconds.
They describe device completion, not decoded source tempo or a speaker recording.
Each guest/host clock bracket contains 1×; stream identities/epochs remain stable
within each bracket. Each cell is one capture, not a confidence interval.

| Phase | MLFQ Firefox / Quake | EEVDF Firefox / Quake |
| --- | --- | --- |
| Firefox alone | 0.985 / — | 0.980 / — |
| Both, Quake focused | 0.627 / 0.547 | 0.613 / 0.535 |
| Both plus CPU, Quake focused | 0.604 / 0.535 | 0.631 / 0.547 |
| Both plus CPU, Firefox focused | 0.621 / 0.537 | 0.590 / 0.533 |
| Firefox after Quake closes | 0.987 / — | 0.998 / — |
| Quake reopened | 0.619 / 0.542 | 0.633 / 0.552 |

The repeated close/reopen effect, small CPU-load effect and similarity across
policies support testing the shared audio path independently of more scheduler
tuning. They do not prove the mixer alone causes every browser/game stall.
The older [MLFQ mixed run](mixed-desktop-timing.md) shows the same broad pattern
on an earlier kernel, but is not pooled into this pair as an identical repeat.

## Frame cadence and responsiveness

| Loaded phase | MLFQ Firefox callbacks/s | EEVDF Firefox callbacks/s | MLFQ Quake swaps/s | EEVDF Quake swaps/s |
| --- | ---: | ---: | ---: | ---: |
| Quake focused | 4.41 | 5.00 | 8.17 | 7.39 |
| Firefox focused | 5.69 | 3.79 | 7.91 | 9.25 |

Loaded Quake complete-interval p95 frame times are 177–185 ms on MLFQ and
167–177 ms on EEVDF; boundary-crossing intervals remain explicit in the raw
analysis. These are SDL completion intervals, not physical presentation.
The single input dispatch-to-DOM brackets are 96–211 ms and 75–185 ms, with
DOM-to-RAF delays of 235/200 ms. One event cannot establish a latency percentile.

Worst periodic-sleeper p95 is 1.34/1.91 ms, while maxima reach 74.7/77.2 ms.
Every CPU worker makes nonzero progress and no periodic sleeper wakes early.
Small periodic p95 does not imply a responsive desktop. This single pair has
no consistent policy winner or full default-policy acceptance.

## Independent gates and next discriminator

All sixteen original guest jobs retain collected exit zero, complete identity,
output/EOF and empty groups. Both original fixture servers, controllers and VM
owners exit zero; all eight filesystem checks pass. Heap integrity, audio drains,
policy/ledger totals and ready counts pass. All twelve HDA captures have zero
invalid clocks/positions, unsafe publication, contended records or saturation;
final serial logs have no native HDA fault.

Both final runtime gates are clean, with all eight baselines valid, zero debt
and no correction episodes. Cleanup histories record 1,650,676/2,118,662 events
without a correction and retain 512 rows each. They do not capture the entire
run or provide positive debt recovery evidence. Earlier failed runtime gates
remain failed. No scheduler or audio default changes.

Raw reviews are `unattended-mixed-policy-{mlfq,eevdf}-a-20261021`, each with
`mixed-desktop-control/verification.json` and `summary.json`, executed controllers,
source/fixture hashes, buffered traces, PCM, original receipts and offline checks.

The next cheap control changes only independent OSS producers' negotiated
capacity: four/eight/four 4096-byte fragments, with both descriptors configured
before a common release. Source data, 4096-byte write requests and retry loop
stay fixed. This tests capacity/refill phase without expanding the unsuccessful
[refill waiting policy](audio-refill-coalescing.md). Exact accepted-frame
contribution accounting would distinguish empty queues from positive short tails;
preparation counts alone cannot do so because submissions can be partial/rejected.

The [configured-barrier capacity control](audio-capacity-control.md) subsequently
completes exact drains but retains substantial phase variance and no buffer-size
fix. Accepted-contribution accounting remains the next discriminator.
