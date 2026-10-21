# Shared IOAPIC routing and duplicate dispatch

## Confirmed defects

The precise host/guest trace cohort
`unattended-reservation-host-compact-mlfq-b-20261021` boots five virtio disks
on PCI IRQ lines 10 and 11. Both lines request preferred vector 50. The old
registration code installs a different per-line stub at that same vector;
IRQ 10's later registration overwrites IRQ 11's IDT entry. Each disk also
registers the same callback, although that callback scans **all** disks.
Consequently shared dispatch repeats the complete scan for each registration.

During two matched audio-worker late-wakeup intervals (53.945 and 41.277 ms
before timer-handler arrival), the conservative clock-alignment cores cover
38.073 and 25.411 ms. CPU 0 reads each of the five legacy disk ISR ports about
252 and 173 times, respectively; the network ISR is read 126 and 86 times.
The latter reads return zero in the retained examples. Source and boot logs
prove routing/duplicate-registration defects; these overlapping I/O bursts do
not independently prove the cause of every timer gap. IRQ arrival contexts
are not unbiased CPU profiles. The NIC's existing zero-status wake behavior
is deliberately unchanged in this correction.

## Correction and ownership

Distinct shared lines receive distinct vectors from the explicitly reserved
INTx pool 48–63 excluding 49 (xHCI MSI-X). Fallback choices remain in priority
class 3, the class of the original vector 50. Existing lines retain their
actual vector regardless of subsequent preferences. Direct IDT installation
cannot consume the pool or overwrite its registered stubs. Exceptions, timer,
keyboard, mouse, xHCI MSI-X and high IPI/spurious vectors stay outside this pool.

Identical `(IRQ line, callback)` registrations increment an ownership reference
count but consume one dispatch slot. Matching unregister calls drop references;
only the last removes the callback. Removing a callback shifts its function and
reference count together. Empty lines stay masked until a new registration
reactivates them, at which point their original vector/stub is unmasked again.
Each dispatch invokes each distinct callback once and sends one EOI.

There are four compiled dispatch stubs, hence capacity is four **distinct IRQ
lines over the registry's lifetime**, even if some become empty. Empty slots
are retained, not recycled. Chain/stub/reference exhaustion, null callbacks
and PCI's invalid-IRQ sentinel fail explicitly before publication instead of
returning a success-looking preferred vector. Existing driver callers do not
check a failure sentinel, so these invalid kernel configurations panic.
This is not complete validation against the IOAPIC's supported input count.

Registration/dispatch synchronization and module-unload callback quiescence
remain preexisting limitations. These sequential registration tests and boot
checks do not establish concurrent unload safety. The patch does not add a
registry lock and claim that such a lock alone would make module unload safe.

## Verification

`test_shared_irq_registry` compiles the same private registration and dispatch
bodies used by `idt.cpp`, substituting only hardware installation/routing,
EOI and panic backends. Before behavior changes, four checks fail: vector
uniqueness, correct line dispatch, duplicate invocation and reactivation.
The fixed bodies pass those cases plus reserved-vector protection, shared-line
preferences, paired duplicate ownership, unregister shifts, retained empty
identity, chain/stub exhaustion without partial mutation, reference overflow,
invalid inputs and exactly one EOI per dispatch. These tests exercise registry
behavior; ISR assembly/register restoration remains a separate test domain.

All 75 hosted suites pass, including a rebuild of the affected suite after
the final freestanding-compatible limit check. Both native build passes,
independent symbol-table regeneration and all thirteen old ABI-34 modules/109
imports pass.
Astra reviewed the actual patch and its retained lifetime/range limitations.

The frozen native candidate is
`163bb95f558db64224c52ec902d6d2757a51a064f15ed7747f1cda66fdf99844`.
Its first private boot verifies IRQ 11→50, IRQ 10→48 and a two-callback shared
IRQ-10 chain (disk plus network). The real Firefox/GL3 Quake/bounded CPU-load
comparison completes on the same private fixtures, physical audio path, old
modules and diagnostic controls as the compact-clock baseline. All nine original
guest jobs have complete identity/output/EOF and empty groups; controller,
original VM and server exit zero, all four filesystems check cleanly, and heap,
policy/ledger, loaded-capture overlap and current runtime/debt checks pass.
Firefox completes an error-free full media loop; independent guest resource
hash and terminal HTTP transfers reconcile. No native HDA fault is logged.

In the selected Firefox-before, mixed-on-C and Firefox-after phases, largest
observed BSP arrival gaps change from 77.727/50.478/16.826 ms to
14.228/14.223/13.793 ms. The baseline retains 4/4/0 slow worker episodes there;
the candidate retains none. Across all eight candidate captures the worker
probe records no slow episodes (score at least 20 ms of excess sleep beyond
nominal ten ms, setup, service or tail). This does not mean every total sleep
is under twenty ms. Clock/probe validity, retention and exclusion checks pass.

With the existing optional shared-prefix experiment enabled, mixed on-C source
completion changes from 0.813/0.666 to 0.881/0.714 nominal for Firefox/Quake;
other enabled/load phases improve less. In the unchanged default batching
path, concurrent source completion remains near 0.61/0.53. Therefore this is a
correctness repair with a narrow positive diagnostic comparison, not a complete
audio or uninstrumented desktop performance acceptance. It combines routing and
duplicate-dispatch corrections and does not isolate their individual effects.
Prior audio/accounting failures stay open. Scheduler/audio defaults remain
unchanged.

`mixed-prefix-control/shared-irq-comparison.json` retains all eight phase rows,
including non-improving default-path captures; `verification.json` and the raw
records preserve the current checks. Repeated uninstrumented acceptance and
the separate mixed-stream underfeeding problem remain follow-up work.

Evidence paths:

- `build/workloads/timing-scheduler-20261021/reboot-resume/shared-irq-before.json`
  and `shared-irq-after.json` retain actual-body before/after results.
- `build/reviews/unattended-reservation-host-compact-mlfq-b-20261021/` retains
  the pre-fix boot, bounded host/guest traces, clock anchors and strict checks.
- `build/reviews/unattended-shared-irq-mlfq-a-20261021/` retains the candidate
  capsule, original process ownership and `boot-irq-route-check.json`.
