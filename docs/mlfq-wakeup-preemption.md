# MLFQ wakeup preemption with retained service budgets

The [targeted queue probe](targeted-queue-activity.md) found long mixer waits
while CPUs continued running user code. MLFQ previously waited for its current
5/10/20/40 ms wall slice to expire even when a higher-priority task became ready.
The optional MLFQ policy now requests preemption for higher-ready-priority work.
RR remains the default; EEVDF remains available independently.

## Accounting and safety

Simply adding a priority check would treat every interruption as expiry: the
kernel calls `TimesliceExpired` for every positive `ShouldPreempt` decision.
MLFQ distinguishes early interruption from actual budget expiry. Early
interruption preserves priority and consumed service, including when the core
cannot select an eligible peer. Actual expiry consumes one quantum and demotes
one level, retaining excess service debt. Expiry wins over a simultaneous wakeup.
Changing a queued task's priority unlinks and relinks it without changing the
ready count. PID initialization clears the old identity's budget and reason.

Budgets use the existing service ledger, including partial dispatches and
migration, rather than renewing a wall slice on every dispatch. Kernel ownership
and IRQ work remain included; reported KVM steal is excluded. Host blocking not
classified as steal can still consume service. Legacy callers with no positive
service observations retain wall-expiry behavior; an initial entirely stolen
interval is a known fallback edge case.

The existing voluntary-yield/block reward remains: reset budget and boost toward
HIGH. Deliberate yield/block gaming is not solved by this change. On native
blocking, `ReadyQueueRemoveLocked` charges the outgoing owner before
`VoluntaryYield` resets its budget. The later dispatch flush charges only the
remaining scheduler bookkeeping tail. A source review initially questioned
this ordering but withdrew that concern after checking the removal flush.

Debug snapshots expose total policy service and a bounded actual queue walk.
They permit comparison with the kernel ledger without changing policy ABI 2 or
module ABI 34. No default policy or wake-scan mode changes.

## Matched native comparison

Four fresh private eight-vCPU/six-GiB boots run the same generated local Firefox
video/audio fixture with 24 CPU workers and private PipeWire null output.
Order is control, candidate, candidate, control. All explicitly enable the
optional shared wake-scan handoff. This isolates the MLFQ module change **under
shared wake scanning**, not the default BSP configuration.

The kernel is byte-identical in all four cases:
`2c114615fa3af49f3c78d56de7dc17d31bbf94f88362ba53459eb72e36c47aea`.
All other twelve modules are identical. The candidate MLFQ SHA-256 is
`298c4362ffd028cd8d03bd23e7d459bd434aad0cc121c3a5d3fdedbfe98c0304`;
the ignored comparison manifest records both variants and verifies that only
`drivers/sched_mlfq.mod` differs. Control source hashes include the current
checkout; its staged MLFQ binary deliberately comes from the preserved prior
module. An earlier binary comparison against an older kernel failed because
the rebuilt kernel differed, so that older kernel is not used as the control.

| Run | Periodic p95 | Mixer max ready wait | Browser video callbacks/s | CPU batches | Longest silent 10 ms bins |
| --- | ---: | ---: | ---: | ---: | ---: |
| mp-b1, control | 20.397 ms | 50.125 ms | 6.806 | 5,834,816 | 8 |
| mp-c1, candidate | 1.979 ms | 22.652 ms | 7.958 | 5,328,796 | 9 |
| mp-c2, candidate | 2.086 ms | 25.880 ms | 7.999 | 5,365,216 | 3 |
| mp-b2, control | 19.257 ms | 49.179 ms | 6.137 | 5,748,742 | 3 |

Browser callbacks use only the buffered loaded interval, with no under-half-
second buffer samples. They measure presentation callbacks, not decoded frame
throughput. Compared by two-run means, periodic p95 drops about 90%, browser
callbacks increase about 23%, and CPU batches decrease about 8%. These few
repeats show a useful responsiveness tradeoff, not statistical confidence or
general desktop acceptance. Silence results overlap and do not establish an
audio continuity fix. Residual 23–26 ms ready waits and mixer service gaps remain.
The fixed-work CPU batch max/min ratios are 1.024/1.016 in controls and
1.105/1.074 in candidates: every worker progresses, but short-run background
work distribution becomes less even. These are batch counts, not measured
per-thread service shares or a long-term starvation bound.
Captured PCM rate stays close to one; historical accelerated playback is not
reproduced or conclusively explained by this experiment.

All four runs pass original launcher/router completion, drained CPU/browser
lineages, runtime accounting, heap integrity, four filesystem checks and saved
data preservation. Candidate policy service exactly matches the kernel ledger;
reported and walked queue counts agree, with no saturation. Timer/watch clock
and activity validity checks pass. No native HDA publication fault occurs.
The original controller completes, all QEMU executables are absent afterward,
and private services are stopped through their original owners.

Ignored evidence lives under
`build/workloads/timing-scheduler-20261020/mlfq-wakeup-cohorts`, including raw
snapshots, source identities, original controller/launchers and verification.
The comparison manifest and frozen capsules sit alongside that directory.

