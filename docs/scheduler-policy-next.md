# Scheduler policy direction after the timing audit

This is a proposal, not an implemented policy or a recommendation to change the
default. RR remains the default. Current results and safety limits are in
[timing-scheduler-audit.md](timing-scheduler-audit.md).

## What the measurements justify

The bounded CPU/sleeper workload shows that equal compute service does not imply
short wakeup latency. The first repeated RR results have roughly 7–8 ms sleeper
p95 at eight busy threads and 91–94 ms at 24 on eight vCPUs. The completed
same-kernel comparison gives MLFQ roughly 21–23 ms at both worker counts: better
under heavier load, worse under lighter load. Its earlier pilot includes a
roughly one-second shared outlier. Later browser pilots reproduce this
outlier and expose backwards CMOS rebases of the legacy scheduler clock;
the elapsed-clock candidate needs matched browser acceptance. Browser/audio/input workloads must validate any policy choice; a
synthetic sleeper is not an input-to-presentation test.

Observable running-interval accounting is now available independently of the
legacy delivered-interrupt CPU ticks. A generation-keyed ledger uses stable KVM
elapsed nanoseconds minus reported per-vCPU host steal, sampled at dispatch,
exit and timer boundaries. Standard thread/process CPU clocks use this ledger;
`sched runtime status` exposes source, steal availability and errors. It includes
kernel/IRQ work while a task owns the CPU; legacy user/system ticks remain
separate. See the native idle/loaded acceptance and limitations in the audit.
EEVDF still needs the policy callback/ABI integration below: a correct counter
alone does not charge a policy's virtual runtime. Do not treat old `/proc` CPU
ticks or fixed-work batch counts as precise service accounting.

## Why consider EEVDF

