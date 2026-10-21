# Signal interruption of timed waits

Caught signals must interrupt poll and sleeping waits even when the handler has
`SA_RESTART`. Brook's syscall-return whitelist instead replayed poll,
nanosleep, clock_nanosleep and every futex operation. Replaying the original
relative request extended a wait; absolute requests hid the interruption until
the original deadline. This is independent of the unresolved audio speedup and
contention problems.

The restart policy now uses the interrupted frame's own RSI/R10 arguments:
only untimed FUTEX_WAIT/FUTEX_WAIT_BITSET retain futex replay. Existing slow I/O
and wait4 replay stays unchanged. Other pre-existing I/O restart semantics,
including close, have not been broadly certified by this change.

There is also a signal-to-block admission race. The ordinary signal sender used
an unlocked `Blocked` state check before waking. A signal arriving after the
caller's last pending check but before block admission could miss the wake;
futex enqueue could then clear an earlier wake latch. The sender now always
serializes its ordinary wake through SchedulerUnblock. Interruptible admission
checks ordinary and POSIX timer pending signals under the ready lock. Terminal
cleanup and stopped-state yielding precede the ordinary-signal return.
Uninterruptible kernel mutex waits still wait for ownership grants. Sleeping,
pause and sigsuspend paths use interruptible admission.

The timer check introduces ready→timer→signal-action lock nesting. The reviewed
timer/action paths have no reverse scheduler acquisition; timer expiry publishes
a wake latch rather than acquiring the ready lock. This is a source review,
not an exhaustive lock-order proof.

## Regression evidence

`scripts/workloads/signal_waits.c` builds a static x86-64 executable used unchanged
on host Linux and Brook. Thirteen forked cases cover relative/absolute sleeps,
poll, temporary-mask ppoll/pselect, epoll_pwait, relative/absolute timed futex,
and retained read/infinite-futex restart. Every handler uses SA_RESTART. The
parent bounds startup/waiting, kills timed-out children and reaps each child.
Relative sleep remainders must be normalized and positive; absolute sleep
remainder sentinels must stay untouched. Temporary masks must be restored.

To build it with the repository's existing musl toolchain:

```sh
nix-shell --run '$MUSL_CC -O2 -static -pthread -Wall -Wextra -Werror scripts/workloads/signal_waits.c -o /tmp/brook-signal-waits'
/tmp/brook-signal-waits
```

Run that same ELF in Brook with normal process/output collection; do not invoke
it through a timeout wrapper that could lose ownership of its child group.

The host control passes all thirteen. The native pre-fix baseline fails eight:
four sleep cases, poll and three finite futex cases. For example, a 300-ms
relative request interrupted near 50 ms returns success or timeout near 353 ms;
it should report EINTR at the caught signal. The first fixed-kernel native run
passes thirteen, but predates the additional stopped-state review correction.
The final reviewed kernel also passes all thirteen; see the final validation below.

The hosted test executes the extracted production scheduler block body with
IRQ/context shims. It checks pending/masked ordinary and timer signals, terminal
cancellation, ordinary kernel mutex blocking, retained wake latches and the
signal-publication/cleared-latch admission interleaving. A stopped task with a
deliverable signal must reach yield while still Stopped, without queue removal.
The initial policy/admission checks fail twelve assertions on the pre-fix body;
the final hosted checks pass. Hosted shims do not prove real SMP context switches
or arbitrary asynchronous interleavings.

Raw native evidence is private under
`build/reviews/unattended-signal-waits-{baseline,fixed}-off-a-20261021`.
Both original VM owners exit zero and their four primary filesystem checks
pass. Failed semantic exit1 is preserved in the baseline's original complete
job evidence rather than rewritten into a successful job.

## Limits and follow-up

The special SIGCONT after-block gap is now fixed in a separate
[notification/resumption follow-up](sigcont-wakeup.md): the identical eleven-case
ELF fails five baseline cases and passes all eleven after the fix, while the
original thirteen-case restart probe still passes. Default/ignored SIGCONT no
longer spuriously interrupts unrelated reads. This does not certify complete
job-control ordering or pending stop-signal cancellation.
POSIX timer expiry before/after real block admission and broader timeout
restart/overflow semantics remain follow-up coverage. No scheduler default,
quantum, audio lead or diagnostic default changes here.

