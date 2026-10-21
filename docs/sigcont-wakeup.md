# SIGCONT notification and stopped-state resumption

The special SIGCONT sender bypassed the ordinary wake path. It checked Stopped
without the ready lock, resumed only that observed state, then queued SIGCONT
unconditionally. A task already blocked in an infinite futex wait could retain
a caught SIGCONT indefinitely. Conversely, default and ignored SIGCONT could
make unrelated reads return EINTR despite there being no handler to run.

This follows the [earlier signal/wait audit](signal-wait-restart.md). It does
not change clocks, timer delivery, scheduler policy or audio settings.

## Fix and locking

The sender snapshots the signal action and mask. It queues SIGCONT only for a
caught disposition or a blocked signal. Blocked signals remain consumable by
sigtimedwait even with default/ignored dispositions. It publishes that pending
bit before resumption, releases signal locks, and calls SchedulerContinue
unconditionally. The latter now checks Stopped under the ready lock and does
nothing to other states. A queued notification additionally uses the ordinary
SchedulerUnblock path to serialize against interruptible block admission.

An unmasked default/ignored notification therefore creates neither a pending
bit nor an ordinary wake latch, while still resuming a stopped target. A caught
SIGCONT wakes an already blocked waiter. The ordinary wake still respects a
newer stop; it cannot revive a stop that wins after the continuation step.

The on-CPU stop protocol remains unchanged: continuation publishes the parent
event and `resumePending`, leaves the owned stack Stopped, and does not enqueue
it prematurely. A subsequent ordinary wake preserves that token. Repeated
SIGCONT does not manufacture another continuation event while the same stop is
already being resumed, even after the parent consumes its first event.

Linux v6.12's [`prepare_signal` and `sig_ignored`](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c)
separate unconditional stop/continue effects from signal delivery and preserve
blocked notifications. Brook's action/mask reads remain snapshots: this change
does not claim atomicity with concurrent sigaction changes, pending stop-signal
cancellation, full process-group semantics or complete Linux job-control order.

## Hosted tests

The sender and unblock function bodies are extracted into
`process_signal_send.inc` and `scheduler_unblock.inc`, used by production and
`test_signal_wait`. The hosted test mocks signal publication, queue/parent
side effects and IRQ/context boundaries, rather than duplicating the sender's
decisions. Ordinary non-SIGCONT sender behavior is unchanged by extraction.

The matrix covers default/ignored notifications against Running, Ready, Blocked
and Terminated; caught/default/ignored × masked/unmasked × on/off-CPU Stopped;
repeated continuation after parent consumption; masked synchronous waiting;
caught blocked notification; and publication before block admission after a
cleared wake latch. A deterministic companion injects a newer stop between
continuation and ordinary unblock and requires no token resurrection or ready
insertion. These shims do not prove arbitrary real SMP interleavings.

Initial actual-body checks fail twenty assertions before the fix. All 73 hosted
suites pass afterward, and the added newer-stop companion passes the affected
suite. Both native symbol-table links, independent table regeneration and all
thirteen original module imports pass at ABI 34/109 exports.

## Native probe and baseline

`scripts/workloads/sigcont_waits.c` builds one static x86-64 ELF for Linux and
Brook. Eleven forked cases cover caught SIGCONT during infinite poll, futex,
epoll and read; default/ignored blocked reads; masked default/ignored synchronous
signal waiting; and default/ignored/masked stopped reads with WCONTINUED.
The parent observes each child sleeping in `/proc/PID/stat` before signalling,
uses a finite rescue signal for missed notifications, and bounds/reaps every
child. Completion readers stay open through rescue, and parent notification
polls retry EINTR using a fixed deadline.

The finalized executable SHA-256 is
`8e630394176e3f72d67c71bc505e099a3fcf835efa8fdb1875d683db99ee6c68`.
It passes all eleven cases on host Linux. The unchanged pre-fix kernel
`ef0bf43b…` fails five in
`unattended-sigcont-baseline-mlfq-c-20261021`: the infinite futex requires rescue,
and default/ignored reads, both ordinarily blocked and stopped/continued,
incorrectly return EINTR. All eleven children are reaped. The probe's expected
exit1 remains in the complete original identity/group/output/EOF evidence; the
controller and original VM owner exit zero, four filesystem checks and final
runtime/policy/heap/IRQ gates pass.

Earlier private baseline pilots A/B are preserved separately. A completion
reader closed before a rescued child finished. Pilot B also counted two
parent-only notification failures despite those children passing; its first
poll return/errno was not retained, so the exact cause is not established.
The final parent wait retries EINTR against a fixed deadline and retains the
reader through child exit. The final C run distinguishes harness outcomes from
the five child semantic failures. Do not pool pilot counts with the final
same-ELF comparison.

## Native fixed result

`unattended-sigcont-fixed-mlfq-a-20261021` repeats the identical eleven-case ELF
with kernel
`ee4310ff055494f647dcf417e633e9a05becb1d31e2048b066caf73d0dd7aebd`.
All eleven pass without rescue, including the previously indefinite futex and
four incorrect read interruptions. The unchanged earlier thirteen-case
SA_RESTART executable also passes all thirteen. Both arms use the same original
thirteen modules, MLFQ/blocking accounting, eight vCPUs/eight GiB and host mask
0–15. Audio coalescing and the other expensive/owner/history probes stay off.

The final baseline/fixed pair has six original transfer/probe jobs with complete
identity/group/output/EOF receipts. Both controllers and original VM owners exit
zero, all eight primary filesystem checks pass, runtime error counters remain
zero, policy service matches the ledger, ready counts agree, heap integrity
passes and IRQ IF/stale-ticket counters remain zero. The expected baseline
semantic failure is preserved. Checked evidence is recorded in
`build/workloads/timing-scheduler-20261021/sigcont/verification.json`.

This establishes the bounded SIGCONT regression and retained wait/restart
behavior. Older browser/game runtime failures, historical playback acceleration,
arbitrary SMP stop/continue ordering and wider timer races remain open.
