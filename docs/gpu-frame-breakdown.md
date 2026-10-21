# Empty desktop frame breakdown

This is the first control for the GPU performance investigation. It measures
the shared compositor/virgl path, not a Vulkan application. No pacing,
buffering, fence, resource lifetime or scheduler-default behavior has changed.

## Host polling baseline

Two private empty-desktop runs use the same frozen kernel/modules, eight
vCPUs/8 GiB, optional MLFQ, GPU composition and 1920×1080 scanout. No app windows
exist at either endpoint. The render node is `/dev/dri/renderD128`, PCI
00:02.0, Intel UHD 630 (`8086:9bc5`); the separate NVIDIA card is not selected.
PipeWire/HDA remain configured but no application plays audio.
The initial device display-info reply advertises 1280×800; the driver then
creates and binds a 1920×1080 primary display and compositor render target.
The final serial inventory verifies the latter dimensions and active GPU DRAW
composition for every cohort; the initial reply is not the tested resolution.

Quiet eight-second host-cost windows, three-second CPU/CS profiles and separate
six-second host traces are kept distinct. Only the private QEMU fallback fence
poll changes between the two runs:

| Measurement | 10-ms host poll | 1-ms host poll |
| --- | ---: | ---: |
| Quiet compositor interval p50 / p95 | 27 / 33 ms | 19 / 20 ms |
| Quiet QEMU process CPU cores | 0.4983–0.4987 | 0.2760–0.2762 |
| Trace compositor fence-response p50 / p95 | 9.544 / 10.012 ms | 1.797 / 2.413 ms |
| Trace response → next submit p50 / p95 | 16.439 / 22.447 ms | 16.794 / 17.716 ms |

These are one ordered pair, not repeated balanced trials. Host fence response
is completion observation, not GPU execution. The source sleeps for 16 ms
**after** rendering; remaining frame work and wakeup latency also contribute
to response→next-submit. Do not add these medians into a synthetic frame.
The 10-ms CPU profile contains 121 compositor samples: 119 include
`SubmitCommand`, and 116 include its completion wait. Most samples on the
other cores are idle. IRQ sampling and profile-writer work prevent treating
those counts as unbiased CPU percentages. Module addresses are resolved using
the exact staged module hashes, recorded load addresses, loader section
placement and bounded function intervals.

Evidence is under `build/reviews/unattended-empty-desktop-gpu-{10,1}ms-fixed-mlfq-a-20261021/`.
All original controllers/VM owners exit zero and all four filesystems per run
check cleanly. This does not establish application performance or recommend a
normal QEMU default change.

## Reconciled native frame recorder

`gpu frame timing on|off|reset|status|frames` exposes an optional 256-row
recorder. The disabled path does not read clocks or acquire recorder locks.
Recording uses stable KVM elapsed time and try-locks; any contention delta,
clock failure or invalid frame rejects analysis. It does not promise a fixed
probe execution bound. Normal controls/exports take a lock and are not panic
inspection commands. Stop before exporting all rows in one response.

The frame starts before deferred captures and scene draining, and ends after
debug polling, immediately before blocking. Nested scopes are exclusive:
their time is removed from their parent, so every retained row must sum to
`end_ns - begin_ns`. The explicit `other` phase covers residual frame work.
Epoch, frame and LIFO tokens prevent stale/control-raced scopes from charging
a later frame. Stop may discard one incomplete frame; overwritten prefixes
remain visible. First/last fence IDs allow correlation with independent host
traces without assuming that response timestamps are actual scanout.

The corrected native review is
`unattended-frame-breakdown-gpu-1ms-fixed-mlfq-b-20261021`, frozen kernel
`3aa630e73001eb5d7edc294560ad236a6ff8f74b9a9aa884055c1daf4c5a38f0`.
It captures 352 complete cycles, retains the last 256 and reports 96 overwritten
rows, no aborted/invalid frames, clock failures or contended records. Every
retained row reconciles exactly. Times below are guest elapsed, including
preemption, host steal and recorder overhead; they are not CPU execution times.

| Interval | p50 | p90 | p95 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Frame body | 2.558 ms | 2.990 ms | 3.470 ms | 10.890 ms |
| Frame start → next start | 18.005 ms | 19.006 ms | 20.009 ms | 27.006 ms |
| Frame end → next start | 15.533 ms | 16.017 ms | 16.159 ms | 16.983 ms |
| Installed remaining sleep budget | 15.113 ms | 15.172 ms | 15.840 ms | 15.999 ms |
| Resume minus installed deadline | 0.401 ms | 0.871 ms | 0.940 ms | 1.005 ms |

