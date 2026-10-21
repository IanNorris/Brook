# POSIX timer expiry and blocking waits

The timing audit now checks the production timer implementation together with
the production block/unblock bodies, plus a finite native syscall probe.
The tested cases pass without a production change. This strengthens coverage
of timer delivery; it does not establish scheduler latency under load.

## Expiry and admission protocol

The BSP timer interrupt calls `PosixTimersTickLocked` with the process registry
lock held. Expiry uses the timer's selected clock: relative realtime timers
select monotonic time, while absolute realtime timers use `RtcNowNs()`.
It records the pending notification and publishes each eligible recipient's
`pendingWakeup` latch. It does not acquire the ready lock while holding the
timer lock. The subsequent wake scan selects off-CPU blocked recipients and
passes referenced processes to the ordinary unblock path.

Interruptible block admission holds the ready lock and checks pending POSIX
timer signals as well as ordinary signals. If expiry won before admission,
that pending check or the retained wake latch prevents a missed block. If
expiry wins after those checks, the latch remains available to the blocked
wake scan. Stopped and terminal handling retains priority; ordinary timer
delivery cannot resume a job-control stop.

The lock paths are registry→timer→signal-action for expiry and
ready→timer→signal-action for admission. The inspected timer/action operations
do not acquire the scheduler locks in reverse. Hosted interleavings test
specific paths, not an exhaustive lock-order or real SMP proof.

## Hosted coverage

`test_posix_timer_clock` includes the actual `posix_timer.cpp`, real `Process`
layout, and shared production scheduler block/unblock bodies. IRQ transitions,
context switches, queue side effects, signal actions and registry target lookup
are shimmed. The original independent realtime/monotonic epoch tests remain.

Added cases exercise:

- Expiry before admission; immediately before the ready lock; after clearing
  an earlier wake latch; while blocked with the ready lock still held; after
  that lock is released; and pending publication by an expired timer query
  without an interrupt tick or wake latch.
- Masked caught/ignored notifications remaining synchronously consumable;
  unblocked ignored signals and SIGEV_NONE not interrupting an ordinary wait.
- Relative realtime versus absolute realtime clock selection, malformed and
  unrepresentable requests leaving an existing deadline unchanged, and
  addition-overflow rejection.
- Thread-target incarnation matching, one pending notification across periodic
  expirations, exact overrun accumulation, delete/exit cleanup, no timer-ID
  reuse, overrun saturation and periodic deadline overflow disarming.

All 73 hosted suites pass. Final additions to fixture initialization and the
periodic overflow case pass the affected suite. These tests verify Brook's
current overflow rejection contract; they do not certify Linux behavior for
very large, structurally valid timeout values. That comparison remains open.

## Native syscall probe

`scripts/workloads/posix_timer_waits.c` builds a static x86-64 executable and
runs unchanged on Linux and Brook. Each of eighteen forked cases has a guest
alarm and a parent watchdog, and every child is reaped. The cases cover:

- Relative/absolute monotonic and realtime timer notifications interrupting
  sleeps, poll, infinite futex and epoll waits, with SI_TIMER and value cookies.
- An already-pending masked timer consumed by sigsuspend; caught and ignored
  masked synchronous waiting; ignored and SIGEV_NONE sleep controls.
- Relative monotonic and absolute realtime timerfd readiness; periodic overruns;
  deletion, invalid nanoseconds, thread-targeted notification, disarming an
  already-pending timer, and timerfd rearming clearing unread expirations.

The executable SHA-256 is
`8285656d7892636492be6dca923b12fae65964ef22605e2f8803227e079defe1`.
All eighteen cases pass on host Linux and in
`unattended-posix-timer-mlfq-a-20261021`. The VM uses unchanged kernel
`ee4310ff055494f647dcf417e633e9a05becb1d31e2048b066caf73d0dd7aebd`,
the original thirteen ABI-34 modules, MLFQ/blocking accounting, eight vCPUs/eight
GiB and normal host mask 0–15. Coalescing, history, owner/yield and expensive
timing probes are off. No graphics workload, CPU pressure or audio source runs.

Both original transfer/probe jobs finish with complete identity/group/output/EOF
receipts. The controller and original VM owner exit zero, four filesystem
checks pass, and runtime, policy-ledger/ready-count, heap and IRQ IF/stale-ticket
gates remain clean. Checked evidence is retained in
`build/workloads/timing-scheduler-20261021/posix-timer/verification.json`.

The native probe covers actual expiry/delivery behavior; deterministic hosted
tests supply the specifically arranged admission interleavings. Neither proves
every asynchronous race, physical/unstable-clock fallback, arbitrary load,
complete CPU-timer/job-control support or Linux-compatible huge-timeout
saturation. Older application runtime failures and historical playback
acceleration remain open. No scheduler default or audio policy changes.