The upstream [EEVDF description](https://docs.kernel.org/scheduler/sched-eevdf.html)
selects among tasks owed service (nonnegative lag), choosing the earliest
virtual deadline. Shorter service requests can improve latency while weighted
service accounting retains fairness. Its sleeper treatment prevents short
sleeps from erasing a task's service debt. These properties address both the
loaded FIFO latency and the risk of rewarding every short block in MLFQ.
This is a design motivation, not evidence that EEVDF will fix Brook's playback
speedup or kernel stalls.

## Mechanism prerequisites

1. Preserve serialization of every policy-state access. Timeslice reads and
   expiry decisions now hold `g_readyLock`, as do registry lookup and replacement.
   Rebuild holds ready/all-process locks, excludes terminated and still-on-CPU
   ready tasks, and preserves initial priorities. Task policy registration is
   atomic with birth, closing the initialize/publish gap. Native switch/churn
   coverage is recorded in the audit; permanent provider ownership is now enforced as described below.
2. Define task lifetime and queue ownership across Ready-but-on-CPU windows,
   post-switch requeue, exit, stop/continue, wake races and PID reuse. Preserve
   the existing off-CPU publication and lifetime holds. Never dispatch the same
   task on two CPUs.
3. Policy registration now permanently retains the table, name and callback
   providers before publishing registry pointers. Module mappings are published
   before init; registered providers refuse unload even when inactive or when
   initialization later fails. A short loader registry lock serializes slot
   reservation, retention and unload claims, never module code or page teardown.
   Metadata snapshots copy strings under that lock. This deliberately does not
   implement policy unregister/hot replacement; providers remain resident until
   reboot. Targeted host races and native active/inactive unload refusal are
   recorded in the audit. New EEVDF callbacks must join the retained dependency
   list when its policy ABI is extended.
4. Wire the measured runtime ledger into an explicit policy ABI callback.
   The existing ledger accounts each running interval once, survives policy
   switches, retains exited siblings in the live group's total, detects PID
   reuse and rebinding on Brook nonleader exec, and subtracts available KVM
   steal. Every flush matters, including CPU-clock queries before a later
   block/preemption: do not silently omit that already-committed partial service
   from the policy. Define transition baselines for running tasks. CPU accounting
   includes kernel/IRQ work; source failures discard an unknown interval rather
   than charging it to the next task. Fallback/unsupported-steal modes must stay
   explicit. Separate user/system accounting and legacy CPU statistics remain
   future work.
5. Add an explicit policy preemption decision. Ordering `PickNext` alone cannot
   preempt a low-priority 40-ms MLFQ slice promptly when a sleeper wakes. At a
   safe user-mode timer boundary, evaluate the decision and pick under the
   ready lock. Preserve kernel non-preemptibility until its locking contract is
   separately changed. An IPI can request reevaluation; it cannot justify an
   unsafe switch from arbitrary kernel code.

Changing `SchedOps` requires an explicit ABI/version transition and rebuilding
all affected modules. Registration must validate the required callbacks. The
16-KiB state bound now has compile-time and host checks; a larger EEVDF table
must get an explicit boot-time allocation/capacity design, never a per-tick
allocation or another unchecked fixed-size assumption.

## Proposed first EEVDF contract

- Start with equal weight 1024 per task; make weight/nice mapping explicit before
  exposing priorities. Process/app-group fairness is a separate hierarchy, not
  a consequence of thread fairness. Do not advertise realtime guarantees.
- Keep virtual runtime, virtual deadline, remaining request and bounded sleeper
  lag per task. Document fixed-point precision, saturation/rebasing and maximum
  lifetime. No unsigned-underflow interpretation of a regressing time source.
- Maintain the runnable weight/time model across both queued and currently
  running tasks on all CPUs. Select the earliest eligible deadline among tasks
  actually available to dispatch. Define progress when none are eligible;
  avoid unbounded queue scans or accidental all-CPU idling.
- Preserve service debt across brief sleeps, clamp exceptional sleeper credit,
  and test repeated block/yield patterns. Do not reset debt on wakeup.
- Charge partial service before a task blocks or is preempted. Carry remaining
  requests deliberately; changing request length must not create free service.
- Prefer the current global lock/queue initially so the new policy does not
  simultaneously change SMP ownership and balancing. Quantify lock/selection
  overhead before considering per-CPU queues.

## Required evidence before a default switch

Use an independent reference model for weighted service and eligibility, with
adversarial sleepers, sustained CPU load, unequal weights, all CPUs running,
empty queues, time updates, deadline overflow and lifecycle/PID churn. Verify
kernel mechanism races separately; serialized policy calls in a host model do
not prove actual SMP safety. Keep compiled entry audits' supported scope visible.

Run repeated, alternating-order private boots with equal kernel, image, host
CPU affinity, RAM, display visibility and audio backend. Include CPU throughput
and per-task service, periodic deadlines and queue latency tails, pipe/event
wakeups, real UI responsiveness, browser media progress and native audio output.
Retain startup and steady-state measurements separately. Add host contention
with fixed worker/time bounds and reserve control capacity. Verify normal app
exit, task drain, heap integrity, sync, authoritative QEMU/launcher exit and
filesystem/user-data preservation. Keep RR as the control and retain failures.


## Matched Linux reference and EEVDF trial order

The user requested a Linux VM reference for Firefox and games before a heavy-load
EEVDF experiment. Extend the existing diskless Linux/Venus control infrastructure
(`tools/vulkan-linux-control`, `scripts/vulkan_linux_control.py`) into a separate,
persistent benchmark image; do not run the small Vulkan probe and call it a
browser/game baseline. Use the pinned nixpkgs source already used by Brook.

Match eight vCPUs, six GiB RAM, host CPU affinity 0–13, the same QEMU binary,
Intel render node, virtio-GPU/Venus path, display resolution/visibility and silent
44.1-kHz host audio sink. Preserve a manifest of all QEMU arguments, host load,
app/driver/kernel versions, scheduler, image checksums and fixture hashes. Run
one VM at a time. Native SDL/human audio acceptance is a separate cohort.

Use private persistent root/data images with disposable overlays or clones and
fresh profiles. Avoid benchmarking application startup through a shared host
9p/Nix-store mount or copying the full browser closure into guest RAM: those
would change the storage path or available RAM. No licensed assets belong in
the image derivation or git; stage locally owned game data separately. Start
with the same generated Firefox video/input/CPU fixture, then Quake II GL3 demo
and a free game such as SuperTuxKart. Separate startup, warm steady-state,
CPU throughput, buffered video progression, input-to-DOM/RAF opportunity,
audio continuity and storage measurements. Linux is a reference target rather
than a mathematically guaranteed upper bound: compositor, filesystem and guest
Mesa differences must remain visible.

After the current clock and workload measurement corrections, use this control
to guide an optional EEVDF trial under bounded heavy load. Retain RR and MLFQ
controls and the existing default. Implement runtime accounting and module
lifetime prerequisites first, then independently test eligibility/service debt,
wakeup placement, weights, overflow and SMP/lifecycle invariants before native
comparison. Do not substitute IRQ counts for precise virtual runtime or select
a default merely because one playback run appears smooth.


### First Linux reference gate completed

A persistent Linux 6.18.26 image now boots and runs the same static CPU binary,
Firefox 150.0.1/generated-media fixture and Quake II GL3/720p timing probe. The
same QEMU, host renderer, render node, vCPU/RAM/affinity and silent audio backend
are recorded. The 800×600 Firefox pilot uses an explicit Wayland software-vsync
workaround; its default-profile presentation failure remains retained. Linux
selects kvm-clock with lapic-deadline delivery. The installed 962-path system
closure verifies, normal apps drain, the owned launcher exits zero after serial
powerdown and offline ext4 checks pass. These first warm controls and their
limits are documented in `tools/linux-workload-reference/README.md`.

The working Linux pilot delivers about 23 callbacks/s for the 24-fps clip,
continuous audio and 6.56 million fixed-work batches under 24-worker contention;
periodic p95 is 0.45 ms. Brook MLFQ's corresponding first corrected pilot is
20.66 ms and 5.39 million batches, with about 7.6 video callbacks/s. Different
compositors, Firefox vsync settings and caches preclude assigning this entire
gap to the scheduler. This supplies a useful performance target rather than an
isolated EEVDF proof. Retain both Brook controls and require the runtime/lifetime
and SMP prerequisites above before enabling an experimental Brook EEVDF policy.
Standard CPU-time clock IDs 2/3 now use measured task/group running intervals;
the next policy ABI should consume that same accounting. Dynamic encoded CPU
clocks, CPU timers and separate user/system/IRQ classification remain unsupported
or separate legacy work, rather than being silently called elapsed time.
