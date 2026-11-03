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
comparison baseline. Rows are bounded to eight tasks and prioritize changed
representations, then ADD failures, then wait activity; more than eight such
tasks can still hide lower-ranked rows.

With disposable QEMU kernels, the diagnostic fixture made one ordinary
duplicate ADD and one ADD after a cross-type descriptor replacement. With the
option on, counters reported `exists=2`, `changed_target=1`; with it off, they
reported `exists=2`, `changed_target=0`, with comparison unavailable. These
checks do not fix or change epoll behavior.

Subsequently the exact optional-diagnostic pair booted in disposable QEMU and
was installed in the Dell's update slots; its previous boot slot was retained.
With a software GLX smoke test passing, a nested Xephyr display, silent Pulse
sink, and a 256-task limit, Steam remained on its loading screen. Its job peaked
at 183 tasks with zero denied births, and a responsive Dell reported two
browser-helper timeouts and no `epoll_ctl EEXIST` assertions in the completed
client log. Those observations do not exclude other resource limits or prior
run failures, but make the reproduced epoll defect insufficient on its own to
explain this particular stall. A TERM request to the Steam client completed and
its tracked descendants drained. The diagnostic is not a fix.

The initial diagnostic sorted rows by wait-call volume, which could hide the
rare failures of interest. The current version ranks changed representations
and ADD failures first; a disposable QEMU test with nine higher-volume tasks
confirmed that a lower-volume failure remained visible. The Dell run above
used the earlier ranking, so its top-eight output cannot prove the absence of
kernel-side errors outside those rows.

A subsequent bounded Dell run again reached the loading stall, with a peak of
177 tasks and no denied births. A completed three-second CPU capture reported
1,200 timer samples, 15,589 context switches and no producer drops. Of the
timer samples, 1,126 landed in the idle loop; this is evidence against simple
CPU saturation during that interval, not a measurement of time in every wait
or proof of the cause. Context-switch stacks for the Steam and helper threads
included poll and `recvmsg` waits. The client and its descendants were stopped
and drained after capture.

A prior capture on the same run filled the small RAM boot filesystem: the
profiler reported idle despite a truncated file. The writer now checks short
writes and sync failures, and reports failed output; the profile converter
rejects missing or truncated end records unless explicitly asked to accept a
partial capture. A disposable QEMU test confirmed a successful short capture
and an explicit failure after filling its boot filesystem. A readable footer
alone does not establish that a failed sync was durable.

The per-thread syscall timing diagnostic originally depended exclusively on
stable KVM pvclock. On the physical Dell it produced invalid timestamps, so it
could not identify the blocked syscall. It now selects a source at watch time:
stable KVM time when available, otherwise the raw BSP LAPIC interrupt counter.
The latter is explicitly reported as `lapic_delivered_ticks`: it has nominal
1 ms granularity and can drift or undercount interrupt stalls, so it is not a
reliable bound on wall-clock latency. Audio's separate clock is unchanged.
A non-KVM QEMU test of a repeated 10 ms sleep reported thousands of completed
syscalls, zero invalid timestamps and the expected coarse-source label. Physical
Dell behavior and the actual Steam wait remain to be verified.

A full fix must retain open-description identity across dup, fork and queued
SCM_RIGHTS, resolve readiness against the watched description rather than the
waiting process's numeric descriptor table, remove watches after the final
owner closes, and preserve nested-epoll graph safety and waiter lifetimes. A
numeric-generation-only fix or unconditional removal on close would not meet
those requirements.
