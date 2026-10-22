# Retained filesystem contexts: implementation progress

## Active object-relative implementation

The renewed goal continues beyond the historical checkpoint below. This is an
intermediate implementation report, not completion of the process-context goal.

Implemented so far:

- Distinct `VfsObject` identities, independent file descriptions and directory
  iterators, checked reference ownership, and ABI **36** backend factories.
- Ext2 component lookup, parent, link reading and exact-object opens backed by
  retained open-inode records. A held removed inode cannot be reused.
- FAT component operations using the upstream implementation's internals without
  changing the submodule. Held directories pin clusters; held file objects pin
  directory-entry slots. Conflicting FAT removal/file rename returns `EBUSY`.
  FAT16 and FAT32 are covered. FAT replacement-rename atomicity remains an old
  limitation; the existing delete-destination-first implementation is not fixed.
- A retained VFS walker with explicit attachment edges, virtual missing mount
  ancestors, root clamping, stacked-root parent traversal and no pathname
  reconstruction for lookup authority. Root replacement is rejected. Production
  boot now installs FAT root before procfs. Unsupported object backends return
  `EOPNOTSUPP`; old APIs remain available while consumers are migrated.
- Legacy mount dispatch follows the visible attachment tree so an overmount
  also hides older descendants. Successful ancestor rename updates legacy mount
  labels only in the affected attachment subtree. Virtual links and mountpoints
  are reserved against conflicting VFS mutations.
- Exact-object opens transfer a mount hold to the vnode before the retained path
  can be released. A held path pins backing through its final object destructor.

Not implemented yet: retained parent mutation callbacks, process `FsContext`,
`CLONE_FS`/exec sharing rules, the complete relative-syscall/exec/shell migration,
and retained proc PID/fd broker objects. No namespace, chroot or Steam container
support is claimed. Steam authentication is still blocked by those later layers.

Validation for this phase: all **96 hosted tests** pass; six targeted tests pass
with AddressSanitizer and UndefinedBehaviorSanitizer. The tests combine the
production VFS and Ext2 implementation on a disposable real filesystem image,
plus actual FAT16/FAT32 backend images checked with filesystem repair tools in
read-only mode. They cover rename and replacement, retained overmount behavior,
stacked roots, hidden-descendant visibility parity with legacy opens, unrelated
hidden-tree rename, virtual ancestor detach, concurrent namespace transactions,
allocation failure unwind, orphan identity, independent iterators and object-to-
vnode lifetime transfer. The expanded native VFS suite also passes with the
real FAT backend, kernel locks and private memory disks. These tests do not prove the not-yet-
implemented context/syscall semantics. A complete ABI-36 desktop acceptance VM
has not been run in this phase.

Current evidence: `build/workloads/retained-fs-objects-20261022/` (ignored).
The earlier ABI-35 acceptance and its kernel hash below are historical evidence.

## Historical mount-ownership checkpoint (`8ea2322c`)

The first implementation checkpoint fixes **mount ownership of open vnodes**.
It does not implement process filesystem contexts, an object-relative walker,
`CLONE_FS`, namespaces, chroot or Steam runtime containers. Steam authentication
is not ready on Brook. The existing failure remains pressure-vessel opening
`/proc/self/root`, followed by the real container requirements documented in
[the feasibility study](steam-runtime-feasibility.md).

Astra reviewed the ownership prerequisite and the integration approach. The
original proposal to reconstruct an Ext2 directory's current pathname and use it
under one VFS lock is insufficient: global lookup can select an overmount even
without a concurrent rename. A detached directory can have no reconstructible
name at all. Adding FsContext and routing callers through this shortcut would
preserve the wrong authority model.

The authorized goal permits a tested coherent checkpoint if integration requires
substantially broader changes. This checkpoint takes that boundary: retain the
reviewed lifetime fix, document the required lookup contract, and do not migrate
process state through the rejected shortcut. The full retained-path/context slice
remains unfinished. No unsupported operation is reported as implemented.

## Implemented ownership

Every successful VFS ordinary open, executable open and pathname-socket creation
holds the actual MountEntry that dispatched it. The hold is acquired under the
existing VFS lock before the vnode is published. Backend-open failure acquires no
hold; rejection after open uses the ordinary final-close unwind.

