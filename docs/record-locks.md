# Regular-file record locks — 17 October 2026

Brook now implements process-associated `F_GETLK` and nonblocking `F_SETLK`
for ext2 regular files. Queries return a real conflicting owner/range or
`F_UNLCK`. Shared readers and exclusive writers, conversion, splitting,
coalescing, backwards ranges and locks extending beyond future EOF use one
inode's retained `FileObject`, rather than a descriptor, pathname or per-open
vnode identity. The syscall validates the x86-64 flock ABI, descriptor access
mode and signed offset arithmetic.

Owners combine TGID with the leader incarnation. Threads share locks; fork
children do not inherit ownership. Logical close of any matching descriptor
releases the owner's locks, including independent opens, dup2 replacement,
CLOEXEC and final group teardown. The FD-table lock orders close against new
acquisition. Deferred unpin/resource finalization does not release locks again:
another live descriptor may already have acquired new locks by then.

Each inode has a short record spinlock and a dynamically allocated interval
list. Up to two new nodes are prepared before acquiring FD/record locks; edits
reuse boundary nodes and detach retired nodes. Allocation failure or a conflict
leaves existing locks unchanged. Allocation, freeing, user-buffer access and
filesystem I/O happen outside both locks. There is no fixed range-slot capacity.

This is an intermediate compatibility improvement, not complete Linux locking:

- Contended `F_SETLKW` returns `EOPNOTSUPP`; uncontended acquisition works.
  Cancellation-safe blocking, wakeups and deadlock detection remain open.
- OFD locks are explicitly rejected. They require shared open-file-description
  ownership, which is separate from traditional process locks.
- The separate BSD `flock(73)` syscall is still an existing success stub. It is
  not implemented or accepted by these POSIX record-lock tests; its ownership
  must also follow shared open file descriptions when that work is undertaken.
- Filesystems without a stable retained file object are unsupported.
- Guest hard-link creation is still unimplemented. Offline-created ext2 aliases
  can test the inode identity path without pretending that syscall works.
- Ordinary leader exec preserves locks unless a matching descriptor is closed.
  Brook's existing nonleader-exec behavior detaches into a new group while old
  peers survive; Linux-compatible locking across that transition remains open.
- Locks are advisory. They do not prevent uncooperative ordinary reads/writes.

Semantics reference: [Linux record locking manual](https://man7.org/linux/man-pages/man2/F_GETLK.2const.html).

## Validation

All 30 hosted suites pass, including the actual interval implementation and
actual logical-close callback. Tests cover a 10,000-operation independent
byte-array oracle, 4,096 disjoint intervals collapsed by one conversion,
allocation failures, boundaries/overflow, PID incarnation isolation, concurrent
exclusive owners, distinct inode isolation, deferred-close/reacquire/unpin and
forced-claim behavior. Kernel/module builds and all 12 module import checks pass.
The bootstrap observer's expected suite count is updated to 30; its five tests
pass. This does not claim a new complete guest bootstrap run.

`scripts/workloads/record_locks.py DIRECTORY` is a multi-process Linux/guest
probe. Its 14 Linux cases pass. Two optional paths select an already-prepared
hard-link pair; the probe verifies matching device/inode, does not write their
contents, and leaves those fixture names in place.

The first guest candidate passes 11 of its original 12 cases, including
fork exclusion, shared readers, partial unlock, conversion, independent-open
close, dup2, owner exit, leader exec, CLOEXEC and thread ownership. Hard-link
creation fails with `EPERM` before that case reaches locking. This run also
confirmed the previously fake unlocked-file query now returns `F_UNLCK`.

The final kernel passes all 14 guest cases with exit zero, including the
offline-prepared aliases, descriptor access modes and invalid/overflowing
ranges. It reruns the SQLite diagnostic and reaches the same later mapping
failure described below. Session: `419eb00d1a214e1a8a01b4e017cbe96b`; exact
outputs are in `record_locks-final.json` and `sqlite_locking-recovered.json`
inside its acceptance directory.

On that same final kernel, Chromium navigates to Example Domain (window title
observed) and Codium renders its restored document. Native input appends and
saves a line; a guest read verifies the contents, owner `1000:100` and SHA-256
`19993f32235c24aadc6a76057ce86017df05486f413f89960b2fbd038bff73d1`.
Both native closes release their registered groups and the desktop returns to
ten tasks, using about 283 MB of physical memory. These are smoke checks, not
complete browser/editor acceptance or a startup benchmark.

## SQLite boundary

Before this change, SQLite 3.51.2 WAL failed with `SQLITE_PROTOCOL` because the
unimplemented GETLK path left a write-lock query unchanged. With real locks,
the fresh rollback-journal transaction still passes and WAL reaches a later
failure: `SQLITE_IOERR_SHMMAP` (5386). A separate guest probe confirms private
writable regular-file mmap succeeds while shared writable mmap returns
`EOPNOTSUPP` (95). That is an existing explicit guard until coherent regular-file
writeback is complete. The locking patch does not remove it or force a private
mapping in its place.

Therefore WAL, Firefox profile databases and Firefox audio are not accepted by
this work. Next investigate coherent writable file mappings and their resize,
writeback and teardown guarantees before repeating browser/audio acceptance.
The preceding [mapping report](mapping-lookup-cache.md) retains the earlier
profile, navigation and empty-audio-capture evidence.

## Local evidence

Runtime images and captures remain under ignored `build/` directories:

- `build/record-locks-host.log`, `build/record-locks-linux-control.log`,
  `build/record-locks-bootstrap-observer.log`.
- First guest: `build/workloads/record-locks-20261017/acceptance/`, session
  `c4f64b89904f4e918ce8ac0264cb4f2f`, kernel SHA-256
  `439ae784e641fd2c393e126c90b7f7eb504667294086e31ff6572189a65fc9fa`.
- Final revision: `build/workloads/record-locks-final-20261017/`, kernel SHA-256
  `6daaa932d90c6dafd1f8b18c7dd1a15cdca8e3cf8949c2fe304fe176efe68de4`.
  This includes the reviewed mode-3 access check and an offline-created alias
  fixture whose inode link count is two. Offline checks pass after preparation.

Both guests use eight CPUs, 4 GiB RAM, software 1080p display and silent QEMU
audio. The first stopped after completed probe jobs, acknowledged checked
filesystem/device flush, exited zero, and passed data/Nix/FAT offline checks.
The final guest also completes those shutdown and filesystem checks, with no
user/kernel fault or exit-group timeout marker in its preserved serial log.
Its `acceptance/editor-native-crop.json` refers to a readable 820×620 crop of
the completed framebuffer; it shows the saved Codium document at native scale.
