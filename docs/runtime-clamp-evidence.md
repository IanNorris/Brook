# Runtime service-clamp evidence

The MLFQ [targeted wakeup control](targeted-wakeup-timing.md) records four
clamped-service samples. Its wall and steal counters never move backwards, but
their difference can still fall below the ledger's previous service floor.
That run remains failed runtime acceptance. Four clamped samples do not
necessarily represent four separate host-delay events: several samples can
remain below one previously established floor while a correction catches up.

`sched runtime status` now exposes `first_clamp` and `max_clamp` objects. Each
records sample sequence, CPU, task PID/incarnation, the preceding and current
wall/steal pairs, the retained service floor and the positive deficit. `valid`
is false until a clamp is observed. The first record is immutable; the maximum
changes only for a strictly larger deficit, preserving the first equal maximum.
For a valid record:

```
deficit_ns = service_floor_ns - (wall_ns - steal_ns)
```

The preceding pair is the previous accepted raw sample on that CPU, not
necessarily the sample that established the service floor. A floor can span
several corrections and ownership changes. Identity is the ledger owner at the
clamped sample, not proof that this task caused the delay or owns all earlier
service represented by the floor. These records do not capture every clamp,
host scheduling event or wall/steal publication interleaving.

The implementation reuses the samples that triggered the existing clamp. It
adds no clock reads, logging, allocations or synchronization. It does not
change sampling retries, subtraction, completion accounting, the monotonic
floor or policy service charges. Formatting occurs after the runtime snapshot
has been copied under the existing ready lock. This is a normal debug-thread
report, not a panic-safe diagnostic. Driver ABI and scheduler defaults remain
unchanged.

Hosted tests check the first complete record, a later smaller correction that
must not replace it, a larger deficit on another CPU/task identity, and exact
catch-up without manufactured service. All 64 host suites pass. The native
build resolves thirteen modules against 106 kernel exports. The frozen capsule
changes only the kernel and its symbol-address table from the targeted-wakeup
capsule; all thirteen modules remain byte-identical. Native evidence is retained
separately from these functional checks.

## Native controls and remaining uncertainty

Three fresh private eight-vCPU/six-GiB boots repeat the generated Firefox
video/audio fixture with twenty-four guest CPU workers. Both mixer service and
targeted wakeup probes are enabled; CPU profiling and mapping counters remain
off. EEVDF keeps its 4-ms normal request. Callback HDA requests 20-ms host
buffering into an isolated PipeWire null sink. A fourth fresh MLFQ boot adds
eight bounded host CPU workers for thirty-five seconds after guest load
readiness, all sharing CPUs 0–13 with QEMU and leaving CPUs 14–15 available.
That fourth run is explicitly a host-contention diagnostic, not a comparable
unmodified policy-performance control.

| Run | Policy | Additional host workers | Runtime samples in observation interval | Reported steal in interval | Clamped samples |
| --- | --- | ---: | ---: | ---: | ---: |
| ce-m1 | MLFQ | 0 | 4,357,418 | 0.258 CPU-s | 0 |
| ce-e1 | EEVDF | 0 | 4,334,619 | 0.286 CPU-s | 0 |
| ce-m2 | MLFQ | 0 | 4,165,159 | 0.254 CPU-s | 0 |
| ch-m1 | MLFQ | 8 | 3,440,512 | 62.999 CPU-s | 0 |

These intervals run from the controller's initial runtime snapshot through its
post-browser-close snapshot, not exactly the thirty-second guest CPU workload
or the thirty-five-second host load. Steal totals combine all vCPUs and are
not elapsed wall time. The bounded host workers all exit zero after approximately
35.07 host seconds. The large steal increase establishes that the contention
control exercised host scheduling pressure, without reproducing service clamping.
It does not establish why the earlier four samples clamped or prove they were
benign. That earlier runtime gate remains failed.

All four boots report zero invalid/backwards/clamped/stale/saturated runtime
counters, with all eight paravirtual-clock and steal records active. Both new
evidence objects correctly report `valid:false` with no clamp. No native
positive-clamp record has yet been collected; positive-record behavior is tested
in the hosted accounting model. No native HDA fault is logged. Original launch
wrappers and audio routers exit zero, all four filesystem checks pass per boot,
and twelve-document/four-profile preservation checks pass. All test VMs, private
services and host load workers are stopped.

The frozen kernel SHA-256 is
`de6282384a7ebdb0e8709558146f8bae5502d6fcd00940a8713d4331ebc96823`.
Source hashes, the unchanged-module manifest, original job/launcher receipts,
host load ownership/exits and the result verifier are retained under the ignored
`build/workloads/timing-scheduler-20261020/clamp-evidence-*` and
`clamp-host-stress` directories. Keep this capture available for recurrence;
do not relax the accounting gate from a non-reproduction. The next latency
measurement should separate BSP timer arrival from time spent reaching and
performing the blocked-wakeup scan.
