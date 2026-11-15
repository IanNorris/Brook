# File descriptions, offsets and fd-table races

_14 open entries (1 high, 7 medium, 6 low). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** How fds are allocated, published, duplicated and closed (`FdAlloc` vs `FdTablePublishOwned`), plus the missing shared open-file description (H-20): shared offset and status flags, `F_GETFL`/`F_SETFL`, `flock`, `lseek` on non-seekable fds, close-by-number on error paths.
- **Why it matters:** Shell redirections (`cmd >f 2>&1`) overwrite instead of interleave, `O_CLOEXEC` is lost on several paths, and error paths can close a sibling thread's reused fd.
- **Dependencies:** Touches every fd type, so coordinate with 05 (SCM_RIGHTS: M-63, L-34) and 07 (epoll/poll key on fd numbers: M-19, M-58 should be redone on top of the open-file description). Do the small publish-order entries first; H-20 is its own branch.
- **Suggested order:** M-45 (publish helpers everywhere), M-52, M-63, L-45, L-57, L-59, L-71 (the same publish/pin theme), L-34 (merge snapshot code with `FdBumpRefcount`), M-50, L-63, M-48, M-53, M-67, then H-20 on a dedicated branch.
- **Test approach:** `test_fd_table`, `test_scheduler_fd_snapshot` and `test_syscall_user_memory/dir_open.cpp` (publish with pins, O_CLOEXEC). H-20 needs a QEMU shell test: interleaved output for `cmd >f 2>&1` and `{ a; b; } >f`, and a forked child's `F_SETFL O_NONBLOCK` reaching the parent.
- **Size:** Medium for the 13 small entries; H-20 is large (a branch of its own).

### Shared open-file description (H-20)

Today `FdEntry` holds `seekPos` and `statusFlags` per slot, and `dup`, `dup2`, `fork` and SCM_RIGHTS copy them.

A correct fix gives each open a refcounted `OpenFile { refs; lock; offset; statusFlags; type; handle; }`. `FdEntry` then keeps only `fdFlags` (CLOEXEC) and a pointer to it.

- **Reference counting:** `FdTableDuplicate*`, `FdTableForkSnapshot`, `UnixFdSnapFrom`/`Install` and `FinalizeClosedFd` retain and release the `OpenFile` rather than the handle. The per-type handle refcount moves into the `OpenFile`'s final release.
- **Offsets:** `read`, `write`, `lseek`, `getdents64`, `sendfile` and `copy_file_range` update the offset under `OpenFile::lock`. That lock must be a sleeping lock, because VFS I/O can block. Pipes and sockets ignore the offset and skip the lock.
- **Status flags:** `F_GETFL`/`F_SETFL` and `FIONBIO` act on the shared flags.

This touches every fd type's lifetime, so it needs its own branch and a QEMU shell test. The test should show interleaved rather than overwritten output for `cmd >f 2>&1` and `{ a; b; } >f`, and should show a forked child's `F_SETFL O_NONBLOCK` reaching the parent.

## Entries

### High

### H-20 · dup() and fork() copies get their own file offset and status flags (no shared open file description)

- **Where:** `src/kernel/src/syscall_fd.cpp:362`, in `DuplicateFd`.
- **Severity:** high (confidence high).
- **Problem:** seekPos and statusFlags are copied per slot. With `cmd >f 2>&1` and `{ a; b; } >f`, writers overwrite each other, and F_SETFL O_NONBLOCK on one alias doesn't affect the others.
- **Suggested fix:** A refcounted open-file-description object holding the offset and flags; lock the offset read-modify-write.
- **Test:** QEMU: dup then write "a" and "b" gives "ab"; same for fork.
- **Status (fix pass):** deferred: confirmed; needs a refcounted open-file description through the fd table, fork and SCM_RIGHTS. Design under "Shared open-file description (H-20)" below.

### Medium

### M-45 · Publish-then-mutate fd allocation ignores O_CLOEXEC and other open flags (11 branches)

