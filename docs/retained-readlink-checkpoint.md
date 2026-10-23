# Exact symlink-FD reads and retained entry reporting

readlinkat(fd, "") pins the descriptor and reads its exact retained symlink
object, without following it or reopening an opening name. It supports
O_PATH|O_NOFOLLOW and continues after symlink rename/unlink. Truncation and
no-NUL behavior match Linux. Linux raw controls established ENOENT for an empty
name on a nonsymlink FD, EBADF for an invalid FD and EINVAL for a zero-size raw
call. musl's zero-buffer wrapper differs, so that case uses the raw syscall.

ABI 45 appends reporting-only VfsObjectOps.object_path. FAT reports a file by
its pinned directory-entry coordinates, including empty files, and reconstructs
current ancestor names. Its existing rename/unlink exclusion preserves those
coordinates. Tmpfs reports its retained linked parent/name and rejects unlinked
objects. Proc FD reporting acquires attachment/object references under the FD
lock and drops that lock before backend callbacks. It never substitutes a
Vnode diagnostic label when retained entry reporting fails. Ext2 regular-file
reporting remains explicitly unsupported; directory reporting is unchanged.
Reported strings are never used to reopen the object. Detached attachment
projection retains the existing bounded directory-reporting semantics.

The host loader directory is packaged with lowercase spelling, preserving its
conventional canonical pathname. Guest read-back confirmed the same loader and
cache bytes, and /proc/self/fd reports /lib64/ld-linux-x86-64.so.2, resolving to
the exact opened file. Direct loader, true, Python and Mousepad passed.

Portable readlink controls passed 4,038 checks on Linux and on Brook Ext2 and
an explicitly created private tmpfs instance. Tests cover rename, unlink,
truncation and 2,000 close/reuse iterations. All 101 host tests passed; the
three affected host and ASAN/UBSan targets passed. FAT tests include ancestor
rename and empty-file coordinates; tmpfs tests include file rename/unlink.
Final-ELF entry/scheduler mutation audits, all existing container probes,
nine native regressions, empty jobs/groups, heap, checked stop, original wait
and four offline filesystem checks passed. Astra reviewed ownership and locks.

Evidence: `build/workloads/retained-readlink-20261023/completion-audit.json`.
Session: `d03dcf847f99409989e67df3b8807163`.
ABI-45 kernel with all 13 modules rebuilt:
`9bf60d7f253f37d013b5cfff2e8d52b8e98e0deb504ee032b9eacf4d5ccad16d`.

Two initial fixture setup failures (unsupported unshare and truncating proc-map
open) are retained as diagnostics. Accepted private-tmpfs tests use the existing
clone namespace interface and non-truncating map writes. Earlier dirent fixture
labels called /tmp tmpfs, but that review image mounts Ext2 there; the earlier
documentation is corrected. This checkpoint additionally verifies real tmpfs.

Unchanged SteamRT now completes runtime setup and replaces itself with its
bundled srt-bwrap. The next failure is the first regular-file bind:
usr/etc/bash.bashrc to /newroot/etc/bash.bashrc returns ENOTDIR. Full stderr was
preserved and hash-verified. No runtime child or login acceptance is claimed.
