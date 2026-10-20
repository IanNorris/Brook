# Scheduler lock attribution before a PV waiting experiment

This is the implementation and acceptance contract for the diagnostic, based on the
2026-10-20 source audit and Astra's read-only review. It is not implemented by
`sched kvm status`, and it does not authorize a different scheduler default.
The measured lock delays, failed preemption-deferral comparison, IRQ sampling
bias and native capability checks are in
[audio delivery diagnostics](audio-delivery-diagnostics.md).

## What the current critical sections contain

| Path in `src/kernel/src/scheduler.cpp` | Work under the ready lock | Consequence for attribution |
| --- | --- | --- |
| `RuntimeChargeLocked` | Direct-index runtime ledger update and policy service callback | MLFQ's callback updates counters directly; no process-list scan in the ledger sample |
| `SchedulerKernelRegionEnter/Leave` | Reference/depth/latch updates | `ProcessRef/Unref` are atomic count changes; neither frees a process inline |
| `PickNextLocked` | Policy selection, PID validation, eligibility checks and reinsertion | Bounded by the initial ready count, but can examine multiple candidates; do not assume one queue operation |
| MLFQ `Tick` | Once-per-second promotion of queued lower-priority tasks | Queue-sized operation, unlike ordinary MLFQ enqueue/dequeue/accounting |
| `SchedulerAddProcess` | Publication, policy registration, lifetime and PID lease accounting | Nests ready → all-process → PID locks; another lock's wait can extend ready-lock ownership |
| `SchedulerSwitchPolicy` | Policy reset and restoration of registered processes | Nests ready → all-process; fresh performance boots avoid hot-switch episodes |
| `DoSwitch` | Pick/claim, runtime ownership, current-task and pending-requeue/retire publication | Raw release precedes TSS/syscall-stack setup, profiler CS record and physical context switch |
| `DrainPostSwitch` | Deferred requeue under the ready lock, with reference cleanup in separate paths | Dispatch-to-drain time is not one continuous ready-lock hold |

The normal MLFQ paths do not identify an obvious expensive operation whose
removal explains the measured multi-second waits. This is source evidence, not
a bound on execution time: host descheduling and nested lock waits can suspend
even a short critical section. Error/diagnostic printing and policy replacement
also exist and must be recognizable in a capture. Do not optimize from the
post-STI sample counts as though they were unbiased instruction occupancy.

## Capture protocol

Start with one selected lock, `g_readyLock`, and fixed per-CPU storage. Select
the lock before enabling; do not allocate slots or dereference Process pointers
in the observed path. Each record needs the diagnostic epoch, CPU, local
publication sequence, ticket, phase, call site and available wall/steal samples.

1. Record reservation/waiting after a ticket has actually been reserved.
   Record acquisition only after that ticket is served. Include successful
   `TicketTryClaim` acquisitions and its stale-ticket wait fallback. A failed
   try-claim is neither an owner nor a reserved waiter.
2. Record both normal and raw releases. Take a pre-publication timestamp and,
   if useful, a post-publication timestamp before restoring IF. The post-release
   update belongs only to the releasing CPU's record: clearing a shared owner
   field afterward could erase the successor's publication.
3. Retain bounded slow acquisition/ownership episodes locally. A bounded number
   of contention snapshots may read `next`, `serving`, matching owner/waiter
   records and relevant KVM preemption hints. Avoid sampling every spin.
4. Aggregate/export outside the scheduler lock. Data publication and snapshots
   must use atomic fields and bounded sequence validation. Never wait for an
   odd sequence belonging to a host-descheduled publisher.

Reservation, acquisition and release publication each have a gap relative to
the real operation. Report unmatched, stale or changing snapshots as unknown.
An acquisition timestamp includes any delay after observing its turn and before
recording that observation. A timestamp before advancing `serving` is not the
unlock instant; a wide release bracket cannot identify who caused its delay.

Controls advance epochs without clearing live per-CPU records. A release must
close its original ticket/epoch, even if tracing was disabled or reset while it
was held. Local sequence and epoch identities must prevent a reused ticket from
accepting an old completion. Exhaustion must be explicit and disable further
capture rather than silently wrap identities. Missing clocks, unavailable steal
or hints, dropped records and inconsistent reads remain visible.

## Interpretation

| Matched evidence | Supported interpretation |
| --- | --- |
| Served ticket matches an acquired record | An identified holder was observed |
| Published release advances serving, but matching waiter has no acquisition record | Handoff/admission delay or a publication gap; absence alone does not prove no holder |
| Long ownership interval with a substantial matching steal increase | Reported host descheduling contributed during ownership |
| Long ownership interval without a corresponding steal increase | Guest work or unreported host delay; not proof of expensive guest execution |
| Positive preemption hint for a matched CPU/ticket | Supporting host-preemption evidence at that observation, not an interval-wide state |

Neither a zero hint nor wall-minus-steal alone establishes exact guest execution.
Ready-lock ownership can include waiting for the nested registry/PID locks.
Correlate a slow owner's call site before instrumenting those locks separately.
Do not add another diagnostic lock inside the observed critical section.

## Acceptance before changing lock behavior

