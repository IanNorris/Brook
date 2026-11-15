# File descriptions, offsets and fd-table races

_8 open entries (0 high, 5 medium, 3 low; M-67 and L-34 partly fixed). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md); H-20, M-45, M-50, L-57, L-59 and L-63 were fixed on branch `work/open-file`._

- **Scope:** How fds are allocated, published, duplicated and closed, on top of the shared open file description (H-20, done): `flock`, close-by-number on error paths, SCM_RIGHTS publication order, `close_range` UNSHARE, retention under the fd spinlock.
- **Why it matters:** Error paths can close a sibling thread's reused fd, received SCM_RIGHTS fds can leak on a fault, and `flock` does not exclude.
- **Dependencies:** Coordinate with 05 (SCM_RIGHTS: M-63) and 07 (epoll/poll key on fd numbers: M-19, M-58 can now key on the `OpenFile*` description).
- **Suggested order:** M-52, M-63, L-45, L-71 (the same publish/pin theme), M-48, M-53, M-67 (UNSHARE), L-34 (self-cycle GC).
- **Test approach:** `test_fd_table`, `test_scheduler_fd_snapshot` and `test_syscall_user_memory` (publish with pins, O_CLOEXEC, shared descriptions); `scripts/test_open_file_qemu.py` for guest-visible fd semantics.
- **Size:** Small to medium per entry.

### Shared open-file description (H-20), as built

Built on `work/open-file`; see `src/kernel/src/open_file.h`. It differs from the original proposal in one respect: the per-type handle reference stays per slot.

- **`OpenFile { refs; statusFlags; offset; posLock; }`** is allocated by every publication of a new description (`FdAlloc`, `FdPublishNew`, `ProcessCreate`'s standard fds). `FdEntry` keeps the immutable identity (`type`, `handle`, pipe-direction `flags`, diagnostic `dirPath`), the per-descriptor `fdFlags` (FD_CLOEXEC) and a pointer to its `OpenFile`; `seekPos` and `statusFlags` are gone from `FdEntry`, so every reader goes through `FdStatus()` or the position helpers.
- **Reference counting:** each live slot (and each queued SCM_RIGHTS record) owns one `OpenFile` reference. `FdRetainCopy` (dup, dup2/dup3, F_DUPFD, fork, exec-in-thread unshare, SCM_RIGHTS send) retains the description alongside the handle; `FinalizeClosedFd` releases both. `FdClaimResult` and `UnixFdSnap` carry the `file` pointer. Keeping the handle reference per slot means no fd type's lifetime changed; moving it into the description's final release (the original proposal, which would also remove the deep copies of DevKlog/DevTty and retention under the fd spinlock, L-71) is a possible follow-up.
- **Offsets:** `read`, `write`, `writev` O_APPEND, `lseek` and `getdents64` hold `posLock` (a `KMutex`) across the offset read-modify-write for regular files, directories, memfd and synthetic files (`FdAtomicPosition`, Linux FMODE_ATOMIC_POS). Devices and pseudo files use the shared offset without the lock. `sendfile` reads and stores the offset atomically without the lock, as Linux does. Brook has no `copy_file_range`.
- **Status flags:** `F_GETFL`, `F_SETFL` (Linux SETFL_MASK only, M-50) and `FIONBIO` act on the shared flags with an atomic update. Call-only open flags (O_CREAT, O_EXCL, O_NOCTTY, O_TRUNC, O_CLOEXEC) are stripped when the description is created.
- **SCM_RIGHTS:** the receiver gets a new slot naming the sender's description; FD_CLOEXEC comes from MSG_CMSG_CLOEXEC. Snapshot and release reuse `FdRetainCopy` and `FinalizeClosedFd` (L-34).
- **Tests:** `test_fd_table` (sharing across dup/fork/replace/close, posLock serialization), `test_syscall_user_memory` (rw.cpp: `ab` through dup aliases, independent opens, O_APPEND, shared readers; socket.cpp: SCM_RIGHTS shares the description), `test_scheduler_fd_snapshot`, and `scripts/test_open_file_qemu.py` (guest: dup family, fork, child F_SETFL O_NONBLOCK reaching the parent, SCM_RIGHTS, exec with per-fd CLOEXEC, BusyBox `cmd >f 2>&1` and `{ a; b; } >f`).

## Entries

### High

### Medium

### M-48 · flock() is a stub that always succeeds

- **Where:** `src/kernel/src/syscall_file_ops.cpp:117`, in `sys_flock`.
- **Severity:** medium (confidence high).
- **Problem:** No EBADF or EINVAL checks and no exclusion, so git, nix and package managers believe they hold locks. Record-lock machinery already exists.
- **Suggested fix:** Whole-file locks owned by the open file description, at least with validation and EWOULDBLOCK.
- **Test:** test_record_locks flock cases; QEMU: a second LOCK_EX|LOCK_NB gives EWOULDBLOCK.

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
- **Status (`work/open-file`):** partly fixed: CLOSE_RANGE_CLOEXEC now sets FD_CLOEXEC under ProcessFdLock. CLOSE_RANGE_UNSHARE is still accepted and ignored.

### Low

### L-34 · SCM_RIGHTS snapshot ref/unref duplicates FdBumpRefcount/FinalizeClosedFd and has drifted

- **Where:** `src/kernel/src/syscall_socket.cpp:1019`, in `UnixFdSnapFrom`.
- **Severity:** low (confidence high).
- **Problem:** BlockDevice, DevTty and DevDsp are rejected here but supported by FdBumpRefcount. Passing a socket over itself creates an uncollectable cycle (no GC).
- **Suggested fix:** Use FdBumpRefcount and FinalizeClosedFd.
- **Test:** Host per-FdType refcount balance.
- **Status (`work/open-file`):** partly fixed: `UnixFdSnapFrom` now uses `FdRetainCopy` and `UnixFdSnapRelease` uses `FinalizeClosedFd`, so every type dup() supports can be passed and the snapshot carries the shared open file description (H-20). Passing a socket over itself still creates an uncollectable cycle.

### L-45 · SCANOUT_ADOPT rollback closes fds by number (may close a sibling's replacement)

- **Where:** `src/kernel/src/syscall_brook_gpu.cpp:145`, in `sys_brook_gpu_handoff_ctl`.
- **Severity:** low (confidence medium).
- **Problem:** A sibling can close and reopen an fd number before the CopyToUser failure, so the rollback closes an unrelated descriptor. scene_create already handles this with an FdRef identity compare.
- **Suggested fix:** Pin each fd and verify type and handle before closing, through a helper shared with scene_create.
- **Test:** Host fd-table test: a number replaced between publish and rollback is not closed.

### L-71 · Handle retention and allocation while holding the per-process fd spinlock

- **Where:** `src/kernel/src/syscall_fd.cpp:363`, in `DuplicateFd`.
- **Severity:** low (confidence low).
- **Problem:** Calls kmalloc and per-type Ref functions under ProcessFdLock, with an undocumented lock order. Not confirmed whether they can sleep.
- **Suggested fix:** Pin, copy, unlock, then retain.
- **Test:** dup/fork stress with lock debugging.
