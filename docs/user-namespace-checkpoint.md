# Bounded user namespace foundation

Status (2026-10-22): accepted on top of 8b8097dd after source review, Linux
differential testing, native regressions and checked teardown. Steam integration
and full container security are not established.

The first supported creation requests are fork-style NEWUSER, NEWNS, or both
with SIGCHLD. Unsupported namespace domains, shared FS/VM, threads, vfork and
ancillary clone features remain errors. Creation prepares user credentials before
copying mount topology and before publishing a child. Unshare/setns remain out.

Canonical initial-namespace UID/GID values remain in Process, inode ownership,
IPC objects and queued Unix credentials. Namespace-visible values are converted
only at ABI boundaries; ordinary unmapped ID reporting uses 65534 and never
authorizes an operation. Map-file reads use raw UINT32_MAX for unmapped IDs. Namespace creation never substitutes inner UID 0 into Process::euid.

Each refcounted user namespace owns its parent and creator's canonical identity.
UID/GID maps are write-once single-ID ranges (count exactly one); unsupported
multi-range/subordinate-ID delegation is rejected. The selected unprivileged
write maps only the creator/writer's effective parent identity. Validate a whole
bounded write before publication; zero-offset single writes only. GID mapping
requires irreversible setgroups=deny without parent CAP_SETGID. Namespace map
vnodes retain the exact namespace and opening credentials, never a PID lookup
at write time. Viewer-relative map reads must not confuse unmapped IDs with root.

Task capability state is separate from numeric IDs. Child namespaces start with
namespace-local capabilities; fork inherits, selected capset/bounding drops are
real, and ordinary exec recalculates capabilities for namespace-root vs nonroot
IDs. No file capabilities or set-ID gains are introduced. Initial-namespace root
retains Brook's prior privilege model and does not advertise successful capset
privilege drops. Unsupported securebits/ambient operations remain explicit errors.

Mount namespaces own their user namespace. Authority is checked in that owner;
NEWUSER alone gives no authority over inherited parent-owned mounts. Copying a
mount topology into a less-privileged owner locks inherited attachments against
individual detach, preserving covered content. Fresh child-created directory
binds remain detachable. Filesystems and global SysV IPC retain initial-owner
privilege checks. Existing globally privileged controls must not consult an
inner UID or a child capability as global authority.

Locks never span allocation, VFS callbacks, user copying or process destruction.
Task namespace identity is immutable after publication. A short credential lock
publishes credential state and serializes map publication. Owned
references are acquired before publication and released after unlocking. Failure
must leave parent mappings, credentials and topology unchanged.

The decisive acceptance case maps inner 0 to outer 1000: child-private bind
works, inherited/parent mounts and outer-root files remain protected, and clearing
capabilities prevents further private binds. Also test the observed Steam identity
map (1000 to 1000), exec/drop behavior, unmapped IDs, malformed/repeated map
writes, GID denial ordering, fork inheritance and exact namespace cleanup.
Linux differential probes and native acceptance must precede completion. Full
Linux security/container compatibility and Steam login remain outside this phase.

Linux comparison corrected an initial test assumption: a passed uid_map open
file can delegate its opener's single-ID mapping authority even to a sibling
namespace. Brook follows that file-credential contract, pins the exact namespace
past process exit, and rejects files opened without the required authority.
Privileged parent-map exemptions still check current and opening authority;
the unprivileged creator/self single-ID case uses opening authority.
The live Linux discovery failure is preserved; it was a test expectation error,
not accepted Brook behavior.

## Implemented boundary

The per-task namespace pointer is immutable after publication and independently
retained during fork/thread credential inheritance. Namespace parents, mount
owners, proc map objects and opening credentials own separate checked references.
The initial namespace is represented by null and retains Brook's prior root
privilege model; capget/capset does not pretend to drop initial-root authority.
Depth is bounded to eight private levels, and a new namespace requires mapped
effective user and group identities in its parent. No UID/GID ranges beyond one
ID, subordinate-ID delegation, unshare, setns or other namespace classes are
advertised. Both fork-style clone and clone3 use the same unpublished preparation
path; other clone features remain rejected for namespace creation.