Hosted checks must cover normal and raw releases, successful/failed try claims,
stale-ticket fallback, ticket wrap, sequence/epoch exhaustion, enable/reset/off
during waiting and ownership, stale completions, CPU bounds, missing clocks,
and deliberately inconsistent publications. Real concurrent writers/readers must
exercise bounded snapshots without introducing data races or blocking readers.
Any nested selected-lock assumption must be explicit and tested.

Native acceptance must verify default-off behavior, unchanged IF state, ticket
order, existing timeout behavior, ownership/accounting and all pick/claim/off-CPU
safety gates. Use fresh quiet and bounded-host-load cohorts, matched kernel and
modules, exact original process waits, full-window audio criteria, clean shutdown
and saved-data checks. Measure instrumentation cost separately. Retain failures
and unknown episodes; a cleaner capture is not a performance improvement.

Only attribution sufficient to separate these mechanisms justifies a default-off
PV waiting or directed-yield experiment. Per-core queues, kernel preemptibility
and EEVDF remain distinct comparisons; none follows automatically from KVM
advertising the necessary feature bits.

## Implemented first version

`sched lock-trace on|off|reset|status` observes only `g_readyLock`. It is off at
boot and samples no clocks while off and without an older active capture.
Ordinary acquisition and the successful-reservation callback in `TicketTryClaim`
publish reservation and acquisition. Both release paths publish before/after
the existing `serving` increment, before any IF restoration. Neither ticket
admission, the spin timeout nor scheduler policy callbacks change.

Per-CPU records use atomic payload fields and bounded sequence validation. Each
CPU is the sole writer of its slots, with IF masked; controls never clear them.
A local serial distinguishes reuse of the same uint32 ticket. Enable and reset
advance the window. Off preserves the current window and stops new reservations;
older reservations can finish their original window. Reset preserves enabled
state inside its CAS, so a stale external snapshot cannot undo a concurrent off.

Status exports current, largest observed wait and largest observed hold records
for each CPU, sample-valid/steal flags, active CPU mask, lifetime completion/loss/
clock-error counters, and before/after ticket/window observations. Caller tags
are hexadecimal addresses of static caller-name strings in the matched kernel
ELF. Sample order errors and missing samples are retained, not silently converted
to valid durations. Epoch/serial/publication exhaustion never wraps identities.

This first version is not a complete event journal. Maxima may retain different
episodes, so the predecessor and successor needed for a handoff match can be
missing. There are no per-spin KVM hint snapshots. Unmatched or changed rows
remain inconclusive; neither a long hold nor a clean capture establishes a
performance fix. Instrumentation overhead and native loaded attribution remain
acceptance work before considering a PV lock algorithm.

The reducer/publication tests cover ticket reuse, stale completions, in-flight
off/reset, same-CPU reentry rejection, independent max-wait/max-hold retention,
CPU bounds, missing and backwards clocks, publication/epoch exhaustion,
successful/failed/stale-success try reservations and cross-CPU active-bit
preservation. Concurrent atomic publishers/readers and per-CPU episodes with
concurrent controls pass. All 69 hosted suites and the compiled entry audit pass
(twelve supported templates, 56 mutation rejections; four templates remain
unsupported). Astra found no lock-safety blocker and identified the reset/off
control race, which was corrected before the final build.

## First native acceptance

A fresh private headless/muted VM boots the frozen kernel
`a065ff1db0ecf68eb2642449b01ac3a876676fa9e0e040221f321cebae18b4f2`
with thirteen matched ABI-34 modules, eight vCPUs, MLFQ and explicit
IRQ-accounting try. Default status is disabled, epoch zero, active mask zero,
and zero records/counters on every CPU. The controller enables capture and
runs one tracked unprivileged Python job: eight threads each execute 1,000
getpid/stat/short-sleep iterations. The original job exits zero with both output
streams at EOF, no lost bytes and no remaining guest tasks.

After off, observed current records are released, the active mask is zero,
and 707,838 lifetime completions have zero publication losses or clock errors.
Retained slow episodes exist on all eight CPUs. Max observed reservation-to-
acquisition is 32.005 microseconds; max observed acquired-to-pre-release is
30.616 microseconds. The retained ownership intervals have zero reported steal
increase. These are instrumented quiet-probe observations, not an overhead
comparison, service-time proof or reproduction of the seconds-long loaded
stall. The retained predecessor/successor records need not form a matched pair.

Off/reset remains disabled with a newer epoch; on/reset remains enabled with
a newer epoch. IRQ-accounting IF-error counters remain zero. Runtime ledger,
heap, acknowledged guest final sync, QEMU exit, separate original wrapper wait,
four offline filesystem checks and twelve saved-document/four original-profile
comparisons pass. Evidence and caller-name decoding against the exact ELF are
in `build/reviews/lock-trace-native-20261020`. This establishes functional
integration, not complete native raw/normal-release or fairness coverage.

The user live review subsequently starts the earlier established baseline,
without this trace code, with explicit speaker-audio authorization. Do not
build or stage further kernels while that review VM is running. Loaded trace
attribution and instrumentation-cost controls remain the next private work
after handback; this diagnostic is not a scheduling performance fix.