- **Where:** `src/kernel/src/syscall_open.cpp:175`, in `OpenAtInternal`.
- **Severity:** medium (confidence high).
- **Problem:** Affected branches: O_PATH, fb0, keyboard, tty, renderD128, /dev/dri, DRM sysfs, synthetic /etc and klog. Flags are set after FdAlloc publishes the fd, or not at all, so fds leak across exec. open("/etc/hostname", O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC) succeeds.
- **Suggested fix:** One FdEntry builder plus FdTablePublishOwned; reject write and O_EXCL on read-only synthetic nodes.
- **Test:** QEMU: per synthetic path, F_GETFD after O_CLOEXEC; O_WRONLY on /etc/passwd fails.

### M-48 · flock() is a stub that always succeeds

- **Where:** `src/kernel/src/syscall_file_ops.cpp:117`, in `sys_flock`.
- **Severity:** medium (confidence high).
- **Problem:** No EBADF or EINVAL checks and no exclusion, so git, nix and package managers believe they hold locks. Record-lock machinery already exists.
- **Suggested fix:** Whole-file locks owned by the open file description, at least with validation and EWOULDBLOCK.
- **Test:** test_record_locks flock cases; QEMU: a second LOCK_EX|LOCK_NB gives EWOULDBLOCK.

### M-50 · F_GETFL reports open-only flags; F_SETFL accepts any bit

- **Where:** `src/kernel/src/syscall_fd.cpp:626`, in `sys_fcntl`.
- **Severity:** medium (confidence high).
- **Problem:** F_GETFL returns O_CREAT, O_EXCL, O_TRUNC and O_CLOEXEC. F_SETFL can set O_PATH or O_TRUNC.
- **Suggested fix:** Strip open-only flags at open; apply only the settable mask in F_SETFL.
- **Test:** QEMU: F_GETFL after open(O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC) equals O_RDWR.

### M-52 · Unpinned fd publish, then CloseProcessFd(fd) on copyout failure, can close a sibling's descriptor

- **Where:** `src/kernel/src/syscall_drm.cpp:1065`, in `DrmRenderIoctl (PRIME_HANDLE_TO_FD), IntelRenderIoctl`.
- **Severity:** medium (confidence medium).
- **Problem:** 4 sites: virtio PRIME export (`int fd = FdAlloc(proc, FdType::DrmPrime, p);`), Intel PRIME export, Intel syncobj export and cache-probe alloc. Each publishes unpinned and closes by number if the copy fails, so a reused number gets closed. The sync-file paths avoid this with a pinned publish. 3 sites also flatten the publish error to -EMFILE.
- **Suggested fix:** Pinned publish, then close via the pin's identity; propagate the real error.
- **Test:** Host fd-table test: an fd closed and reopened by a sibling survives the forced copyout failure.

### M-53 · Intel GEM_MMAP callback re-resolves the render fd by number (fd-reuse TOCTOU)

- **Where:** `src/kernel/src/syscall_drm.cpp:958`, in `IntelRenderIoctl (user.mapBuffer lambda)`.
- **Severity:** medium (confidence medium).
- **Problem:** sys_ioctl pins the FdEntry, but the callback calls sys_mmap with the raw number, which a sibling can dup2 over in the meantime.
- **Suggested fix:** Pass the pinned BdwGpuFile to an internal mmap helper.
- **Test:** QEMU: GEM_MMAP racing a dup2 over the same number.

### M-63 · Received SCM_RIGHTS fds published before their numbers reach userspace; a fault leaks them

- **Where:** `src/kernel/src/syscall_socket.cpp:1464`, in `sys_recvmsg`.
- **Severity:** medium (confidence medium).
- **Problem:** When the cmsg or msghdr copy faults, the installed fds stay unreported. socketpair has the same order, and its rollback `CloseProcessFd(proc, fdA)` closes by number, which can hit a reused fd.
- **Suggested fix:** Pinned publish (FdTablePublishOwnedPinned), copy out, then commit; close by identity.
- **Test:** QEMU: a recvmsg control buffer made read-only; the fd count is unchanged after EFAULT.

