# Private namespace root-relocation checkpoint

The Steam working branch implements `pivot_root` for a private, noninitial
mount namespace. It supports a new mounted root with an `oldroot` directory and
the self-stack `pivot_root(".", ".")` form. This extends
[recursive bind](recursive-bind-checkpoint.md). It does not yet make SteamRT
usable: unchanged bubblewrap next requires `/proc/self/mountinfo`.

## Topology and context publication

The caller needs CAP_SYS_ADMIN in the mount namespace owner. The old root must
be the caller's exact namespace root; the new root must be a live, unlocked,
owned mount root, and put_old must be reachable below it. Shared propagation,
foreign/detached roots and changing the initial legacy/kernel namespace remain
unsupported. Graph ancestry uses retained paths, not string prefixes.

All reporting labels and allocations are prepared under the VFS mutation
transaction. Publication detaches the new root from its old cover, attaches the
old root at put_old, changes the namespace root and transfers its inherited
root lock. Covered-path releases happen after short locks are dropped. A
mount-target-specific follow-down lets `umount2(".", MNT_DETACH)` remove the
covering old root after a self-pivot; ordinary retained directory lookup keeps
its existing semantics.

A live FsContext registry serializes snapshots, copies and root publication.
Lock order is VFS transaction, registry, context; the graph callback additionally
uses registry then metadata-view lock. That callback does no allocation, backend
work or destruction. Exact old-root matches in root/cwd are replaced together;
descendant cwd and held directory descriptors retain their original identities.
New path references are retained under publication; counted old-root references
are released outside short locks.

Context copies allocate first, then copy and register in the same registry
window. Context creation validates its explicit root against the current or
prepared namespace root under the VFS transaction through registration. Stale
creation fails EAGAIN. This bounded Create API does not establish arbitrary
chroot-root creation. Context revisions prevent a path-based chdir from
publishing an obsolete lookup after root/cwd changes; it retries resolution.
Intentional retained-fd fchdir remains a valid old-tree capability.

The graph and context validation follow the relevant structure of upstream
[Linux pivot_root](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/refs/tags/v6.12/fs/namespace.c)
and [exact root/cwd replacement](https://github.com/torvalds/linux/blob/v6.12/fs/fs_struct.c#L54).
This is a bounded private-topology implementation, not complete Linux container
isolation or a claim that retained capabilities are revoked.

## Validation and limits

Evidence: `build/workloads/pivot-root-20261023` (private, ignored).
The identical static `pivotrootcheck` binary passes Linux 6.18.26 and Brook:
fixture 53 and parent 8 checks, plus an executed child (inherited/cumulative
counts are not independent totals). It exercises both pivots, direct dot
unmount after self-pivot, observer-process root/cwd rebasing, retained old-root
fchdir, root identity and execution after the final pivot.

The exec probe uses Brook's existing `/proc/self/exe` pathname rewrite. It proves
execution after relocation, not retained executable-inode or proc magic-link
semantics across rename/unlink. Fresh/bind roots continue to suppress synthetic
brokers: recursive binding does not invent device nodes or executable aliases.
Read-only/noexec bind-remount enforcement and mountinfo reporting are not part
of this checkpoint.

All 100 host tests pass. Actual-VFS tests cover both graph rewrites, allocation
failure before publication with unchanged contexts/topology/references, shared
and independent contexts, unchanged descendant cwd, stale Create and chdir
rejection, retained descriptors, concurrent context copying/final release and
conditional cwd publication. The affected VFS test passes 20 consecutive runs
and ASan/UBSan. This concurrent stress does not prove every possible scheduler
interleaving. Compiled entry and scheduler-start audits with mutations pass.

Frozen ABI-43 kernel SHA256:
`572b38589204302c6f595422313db5405e195b91de65942b811ecf31f11432ac`.
All 13 modules match. The internal opaque context representation and snapshot
are not exported module interfaces; no module-visible layout changed.
Probe SHA256:
`a3fae542c4c599c1d92b0f317a5cf28396167d5ff279a3242657abd0176da252`.
Native session: `0966ed8301194ee7be7187a372758180`.
Review: `build/reviews/pivot-root-off-gpu-1ms-fixed-mlfq-a-20261023`.

The pivot, recursive-bind, both tmpfs modes, propagation, proc namespace,
user-namespace and comprehensive mount probes pass, as do nine
credential/filesystem/IPC regressions and Mousepad launch/render/close.
All original jobs and the desktop client group finish empty. Heap integrity,
checked shutdown, original wrapper exit zero and four clean offline filesystem
checks all pass. The Linux unit terminates with MainPID 0,
no control group and success; its original VM exits zero and passes offline
filesystem checks.

## Actual unchanged bubblewrap result

Bubblewrap 0.12.0 with the previously accepted executable hash runs as UID 1000:

```
srt-bwrap --unshare-user --bind / / -- /boot/BIN/NNP
```

It completes the first pivot and advances into new-root binding setup, then
exits 1:

```
bwrap: Can't open /proc/self/mountinfo: No such file or directory
```

The collecting wrapper exits zero while preserving that actual application
exit and stderr. Its trivial child has not executed. The successful portable
probe's child is a separate acceptance result; it must not be confused with
unchanged bubblewrap or SteamRT working.