Canonical IDs stay in Process, filesystem ownership, global SysV IPC, stored Unix
credentials and signal queues. Conversion covers ID/getres/group syscalls, stat
and statx, exec auxiliary IDs, proc status, Unix credentials at the send/receive
boundary, signal delivery/wait/signalfd and SysV IPC control. Existing ownership
checks compare canonical IDs. Private capabilities never authorize initial-owned
filesystem/global IPC overrides or kernel diagnostic controls. Signal permission
checks preserve canonical-ID matching and same-session SIGCONT.

Private capget/capset v1/v2/v3 expose real effective/permitted/inheritable state;
selected capset transitions cannot grow permitted state and validate bounding
constraints for new inheritable bits. Bounding read/drop is supported. Ordinary
exec recalculates the supported namespace-root/nonroot case, bounded by NNP.
Root-to-nonroot ID transitions remove capabilities appropriately. Securebits,
ambient/keepcaps, file capabilities and set-ID privilege gains remain outside
this slice. This is not full Linux capability compatibility.

Map files are typed retained proc objects, accessible by normal paths/dirfds and
O_RDONLY/O_WRONLY/O_RDWR. Open validates canonical DAC; write uses a copy of the
opener's authority, not a recyclable PID. Maps are write-once, parse whole bounded
writes, and retain their exact namespace past task exit. The supported GID map
requires setgroups denial unless the selected privileged parent checks pass.
Parent UID 0 mapping has the separate CAP_SETFCAP restriction, for any inner ID.
The unprivileged exception intentionally maps only the opener/creator's saved
effective identity, including when the file description is delegated. Namespace
capabilities do not substitute for that mapping policy.

Less-privileged mount copies lock inherited attachments. Directly detaching one
fails; new binds are detachable without unlocking the covered attachment. NEWUSER
alone retains its parent-owned mount namespace and cannot administer it. NEWUSER
plus NEWNS prepares credentials first, then the copied topology and root/cwd,
then publishes the child. Failure releases prepared user/mount state without
mutating parent credentials or topology.

## Evidence and discovery

Evidence root: `build/workloads/user-namespace-20261022`.
First native ABI-41 kernel SHA256 (superseded by the final corrections below):
`51e29384d648326d5be824b2dc1788a3f3bb4a92cc67dbdd152bbd264358b62e`.
All 13 modules match ABI 41. Native session
`542d55d569c947569f1a2da86853f175` uses checked private copies from the accepted
mount-namespace review, with original VM/job identities retained.

All 99 host tests pass together after production changes. Seven affected tests
pass ASan/UBSan, including the actual shared credential implementation, allocation
failure, concurrent write-once publication, canonical ownership and namespace
storage balance. The VFS fixture tests real owner references and locked inherited
attachments. Compiled entry/start audits and mutation checks pass; those remain
bounded structural checks, not proof of arbitrary scheduler/handler safety.

Linux comparison A passes root and identity maps, exec, drops, inherited mounts,
protected files and SO_PEERCRED. Comparison B preserves a failed test expectation
about sibling writes through a passed uid_map file. Corrected service C on that
same original VM passes, including capless-opener denial and descriptor lifetime
after target exit. These Linux VMs shut down normally, original launchers exit zero
and offline filesystem checks pass. No observation timeout causes a replacement.

The first native core probe passes both inner-0 and inner-1000 mappings, exec
auxiliary IDs and bounding drops, parent/child mount separation, inherited detach
denial, canonical protected-file access, nonowner O_RDWR rejection and transferred
map descriptors after target exit. All descendants terminate. Its counters are
inherited/cumulative and must not be summed as independent test cases.