### M-67 · close_range accepts CLOSE_RANGE_UNSHARE but ignores it; sets CLOEXEC without a lock

- **Where:** `src/kernel/src/syscall_fd.cpp:250`, in `sys_close_range`.
- **Severity:** medium (confidence medium).
- **Problem:** Threads sharing the table are affected, and the unlocked write races publish.
- **Suggested fix:** Implement UNSHARE or return EINVAL; set CLOEXEC under ProcessFdLock.
- **Test:** test_fd_table range-CLOEXEC under the lock.

### Low

### L-34 · SCM_RIGHTS snapshot ref/unref duplicates FdBumpRefcount/FinalizeClosedFd and has drifted

- **Where:** `src/kernel/src/syscall_socket.cpp:1019`, in `UnixFdSnapFrom`.
- **Severity:** low (confidence high).
- **Problem:** BlockDevice, DevTty and DevDsp are rejected here but supported by FdBumpRefcount. Passing a socket over itself creates an uncollectable cycle (no GC).
- **Suggested fix:** Use FdBumpRefcount and FinalizeClosedFd.
- **Test:** Host per-FdType refcount balance.

### L-45 · SCANOUT_ADOPT rollback closes fds by number (may close a sibling's replacement)

- **Where:** `src/kernel/src/syscall_brook_gpu.cpp:145`, in `sys_brook_gpu_handoff_ctl`.
- **Severity:** low (confidence medium).
- **Problem:** A sibling can close and reopen an fd number before the CopyToUser failure, so the rollback closes an unrelated descriptor. scene_create already handles this with an FdRef identity compare.
- **Suggested fix:** Pin each fd and verify type and handle before closing, through a helper shared with scene_create.
- **Test:** Host fd-table test: a number replaced between publish and rollback is not closed.

### L-57 · timerfd_create and epoll_create publish the fd and then patch its flags

- **Where:** `src/kernel/src/syscall_timerfd.cpp:84`, in `sys_timerfd_create`.
- **Severity:** low (confidence medium).
- **Problem:** A sibling fork+exec can see the fd without CLOEXEC.
- **Suggested fix:** FdTablePublishOwned with the flags preset (as eventfd2 and signalfd4 do).
- **Test:** Fork+exec race never inherits a TFD_CLOEXEC fd.

### L-59 · CreatePipe publishes descriptors before setting endpoint flags

- **Where:** `src/kernel/src/syscall_pipe.cpp:74`, in `CreatePipe`.
- **Severity:** low (confidence medium).
- **Problem:** The flags, status flags and CLOEXEC are written after FdAlloc and without the fd lock, so a sibling's dup or fork can see a write end with flags=0 or no CLOEXEC. Every error is flattened to EMFILE.
- **Suggested fix:** Publish the entry with its flags preset (FdTablePublishOwned).
- **Test:** Host: FdAlloc with initial flags.

### L-63 · lseek succeeds on sockets, ttys, keyboard and epoll fds

- **Where:** `src/kernel/src/syscall_fd.cpp:456`, in `sys_lseek`.
- **Severity:** low (confidence medium).
- **Problem:** Should be ESPIPE. A VfsStat error becomes EINVAL, and seekPos is updated unlocked.
- **Suggested fix:** A seekable-type allowlist; propagate errors.
- **Test:** QEMU: lseek on socketpair, tty and epoll gives ESPIPE.

### L-71 · Handle retention and allocation while holding the per-process fd spinlock

- **Where:** `src/kernel/src/syscall_fd.cpp:363`, in `DuplicateFd`.
- **Severity:** low (confidence low).
- **Problem:** Calls kmalloc and per-type Ref functions under ProcessFdLock, with an undocumented lock order. Not confirmed whether they can sleep.
- **Suggested fix:** Pin, copy, unlock, then retain.
- **Test:** dup/fork stress with lock debugging.
