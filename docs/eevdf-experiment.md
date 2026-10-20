# Optional EEVDF experiment

Brook has an experimental `eevdf` scheduler module. RR remains the default.
The timing audit and CPU-clock work are prerequisites, not evidence that the
reported Quake II/Firefox playback acceleration has been reproduced or fixed.
Current limits and earlier measurements are in
[timing-scheduler-audit.md](timing-scheduler-audit.md).

## Policy contract

This is an original Brook implementation of EEVDF's eligibility/deadline model,
not a Linux scheduler port. The basic rule is to choose the earliest virtual
deadline among entities owed service. Short sleeps must not erase negative lag;
see the [upstream explanation](https://docs.kernel.org/scheduler/sched-eevdf.html).

Brook's explicit choices are:

- Initial per-thread weight is 1024. Existing priority hints select physical
  requests of 1/2/4/8 ms. Live nice/weight controls and process/app-group fairness
  are not implemented. An internal initial-weight setter exercises unequal
  weights in host tests without exposing an unfinished user API.
- Virtual time is the weighted mean of runnable entities, including tasks
  already running on other CPUs. Queued and running are distinct states. Only
  queued entities can be picked; the existing kernel mechanism still owns CPU
  claims, post-switch requeue, task references and stack release.
- Requests retain remaining service across early preemption, voluntary yield
  and short sleeps. A fractional remainder prevents repeated tiny CPU-clock
  flushes from discarding weighted virtual service.
- A blocked entity with negative lag remains a nondispatchable deferred member
  until its debt decays through virtual progress. A fixed min-heap removes all
  newly expired deferred entries to a fixed point in O(k log 1024). Mean/sum
  updates are constant time, avoiding repeated full scans with interrupts off.
- Sleeper lag is bounded to two requests. When a nondeferred sleeper returns,
  placement compensates for the returning weight's effect on the mean. Late
  outgoing service after remote block publication still debits the entity.
- For an available CPU slot, if every available entity is ahead of the global
  mean while lower-vruntime entities already run on other CPUs, the selector
  uses the least-served available frontier. It retains debt and avoids idling
  usable CPUs. This is a deliberate global-queue SMP choice; it does not claim
  identical behavior to Linux's per-CPU scheduler or balancing.
- Signed 128-bit totals avoid weighted-sum overflow; an original bounded 128/64
  divider avoids freestanding compiler-runtime dependencies. Coordinates rebase
  at a defined virtual threshold. Exceptional arithmetic saturation is counted
  instead of wrapping; those cases are not accepted as performance evidence.

`eevdf_policy.h` holds no Process pointers and performs no allocation. Its fixed
state fits the explicit 64-KiB policy storage contract. Selection scans the
bounded member list; it is not a new per-CPU balancing mechanism.

## Kernel and module ABI

Module ABI 34 rejects older providers before module initialization. Scheduler
ABI 2 validates its version, table size, state size and required callbacks.
Registration now returns checked success; a failed registration cannot be
reported as a successful scheduler module initialization. All provider/table/
name/callback pages remain permanently retained, including inactive policies.

The new callbacks are `AccountService`, `SetRunnable` and `ShouldPreempt`.
Running-interval charges include partial slices and CPU-clock-query flushes;
they are forwarded before changing ledger ownership. RR and MLFQ preserve their
wall-slice decisions through the same interface. EEVDF uses observed service
and eligible deadlines. Preemption decisions run only at safe user-mode timer
boundaries; arbitrary kernel code remains non-preemptible.

Local block/exit publication flushes service before removing runnable status.
Restore initializes running entities as runnable without enqueuing them; blocked
or stopped tasks still on a CPU are not made dispatchable. Stale queue entries
lose runnable status. A final interval from a terminated task excluded by a hot
policy replacement is observed separately, without resurrecting its entity.

A hot switch resets policy history. The caller's outstanding interval is charged
to the old policy; other CPUs begin the new policy at their next local accounting
boundary. This is not a simultaneous cross-CPU fairness epoch. Hot-switch tests
establish safety; performance comparisons use fresh private boots and settle
before measuring workloads.

`sched policy service` on the debug channel reports policy/ABI/state size,
policy-observed service, the runtime ledger since selection, inactive retirement
service and queue/debug counters. Their equality within one snapshot checks
end-to-end charge forwarding. `sched runtime status` retains clock/steal/source
failure counters. Both are normal debug-thread operations, not panic-safe APIs.

All scheduler bodies are compiled without SIMD/FPU registers. EEVDF and MLFQ
module builds use `-O2`; comparisons must use rebuilt controls, not attribute a
difference from old unoptimized modules solely to scheduling policy.

## Staging and selection

Build/check with every QEMU stopped. From a configured development tree:

```sh
nix-shell --run 'cmake --build build/release/kernel --target kernel check_module_symbols --parallel 4'
nix-shell --run 'cmake --build build/release/kernel --target kernel --parallel 4'
```

The second kernel link incorporates the generated symbol table. A private review
must contain copied disks/artifacts and its normal launcher. Stage the complete
matched build, substituting its exact ELF SHA-256:

```sh
nix-shell --run 'python3 scripts/stage_review_kernel.py --review build/reviews/PRIVATE_REVIEW --module-abi 34 --expected-kernel-sha256 ELF_SHA256'
```

The caller must supply a private copy. The helper refuses live QEMU, boot targets
outside the review tree, symlinked/hardlinked boot files, old module ABIs and
existing incomplete staging evidence. It locks the review, changes a
reflink/sparse temporary FAT copy, round-trips all modules through FAT and records
hashes before replacing the private boot image. It does not touch data/Nix/tmp
disks or launch a VM. Keep failed staging evidence for inspection.

Select `sched eevdf` in that review's boot script, or `sched policy eevdf` over
the normal debug channel. `rr` and `mlfq` remain available. No default boot script
is changed by enabling the optional module.

## Evidence and remaining gates

The 61 host suites pass, including the actual `.so` ABI, a 100,000-step eight-CPU
ownership/fairness model, unequal feasible SMP weights, fractional service,
sleep/deferred debt, late outgoing service, PID reuse, restore states, arithmetic
rebasing/saturation and 20,000 independent division vectors. These finite models
do not prove arbitrary kernel SMP or all starvation bounds. Compiled entry/start
mutation audits and module ABI/import/register-use audits also pass.

The first eight-vCPU native pilot passes standard CPU-clock and futex tests,
24 CPU workers/four periodic sleepers for 20 seconds, and 128 forked children
with four threads each. Policy-observed service equals the runtime ledger at
all five checkpoints. Its wake p95 is 1.7–1.8 ms and maximum 8.8 ms, with no
skipped 16-ms periods in that run. All tracked jobs drain; heap, clean exit,
four filesystem checks and twelve documents/four original profiles pass.
This pilot is not a matched policy comparison or a playback-speedup reproduction.

Matched fresh-boot RR/EEVDF/MLFQ forward/reverse CPU controls are being collected
under `build/workloads/timing-scheduler-20261020/eevdf-cpu-cohorts`. Repeated
browser/video/audio/input trials, bounded host contention, switching under churn
and further real apps remain acceptance work. Keep the observed multi-second
kernel/wakeup stalls from the earlier heavily contended fork test visible;
policy changes do not establish kernel preemptibility or fix BSP-only wakeups.
A default switch requires the broader gates in
[scheduler-policy-next.md](scheduler-policy-next.md).

The first matched controls on kernel `9cc712e6...` give RR 26.37-ms wake p95,
EEVDF 1.88 ms and two MLFQ runs 23.93–23.99 ms. EEVDF completes about 3.3% fewer
fixed-work batches than RR in that first pair. These are preliminary CPU-only
results; repeats and browser/media trials are not interchangeable with them.
All four accepted runs pass task drain, heap, original wrapper/QEMU shutdown,
four filesystem checks and saved-data preservation.

A later readiness read overlaps the launcher's own debug queries and gets a
connection reset. The outer observation process is lost, while the original VM
and bridge remain alive. This is retained as an orchestration failure rather
than being called an OS panic or silently restarted. That same VM subsequently
passes 24 RR/EEVDF/MLFQ transitions during CPU and 128-child/four-thread churn;
EEVDF service matches the ledger at every inspected epoch, and actual bridge
QEMU exit is zero after clean shutdown with four filesystem checks passing.
Its original wrapper status is unavailable, so it remains excluded from matched
acceptance. An initial diagnostic duration 40 is also retained with terminal
usage exit 64 (the fixture accepts at most 30 seconds); the valid 30-second job
has a separate persistent identity. Repeated CPU controls serialize readiness
and isolate launcher supervision. These limitations remain in the experiment
receipts instead of being erased by a successful later run.

## Completed CPU controls

Two fresh boots per policy, forward/reverse ordering, use the same final
`9cc712e6...` kernel, eight vCPUs, 6 GiB, host CPUs 0–13, silent `none` audio,
same disks/static CPU binary and 24-worker/four-sleeper 20-second workload:

| Policy | Wakeup p95 range | Largest wakeup lateness | Mean work batches | Skipped 16-ms periods across two runs |
| --- | --- | --- | ---: | ---: |
| RR | 26.37–91.39 ms | 95.21 ms | 4,677,696.5 | 6,603 |
| MLFQ | 23.93–23.99 ms | 47.40 ms | 4,573,674.5 | 1,769 |
| EEVDF | 1.56–1.88 ms | 8.88 ms | 4,488,757.5 | 0 |

EEVDF trades about 4.0% of RR's fixed-work throughput for much lower observed
wake latency here. A zero skip count does not mean zero deadline lateness or
hard realtime guarantees. RR's large run-to-run latency variation remains
visible. Batch ratios are not precise CPU-time fairness. Request lengths differ
(RR's 10-ms wall slices versus EEVDF's service requests), and these are CPU-only
controls, not a claim that EEVDF alone explains every application difference.
All six accepted cohorts drain tasks and pass heap, actual original launcher
exit, four filesystems and saved-data/profile preservation. The excluded
orchestration run is retained separately. Browser video/audio/input comparisons
and broader contention/real-workload acceptance remain open; RR stays default.

## First mixed-browser controls

Three further fresh boots use the same final kernel/modules as the CPU controls,
eight vCPUs/six GiB, affinity 0–13, an 800×600 Firefox window on 1080p,
private 44.1-kHz PipeWire null sink, callback HDA, generated buffered 24-fps
video, four input probes and 24 CPU workers for 30 seconds. This is one trial
per policy, not a repeat/reverse-order browser acceptance campaign.

| Policy | Periodic p95 | Worst periodic delay | Buffered video callbacks/s | Input dispatch → DOM bounds |
| --- | ---: | ---: | ---: | --- |
| EEVDF | 3.405 ms | 2634.765 ms | 4.646 | 0–263 ms |
| RR | 89.415 ms | 98.127 ms | Insufficient loaded samples | 17.21–19.69 s |
| MLFQ | 22.250 ms | 77.412 ms | 8.155 | 0–157 ms |

Input bounds come from the buffered trace's guest/host clock brackets, not the
much larger HTTP receipt latency. EEVDF and MLFQ each advance media at about
1.001× page time and decode about 24 frames/s, but drop many frames. EEVDF's
low periodic p95 therefore does not establish a better desktop: presentation
is worse than MLFQ here, with a 2.89-second trace gap and a 2.63-second worst
periodic wakeup. Kernel code remains non-preemptible. More profiling is needed
to attribute those stalls rather than assuming the policy alone caused them.

All three cohorts capture **zero audio frames** and log Firefox cubeb
`OnMediaSinkAudioError`. A page reporting an audio track is not evidence of
successful output. This shared failure needs an independent audio-path
investigation; it prevents desktop acceptance for every policy, and must not
be reported as an EEVDF-specific regression or as successful silent playback.
The first Linux reference delivered about 23.07 callbacks/s with nonzero
captured audio, subject to its documented software-vsync workaround and other
reference configuration limits.

All three application lineages drain, original launchers exit zero, four offline
filesystem checks pass and the twelve saved documents/four original profile
trees retain their checked data and ownership. Those structural gates passed;
**desktop acceptance did not**. Evidence and explicit gate distinctions:
`build/workloads/timing-scheduler-20261020/eevdf-browser-cohorts/gates.json`.
The optional module is committed; RR remains the default. Next work is the
common Firefox audio-sink failure, scheduler/kernel stall profiles, and repeated
browser/game trials before proposing a default policy change.