The interface rules are described by [Linux signal(7)](https://man7.org/linux/man-pages/man7/signal.7.html).
For finite futex behavior, the pinned Linux 6.12
[wait implementation](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/futex/waitwake.c)
stores a restart block, while the x86
[handler path](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/kernel/signal.c)
turns that result into EINTR for a caught handler. Untimed waits use the
SA_RESTART-sensitive result. The same static host probe confirms this distinction.

## Final reviewed kernel

The final-source kernel is
`b3ffe02c3254f5911016d66bc784ef667ce51b3c6315dec383a335974e45d84c`.
Both symbol-table links and independent regeneration pass; all thirteen original
modules retain their hashes and resolve against 109 exports at ABI 34. All
**72 hosted suites** pass, including the added stopped-state assertion.

`unattended-signal-waits-final-off-a-20261021` repeats the same thirteen-case ELF
with all owner/yield, idle-coalescing, history and lock-trace flags disabled.
All thirteen cases pass, all children are reaped, and the original probe group
has complete output/EOF and collected exit0. Final runtime error counters stay
zero, policy service matches the ledger, ready counts agree, heap integrity
passes and IRQ IF/stale-ticket error counters stay zero. The original VM owner
exits zero and all four primary filesystem checks pass.

This validates the bounded caught-SIGUSR1 cases and reviewed admission body;
it is not evidence that browser or game playback has recovered.

## Browser follow-up: correctness pass is not runtime acceptance

One matched baseline/final Firefox pair uses fresh private profiles and images,
original thirteen modules/12-KiB HDA, MLFQ, blocking AP accounting, eight vCPUs,
eight GiB, physical PipeWire callback audio and normal host masks 0–15. Owner,
yield, idle coalescing, history and expensive timing recorders remain off.
Each arm includes two browser launches, twenty seconds of eight guest CPU
workers during the first, one timestamped input, and finite four/eight-fragment
OSS sources. Both VMs exit zero; all eight primary filesystem checks pass.
Sixteen original guest jobs have complete identity/group/EOF/exit receipts.

| Observation | Baseline | Final signal fix |
| --- | ---: | ---: |
| Loaded audio completion / guest second | 0.9794 | 0.9569 |
| Loaded video callbacks / page second | 10.97 | 10.68 |
| Relaunch quiet video callbacks / page second | 11.82 | 11.54 |
| Finite four-fragment source drain | 10.0646 s | 10.0757 s |
| Finite eight-fragment source drain | 10.0216 s | 10.0232 s |
| Final runtime clamps | 0 | **2 — failed gate** |

This single shared-host pair does not establish a performance change. Stored
PCM covers approximately the full host capture interval in both arms; its
quiet bins do not measure speaker glitches. Finite packet fits are near 1× per
stored PCM second and do not reproduce the historical playback speedup.

The final arm's first clamp is CPU3/PID621/incarnation621 at guest wall
132.683339517 s: wall advances 58,758 ns while reported steal advances 66,440 ns,
leaving a 7,682-ns deficit. The second browser's pre-close runtime snapshot has
zero clamps; the first finite OSS source starts later at 143.139288498 s.
Thus the retained counter places this failure after that browser snapshot and
before the finite source, not inside its measured audio interval. No causal
history was enabled, so it does not identify the faulty publication or prove
that the signal change caused the failure.

This is another native **MLFQ** runtime-gate failure. Earlier targeted-wakeup
MLFQ and physical-output RR controls already had clamps; the later matched
normal-placement study had clean MLFQ/RR cohorts and two failed EEVDF cohorts.
All of those older failures remain valid; the latest recurrence reinforces a
cross-policy accounting question. A bounded history capture is the next diagnostic,
without changing clamp classification or scheduler defaults.

Raw browser evidence is under
`build/reviews/unattended-signal-browser-{baseline,fixed}-a-20261021`.
The checked reduction is
`build/workloads/timing-scheduler-20261021/signal-browser-cohort/policy-summary.json`;
`signal-waits/verification.json` records the unchanged probe binary, frozen
module/kernel checks, original guest jobs and separate failed browser gate.


The follow-up `unattended-signal-clamp-history-fixed-a-20261021` repeats two
Firefox launches, first-launch guest CPU load and both finite sources with only
the existing-sample history enabled. It records 8,585,266 events/6,561,854 runtime
samples with no clamp. The stopped export retains 512 rows; per-CPU sequence,
owner/floor continuity, fresh sample attempts/charges and total overwritten
prefix accounting verify. Eight original guest jobs, the original VM owner and
all four filesystem checks pass. This negative capture does not explain or
clear any failed run and cannot establish an absence of observer effects.
