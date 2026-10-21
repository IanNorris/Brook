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

## Loaded recorder validation

The first Quake capture exposed two diagnostic problems. Foreign GPU callers
attempted the recorder lock before the model rejected their owner identity;
the stopped frame export also sent each scalar separately. Its export exceeded
60 seconds. That failed capture remains under
`build/reviews/unattended-preparation-overlap-gpu-1ms-fixed-mlfq-b-20261021`.
Frozen rows were recovered read-only after app exit, but the unpersisted original
start/stop acknowledgements prevent accepting it as a normal capture. A bulk
stdout trace download also failed its loss gate. The complete timestamp file
was subsequently recovered from the checked offline private disk; neither
original transport failure is counted as a pass.

Runtime probes now check an atomic owner hint before attempting the lock or
reading the clock, then retain the model's authoritative ownership check under
the lock. Stop/reset clear the hint. Actual-owner contention remains counted
and rejected by the analyzer. Each row is serialized through one bounded reply
buffer, preserving exact JSON while reducing socket writes. Hosted tests compile
the actual wrappers and row serializer, checking foreign admission, genuine
owner loss, reset/stale tokens, unavailable clocks and maximum-value chunking.
Release build, all 81 hosted suites and 11 analyzer tests pass.

Native review
`build/reviews/unattended-loaded-recorder-gpu-1ms-fixed-mlfq-a-20261021`
uses kernel
`f254dca9431761b3ed83befe56138118ae2d3612fd60fb6247208052fe10f0aa`
and unchanged overlap GPU module
`81ab428a1b8f19d2978231343241f4b972012cc2784b26b6f00ebd36d157379b`.
It retains eight vCPUs, 8 GiB, MLFQ, 1920×1080 GPU composition and the private
1 ms host fence poll. Quake GL3, Venus vkcube and vkmark run sequentially, each
with 256 reconciled compositor rows and no recorder contention delta.

| Workload | Actual client | Body p50 | Compositor interval p50 |
| --- | --- | --- | --- |
| Quake 2 GL3 | 1280×720 | 4.185 ms | 11.017 ms |
| Venus vkcube | 800×600 | 4.999 ms | 5.997 ms |
| Venus vkmark | 1280×720 | 6.015 ms | 6.043 ms |

These compositor intervals are not application frame times. Quake's buffered
SDL trace, hash-checked after offline extraction, completes 72 swaps within the
retained 3.225-second guest-clock window (22.33 swaps/s); complete inter-swap
interval p50 is 49.95 ms and p95 61.99 ms. Boundary-crossing gaps remain explicit.
Vkmark reports vertex 37 FPS / 27.027 ms and texture 38 FPS / 26.316 ms, both on
Virtio-GPU Venus backed by Intel UHD 630. Vkcube requested 1280×720 but negotiated
800×600, so it must not be presented as a 720p performance result.

Quake's stopped row export takes 1.42 seconds. Window minimize/restore replies
are separately timed (about 255/137 ms for Quake), including bridge overhead;
they are not input-to-photon measurements. Screenshots, CPU profiles and timing
windows are separate. All owned apps exit with complete output, no remaining
tasks or windows, clean heap/runtime checks, successful original VM owner exit
and four clean offline filesystem checks. This is one candidate cohort without
a matched performance control or comprehensive rendering-correctness verdict.
The outstanding GL/Vulkan comparisons and larger fence/pacing targets remain
open.
