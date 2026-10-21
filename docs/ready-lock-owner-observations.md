# Ready-lock owner observations

The [idle-accounting coalescing experiment](irq-ready-lock-sites.md) removes an
initial AP-idle acquisition but retains poor loaded audio. Existing bounded
wait traces identify callers and observation brackets, not the CPU holding the
lock. This default-off diagnostic tests whether a sampled, published holder is
reported as preempted before considering an advisory host-yield experiment.
It does not change the ticket protocol, scheduler policy or audio defaults.

`sched lock-owner on|off|status` enables new observations or reads cumulative
per-CPU counters. There is no reset. Only `g_readyLock` participates. The snapshot
contains one packed atomic `{valid, CPU, uint32 ticket}` owner descriptor and
independently read counter rows; it is not an atomic system snapshot.

## Ownership and observation contract

Normal acquisition publishes **after** the exact ticket becomes current.
Successful try acquisition publishes after `TicketTryClaim` returns, including
its stale-CAS/wrap wait. Failed tries publish nothing. Both ordinary and raw
release clear the descriptor **before** incrementing `serving`; clearing after
release could erase the next owner's publication. Disabling observations does
not cancel cleanup of a previously published owner. Acquisition while disabled
need not publish, so enabling during an existing hold can observe an unavailable
owner until that hold completes.

A waiter samples at spin checkpoints 64, 128, …, 33,554,432: at most twenty
checkpoints before the existing 50,000,000-spin timeout. It reads `serving`, the
owner descriptor, the registered owner's independent KVM preemption hint, then
rechecks `serving` and the descriptor. Classifications are missing publication,
ticket mismatch, changed ownership, self, missing steal record, preempted and
not preempted. Unknown hint bits are counted separately and never modified.
The reader performs no CPUID, clock read, allocation, hypercall, added lock or
wake/sleep operation. It never authorizes entry, revokes ownership or changes IF.
The existing acquire/release ordering, PAUSE path and timeout remain authoritative.

Counts are **not durations, continuous exposure, sampling probabilities or a
complete contention history**. Missing publication includes release/handoff
and post-grant/pre-publication gaps. A waiter can be descheduled after its ticket
becomes current but before it publishes. The outer loop check can also race with
handoff, so a checkpoint can observe an already granted waiter. A clear hint
does not establish that a CPU is running. Even matching before/after observations
do not prove continuous ownership or a preemption duration. Full uint32 ticket
cycles are not globally unique lifetime identities.

One uninterrupted enabled collection interval avoids a further bookkeeping
ambiguity: off/on during a live wait can suppress its initial `waits` increment
but admit later checkpoints. Thus `observations <= 20 * waits` is not a general
invariant across arbitrary control transitions. Each wait retains its own
bounded checkpoint sequence.

Disable admission and drain before requiring stable rows, owner descriptor
zero, per-CPU `published == cleared`, classifications summing to `observations`,
zero publication errors/self observations and no saturation. Positive preempted
observations qualify the sampled hint, not a dominant cause of audio stalls.
Counts across setup, load and drain must not be labelled as load-only evidence.

## Validation and native protocol

All **71** host suites pass. The new suite uses the actual atomic ticket/try
helpers with eight concurrent owners, 24,000 acquisitions and uint32 wrap. It
checks exclusive protected work, matched publication/cleanup and classifications.
Focused cases cover disable-before-release, handoff during hint sampling,
missing/stale/self ownership, unsupported records and successor publication.
The actual KVM byte-reader test covers unavailable CPUs, unchanged failure
output, independent odd-version hint reads, no mutation and no CPUID calls.
These hosted tests do not execute the scheduler's checkpoint wiring or establish
absence of instrumentation interference.

The native kernel links through both symbol-table passes; independent table
regeneration matches. Thirteen preserved modules resolve against 109 exports
at ABI 34. Frozen kernel SHA-256 is
`ce5eb59fa228ce06b448279371a205dfcc7d5c2d69e88af0f680144f310528b4`.
Only the kernel and symbol table differ from the preserved module capsule.

