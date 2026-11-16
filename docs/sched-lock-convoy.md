# Scheduler-lock convoy under vCPU overcommit

QEMU guests with eight vCPUs froze or crawled when the host was loaded:
every sampled vCPU sat in `SchedLockWaitForTicket`, `time.sleep(1 ms)` x200
took 14 s, and bridge queries took 12–25 s. This note records the measurement,
the cause and the two changes on `work/sched-convoy`. Evidence lives in the
ignored `build/sched-convoy/` directory of that worktree.

## Measurement: `sched lock-stats`

`sched lock-stats on|off|reset|status` (default off) reports, for the ready,
all-process and PID locks: acquisitions, contended acquisitions, total and
maximum wait, hold and handoff (predecessor release to next ownership), the
measuring vCPU's own KVM steal during waits and holds, log2 histograms (bucket
`b` starts at 2^(b+6) TSC cycles) and per-call-site rows keyed by caller and
line, sorted by total hold. Times are TSC cycles converted with the calibrated
TSC frequency. Each CPU writes only its own row with IF clear; reset bumps an
epoch and rows clear lazily. Off costs one relaxed load per acquisition and
one owner-local field check per release. `sched lock-owner` (existing) only
counts owner-publication classifications at spin checkpoints; it has no
durations or sites.

## What the numbers show

A single eight-vCPU desktop VM, toggled at runtime between configurations.
"Pinned overcommit": the QEMU process pinned to host CPUs 0–3, four host busy
loops pinned to the same CPUs and seven guest busy-loop jobs, so vCPUs stay
runnable and compete for host time. Each configuration ran two rounds of five
bridge queries plus the 200 x 1 ms sleep job, with statistics on.

| Configuration (pinned 4 CPUs) | sleep 200x1ms, guest s | sleep job, host s | query p50 | ready-lock contended | ready-lock wait (all CPUs) | hold (all CPUs) |
| --- | --- | --- | --- | --- | --- | --- |
| A: FIFO ticket, no idle skip (old behaviour) | 1.62 / 1.49 | 31.1 / 30.6 | 0.25 / 0.11 s | 86.7% | 588 s | 1.6 s |
| B: FIFO ticket, idle skip | 2.08 / 1.31 | 33.1 / 31.7 | 0.17 / 0.11 s | 88.4% | 566 s | 1.6 s |
| C: unfair claim, no idle skip | 0.57 / 0.53 | 1.53 / 1.54 | 0.05 / 0.06 s | 3.7% | 1.9 s | 0.43 s |
| D: unfair claim, idle skip (new default in VMs) | 0.62 / 0.46 | 1.51 / 0.82 | 0.04 / 0.05 s | 3.7% | 2.4 s (2.3 s own steal) | 0.47 s |

With statistics off the old behaviour does not merely crawl; it freezes. Under
the same pinned load, the unmodified `main` kernel (`c48be213…`) answered no
bridge query and never completed the sleep job in two separate runs (four
rounds, 20 of 20 queries timed out). The guest stayed frozen after the host
loops stopped, held there by its own busy jobs, and recovered within 12 s once
QEMU was unpinned. The new kernel in `ticket` mode with statistics off froze
the same way. In `unfair` + idle-skip mode with statistics off, three rounds gave
sleep 0.64–0.90 s guest time, sleep job 1.6–2.7 s and query p50 0.08–0.11 s.
No D configuration froze in any run: 2- and 4-CPU pinning, host-only load of
32 and 64 loops, with statistics on and off. Statistics-on timing delays the
lock enough that FIFO mode limps instead of freezing; treat the A row above as
a lower bound on the damage.

With QEMU pinned to two host CPUs, A spent 95.8% of acquisitions contended
(594 s aggregate wait, sleep job 37.5/31.9 s); D stayed at 2.8% (sleep job
2.2/2.2 s).

In A, the eight vCPUs together spent about as much time waiting for the ready
lock as the whole measured interval: the guest did little else. Holds were
short (mean under 1 µs, maximum about 2 ms), so no long critical section
explains the waits. The handoff histogram
has its mode at 5–11 µs and a tail to 35 ms: ownership was being passed to a
vCPU the host was not running. During the original freeze, memory reads showed
`next - serving == 8` (every vCPU holding a ticket) with `serving` advancing
about 740 times per second, against a demand of tens of thousands per second
from per-CPU timer ticks.