Unmount checks these holds under the exclusive VFS lock, before invoking backend
busy checks or teardown. Final vnode destruction retains the mount through the
backend close callback and vnode destruction, then atomically releases its hold.
A concurrent unmount may conservatively report busy during close. It cannot free
backing state while that close is using it. The fixed mount table slot cannot be
reused while a vnode holds it. There is no lazy-detach implementation here.

`VfsDirectoryPath` uses the vnode's actual mount when prefixing a backend-reported
current name. Its result remains a reporting/legacy pathname, **not an object
capability for subsequent global lookup**. Backends without `directory_path`
retain their previous opening-name behavior; this change makes no new
rename-stability claim for those backends.

`VfsRetain` requires an already-protected live reference. `VfsRelease` drops an
owned reference and performs final destruction only on the last drop.
`VfsClose` remains the existing final-destruction API; it is not a shared-reference
drop. Final destruction must run outside caller spinlocks. All production vnode
allocation paths initialize the appended owner field, including raw kmalloc
sites where C++ default member initializers do not run.

Vnode layout changes raise the module ABI from 34 to **35**. The acceptance build
rebuilds and verifies all 13 modules together with the kernel. Existing ABI-34
review images are preserved as historical evidence, not reused with new modules.

## Required next lookup contract

Implement an object-relative walker before migrating process context consumers.
Keep one global namespace initially; this is not authorization for namespace
creation or container implementation.

1. **Retained start and root.** A path owns a mount/attachment reference and a
   backend object reference. General paths can represent files and sockets;
   root/cwd require directories. Names are optional display data. A held object
   cannot be replaced by another object with a reused inode/cluster identifier.
2. **Single-component backend operations.** Lookup takes a retained parent and
   one validated name. It returns a retained child without following a symlink.
   The VFS walker handles symlinks, `.` and `..`, mount transitions, trailing
   slash and final-component policy. Do not reuse Ext2's whole-path resolver as
   if it enforced process root: it resets absolute targets to EXT2_ROOT_INO and
   handles components internally. Extract identity-based component helpers.
3. **Object parent mutations.** Resolve to a held parent plus final name, then
   create/remove/rename under the same VFS mutation transaction and backend
   locks. Both rename parents belong to that transaction. Check mount/attachment
   identity for EXDEV and recheck backend permissions, sticky bits and inode
   state at mutation. Do not convert either parent back to a pathname.
4. **Explicit mount edges.** Enter a child mount using the parent attachment and
   mountpoint object's identity, not a string-prefix search. At an attachment
   root, `..` crosses through the mountpoint in the parent attachment and then
   applies parent traversal; returning the mountpoint alone is insufficient.
   At the effective process root it remains there. Absolute paths and absolute
   symlink targets restart at that root. Preserve inherited outside-dirfd behavior;
   chroot alone is not revocation of those descriptors.
5. **Backend support with honest limits.** Ext2 already has retained open-inode
   objects and inode-relative primitives to build on. A removed directory can
   remain referenced even when getcwd returns ENOENT. Operations on it must use
   backend state and return the appropriate errors; path reconstruction is not
   a substitute. FAT needs a retained directory lifetime/generation policy:
   `DIR.obj.sclust` alone does not prevent reuse. Its volume root can have an
   explicit stable representation; that does not solve mutable FAT directories.
   Unsupported new object-relative operations return EOPNOTSUPP rather than
   accepting a stale opening pathname.
6. **Synthetic objects.** Proc PID/task/fd objects retain incarnation and access
   checks. `/dev` and special proc descriptor opens must participate in the
   same retained traversal/dispatch contract. Calling a new backend opener
   directly from openat must not silently bypass existing sys_open handling.

The existing mount table contains pathname attachments only. Giving it retained
mountpoint edges requires a boot-order decision: kernel.cpp currently mounts
`/proc` before mounting the FAT root. Either establish the root first or perform
an explicit attachment-linking phase before user processes start. Define this
phase and rollback before changing mount publication.

