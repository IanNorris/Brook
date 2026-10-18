# Filesystem capacity reporting

`statfs` and `fstatfs` now report ext2 and FAT capacity using the 120-byte
Linux x86-64 ABI. Previously both returned success with a zero-filled result;
pathname and descriptor validity were not checked. Firefox rejected a synthetic
local-storage write with `QuotaExceededError` on that kernel.

The VFS provides a kernel-buffer accounting callback. Ext2 snapshots live block
group free-block and free-inode counts under its metadata lock, including dirty
allocations. Available blocks exclude the superblock's reserved-block count.
FAT queries free clusters under its filesystem lock and reports cluster-sized
allocation units; inode counts are zero because FAT has no fixed inode pool.
Both provide filesystem type, identity, name limit and fragment size. The Linux
ABI adapter sets `ST_VALID` and clears spare fields.

Path queries retain a metadata vnode without requiring final-file read
permission or preloading FAT file contents. Descriptor queries pin the existing
FD, so duplicated and deleted-open files remain queryable. User strings are
copied page by page with distinct fault/length errors. Results are staged in
kernel memory and copied after filesystem locks have been released.

## Verification

The static musl probe `scripts/workloads/fs_space.c` runs against writable
mounts, for example `/data/home`, `/nix` and `/boot`. It creates private temporary
fixtures and checks capacity changes after a 256-KiB allocation and reclamation,
filesystem identity through pathname/FD/dup, FAT cached-file descriptors, ext2
mode-000 and deleted-open files, relative paths, a cross-mount symlink, invalid
FDs, absent paths, overlong names, and input/output guard-page boundaries.
`--host /tmp` provides a Linux control run with concurrent-host counter changes
excluded from exact comparisons. Build with an x86-64 musl compiler, for example
`musl-gcc -static -O2 -Wall -Wextra scripts/workloads/fs_space.c -o /tmp/fs-space`.

Guest reclamation comparisons follow `sync`: ext2 orphan blocks can remain
allocated until that boundary. Existing FAT removal policy returns `EBUSY` for
a directory while any file on the volume is open/mapped. The probe reports
that cleanup deferral explicitly. Acceptance removes only the recorded empty
private directories offline, after checked shutdown and an initial filesystem
check, then checks FAT again. It does not hide a failed removal.

Evidence from 18 October 2026 is under ignored build directories:

- `build/statfs-final-kernel-build-20261018.log`: final kernel build, compiled
  entry audit for its supported templates, and Linux probe control pass.
- `build/statfs-final-host-suites-20261018.log`: all 38 hosted suites pass.
- `build/workloads/statfs-candidate-20261018`: console accounting/ABI checks,
  independent ext2 geometry comparison, checked exit and offline disk checks.
  Earlier failed probe assumptions are retained in the original receipts.
- `build/workloads/statfs-firefox-20261018`: final-kernel guest probe, preserved
  home-file check and successful Firefox storage write with unchanged browser
  preferences. Its `reboot` directory records a successful separate cold read
  of that Firefox marker without rewriting it; Chromium's prior marker and the
  home document also survived. Both browsers launched normally and coexisted
  during this narrow storage test; this is not a browser co-use performance run.

## Remaining scope

Only ext2/FAT have accounting callbacks. Other filesystems and valid non-vnode
FDs return `ENOSYS`, rather than fabricated capacity. This is not full pathname
conformance: the existing resolver still needs a search-permission/error audit,
paths are limited to 511 bytes, and FAT's deeper internal path bounds need
separate work. No hot-unmount race acceptance is claimed.

Ext2 total blocks use the raw superblock count without subtracting Linux-style
metadata overhead. Reporting reserved blocks does not establish allocator
reserve enforcement. Mount flag reporting currently adds only `ST_VALID` to
the drivers' zero flag set. Broader filesystem credentials, SQLite shared-file
mappings, real-site cookies, and browser co-use remain separate acceptance work.
A passing synthetic storage test does not prove every Firefox profile or site
persists correctly.
