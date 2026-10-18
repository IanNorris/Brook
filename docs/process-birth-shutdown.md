# Process births, shutdown admission and PID capacity

This records the reservation/PID-capacity foundation and its original tests.
The [orderly shutdown coordinator](orderly-shutdown.md#implemented-coordinator)
now uses the gate in live launcher/MCP shutdown, together with service teardown,
cleanup drain and a final block-write barrier. Statements below about future
coordination describe the foundation's original acceptance scope.

The shutdown coordinator must distinguish an empty task census from a process
still being constructed. `ProcessCreateImpl`, `ProcessFork` and
`ProcessCreateThread` allocate a PID before `SchedulerAddProcess` publishes the
process. A final census alone cannot see that unpublished interval.

## Reservation and publication

`ProcessIdPool<MAX_PROCESSES>` now owns allocation, LIFO recycling and unpublished
user-birth reservations under the scheduler's existing PID lock. User allocation
checks the closed gate and reserves the birth in the same critical section.
Kernel-thread allocation stays available while user births are closed, subject
to the same finite PID capacity. Construction failures release the reservation
through the existing `SchedulerFreePid` paths. PID release can precede deferred
`FreeProcessStruct` / address-space cleanup, so a drained reservation count is
not proof that destruction, mapped-file writeback or descriptor closes finished.

`SchedulerAddProcess` clears the reservation while still holding the ready and
process-registry locks, after registry insertion. It takes the PID lock last:
ready -> registry -> PID. Allocation/release never takes another lock while
holding PID. Clearing after the publication locks are released would be unsafe:
a child could run, exit and recycle its PID before the original publisher clears
the reservation, accidentally clearing a different birth's lease.

PID zero remains reserved. Full capacity returns zero instead of incrementing
beyond the PID table and panicking. Both ordinary user-image creation and kernel
thread creation now check zero before allocating further resources; fork and
thread creation retain their existing failure handling. The value state rejects
invalid IDs and duplicate releases before reuse. It does not add generation
validation to the existing PID-only release API; callers must still own the PID.
No Process layout or module ABI change is involved.

`SchedulerSetUserBirthClosed` is an internal primitive for the future lifecycle
coordinator. Closing is **not yet exercised by a live shutdown coordinator**.
The host value-state fixture exercises closure with admitted births, failed
construction, publication after closure, reopening, concurrent callers using an
owner lock, full capacity and reuse. The guest workload tests the real open-gate
allocation/publication/cleanup paths. The production gate stays open during
these guest tests.

A future coordinator must close admission only after app-save/close handling,
wait for pending births to drain, and then check remaining user tasks. Pending
zero is not an assertion that registered tasks exited or writes stopped. The
gate does not stop existing user tasks, their shared-mapping stores, kernel
writers, or pending top-level launcher requests. A final writer barrier and
service coordination are still required before the final checked flush.

The independent scheduler-policy replacement / InitProcess publication race
in [the MLFQ safety audit](mlfq-safety.md) remains open. This change does not
repair it or enable MLFQ; the default remains round robin.

`vm_stats` gains `user_birth_closed` and `pending_user_births`. The pair is
sampled under the PID lock; process counts and other telemetry are sampled
separately. Neither field by itself is a shutdown-completion signal.

## Capacity acceptance (18 October)

Session `d6dfc0b4c2024c4fabc7b39a19de17a9`, eight CPUs and 8 GiB, used kernel
SHA-256 `3ac2c60958927db9ce40458ccd19551bf75ee827d55d1525181a66deedb32adb`.
The static, guest-only `scripts/workloads/pid_capacity.c` fixture is explicitly
bounded to 1,536 children and a two-second hold. Children close the pipe's write
end and block on its read end; the parent closes the last writer to release
them, waits every child, and performs 64 further fork/wait cycles. The fixture
was not executed on the host.

Two rounds each reached **1,008 children**, returned the existing fork error
ENOMEM, reaped all 1,008 with successful wait statuses, and completed 64 recovery
forks. Serial contains exactly two `FORK: PID allocation failed` records,
confirming PID allocation was the limiting path rather than inferring it merely
from ENOMEM. POSIX/Linux EAGAIN reporting for PID exhaustion remains a separate
errno refinement. No kernel panic or user fault occurred. A separate Python
64-thread barrier/join probe passed.

Process counts returned to 12 after each round and after the thread test, with
zero pending births. The sampled capacity statistics arrived during cleanup
(230 tasks); they are not a measurement of the peak task count. PMM used memory
rose from about 231 MB cold to 336 MB after the first round. During the second
round the committed kernel heap remained 116,652 KiB; live heap bytes rose by
131,328, consistent with a new retained debug command's output buffers but not
proof of complete absence of leaks. No throughput improvement or general memory
reclamation guarantee is claimed. The workload can cause long compositor
intervals; this is a correctness/capacity probe, not smooth-desktop acceptance.

All four tracked commands ran as UID 1000 and exited zero, with both original
and full command lineages empty and exact stdout/stderr EOF without gaps/drops.
Heap integrity passed and audio was inactive. Checked sync succeeded, QEMU
exited and the original launcher returned zero. All three read-only filesystem
checks passed. Seven owned files (five existing documents, the lifecycle probe
and the executable fixture) retained exact hashes and UID 1000/GID 100.

An independent read-only review found no missing reservation release, new unsafe
publication path or PID-lock inversion in the inspected creation/failure paths.
It explicitly does not establish live coordinator or final-writer quiescence.

All 42 native suites and the compiled entry audit pass; nine bootstrap observer
tests pass. Full bootstrap renewal for this 42-suite source remains pending.
These tests do not establish an integrated shutdown fence, unsaved-document
handling, service teardown, a writer barrier, OOM victim selection, or application
behavior under prolonged resource exhaustion.

Evidence: `build/workloads/shutdown-birth-gate-20261018/acceptance-audit.json`,
`review/pid-capacity/command-evidence.json`,
`review/pid-capacity-second/command-evidence.json`,
`review/thread-publication/command-evidence.json`,
`build/shutdown-birth-gate-build-20261018.log`,
`build/shutdown-birth-gate-observer-20261018.log` and
`build/pid-capacity-probe-build-20261018.log`.
