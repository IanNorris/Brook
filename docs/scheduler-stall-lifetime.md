# Idle stall reporter lifetime

The idle BSP's persistent-ready diagnostic selected a raw `Process*` under
`g_allProcLock`, released that lock, then read PID, name and trace entries and
used the PID for a policy query. A remote termination followed by wait/reap
could detach and destroy that process before those reads. The scan also read
per-CPU pending-requeue slots without their ready-lock protection, and treated
two different lifetimes of a reused PID as one persistent candidate.

The reporter now takes the established ready → all-process lock order, checks
eligibility and pending requeues, and copies a bounded value snapshot. Policy
debug data is captured while the policy remains serialized. Persistence
requires both PID and incarnation. Both locks are released before formatting;
no raw process pointer leaves the protected scan. The name copy always ends
with a terminator. A persistent candidate remains diagnostic evidence, not a
proof that every Ready task must immediately run.

The actual reporter body is shared with `test_sched_stall`. Its retirement
case makes the process page inaccessible at the first post-unlock opportunity:
the extracted original body segfaults; the fixed body still reports its saved
name and trace. Additional cases cover PID reuse, legitimate deferred requeue,
a remotely running task, termination, CPU ineligibility, an absent policy
debug callback and an unterminated source name. Lock shims require the stated
order, protected policy reads, and printing outside both locks. Eight fixed
cases pass; the legacy body fails all eight including the lifetime crash.
This is a deterministic arranged interleaving, not arbitrary concurrent kernel
execution or a full scheduler-mechanism proof.

A fail-fast release kernel/module build, compiled entry/startup audits, and all
77 hosted suites pass. The first attempted environment command used
`nix develop` and failed because this repository uses `shell.nix`; the completed
build used `nix-shell`. Evidence is retained under
`build/workloads/timing-scheduler-20261021/scheduler-stall-safety/`, including
`legacy-actual-body.log`, `fixed-actual-body.log`, `full-build.log` and
`host-tests.log`. Native boot/profile validation follows separately. There is
no scheduling-policy, timeslice, preemption or audio-default change.
