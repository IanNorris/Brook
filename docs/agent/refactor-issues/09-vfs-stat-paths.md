# VFS, stat and paths

_12 open entries (4 medium, 8 low). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** `open`/`openat` flag handling (`O_TRUNC`, `O_TMPFILE`, `O_NOFOLLOW`), path length limits, `/boot` alias fallback, synthetic `/etc` and `/proc` files, `fstat`/`getdents64` consistency, `AT_EMPTY_PATH`, legacy wrappers, vnode refcounting.
- **Why it matters:** M-46 can truncate a file that is then refused as a directory, and M-65 can modify files under `/boot`.
- **Dependencies:** M-49 shares a `PATH_MAX` with M-15 (04). L-39 (synthetic file list copied four times) is the natural place for the synthetic-node registry in 14.
- **Suggested order:** M-46, M-65, M-66, M-49 (with M-15), L-58, L-64, L-38, L-40, L-41, L-39, L-42, L-65.
- **Test approach:** `test_syscall_user_memory/dir_open.cpp` (open, getdents) with its fake VFS lookups; extend `fakes.cpp` for stat. `test_busybox_lookup` and the ext2 tests cover resolution.
- **Size:** Medium: 12 entries, 1 or 2 sittings.

## Entries

### Medium

### M-46 · O_TRUNC truncates the file before the O_DIRECTORY check rejects it

- **Where:** `src/kernel/src/syscall_open.cpp:545`, in `OpenAtInternal`.
- **Severity:** medium (confidence high).
- **Problem:** open(file, O_WRONLY|O_TRUNC|O_DIRECTORY) erases the file, then returns ENOTDIR. Opening a directory for writing is not rejected with EISDIR (low confidence).
- **Suggested fix:** Pass the directory requirement into VfsOpenAt; return EISDIR and EINVAL as Linux does.
- **Test:** QEMU: the size is unchanged after ENOTDIR.

### M-49 · Too-long paths return EFAULT; limits vary between 256 and 512 bytes (33 sites)

- **Where:** `src/kernel/src/syscall_stat.cpp:376` (snippet matches 3 lines: 376, 384, 664), in `sys_stat`.
- **Severity:** medium (confidence high).
- **Problem:** mkdir fails on a 300-byte path while mkdirat succeeds. PATH_MAX is 4096, and a too-long path should give ENAMETOOLONG.
- **Suggested fix:** A shared CopyUserPath() with a PATH_MAX buffer, using CopyUserCStringResult.
- **Test:** QEMU: 300- and 4000-byte paths succeed; a 5000-byte path gives ENAMETOOLONG.

### M-65 · /boot alias fallback can create or modify files under /boot and ignores O_NOFOLLOW

- **Where:** `src/kernel/src/syscall_open.cpp:472`, in `OpenAtInternal`.
- **Severity:** medium (confidence medium).
- **Problem:** The ENOENT retry keeps O_CREAT, O_TRUNC and write access, so open("/etc/x/y", O_CREAT) creates /boot/etc/x/y. followFinal is dropped.
- **Suggested fix:** Use the alias only for read-only opens without O_CREAT or O_TRUNC, and honour O_NOFOLLOW.
- **Test:** QEMU: O_CREAT under a missing directory gives ENOENT and nothing is created under /boot.

### M-66 · O_TMPFILE treated as O_DIRECTORY; opens the directory itself

- **Where:** `src/kernel/src/syscall_open.cpp:445`, in `OpenAtInternal`.
- **Severity:** medium (confidence medium).
- **Problem:** The 0x400000 bit is never checked, so open(dir, O_TMPFILE|O_RDWR) returns a writable directory fd and glibc and systemd skip their fallbacks.
- **Suggested fix:** Return -EOPNOTSUPP while O_TMPFILE is unsupported.
- **Test:** QEMU: open("/tmp", O_TMPFILE|O_RDWR) gives EOPNOTSUPP.

### Low

### L-38 · Legacy wrappers duplicate their *at() counterparts and have drifted