## Default BSP wake-scan comparison

Four further fresh private boots use the same frozen kernel/module variants,
fixture and workload, again in control/candidate/candidate/control order. This
series explicitly retains BSP wake scanning; AP scan counts stay zero. No
kernel rebuild or module change intervenes.

| Run | Periodic p95 | Mixer max ready wait | Browser video callbacks/s | CPU batches | Longest silent 10 ms bins |
| --- | ---: | ---: | ---: | ---: | ---: |
| mb-b1, control | 21.462 ms | 45.147 ms | 6.340 | 5,600,320 | 4 |
| mb-c1, candidate | 5.528 ms | 21.068 ms | 7.992 | 5,517,065 | 3 |
| mb-c2, candidate | 5.728 ms | 36.302 ms | 7.824 | 5,294,795 | 8 |
| mb-b2, control | 23.529 ms | 44.113 ms | 6.655 | 5,767,963 | 4 |

By two-run means, periodic p95 improves about 75%, browser callbacks about 22%,
and fixed-work CPU throughput decreases about 5%. Background batch max/min
ratios are 1.023/1.017 in controls and 1.093/1.094 in candidates. Thus the
responsiveness gain survives the default wake path, with the same short-run
fairness caveat. Silence still does not improve consistently: the second
candidate has eight silent bins versus four in either control.

Mixer maxima in both tables cover the diagnostic load window, which includes
workload startup/drain around the actual CPU interval. Browser and periodic
statistics have their own stated loaded scopes. Do not infer that every retained
mixer maximum occurred inside the CPU helper's exact thirty-second interval.
For example, mb-c1's 157-ms wakeup lateness precedes that interval.

All four boots pass the same lifecycle, accounting, queue consistency, heap,
filesystem and preservation checks as the shared-scanning series, with no
native HDA fault. Original owners complete, and the loopback service stops.
The private null-audio service is deliberately retained for the subsequent
Quake II test. Ignored `mlfq-bsp-cohorts` retains raw data, verifiers and original
owners; the comparison uses the same frozen capsules and module manifest.

### Separating late wakeups from ready waits

A stricter offline correlation requires the same mixer PID/incarnation and
stable clock, valid non-saturated probes, a nominal sleep due upper bound inside
a retained BSP arrival gap, and ready publication inside that arrival's wake
scan. The entire mixer event must lie within the CPU helper's recorded start/end
for the loaded subset below. A later dispatch must precede the mixer's resume.

| Run | Retained loaded matches | Largest matched deadline lateness | Containing BSP gap | Ready wait in that event |
| --- | ---: | ---: | ---: | ---: |
| mb-b1 | 6 | 107 ms | 115.806 ms | 5.487 ms |
| mb-c1 | 11 | 88 ms | 89.150 ms | 0.146 ms |
| mb-c2 | 12 | 77 ms | 80.220 ms | 0.041 ms |
| mb-b2 | 5 | 78 ms | 81.972 ms | 0.037 ms |

Every matched episode has at least one independently observed timer arrival on
each of the other seven CPUs inside the BSP gap, proven by their valid complete
window maximum-gap bounds. Reported steal in the four listed gaps is only
0.037–0.097 ms. Dispatch-to-mixer-resume is about 3–4 microseconds. These episodes
therefore locate most of their delay before ready publication, separate from
policy dispatch waits. They do not explain why BSP interrupt entry was delayed,
prove a safe handoff, or establish a silence-to-wakeup causal alignment.

The 157-ms mb-c1 event outside the exact CPU interval is also retained: its
167.230-ms BSP gap ends at an instruction immediately following interrupt
restoration in `SchedulerBlockImpl` in the exact frozen binary. This is an
endpoint, not proof that the whole gap was spent in that function or in any
particular lock. Guest interrupt masking and host device waits remain distinct
leads requiring additional evidence. Top-sixteen retention means matched counts
are not event rates or complete latency distributions.

## Regression coverage and remaining acceptance

Hosted tests load the production policy through its vtable. They cover repeated
early interruptions, exact service boundaries, multi-quantum debt at lowest
priority, simultaneous expiry/wakeup, continuation without an eligible peer,
block/reset/final-charge callback order, queue relinking, PID reuse, saturation
and inactive accounting. An eight-CPU model checks unique ownership, periodic
wakeup latency, background progress and exact service totals; the existing
100,000-step interleaved model also remains. Callback models do not prove native
affinity rejection or assembly context-switch correctness.

The initial candidate passed all 66 hosted suites, thirteen-module import audit
and the compiled entry audit (twelve supported templates, 56 mutations; four
paths unsupported). The additional review tests pass in the existing Nix
toolchain (`mlfq-wakeup-review-tests-nix.log`); an initial direct build outside
that shell fails to locate libc++, so it is retained as an environment failure.
Broader
desktop/gaming acceptance, adversarial yield
behavior and direct EEVDF eligibility/debt diagnostics remain open. This is an
optional policy improvement, not a default scheduler decision.
