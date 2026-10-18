# Retained directory lookup — 18 October 2026

The Gallery fixture exposed a filesystem compatibility bug: `open` saved only
62 path bytes plus a slash in `FdEntry::dirPath[64]`. Relative operations joined
their names to that truncated prefix. A valid, absolutely accessible desktop
entry therefore returned ENOENT through its directory descriptor, and Python's
fd-safe recursive cleanup stopped with ENOTEMPTY. The old-kernel negative
control is retained under `build/workloads/gallery-links-20261018/`.

A directory vnode now owns its complete canonical opening path. It shares that
allocation through existing vnode references across dup/fork and frees it on
final close. The 64-byte FD field remains a diagnostic label, not an operation
prefix; no large buffer was added to all 1,024 descriptor slots. Relative access,
open/stat/mutation helpers, ownership lookup and fchdir use the retained vnode.

For ext2, a new vnode callback reconstructs the current filesystem-relative path
from its retained inode, `..` parents and the parents' live entries. The namespace
read lock excludes rename/removal during reconstruction, and the ext2 read lock
protects inode and directory reads. Depth and output bounds fail explicitly.
Sequential rename and replacement of the opening name therefore keep addressing
the original inode; a detached directory cannot start addressing its replacement.
Other backends use the complete canonical opening name as a fallback.

This changes Vnode/VnodeOps and requires module ABI **19**, with all modules
rebuilt. The label retained in scheduler FD diagnostics may still show only its
first 63 bytes. It must not be used as a resolution authority.

## Verification scope

The owned `scripts/workloads/dirfd_identity_check.py` probe covers exact directory
path lengths 63, 64, 127 and 230 bytes, openat/statat/unlinkat, canonical symlink
opens, nofollow metadata, readlinkat, access, mkdirat, renameat, rmdir, fchdir,
dup/fork retention, directory rename/replacement and unlinked-name replacement.
Success requires Python's standard fd-safe `shutil.rmtree` cleanup. Both the
Linux control and Brook candidate passed 42 checks as UID 1000. The Brook probe
reported 0.054 guest-monotonic seconds; this is a smoke-check timing, not a
matched workload performance benchmark.

The same private Gallery install/repair matrix then passed using standard
fd-safe cleanup, removing its previous absolute-path workaround. Both probes
used owned fixture data; no shared profile or saved user document was modified.
Release build, supported entry-template audit and all 40 native suites passed.
The candidate kernel SHA-256 is
`0ca6d6d46fc78f7034b9925d428c9544f99ed4dd1bf9d8dd9b17081123cecec0`.
All twelve frozen module headers report ABI 19. GIMP's normal catalogue launch
reached its main window and reopened the preserved XCF through Recent Images;
native close drained its full recorded launch lineage. Four tracked guest
commands exited zero with both output EOFs, no gaps/drops and no remaining
original/full-lineage tasks. Heap integrity passed, audio was inactive, checked
sync completed, QEMU and its launcher exited zero, and read-only boot/data/Nix
filesystem checks returned zero. The full serial audit found no kernel/device
or user fault. Five saved documents retained exact hashes and UID 1000:GID 100
ownership. Local evidence is under
`build/workloads/dirfd-identity-20261018/acceptance-audit.json` and `review/`.
These host-backed private disks were cloned from the checked Gallery follow-up;
the original panic disks were not reused.

## Remaining identity work

Path reconstruction and the eventual operation are still separate: this is not
an atomic inode-relative *at walk under concurrent namespace replacement. It
still rechecks ancestors along the reconstructed absolute path, which can differ
from Linux's permission semantics for a retained directory. Filesystem roots,
mount teardown/replacement and deleted-directory operations need broader tests.
FAT/procfs do not have ext2's rename-aware retained-inode callback. Process CWD
remains a string after chdir/fchdir and does not follow later renames. Those
limits must be resolved before claiming pinned namespace/account isolation.

Reconstructing parent names adds work to ext2 dirfd operations; profile real
workloads before introducing a cache, and invalidate any future cache on namespace
changes rather than silently reintroduce stale-name identity. This follow-up
does not establish a performance improvement or renew both bootstrap paths.
