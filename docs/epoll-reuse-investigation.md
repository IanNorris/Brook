# epoll descriptor reuse investigation

The Dell Steam sign-in attempt logged repeated `epoll_ctl` `EEXIST` assertions
and browser-helper watchdog restarts. The last task-limit sample preceded the
stall and had no denied births, so it cannot establish the cause of the stall.

`scripts/probe_epoll_reuse.py` passes on Linux for close/reuse, a surviving
duplicate, `dup2` replacement, and inherited epoll with divergent descriptor
tables. On the Dell, the close/reuse and `dup2` cases fail because `MOD`
incorrectly accepts the new description; a separate close/reuse reproduction
returns `EEXIST` for `ADD`. This establishes a Brook compatibility defect, not
that it caused Steam's restarts.

`epoll diag on` counts `ctl_add_exists`. A kernel built with
`BROOK_EPOLL_TARGET_DIAGNOSTICS=ON` additionally records a type/handle
representation per watched slot and reports `target_comparison_available`.
The option is off by default because it increases each epoll instance's size.
When enabled before registration, `ctl_add_changed_target` counts ADD failures
whose current representation differs from the recorded ADD/MOD representation.
This comparison is diagnostic: different wrappers can represent the same
description, and reused addresses can conceal a different one. No handle values
are exposed, and registrations made before enabling the diagnostic have no
comparison baseline. Rows are bounded to the eight most active tasks and can
omit a low-volume helper.

With disposable QEMU kernels, the diagnostic fixture made one ordinary
duplicate ADD and one ADD after a cross-type descriptor replacement. With the
option on, counters reported `exists=2`, `changed_target=1`; with it off, they
reported `exists=2`, `changed_target=0`, with comparison unavailable. These
checks do not fix or change epoll behavior, and no diagnostic kernel has been
deployed to the Dell for this investigation.

A full fix must retain open-description identity across dup, fork and queued
SCM_RIGHTS, resolve readiness against the watched description rather than the
waiting process's numeric descriptor table, remove watches after the final
owner closes, and preserve nested-epoll graph safety and waiter lifetimes. A
numeric-generation-only fix or unconditional removal on close would not meet
those requirements.