A same-kernel observation-off/on pair retains MLFQ/AP try, idle coalescing off,
original 12-KiB HDA lead, physical callback audio with 20-ms buffering, eight
vCPUs and the finite four-fragment direct-OSS source. Eight bounded host workers
share CPUs 0–7 for twenty seconds; named I/O-helper exceptions retain CPUs 0–15.
Lock trace, runtime history and expensive timer/ready recording stay off in both.
Useful loaded progress, original source EOF/group identity, exact drain, final
runtime/policy/heap checks and original VM/four-filesystem gates remain separate.
The results below validate the narrow observation path; this diagnostic is
not a fix or default-policy gate.

## Native off/on results

Both private runs reproduce poor loaded audio. The observation-off original
source fails with EPIPE after **32.058 guest seconds**, with 685,212 frames
submitted and final completion unknown. Its measured loaded completion is
**0.009994 nominal per guest second**. The enabled source drains exactly
1,764,000 frames in **58.190 guest seconds**, but loaded completion remains
**0.025039 nominal per guest second**. Every WAV and complete audio-query host
bracket lies inside the observed execution interval of every host worker.

The enabled interval records:

| Observation | Count |
| --- | ---: |
| Published and cleared ownership | 2,579,899 each |
| Wait entries | 88,264 |
| Bounded checkpoints | 72,188 |
| Missing owner publication | 70,813 |
| Ownership changed across qualification | 215 |
| Qualified owner, KVM preempted bit set | **82** |
| Qualified owner, preempted bit clear | 1,078 |
| Ticket mismatch, self, missing record, other hint bits, publication errors | 0 |

These are whole enabled-interval counts, including setup and drain, **not
load-only counts or evidence that owner preemption dominates stall duration**.
Missing publication is deliberately ambiguous and cannot identify a descheduled
next-ticket waiter. The positive 82 observations establish that the proposed
hint condition occurs; the large missing-owner category motivates separating
already-granted checkpoints from genuine unpublished-owner waiting in further
attribution. No CPU identity or duration is reconstructed from aggregate counts.

After disabling, three stable snapshots have owner zero, equal published/cleared
counts on every CPU, reconciled classification totals and no saturation/errors.
This is a drained-workload consistency check, not a global CPU rendezvous or
proof that every possible in-flight observer has finished. All final runtime,
policy/ledger, heap and IRQ IF/stale-ticket gates pass. The failed source remains
failed, with original identity, empty group, exit 1 and complete output preserved.
Both original VM owners exit zero and all eight filesystem checks pass.

Aggregate vCPU host execution is 10.53 CPU-s off versus 11.53 CPU-s on across
roughly 19.9-second observed subintervals. One variable pair cannot quantify
causal diagnostic overhead or establish a playback improvement. Neither probe
mode repairs historical acceleration, earlier EEVDF clamps or severe pressure.
Raw evidence is under
`build/reviews/unattended-lock-owner-{off,on}-a-20261021`.
`reboot-resume/summarize-lock-owner.py` checks original source/VM ownership,
mode/counter reconciliation, host overlap and native gates before reducing
`build/workloads/timing-scheduler-20261021/lock-owner-summary.json`.

## Conditional advisory-yield follow-up

Only positive owner-qualified preemption exposure justifies considering a
separate default-off hint. KVM's documented `KVM_HC_SCHED_YIELD` takes an APIC ID;
Linux resolves it through an APIC map and may decline to yield. Its return alone
does not establish that the target ran. See the primary
[KVM hypercall documentation](https://www.kernel.org/doc/html/v5.16/virt/kvm/hypercalls.html)
and pinned [Linux v6.12 implementation](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/kvm/x86.c).
The pinned source is a reference, not an assertion of the current host version.

Any future experiment must cache feature/instruction selection outside the wait
loop, use Brook's actual CPU-to-LAPIC mapping, recheck serving/owner around the
hint decision, bound retries and per-acquisition hints, and keep exact ticket
admission authoritative. A stale hint target may affect performance only. It
must preserve IF and retain ordinary PAUSE fallback. No HLT or guest wake/kick
protocol is part of this proposal. Compare observation-only with hint-enabled
on the same build; keep ordinary QEMU/PLE settings, host directed-yield counters
and useful audio progress gates. More host CPU or successful yields alone is
not playback acceptance. Zero eligible hints makes that experiment inconclusive.