Cause: **lock-waiter preemption on a FIFO ticket lock.** A ticket lock hands
the lock to one specific waiter. Under overcommit that waiter is often
descheduled by the host, and every other vCPU spins (IF clear) until the host
runs it. Because each CPU's timer interrupt takes the lock at 1 kHz, demand
exceeds the handoff rate and the guest stops making progress. KVM's PLE
directed yield cannot help much, because the vCPU that must run next is a
waiter, not a holder. The existing owner-directed `lock-yield` hint helps only
for holder preemption, which is why it improved one earlier observation without
addressing the convoy.

With the host loaded but the guest idle, the convoy does not form (vCPUs halt
between ticks and get host wakeup preference), but idle CPUs still generated
most of the lock traffic: in a 32-host-loop run, `irq-idle-dispatch` and
`irq-accounting-ap-idle` accounted for 220k of 597k ready-lock acquisitions.
With idle skip these fell to 17k, total acquisitions to 417k, total wait from
1.28 s to 0.06 s.

## Changes

1. **Unfair claim under a hypervisor** (`sched_lock_claim.h`). When CPUID
   reports a hypervisor, scheduler locks are taken by a compare-and-swap on the
   existing ticket counters that succeeds only while no ticket is outstanding
   (`next == serving`). Whichever vCPU is running when the lock frees takes it.
   After 65,536 failed checks the waiter reserves an ordinary FIFO ticket,
   which bounds starvation (the concurrency test sees a handful of fallbacks
   in 32,000 contended acquisitions). FIFO, try and unfair acquirers interoperate,
   so `sched lock-mode ticket|unfair` switches at runtime. Bare metal keeps FIFO
   tickets. Ticket identities, the owner observation (now also from the
   unfair spin, with the same one-hint budget) and the lock trace still work;
   in unfair mode the trace's wait bracket covers only a fallback wait.
2. **Idle ticks skip the ready lock when nothing is queued.** Every ready-lock
   release publishes the policy's ready count to a lock-free hint. An idle
   CPU's timer tick that sees zero skips both the runtime sample and the idle
   dispatch; the next dispatch's ownership sample charges the whole idle
   interval to the idle owner, as before. Enqueuers already kick an idle CPU
   after releasing, and every tick rechecks the hint, so a stale zero delays
   pickup by at most one tick. The skip is disabled while ready-latency
   watching or coalesced idle accounting are on, and the BSP still takes the
   lock when the 500-ms lost-enqueue check is due. `sched idle-skip on|off`.

No IRQ-state handling changed: acquisitions still clear IF before the claim and
releases restore it exactly as before.

## Expected effect on the Dell

The Dell is bare metal, so it keeps FIFO tickets; only the idle skip applies.
With four CPUs often idle during I/O-bound loads, each idle CPU drops two
global-lock round trips per millisecond (about 8,000 acquisitions per second
when three CPUs are idle). Some of the Torchlight II mod-load profile's 9.4%
at `SchedLockRelease` is IRQ-deferred samples attributed to the `sti` there.
Since the idle-tick acquisitions run in the timer interrupt and release
without `sti`, they do not produce that skid directly. The skid comes from
sections entered with IF set, so the idle skip mainly shortens other CPUs'
waits. The busiest such sites in these runs are `KernelRegionTryEnter` and
`SchedulerKernelRegionLeave`: two ready-lock acquisitions per user copy (roughly
6,000–8,000 per second each on an idle desktop). They are
the likeliest source of the Dell's release-point samples. Removing them needs
a lock-free region protocol against `SchedulerMarkTerminated` (see follow-ups).
Use `sched lock-stats` on the Dell to confirm before changing it.

## Follow-ups (not done)

- Kernel-region enter/leave: make the outermost enter lock-free. This needs a
  store-then-load (Dekker) protocol shared with `SchedulerMarkTerminated`, which
  currently reads depth and then publishes Terminated under the ready lock.
- Block then yield takes the ready lock twice (`SchedulerBlockImpl`, then
  `SchedulerYield`). Merging them is equivalent to an existing interleaving,
  but the hosted block tests inject races between the two, so it needs test
  changes.
- The BSP scans every process each tick under `g_allProcLock`
  (`PosixTimersTickLocked` and `CheckBlockedWakeups`, about 1 µs per tick in
  these runs). A sorted timer queue would remove that.
- virtio-net TX polls for device completion while holding `g_txLock` (IF set).
  This is not part of the scheduler convoy, but under host load it serialises
  senders for as long as QEMU takes. It needs more than one TX descriptor
  instead of the wait.
