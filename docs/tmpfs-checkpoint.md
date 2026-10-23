# Bounded tmpfs backend

Historical backend checkpoint. The later [shared-memory checkpoint](steam-shared-memory-checkpoint.md)
adds quota-accounted file mappings and provisions canonical `/dev/shm` at boot;
the original mapping rejection below describes the earlier implementation.

Status (2026-10-22): implementation, Linux comparison, native acceptance and
checked teardown pass. This checkpoint adds a real memory filesystem through the
[independent-instance mount API](tmpfs-foundation-checkpoint.md).
It does not implement recursive bind, root relocation or Steam authentication.

## Supported mount and object contract

The mount syscall accepts `tmpfs` with exactly NODEV|NOSUID, optionally SILENT,
and null or empty data. Source is optional and is not resolved as a pathname.
Unknown options, unsupported flag combinations and other filesystem types fail
before instance construction/publication. Capability checks use the mount
namespace owner; the new filesystem retains its creating user namespace.
Failure to attach destroys only the caller-owned prepared instance.

Each instance has a distinct, non-reused Linux-encoded device identity, stable
inode IDs and a 01777 root owned by the caller's canonical UID/GID. Inode IDs
are local to the device. Device minor exhaustion returns an error rather than
wrapping. The backend implements retained regular files, directories and symlinks;
exclusive creation; sparse read/write and truncate; rename replacement and cycle
checks; permissions, sticky directories, SGID inheritance and metadata operations.
The central VFS applies umask once before creation. Hard links, sockets, FIFOs,
device nodes and nonempty mount-option strings remain unsupported.

Inode references are independent from names and file descriptions. Unlink keeps
open data alive; removed directories retain parent identity for already-open
`..` traversal. Links and directory-parent references are removed iteratively
at final filesystem teardown. Final inode release never takes a sleeping lock
or recurses down an attacker-controlled tree. Tree depth is bounded to 255
components; rename checks the whole moved subtree against that bound.

## Authority and restrictions

`ProcessFileActorAcquire` captures canonical IDs, immutable group ownership,
scoped capabilities and mapping availability under the credentials lock, before
the backend mutex. The snapshot represents Brook's current single-ID maps; it
must change with any future multirange mapping implementation. CAP_FOWNER checks
mapped inode ownership; DAC/CHOWN/FSETID overrides also require mapped group
ownership. A displayed UID of zero never supplies authority by itself.

Chown syscalls preserve `-1`, resolve other IDs through the caller's map and then
ask the retained backend to authorize. Ext2 retains its initial-namespace
CAP_CHOWN restriction; tmpfs scopes authority to its filesystem owner. This is a
bounded capability-based chown policy, not every Linux owner/group exception.
Two UTIME_OMIT selectors are validated no-ops, including unchanged ctime.

NODEV is inherent in the supported inode types and suppression of pathname
brokers below the fresh attachment. NOSUID follows the absence of set-ID/file-
capability exec grants. Requested restrictions are stored per instance and
reported by statfs. Existing generic `/proc/mounts` option reporting is not a
full mount-attribute interface. This does not establish arbitrary per-attachment
readonly/noexec/remount enforcement for other backends.

## Memory and mappings

Defaults are 16 MiB of charged storage and 1,024 inodes per instance. Aggregate
limits are 256 MiB, 16,384 inodes and 128 instances. These limits count backend
payload, metadata and object/vnode wrappers, not whole-process memory or general
kernel I/O scratch buffers. Names and symlink storage are charged. Reservations
precede allocation/publication; failed allocations unwind. Unlinked-but-held
files and detached-but-held filesystems remain charged until final release.
Sparse growth changes logical size without allocating proportional storage.
Writes may commit a prefix before ENOSPC, preserving exact size and offset.

