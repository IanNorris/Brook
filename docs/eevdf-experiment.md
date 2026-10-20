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

## First mixed-browser controls — unrouted audio, superseded

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

**Later diagnosis invalidates these runs as matched media/audio acceptance.**
The isolated PipeWire daemon intentionally has no session manager. The Linux
launcher installed explicit routes, but this Brook controller omitted them.
Its QEMU output node was suspended with zero ports and no links. In a subsequent
same-kernel native OSS probe, 13,230 submitted frames had zero completions while
the producer remained blocked. Configuring and linking that same live QEMU node
resumed the same producer, delivered all 529,200 requested frames, and captured
220,928 frames over 5.00655 host seconds. The full interrupted delivery took
218.84 seconds; this is recovery evidence, not a normal-speed 12-second trial.
The first probe's capture preceded producer readiness and is explicitly excluded.

Thus the browser sink errors above do not establish a guest regression. Retain
the latency observations only with this host-audio-stall condition; do not pool
their presentation/throughput with routed Linux/Brook results. The new
`scripts/route_silent_vm_audio.py` observer validates the private daemon, selects
only nodes owned by the exact QEMU process, configures stereo ports and links
them to the null sink. The browser controller now requires a matching route
receipt before starting its workloads. Corrected controls are stored separately
under `build/workloads/timing-scheduler-20261020/eevdf-browser-routed/`.

All three application lineages drain, original launchers exit zero, four offline
filesystem checks pass and the twelve saved documents/four original profile
trees retain their checked data and ownership. Those structural gates passed;
**desktop acceptance did not**. Evidence and explicit gate distinctions:
`build/workloads/timing-scheduler-20261020/eevdf-browser-cohorts/gates.json`.
The optional module is committed; RR remains the default. Next work is the
verified audio routing, scheduler/kernel stall profiles, and repeated
browser/game trials before proposing a default policy change.

## Corrected routed browser controls

The separate `er-e0`, `er-r0`, `er-m0` runs require an explicit route-ready
receipt before browser startup. All share identical controller/page/clip hashes,
the same final kernel/modules and the previous fixed hardware/profile settings.
All three routing observers exit successfully after their owned QEMU exits.
Firefox launcher logs contain no audio-sink error; every capture has PCM.

| Policy | Periodic p95 / worst | Buffered callbacks/s | Media seconds / page second | PCM frames in ~8 s | Longest silent run in captured PCM |
| --- | --- | ---: | ---: | ---: | ---: |
| EEVDF | 3.56 / 56.68 ms | 5.73 | 0.8290 | 350,848 | 110 ms |
| RR | 89.30 / 99.47 ms | 0.034 | 0.0077 | 6,656 | 70 ms* |
| MLFQ | 20.84 / 87.35 ms | 7.21 | 0.9734 | 353,024 | 10 ms |

*QEMU's WAV capture omits intervals with no active output. RR produced only
0.151 seconds of PCM across 8.006 host seconds; the 70-ms silent sequence
inside that short recording does not measure the long missing-output interval.
Likewise EEVDF's 0.994× captured-frame/host-time ratio covers its eight-second
capture, not the entire thirty-second browser interval. It does not contradict
slower media advancement elsewhere in that interval.

Buffered dispatch-to-DOM bounds are 43–215 ms for EEVDF, 0–195 ms for MLFQ and
17.80–20.31 seconds for RR. Browser render opportunity adds 79–203 ms, 28–166 ms
and 55–71 ms respectively. These include guest/host clock uncertainty and are
not measurements of physical display latency. Buffered playback remains ready,
with at least 40.14/21.91/28.88 seconds ahead for EEVDF/RR/MLFQ; simple network
buffer starvation does not explain these loaded results. No 1.3× acceleration
is demonstrated by these tests.

All application lineages drain, heap checks pass, original launchers exit zero,
four filesystems pass offline checks and twelve documents/four original profile
trees retain their checked data/ownership. `eevdf-browser-routed/gates.json`
records these structural gates separately from desktop/default-policy acceptance.
One routed trial per policy is insufficient for a statistical decision. EEVDF
has the best synthetic wakeup p95, but MLFQ has the stronger media result here;
neither approaches the first Linux reference's 23.07 callbacks/s. RR remains
the default. Next profiling should target media/compositor CPU service and
non-preemptible kernel intervals, followed by repeated/reverse-order trials and
Quake II/interactive-game checks on the optional policy.

## Loaded CPU/context-switch profiles

Separate `bp-e0`/`bp-m0` diagnostic cohorts use `--profile-ms 2000`, with process
snapshots and the same routed browser/24-worker fixture. These are not additional
unprofiled performance repetitions. Both exports have matching producer counts
and zero drops: EEVDF has 1,537 samples/22,683 switches, MLFQ 1,547/22,185.
The analysis reconstructs each archived module's section placement from the
loader's section-order/alignment rules and its boot-logged base, resolving GPU
addresses against the exact `.mod`, rather than attributing them to the final
kernel symbol. Raw captures, Speedscope, analysis and the frozen analysis script
are under `build/workloads/timing-scheduler-20261020/browser-profile-cohorts/`.

