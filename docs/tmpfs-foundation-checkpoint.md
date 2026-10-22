# Fresh mount instances and the tmpfs implementation boundary

This is a mount-instance prerequisite, not a tmpfs implementation. Brook still
rejects bubblewrap's tmpfs request. The next backend must be implemented and
accepted before that syscall can return success.

## Implemented ownership contract

`VfsMountNamespaceMountFresh` attaches caller-prepared filesystem state to a
retained directory in a live namespace. The driver must already be registered,
with the identical provider table and retained-root/unmount callbacks. This
preserves permanent provider retention. Authorization and filesystem options
remain the eventual syscall/factory's responsibility; this is an internal API.

The function validates namespace/target ownership, directory type, reporting
bounds and the provider's retained root before publication. It does not replace
the namespace root. On every preparation error, it drops temporary roots and
allocations without invoking unmount: the caller still owns its prepared state.
On success only, the new backing consumes that state. Calls supplying an already
transferred instance violate the ownership contract.

Fresh backings are explicitly independent. Equal driver/device keys never make
two fresh instances share private state, and legacy `VfsMount` cannot discover a
fresh instance through its device-coalescing loop. Existing block-device
coalescing is preserved. Bind and namespace copy retain the new backing normally;
retained paths/vnodes survive detach and defer final destruction. A failed final
unmount leaves a registered zero-reference backing for checked sync to retry.
No callback or private state is silently abandoned.

Fresh attachments retain a root object, so legacy `VfsUnmount` rejects them.
They use namespace-aware detach and deferred cleanup. The kernel/module ABI
remains 42: the added backing flag is private to VFS and no module table changes.

## Why this checkpoint stops before exposing tmpfs

There is no existing writable RAM filesystem to mount. `ramdisk.cpp` is a fixed
memory block device; it does not implement Linux tmpfs or its inode/metadata
contract. Rebranding an ext2 disk, preallocating a large RAM disk, or returning
success over an ordinary directory would not implement the requested operation.

The actual shipped bubblewrap request is:

```
mount("tmpfs", base_path, "tmpfs", MS_NODEV | MS_NOSUID, NULL)
```

It later constructs directories, symlinks and files in that instance. See
[bubblewrap 0.12.0 setup](https://github.com/containers/bubblewrap/blob/v0.12.0/bubblewrap.c#L3019).
A safe backend therefore needs all of the following before unprivileged exposure:

- Independent instance/device identity, stable inodes, canonical UID/GID ownership,
  root mode, retained parent/component operations, umask applied once, permissions
  and sticky-directory rules. Rename replacement and directory-cycle handling
  must operate on identities, including detached and unlinked objects.
- Per-instance and aggregate limits for mount count, inode/entry/symlink storage,
  metadata and allocated file pages. Reserve before publication and unwind OOM;
  charge unlinked-but-held objects until final release. Sparse file length must
  not drive proportional allocation. The default ceilings and any supported
  `size=` option must be documented rather than silently ignored.
- Explicit NODEV/NOSUID guarantees. A first backend can inherently satisfy these
  by supporting no device nodes and no set-ID/file-capability execution grants,
  while retaining the requested restrictions for future compatibility. Synthetic
  device brokers must remain suppressed beneath the new attachment. This does
  not establish generic mount-attribute enforcement for other filesystems.
- Deliberate mapping behavior. Existing `FileObject` can provide canonical-page
  coherence, but a RAM backend beneath it can duplicate page storage and allocate
  again during writeback. Initial mapping rejection is acceptable only if checked
  explicitly before mapping allocation, with a documented error. MAP_SHARED
  support requires coherent read/write/mapping, fork/unlink lifetime, quota failure
  and final cleanup tests. Do not infer mmap safety from ordinary I/O tests.

These are the concrete backend/accounting/mapping design boundaries for the next
bounded goal. Root relocation, recursive bind, remount and Steam authentication
remain later work. The new internal API does not weaken those rejections.

## Evidence

Private evidence is under `build/workloads/tmpfs-foundation-20261022`.
The actual retained-VFS fixture tests independent equal-key mounts; legacy
non-coalescing; each preparation-allocation failure; provider/root/type failures;
partial root output on callback error; namespace-copy sharing; retained vnode
and path lifetime; and failed final unmount followed by unsuccessful/successful
checked-sync retries. Allocations and attachment counts return to baseline.
All 99 host tests and six affected ASan/UBSan tests pass after review additions.

`tools/at-syscall-probe/tmpfsinstancecheck.c` defines a Linux reference contract
for the future backend. It is explicitly not claimed to pass on Brook. It checks
two real tmpfs mounts, device identity, default ownership/mode, mount flags,
exclusive create and umask, symlink/rename/unlink, held unlinked I/O, sparse growth,
and retained directory metadata after detach. Counters are cumulative within a
process, not independent totals to add.

The Linux reference passes (fixture 52, parent 5 cumulative checks), with its
service inactive, MainPID 0 and no remaining control group. The original Linux
launcher exits zero after ACPI shutdown, and its offline filesystem check passes.
The reference binary SHA-256 is
`2132e0c2510000b19eb89005581e19bc07180d654c780de8276df12d8ca298b2`.

Frozen kernel SHA-256 is
`167122c4d04b91b836c0a2466b4f586272db0b32924be6d95ed3049f66e47436`,
with all 13 matched ABI-42 modules. Compiled entry/start audits with mutations
pass. These are bounded structural checks, not general correctness proofs.
The new internal API is exercised by the hosted fixture; no native syscall
exposes it yet. Native runs exercise existing paths for regressions and retain
the unchanged bubblewrap boundary, rather than claiming new tmpfs acceptance.

Native session `34d0e52219914ef68140699114ed8b27` passes the existing
propagation, proc/user/mount namespace, nine credential/filesystem/IPC and desktop
launch/render/close regressions. Original command identities and terminal results
are retained in
`build/reviews/tmpfs-foundation-off-gpu-1ms-fixed-mlfq-a-20261022`.
All command jobs have no live tasks; the registered desktop client group exits.
Heap integrity passes. Checked shutdown completes, the original launcher exits
zero and all four offline filesystem checks return zero.

The identical shipped bubblewrap executable and UID-1000 smoke-test arguments
remain unchanged. It still exits 1 with
`bwrap: Failed to mount tmpfs: Invalid argument`; the collecting wrapper's exit
zero is not an application success. Its trivial child has not executed, and no
waiting descendants remain. This bounded checkpoint stops at the documented
backend/accounting/mapping design boundary, with the independent-instance
prerequisite implemented and tested.
