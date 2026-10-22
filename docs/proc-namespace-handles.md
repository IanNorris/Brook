# Mount namespace handles for bubblewrap

Status (2026-10-22): implementation, Linux comparison and native acceptance pass.
This phase exposes real mount namespace handles. It does not add other
namespace domains, setns/unshare, tmpfs or root switching.

The shipped bubblewrap 0.12.0 first opens `/proc/<child>/ns` using O_PATH, then
stats `mnt` relative to that directory. Its diagnostic formats an extra `/ns`,
which produced the earlier `/proc/22/ns/ns` message. No such doubled path is
actually requested. The source is
[bubblewrap's namespace_ids_read](https://github.com/containers/bubblewrap/blob/v0.12.0/bubblewrap.c#L2600).
The same executable and smoke-test arguments remain unchanged.

## Object and lifetime contract

Only `mnt` is listed under the new retained proc namespace directory. Its
unfollowed link retains the target PID, incarnation and thread-group identity.
Directory lookup checks those identities and refuses a dead/reused target.
Following the link yields an independently owned namespace object, rather than
parsing its display name to resolve another path.

The followed object's inode is a checked, non-reused mount-namespace ID, shared
by all processes that belong to that namespace. Initial namespace ID 1 is
reserved; private IDs begin at 2. Its device/type identify a namespace object,
not a proc symlink. Readlink reports `mnt:[ID]`; lstat and an unfollowed O_PATH
handle report the proc link instead. Proc link identities follow the existing
live-object interning rules, independently of stable namespace identities.

A followed namespace handle owns both namespace storage and an active topology
lease. Dup, fork and SCM_RIGHTS retain the same open object's ownership. The last
task can exit while its mount tree remains searchable through an already-held
foreign directory. Closing the final namespace-handle copy retires that tree;
plain directory descriptors and unfollowed proc links cannot keep it published.
Final release queues backend cleanup and never takes the sleeping VFS mutation
lock synchronously. Allocation failure releases the prepared active/storage lease.
This lifetime distinction follows
[Linux namespace references](https://github.com/torvalds/linux/blob/v6.12/fs/namespace.c#L5407).

Readlink and follow use an intentionally restricted inspection policy: the
caller's exact thread group, or actual CAP_SYS_PTRACE authority in the target's
user namespace. Creator-parent authority works through the existing canonical
user-namespace capability rules. This does not implement Linux's complete
PTRACE_MODE_READ_FSCREDS/dumpability/LSM policy; unrelated same-UID inspection may
be denied. No permission decision assumes a dumpability setting that Brook does
not track. An already-authorized descriptor remains transferable and does not
repeat path-inspection authorization at fstat or receive time.

## Descriptor behavior

O_PATH now creates a distinct path descriptor over retained VFS objects. Only
O_PATH, O_DIRECTORY and O_NOFOLLOW survive in its status flags; O_CLOEXEC is a
separate descriptor flag. Access, create and truncate bits do not turn it into
a data descriptor. Relative directory lookup, fstat, fchdir and supported fcntl
operations use the retained object. Close, dup, fork and SCM transfer own normal
balanced vnode references.

Path descriptors reject data I/O, seek, sync, direct chmod/chown/truncate,
mutation of descriptor status and ioctl with EBADF. Poll reports POLLNVAL and
epoll rejects registration with EBADF. Ordinary namespace read descriptors
instead return EINVAL on read and ESPIPE on seek. Namespace write opens fail
EPERM. NS_GET_NSTYPE reports CLONE_NEWNS; other namespace ioctls fail ENOTTY.
Other O_PATH operations and synthetic-only pathname brokers are not promoted to
full Linux support by this slice. No writable descriptor is fabricated for an
unsupported backend.

The retained magic-link callback now borrows its source VfsPath so a pseudo
object target can retain the correct attachment. Kernel/module ABI advances to
42; all modules must be staged from the matched capsule.

## Comparison and acceptance

Private evidence is under `build/workloads/proc-namespaces-20261022`.
The portable source is `tools/at-syscall-probe/procnamespacecheck.c`.
Counters are cumulative within each process, not independent totals to sum.

The final Linux probe passes 284 cumulative parent checks. It exercises identity,
link/follow/stat behavior, O_PATH rejection, ordinary handles, sibling-user-namespace
inspection denial, transfer of an already-authorized handle, final-task exit,
ordinary versus O_PATH namespace handles, dup/SCM transfer, fork inheritance and
final-close retirement of nested mounts through a retained foreign dirfd. The
held proc directory/link does not supply an active namespace lease. Stale lookup
after target exit fails instead of following a new process. Forced PID-number
reuse is not demonstrated by this run; incarnation checking is also reviewed in
the shared retained proc lookup code.

The first extended Linux probe mistakenly expected the musl fchmod wrapper to
return the raw syscall error. Musl's pathname fallback produces a different
result; the failed original service is retained, and raw SYS_fchmod/SYS_fchown
comparisons pass. A root-only fixture could not be started through the reference
guest's noninteractive sudo policy, so the unprivileged Linux fixture uses
sibling user namespaces for denied inspection. No guest policy is weakened.
Both original Linux VMs shut down through ACPI and pass offline filesystem checks.

The first Brook candidate exposes an additional ftruncate error mismatch (EINVAL
instead of EBADF on O_PATH). The original failed probe terminates with no live
tasks; its VM shuts down cleanly and all four filesystems check clean. The final
candidate adds the missing guard and reruns the identical Linux-tested binary.

All 99 host tests pass, and six affected ASan/UBSan tests pass. The actual hosted
VFS test covers active handle leases, nested mounts after final task exit,
last-handle retirement, failed acquisition before activation/after retirement,
and final ownership cleanup. Compiled entry/start audits remain bounded
structural checks, not arbitrary handler or scheduler correctness proofs.

The final Brook candidate passes the identical 284-check probe as UID 1000 and
286 cumulative parent checks in the separate root-started cross-UID fixture.
The latter drops privileges before namespace creation and verifies denial to a
UID-1001 outsider alongside authorized descriptor transfer. The tested probe
binary SHA-256 is `66ac6e7a96395491c9f599054d146d216eb04f6ff9e992d9f77c6d8e70fd63bb`.

Accepted kernel SHA-256 is
`4b71a25c6644843e22de188cdc90cd068398e2206503b7132bee735022626935`,
with ABI 42 and all 13 matched modules. Native session
`024197cca28a4c1cb2109c5119bcbdcf` retains the original job identities and
terminal results in
`build/reviews/proc-namespaces-off-gpu-1ms-fixed-mlfq-b-20261022`.
The user-namespace and comprehensive mount regressions pass, as do nine existing
credential/filesystem/IPC regressions. Mousepad launches, renders and closes
with its registered client group observed empty. All command jobs finish with
no live tasks. Heap integrity is OK; checked shutdown completes, the original
launcher returns zero, and all four offline filesystem checks return zero.

The identical shipped bubblewrap executable (SHA-256
`91f2b55f25ecff638f0357abb9cc18fa1f010fd9e48e8fb59b96196827052a6c`)
now advances past proc namespace access and exits 1 at:

```
bwrap: Failed to make / slave: Invalid argument
```

At this historical checkpoint this is the next unsupported mount-propagation
operation, not a successful child
launch. The collecting wrapper exits zero while retaining bubblewrap's actual
exit 1 and stderr; no descendants remain. Mount propagation, tmpfs and root
relocation require subsequent bounded work. Steam authentication is not part
of this phase. The subsequent [private propagation checkpoint](mount-propagation-checkpoint.md)
advances the identical command to the unsupported tmpfs mount.