The lock direction is FileObject gate → backend mutex → brief quota spinlock.
Ordinary I/O, append and truncate reuse FileObject serialization and copy staging.
No file mapping is supported yet: both the syscall and VFS mapping-pin entry
reject this backend with EOPNOTSUPP before mapping registration or replacement.
The new vnode policy defaults to allowing existing backends and advances the
kernel/module ABI to 43. All modules must come from the matched build.
FileObject ordinary I/O creates no canonical mapping pages in this slice.
MAP_SHARED coherence, mapped-file quota charging and mapped resize require a
separate acceptance phase; exec-from-tmpfs is not accepted by this checkpoint.

## Validation

Private evidence is in `build/workloads/tmpfs-backend-20261022`.
The actual backend host fixture uses production FileObject I/O gates and tests
independent devices, sparse holes/zeroed regrowth, retained unlinked files and
removed directories, rename replacement/cycles, sticky/search/ownership denial,
SGID symlinks, no-op timestamp selectors, every preparation allocation boundary,
partial page-allocation failure, concurrent appends, deepest permitted teardown,
per-instance and aggregate byte/inode limits, instance-count exhaustion and exact
reclamation. Canonical page allocation is a failing host stub: ordinary I/O must
not silently allocate such pages. The actual credentials implementation separately
tests snapshot scope, copied maps and retained group-reference ownership.
All 100 host tests and seven affected ASan/UBSan tests pass.

The portable `tmpfsinstancecheck.c` baseline passes on the Linux reference. The
same final static binary is staged for Brook; its separate `--brook` mode checks
the deliberately restricted contract: namespace-root chown, unmapped-ID denial,
ancestor-created unmapped inode ownership, mapping rejection with a preserved
MAP_FIXED sentinel, rejected mount options/flags without replacement, synthetic
`/dev/null` suppression, and quota exhaustion/unlink reclamation. Parent and child
have explicit 60-second deadlines. Cumulative counters must not be summed as
independent test totals.

The final kernel SHA-256 is
`cb420126f8ebcc8a23bf8f4d930ea76ca0af0571cf80ae993989a9a0b3d4e40e`,
with all 13 ABI-43 modules matched. The final static probe SHA-256 is
`922939318eb08d1ef4ff66882c13cfdaec03aa1a3a12d6d2d6de13dbaf63ef13`.
Both versions of the reference probe retain separate named-service receipts;
the final version passes 52 fixture / 5 parent cumulative checks on Linux.
Both original services finish inactive with MainPID 0 and no control group.
The original Linux VM powers down through ACPI, its launcher exits zero, and
its offline filesystem check passes.

Native session `aa9b8522531a41adab783410425bac3d` passes the identical portable
baseline (52 fixture / 5 parent) and the separate bounded Brook contract
(352 fixture / 24 parent). Source/binary hashes and original job identities are
retained in `build/reviews/tmpfs-backend-off-gpu-1ms-fixed-mlfq-a-20261022`.
The propagation, proc/user/mount namespace and nine credential/filesystem/IPC
regressions pass. Mousepad launches, renders and closes with its registered
client group empty. All command jobs finish with zero live tasks. Heap integrity
passes; checked shutdown completes, the original launcher exits zero and all
four offline filesystem checks return zero. Compiled entry/start audits with
mutations pass on the exact frozen kernel; these remain structural checks.

Unchanged shipped bubblewrap 0.12.0 (SHA-256
`91f2b55f25ecff638f0357abb9cc18fa1f010fd9e48e8fb59b96196827052a6c`)
runs as UID 1000 with the identical arguments:

```
srt-bwrap --unshare-user --bind / / -- /boot/BIN/NNP
```

It mounts tmpfs, creates newroot and exits 1 at:

```
bwrap: setting up newroot bind: Invalid argument
```

The request combines BIND|REC with SILENT and the legacy magic value. Recursive
bind is the next unsupported operation; no flags are silently dropped to advance
it. The collecting wrapper exits zero while preserving bubblewrap's real exit 1
and stderr. Its trivial child has not executed, and no waiting descendants remain.
Root relocation and Steam authentication remain outside this checkpoint.
