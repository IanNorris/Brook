# Retained proc directory FD following

`/proc/self/fd/N` can now follow an owned directory capability, including
suffix lookup. It retains the captured mount/object identity across rename,
replacement, overmount, detach and concurrent close/reuse. Access remains
restricted to the current thread group and exact process/group incarnation.
No pathname is reopened from diagnostic link text.

The existing memfd broker now claims memfds only. Generic legacy proc fallback
was removed from open, stat and access: unsupported regular-file FD following
returns EOPNOTSUPP instead of successfully opening a text representation of
the link. Link inspection and O_PATH|O_NOFOLLOW remain supported. Fresh initrds
also contain a real empty /usr directory for host-layout probing.

## Validation

The identical static probe passed Linux (720 fixture / 7 parent cumulative
checks) and Brook (718 / 7, with intentionally narrower cross-process and
regular-file-follow behavior). Both exercised directory traversal, O_PATH,
rename/replacement, overmount, detach, close/reuse races and memfd reopening.
All 100 host tests, final-ELF entry/scheduler audits with mutations, mountinfo,
pivot, recursive bind, tmpfs, propagation, proc namespace, user namespace and
mount namespace probes passed. Nine native filesystem/credentials/IPC programs
and Mousepad launch/render/close passed. Original jobs completed empty; heap,
checked shutdown, original VM exit and all four offline filesystem checks passed.

Evidence: `build/workloads/proc-dir-fd-v2-20261023/completion-audit.json`.
Native session: `44488553e1ce488fb616a17596d37905`.
ABI-43 kernel with 13 matching modules:
`158b9e265c13db28c2831dddbff0a6d61bc6bb2c8fc18eb39df70d1961275909`.

The initial native run exposed the obsolete fallback and is preserved separately
under `build/workloads/proc-dir-fd-20261023`; its independent regressions, memfd
diagnostic, checked shutdown and offline checks passed. Its failed probe is not
counted as accepted.

## Actual runtime result

Unchanged bundled bubblewrap runs the NNP child successfully. The unchanged
Steam Runtime run chain now passes home-directory traversal and the missing
/usr check. It exits 1 at the read-only bind remount of /etc. Its requested
child has not executed; runtime checks were not bypassed and authentication
was not attempted. Read-only bind mounts are the next implementation step.