- **Where:** `src/kernel/src/syscall_path.cpp:258`, in `DoRename`.
- **Severity:** low (confidence high).
- **Problem:** Pairs: rename/renameat, mkdir/mkdirat, unlink/unlinkat, symlink/symlinkat and stat/lstat, which differ in buffer size. FinalizeClosedFd open-codes VfsRelease with `prev <= 1`, hiding underflow.
- **Suggested fix:** Delegate each to the *at form with AT_FDCWD.
- **Test:** QEMU legacy vs *at comparison.

### L-39 · Synthetic /etc and /proc file list copied four times and drifted

- **Where:** `src/kernel/src/syscall_stat.cpp:311` (snippet matches 2 lines: 311, 359), in `do_stat_internal`.
- **Severity:** low (confidence high).
- **Problem:** Only open serves /etc/os-release. Sizes are a fake 64. statx lacks the synthetic files.
- **Suggested fix:** One {path, content} table.
- **Test:** QEMU: stat, statx, access and open agree; st_size equals the bytes read.

### L-40 · fstat returns EBADF for valid fds and masks backend errors

- **Where:** `src/kernel/src/syscall_stat.cpp:419`, in `FstatKernel`.
- **Severity:** low (confidence high).
- **Problem:** DevKlog, DrmPrime, CompositorScene and IntelSyncObj give EBADF. Anon-inode fds report S_IFREG.
- **Suggested fix:** Cover every FdType; propagate errors.
- **Test:** fstat succeeds on every fd type.

### L-41 · getdents64 d_ino does not match st_ino

- **Where:** `src/kernel/src/syscall_dir.cpp:90`, in `sys_getdents64`.
- **Severity:** low (confidence high).
- **Problem:** find -inum, du and the getcwd fallback misbehave. The DRM branch returns 0 rather than EINVAL for a buffer that is too small.
- **Suggested fix:** Carry the backend inode in DirEntry.
- **Test:** QEMU: d_ino equals lstat st_ino on tmpfs and ext2.

### L-42 · readlink("/proc/PID/exe") truncates the PID to 16 bits and skips permission checks

- **Where:** `src/kernel/src/syscall_path.cpp:100`, in `DoReadlink`.
- **Severity:** low (confidence high).
- **Problem:** 65537 wraps to pid 1, and there is no PTRACE_MODE_READ-style check.
- **Suggested fix:** Parse into uint64 with a bound, and add a uid check.
- **Test:** QEMU: readlink("/proc/65537/exe") gives ENOENT.

### L-58 · VnodeRef check-then-act race; VnodeUnref doesn't detect underflow; orphaned VnodeTryRef comment

- **Where:** `src/kernel/src/syscall_internal.h:62`, in `VnodeRef / VnodeUnref`.
- **Severity:** low (confidence medium).
- **Problem:** A vnode can be revived from zero. prev==0 leads to a second VfsClose. The "Try to take a reference..." comment has no function under it.
- **Suggested fix:** A CAS-based VnodeTryRef; treat prev==0 as a BUG.
- **Test:** Host fake-vnode underflow test.

### L-64 · AT_EMPTY_PATH handling duplicated and inconsistent (7 sites)

- **Where:** `src/kernel/src/syscall_stat.cpp:52`, in `AccessAt`.
- **Severity:** low (confidence medium).
- **Problem:** O_PATH fds are rejected inconsistently, and utimensat(AT_FDCWD, "", AT_EMPTY_PATH) gives EBADF.
- **Suggested fix:** AtLookup::InitEmpty() shared by all.
- **Test:** QEMU matrix of dirfd kind × *at syscall.

### L-65 · Unsupported cases return wrong errno or a stub result (fstatfs ENOSYS, sync errors, umount2 flags 0)

- **Where:** `src/kernel/src/syscall_stat.cpp:778`, in `sys_fstatfs`.
- **Severity:** low (confidence medium).
- **Problem:** fstatfs on a pipe gives ENOSYS. sync propagates errors. umount2(path, 0) gives EINVAL.
- **Suggested fix:** Pseudo-fs magic numbers; sync returns 0; a documented errno for umount2.
- **Test:** QEMU checks.
