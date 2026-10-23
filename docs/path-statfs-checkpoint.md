# Filesystem information for O_PATH descriptors

fstatfs now accepts retained O_PATH descriptors in addition to ordinary Vnode
file descriptors. Both use the existing pinned FD reference and VfsSpaceInfo;
no pathname is reopened and no filesystem capacity or type is fabricated.
Unsupported descriptor kinds and backends retain their existing errors.

This is required by pressure-vessel's export discovery. It opens candidate host
directories with O_PATH and calls fstatfs to avoid triggering automounts. The
previous ENOSYS caused it to omit the current working directory, and bubblewrap
then failed its final chdir even though container setup had completed.

The portable probe checks directories, regular files and symlinks, retained
information after rename/unlink, a covered directory FD across a tmpfs overmount,
read-only remount flags, a detached tmpfs FD, invalid/closed FDs and a bad output
pointer. The syscall change does not alter the host-tested VFS/backend code.
The previous socket checkpoint's host and sanitizer coverage is retained with
an explicit source-change record; the final kernel is rebuilt and audited and
native regressions run on the new candidate.

Final acceptance repeats all 58 fixture plus five parent checks on the same
ABI-46 kernel as the successful SteamRT run. Linux passes the matching probe.
See `steamrt-runtime-checkpoint.md` for the final kernel and evidence locations.
