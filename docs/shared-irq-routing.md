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

## Separate network wake filter

The routing repair deliberately retains the network driver's existing wake
behavior. A subsequent source audit finds that `VirtioNetIrqBody` acknowledges
the NIC's read-clear ISR byte but ignores its value, waking `net_poll` on every
shared-line dispatch. A disk interrupt with zero NIC status therefore causes
unnecessary scheduler admissions. This is separate from duplicate registration.

The bounded correction reads the byte exactly once and returns on zero.
All nonzero values retain the existing worker wake, including queue bit 0,
configuration-change bit 1, their combination and unexpected reserved bits.
See the [Virtio PCI ISR status capability](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-740004.1.4.5).
No packet processing moves into IRQ context, no MMIO access is added, and EOI
remains the shared dispatcher's responsibility. This does not implement link or
configuration-change handling, device teardown or callback quiescence.

The handler is the only located reader of this ISR register. Ordinary polling
does not consume its read-clear notification. Existing pending-wake admission
covers a wake between draining and blocking; the periodic five-ms poll remains
a fallback, not a hard latency bound. Mapping precedes registration, and a
missing early poll thread remains tolerated by the existing wake function.

The extended `test_virtio_net_tx` invokes the actual IRQ body with a read-clear
device backend and worker stub. It checks all 256 status values, one read per
dispatch, acknowledgement before wake and a second zero-status dispatch without
an extra wake. The old handler fails 257 assertions; the corrected handler and
all 75 hosted suites pass. The native network module builds and its imports
resolve against the unchanged ABI-34 kernel. Astra reviewed the narrow design.

The native workload cohort completes in
`build/reviews/unattended-net-irq-filter-fixed-mlfq-b-20261021/`.
The capsule keeps kernel `163bb95f...99844` and twelve modules byte-identical;
only `virtio_net.mod` changes (`ed22e246...547c` → `bc8ee35c...61d0`).
Its `net-module-comparison.json` retains complete hashes. The same reduced-probe
controller tests bidirectional local media traffic, disk activity and mixed
apps. All nine original jobs, full resource bytes/hash and HTTP transfers,
error-free browser loop, heap, policy/ledger, HDA, runtime/debt and four
filesystem checks pass. Original controller and VM owner exit zero. Heavy
probe flags remain off around all eight captures. The 512-row cleanup history
replays with no debt episode; this negative observation does not clear the
previous cohort's clamp or earlier positive accounting failures.

Compared with the preceding corrected-IRQ cohort, ordinary mixed source
completion is essentially unchanged: 0.621/0.539 → 0.613/0.534 with batching
off, and 0.610/0.535 → 0.615/0.534 after reopening Quake. Prefix-on-C changes
0.846/0.685 → 0.884/0.717, while prefix-on-A changes 0.872/0.709 → 0.864/0.700.
Browser callbacks improve in some phases and worsen in others. This is a
single fresh-image follow-up, not a replicated population comparison or a
general playback-performance win. Native worker admissions are not counted in
this reduced-probe run: the zero-status wake elimination is established by
the actual-body tests, not inferred from unrelated FPS changes.

`mixed-prefix-control/net-irq-comparison.json` retains all eight phase metrics,
common controller hash, twelve matching modules, explicit changed-module
hashes and both quality reports. Full policy acceptance stays false and all
defaults remain unchanged. An earlier sandboxed staging attempt ends
before starting a VM because the pinned Nix dependency cannot resolve GitHub;
that incomplete review is retained separately as replica A.

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

## Reduced-probe control

A fresh pair repeats all eight phases with the same executed controller,
thirteen module hashes, media hashes, host boot, affinity, MLFQ policy and
physical PipeWire output. Audio timing/delivery/contribution probes, scheduler
timing/decision/lock probes and timer timing/peer probes are verified off before
and after every capture; actual QEMU arguments contain no host trace options.
Cached counters, WAV capture and buffered SDL swap tracing remain enabled, so
this is reduced instrumentation rather than an instrumentation-free test.

| Phase | Baseline source completion | Fixed source completion | Browser callbacks/s, baseline → fixed |
| --- | --- | --- | --- |
| Firefox alone | 0.992 | 0.998 | 11.87 → 11.15 |
| Both, prefix on A | 0.820 / 0.660 | 0.872 / 0.709 | 5.28 → 6.05 |
| Both, prefix off B | 0.612 / 0.535 | 0.621 / 0.539 | 0.52 → 4.76 |
| Both, prefix on C | 0.853 / 0.696 | 0.846 / 0.685 | 5.65 → 5.54 |
| CPU load, Quake focused | 0.912 / 0.731 | 0.870 / 0.698 | 5.80 → 5.26 |
| CPU load, Firefox focused | 0.936 / 0.745 | 0.869 / 0.692 | 6.06 → 6.05 |
| Firefox after Quake | 1.001 | 0.999 | 12.50 → 12.36 |
| Both, Quake reopened | 0.616 / 0.546 | 0.610 / 0.535 | 5.34 → 5.71 |

Source completion is nominal PCM frames completed per measured second, ordered
Firefox/Quake when both run. It is not decoded-media tempo. Browser callbacks
and SDL swaps do not establish physical scanout rate. This single pair has no
consistent overall performance benefit: the isolated baseline video dip is not
a population result, and attract scene, phase and host variance remain possible.
Do not pool it with the heavier diagnostic cohort to claim a general FPS win.
The unchanged default batching path still underfeeds simultaneous sources.

Both arms complete all nine original jobs, full media transfers and a browser
loop, with zero browser trace drops, clean original VM/controller exits and
four clean filesystem checks. Neither logs a native HDA fault. The fixed arm
nevertheless **fails runtime acceptance**: one 13,463-ns service clamp, one debt
episode and catchup, no owner transition or discarded debt. Cleanup history was
enabled later and cannot reconstruct that earlier event. The baseline has no
clamp in this run; this pair alone does not attribute the fixed arm's event to
the routing patch or clear the older accounting failures.

The two review directories are
`build/reviews/unattended-irq-clean-{baseline,fixed}-mlfq-a-20261021/`.
Each retains helper sources and `mixed-prefix-control/verification.json`;
the fixed arm's `mixed-prefix-control/irq-clean-comparison.json` retains all
phase metrics, frame-time percentiles, common-controller hash and both quality
reports. Full policy acceptance remains false. Scheduler/audio defaults stay
unchanged.