Avoid ownership cycles: an attachment must not own a VFS root vnode that strongly
holds the same attachment. Separate backend-root identity/backing ownership from
individual VFS path references. Parent attachment links form a rooted structure;
release replaced edges and backend references outside publication locks. No lazy
detach, propagation, bind mounts or topology snapshots are needed for this first
single-namespace walker, but its identity contract must not preclude them.

## FsContext migration after that gate

Use a process-owned, refcounted context containing retained root/cwd and umask.
Snapshot/retain the context under a short publication lock, release it, then
snapshot its members under the short context lock. Release that lock before VFS
or backend work. VFS namespace lock precedes backend inode locks. No backend
callbacks, final destructors or scheduler calls run under publication locks.

Fork copies the context while retaining paths. CLONE_FS shares it, including
umask; absence of CLONE_FS copies it even for a thread. Exec privately prepares
an unshared context before commit. All unpublished-child failure paths and
final deferred process destruction release owned contexts. A memcpy of Process
must not leave a borrowed context pointer that an unwind treats as owned.
Concurrent chdir uses retained snapshots and last-commit-wins publication;
replaced cwd references are released after unlocking.

Migrate relative open, metadata, mutation and exec together. Include absolute
root handling, dirfd snapshots across concurrent close, chdir/fchdir/getcwd,
umask and creation-mode calculations, shell-created processes, interpreter
lookup and procfs reporting. Process::cwd can remain a display cache, never lookup
authority. Scheduler process snapshots must not perform filesystem work while
holding scheduler/registry locks. This migration changes Process layout and
requires another module ABI review and synchronized build.

## Acceptance gates

The mount checkpoint's native VFS fixture uses a backend **without** can_unmount,
so it proves VFS ownership rather than relying on Ext2's existing busy checks.
It covers backend-open failure, opened-directory executable rejection,
O_TRUNC rejection, overmount owner selection, retained aliases, final close,
and busy checking from inside the final backend close callback. That callback
runs only from an explicit reference drop outside VFS locks. These are ordering
and error-unwind tests, not concurrent stress or allocator-failure injection.

The full context/walker slice must additionally pass Linux differential and
native Brook tests for:

- Renamed cwd and dirfd with an adversarial replacement at the old name;
  concurrent rename versus lookup/mutation and close versus snapshot retention.
- Overmount before and after retaining a directory, child-mount entry, `..`,
  absolute/relative symlinks, permission failures and removed directories.
- Fork independence, CLONE_FS sharing of cwd/umask, absence of CLONE_FS on
  threads, exec unsharing and unpublished-child allocation-failure unwind.
- Last-reference teardown, mount busy behavior, synthetic proc/dev paths,
  supported FAT root behavior and explicit unsupported mutable-FAT cases.

A Linux/Brook pass of the existing filesystem and IPC regression probes is
regression acceptance only. It does not establish these unimplemented semantics.

## Evidence

The native VFS suite and all 90 hosted tests passed. Three existing static
filesystem probes passed on Linux and Brook; the Brook run also passed 974
credential checks, signalfd and SysV semaphore regressions. Every original guest
job exited zero with no live tasks, and heap integrity was OK. The checked guest
sync completed, the original VM launcher exited zero, and all four offline
filesystem checks exited zero. No VM remains running.

The acceptance kernel SHA-256 is
`755b6fc133008b55056d94aecd98a067477c9e2dee36d8645b5ae86c838cbf27`.
All 13 modules were verified as ABI 35 and round-tripped through the private boot
image; 12 loaded, with the optional bochs display driver declining absent
hardware as in the baseline. Supported entry-state/scheduler-start static audits
passed against that exact kernel; these do not certify unsupported return
families. Shutdown acceptance here is checked guest sync followed by QMP stop,
not a serial-confirmed guest ACPI shutdown.

Private evidence lives under `build/workloads/retained-fs-20261022/`; the full VM
review is `build/reviews/retained-fs-off-gpu-1ms-fixed-mlfq-a-20261022/`.
Sources are in git; binaries, images, proprietary packages and raw logs stay
ignored. See the completion receipt there for exact kernel hash, original owner
exit and post-shutdown filesystem checks. No Steam credentials were used.