An older proc-root regression intentionally expected NEWUSER|NEWNS to fail. It
now observes genuine successful creation; that terminal failure is preserved.
The regression is updated to add still-unsupported NEWPID, retaining its original
purpose of rejecting false namespace support. This does not waive the proc-root
or unsupported-flags tests.

## Final accepted checkpoint

The accepted ABI-41 kernel and all 13 matched modules are frozen under
`build/workloads/user-namespace-20261022/kernel-frozen-accepted`.
Kernel SHA256:
`36e04bbd164e881a132fabb5d08a0649d29891d574070a379803b16fa63624c7`.
Native session `330ec2bf359942e4b458d74bc4a2175b`, boot
`d607b87e4a6f443fae29717cd8ba76fa`, retains its original launcher and jobs.
The review is `build/reviews/user-namespace-off-gpu-1ms-fixed-mlfq-b-20261022`.

Final validation passes all 99 host tests serially and seven affected ASan/UBSan
tests. An intermediate parallel host run had an audio-model timeout; its failure
is retained, and the final complete serial run passes. Compiled entry/start audits
and mutation checks pass on the exact accepted kernel.

The identical `usernamespacecheck-report-v2` binary passes in Linux comparison C
and Brook. It checks root and identity mappings, clone/clone3, exec/capability
drops, canonical filesystem and mount boundaries, group/proc IDs, Unix credentials,
signal wait/signalfd IDs, global SysV IPC ownership, retained/delegated map files,
and denial of nonowner or unauthorized writes. Counters are cumulative across
forks; they are not independent totals. Linux comparison C shuts down normally,
with an original-launcher zero exit and successful offline filesystem checks.

Final source review also corrected map-file reads to use the pinned opener's
namespace, with raw UINT32_MAX for unmapped lower IDs. Writable setgroups opens
require current CAP_SYS_ADMIN in the target namespace (EACCES on failure);
read-only opens use DAC. Previously authorized open files retain supported write
authority after capability drops. Unchanged setgroups=allow remains allowed after
a GID map, while a new deny after that map is rejected. These cases pass the final
Linux and Brook probes; earlier evidence is preserved rather than overwritten.

The final Brook run also passes the comprehensive mount probe (190 cumulative
parent checks), all nine filesystem/credentials/IPC regressions, and Mousepad
launch, frame capture and graceful close. Its original desktop wrapper inspected
the close-enqueue snapshot and asserted too early; the retained final observation
proves the exact registered client group has zero live tasks and no window remains.
A separate corrected validation receipt records this without relaunching the app.
The mount probe's copied source metadata initially named the wrong probe; that
metadata is preserved separately and corrected from the unchanged binary's prior
manifest. Neither bookkeeping correction changes the guest results.

All tracked probe jobs finish with zero live tasks. Final heap integrity is OK.
Checked shutdown completes, the original launcher exits zero, and boot/data/nix/tmp
offline filesystem checks all exit zero. No kernel is built or staged while QEMU
is live. This is a bounded, reviewed implementation checkpoint, not a claim of full
Linux namespace/capability security compatibility or Steam login readiness.

## Sources and next gate

Permission rules were checked against [Linux user namespace documentation](https://man7.org/linux/man-pages/man7/user_namespaces.7.html)
and [Linux v6.18's map implementation](https://github.com/torvalds/linux/blob/v6.18/kernel/user_namespace.c).
The descriptor discovery drove the file-authority rule rather than an assumed
extra current-writer restriction. Inherited mount protection follows
[Linux mount namespace documentation](https://man7.org/linux/man-pages/man7/mount_namespaces.7.html).

The next bounded phase should advance unchanged bubblewrap with the required
mount operations: file/socket and recursive binds, enforced remount attributes,
propagation and retained root transitions. Sequence those from the existing
SteamRT trace, first proving a trivial child. Named FIFO and real subreaper/parent
death behavior are separately observed gaps; do not promise QR login from a
namespace foundation alone. No new Steam launch or account authentication was
performed in this phase.