Of EEVDF's 266 kernel samples, 173 leaf samples are GPU `SubmitCommand` and 47
the GPU command spinlock (220/266). MLFQ has 171/67 respectively (238/322).
Compositor samples are 169/176, with only 4/7 outgoing switches over the capture.
The driver holds one global ticket lock across synchronous device round trips;
completion uses a 200-million-iteration polling bound, with no cooperative
wait. Kernel timer interrupts do not preempt arbitrary kernel code. Thus faster
user-mode scheduling cannot itself release those CPUs or allow another GPU
submitter through. These sample counts identify a target, not an exact wall-time
breakdown or proof that changing the lock alone improves presentation.

Both traces also contain many outgoing waits through `UserMappingPageAccess`
(4,385/7,759), which takes the address-space mutation mutex before checking
pages. MLFQ gives Firefox more observed samples (142 versus 68 across Firefox
names), while the synthetic workers account for 1,200/1,267 samples. Switch
counts and finite IRQ samples do not establish service fairness or identify a
safe mapping-lock change. Profiled EEVDF additionally has an unexplained
11.37-second periodic outlier and much worse input than its unprofiled control;
MLFQ's worst is 131.86 ms. The profiler writes to the private FAT disk and adds
significant observer work. Retain this failure and investigate its timing;
do not interpret zero dropped events as low observer overhead.

Both guests pass application drain, routing observer exit, heap, original
launcher exit, four offline filesystem checks and saved-data/profile equality.
The next candidate should address safe cooperative GPU command contention and
completion waiting, preserving early-boot operation and command ownership,
then rerun unprofiled controls and GPU/game stability checks. The mapping waits
and profiler-induced/exposed long stall remain separate unresolved leads.

## GPU command-wait follow-up and receive deadlock

The first GPU candidate used a sleeping FIFO command mutex plus 100-µs initial
polling followed by 1-ms completion sleeps. Early BSP-only initialization kept
bounded polling. `gc-e0`/`gc-m0` passed structural/data checks, but MLFQ's buffered
presentation fell from 7.21 to 4.44 callbacks/s despite higher CPU batches.
That completion-sleep policy was rejected. Its exact sources and verdict remain
in `gpu-cooperative-browser/rejected-sleep-candidate/` under the timing workload
root. These are single pilots, not statistical estimates of a fixed slowdown.

The revised module retains the sleeping command-ownership mutex and polls for
completion. Runtime polling has a five-second stable KVM elapsed deadline,
independent of LAPIC IRQ counts; early boot or failed clock reads retain the
previous finite 200-million-pause fallback. The mutex stays held from request
preparation through response validation. No other submitter may reuse that
shared buffer while the owner waits. Before SchedulerInit, no process exists
and BSP-only module initialization bypasses the scheduler mutex. The added
kernel exports are additive; module ABI 34/layouts are unchanged and all thirteen
modules are staged and checked together.

Uncertain or invalid completion poisons the queue as before. Subsequent request
preparation is redirected into separate scratch storage, preserving the original
DMA request/response pages; poisoned SubmitCommand calls never notify the host.
The host helper tests cover completion, exact deadlines, boot fallback and
failed/regressing clock reads. They do not establish a forced-device timeout or
native late-DMA failure result; that acceptance remains open.

The first mutex-only browser run (`gm-e0`) exposed a separate TCP receive-copy
hang and was stopped with frozen vCPU/task/stack evidence. It is **not** an
accepted run even though QEMU subsequently exits zero. See the
[TCP repair and native checks](tcp-receive-copy.md). With that repair, a fresh
RR guest passes sixteen receive-copy cases and the Quake II GL3/720p timing
smoke check. A fresh EEVDF browser/24-worker cohort completes with 353,152 PCM
frames in 8.00363 host seconds and no silent 10-ms blocks. Buffered media/page
ratio is 0.9607 and callbacks 5.20/s; this does not establish a presentation win.

The revised module is SHA-256
`724914fd7298dfb50d89ce1912b3fbd5d8b916ead51541ba983c48a4daf9f4bb`.
The combined TCP-repair kernel is
`18aeb029431977d80aef5d2e3a534f13371674eef5d7a83cb58645180ac56bd1`.
Earlier sleep and polling-module candidates shared one kernel ELF, so kernel
hash alone cannot identify a GPU experiment: retain the staged module hashes.
The source change is retained on the work branch for further comparison. GPU
performance promotion, repeated/reverse-order MLFQ/RR controls, forced-device
failure behavior, and seven backward nanosecond samples from the RR ledger
remain unresolved. RR remains the scheduler default; the overall timing goal
is still active.


## Per-vCPU clock follow-up

The [pause/resume clock repair](pvclock-percpu.md) replaces shared BSP clock
reads in both the kernel and vDSO. The new EEVDF cohort `pc-be0` passes twelve
injected pauses, 1,885,268 libc/syscall bracket checks on all eight CPUs and both
native libc ABI/lifetime checks. Its loaded browser pilot has periodic p95
4.04 ms and worst 89.43 ms. Buffered video decodes at 23.18 frames/s but delivers
4.30 callbacks/s; the Linux reference's earlier 23.07 callbacks/s remains a
substantial practical target. These are separate single pilots, not a repeated
matched statistical claim. Graphics/compositor, cache and clockevent differences
remain; RR stays the default.

The new cohort exports its buffered trace without loss and reports no sampled
buffer starvation. Its 351,104 captured PCM frames over roughly eight host
seconds include a 90-ms silent run. All app lineages drain and launcher, heap,
four filesystems and original documents/profiles pass. The earlier unexplained
backwards clock samples now have a reproducible pause/resume mechanism and
native repair evidence; the original subjective application speedup still has
not been reproduced.