| Exclusive stage | p50 | p95 | Share of total retained body time |
| --- | ---: | ---: | ---: |
| Deferred captures | 0.0072 ms | 0.0091 ms | 0.26% |
| Scene draining | 0.0011 ms | 0.0015 ms | 0.04% |
| Scene/CPU preparation | 0.0685 ms | 0.0848 ms | 4.09% |
| Texture upload residual | 0.0033 ms | 0.0047 ms | 0.13% |
| Quad recording | 0.0014 ms | 0.0019 ms | 0.06% |
| Command-lock acquisition | 0.0009 ms | 0.0010 ms | 0.03% |
| Vertex upload residual | 0.0008 ms | 0.0010 ms | 0.03% |
| Command construction residual | 0.0014 ms | 0.0018 ms | 0.05% |
| Queue setup and response validation | 0.1752 ms | 0.3366 ms | 7.02% |
| Fenced completion wait | 1.9232 ms | 2.5166 ms | 68.55% |
| Ordinary acknowledgement wait | 0.3566 ms | 0.5685 ms | 19.67% |
| Display flush residual | 0.0004 ms | 0.0007 ms | 0.02% |
| Debug capture | 0.0007 ms | 0.0011 ms | 0.03% |
| Debug polling | 0.0003 ms | 0.0005 ms | 0.01% |
| Other/residual | 0.0008 ms | 0.0010 ms | 0.03% |

“Residual” matters: nested queue/wait time appears in separate exclusive rows.
An alternative attribution view groups complete control-call elapsed time by
the calling phase. Those groups reconcile exactly to queue setup + both wait
categories and must **not** be added to the stage table:

| Calling phase | Calls across 256 frames | Per-frame call elapsed p50 / p95 |
| --- | ---: | ---: |
| Texture uploads | 1024 | 0.371 / 0.630 ms |
| Vertex upload | 256 | 0.036 / 0.076 ms |
| Draw construction/submission | 256 | 1.931 / 2.525 ms |
| Display flush | 256 | 0.109 / 0.186 ms |

There are seven control requests per frame and one fenced request. Reported
request bytes are command packet bytes, not transferred pixel/vertex bytes.
The next ownership experiment can overlap independent CPU packet preparation
with the vertex transfer. Its immediate gain is bounded by the smaller of
transfer wait and CPU construction time; it is groundwork for broader overlap,
not a plausible explanation for all the missing presentation throughput.

## Clock-domain correction and verification

An earlier diagnostic build mislabeled `wakeupTick * 1,000,000` as KVM
nanoseconds. APIC elapsed ticks instead use their own `(origin_ms, origin_ns)`.
The source now exports raw deadline ticks. The analyzer requires stable matching
pvclock origins/mode and unchanged rejected-sample count at both endpoints,
then checks `origin_ns + (deadline_ticks - origin_ms) * 1,000,000` for underflow
and overflow. Early dirty/input wakes remain valid signed observations.

The earlier `…-a` review preserves its original export and an explicitly
hash-guarded recovery of raw ticks, with a separate checked origin conversion.
It is not evidence that the original field was correctly labeled. The final
`…-b` run exports raw ticks directly, requires no legacy recovery, and includes
the complete command attribution. The analyzer source is retained per run.

The final off/on/off quiet windows use the same kernel. Host process CPU cores
are respectively 0.2623–0.2625, 0.2847–0.2850 and 0.2823–0.2825. Before/after
millisecond compositor intervals have p50 19/18 ms and p95 20/20 ms. This small
single sequence bounds interpretation rather than proving zero recorder cost.
Original controller/VM owners, heap/runtime gates and all four filesystem
checks pass. No app workload or full rendering-correctness gate is claimed.

The actual model tests cover exclusive nested scopes, owner filtering, ring
overwrite, stale epoch/frame tokens, wrong-order scope exit, control races,
clock failures and command identity. Ten Python tests reject corrupted
exports, lost rows/records, invalid origins, incomplete attribution and
double-counted time. Release build, entry/startup audits and all 78 hosted
suites pass. The two final model edge cases were rebuilt and rerun separately
in the Nix shell. A direct host rebuild failed to locate libc++; its test was
not run, and the failed invocation is retained rather than counted as a pass.
The broader timing/audio/scheduler findings remain open.

To analyze a bundle of acknowledged start/stop replies, clock snapshots and
stopped rows:

```sh
python3 scripts/workloads/analyze_frame_timing.py capture.json \
  --output breakdown.json --csv frame-breakdown.csv
python3 scripts/test_analyze_frame_timing.py
```

The final review's `frame-breakdown-control/` holds `raw-capture.json`,
`capture.json`, `reconciled-breakdown.json`, `frame-breakdown.csv`, off/on/off
cost brackets and the verification receipt. Artifacts remain private below
`build/`; no licensed application data is committed.
