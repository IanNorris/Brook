# Errno literals, strace and diagnostics

_10 open entries (1 medium, 8 low, 1 info). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** Raw `return -NN` literals across the syscall layer, the strace name table and PID scan, unthrottled serial prints on user-triggerable paths.
- **Why it matters:** Mechanical, but it touches the same lines as every other chunk, so it is best done as one early pass, or per file just after that file's chunk. Wrong numbers are real bugs (L-28 magic numbers, M-03 strace table).
- **Dependencies:** Do before or between the other chunks, never in parallel with a branch that edits the same files. Add the missing constants to `syscall_internal.h` first (L-03), then a grep lint. L-28 and L-31 overlap 06 and 05.
- **Suggested order:** L-03 (constants), L-13, L-14, L-28, L-31, I-13, then M-03, L-01, L-04, L-15.
- **Test approach:** A grep lint for `return -[0-9]` in `src/kernel/src/syscall_*.cpp`. Existing host tests must still pass unchanged (no behaviour change).
- **Size:** Medium and mechanical: 10 entries (about 200 sites), 1 sitting.

## Entries

### Medium

### M-03 · strace name table has wrong numbers and lacks names for special-cased syscalls

- **Where:** `src/kernel/src/syscall_strace.cpp:122`, in `SyscallName`.
- **Severity:** medium (confidence high).
- **Problem:** statfs and fstatfs are listed as 134 and 135, but SYS_STATFS=137 and SYS_FSTATFS=138, so personality(135) prints as fstatfs. Names are missing for futex(202), the epoll_* calls, accept4, mount/umount2/pivot_root, sync, mknod*, capget/capset, memfd_create, timerfd_* and others, so the always-log errors list prints "?". SyscallCategory also omits accept4 from SC_NET, epoll_pwait/epoll_create1 from SC_IO, and faccessat, mknodat, fsync, statfs and fchdir from SC_FILE.
- **Suggested fix:** Generate the name and category tables from one X-macro list keyed by the SYS_* constants.
- **Test:** Host test: every non-default slot of g_syscallTable has a non-null name, and SyscallName(SYS_STATFS)=="statfs".

### Low

### L-01 · StraceEnableName/StraceEnableAll scan only PIDs 1..255

- **Where:** `src/kernel/src/syscall_strace.cpp:311` (snippet matches 2 lines: 311, 334), in `StraceEnableName / StraceEnableAll`.
- **Severity:** low (confidence high).
- **Problem:** TaskIdCapacity is 1024, so pids of 256 and above are never traced. ProcessFindByPid returns an unreferenced pointer that is then written while the process may be exiting.
- **Suggested fix:** Iterate the process table under its lock or while holding a reference.
- **Test:** Host test: StraceEnableName matches pid 700.

### L-03 · Raw negative errno literals in Brook syscalls

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:266`, in `sys_brook_wm_set_client_socket`.
- **Severity:** low (confidence high).
- **Problem:** About 30 sites: -95, -116 (about 10, in the desktop and GPU files), -13, -16, -11, -30, and -38 (in syscall.cpp and strace). ENOTSOCK, EAGAIN and ENOSYS already exist as constants.
- **Suggested fix:** Add EACCES, EBUSY, EROFS, EOPNOTSUPP and ESTALE to syscall_internal.h and use the names.
- **Test:** Grep lint for `return -[0-9]` in syscall_*.cpp.

### L-04 · Unthrottled serial print for out-of-range syscall numbers; lookup triplicated

- **Where:** `src/kernel/src/syscall.cpp:660`, in `SyscallDispatchInternal`.
- **Severity:** low (confidence high).
- **Problem:** `!fn` only happens for num >= SYSCALL_MAX, so a user loop floods serial (sys_not_implemented is rate-limited). The lookup and FATAL-AVOIDED block is repeated twice in SyscallDispatchInternal and once in SyscallDispatchTraced.
- **Suggested fix:** One SyscallLookup(num) that routes out-of-range numbers to sys_not_implemented.
- **Test:** Host test: 10k calls with num=SYSCALL_MAX+1 give -ENOSYS with bounded log output.

### L-13 · Raw negative errno literals in DRM and ioctl

- **Where:** `src/kernel/src/syscall_drm.cpp:549`, in `DrmRenderIoctl, sys_ioctl`. Also in: syscall_drm.cpp, syscall_ioctl.cpp.
- **Severity:** low (confidence high).
- **Problem:** About 36 literals (-25, -95, -16, -32, -125, -75, -13, -110, -17), some of which already have constants.
- **Suggested fix:** Add the missing constants to syscall_internal.h and use them.
- **Test:** Grep lint.

### L-14 · Raw negative errno literals in memory/memfd/futex/exec (~25 sites)

- **Where:** `src/kernel/src/syscall_memory.cpp:377`, in `sys_mmap`.
- **Severity:** low (confidence high).
- **Problem:** Literals -95, -13, -75, -35, -17, -21 and -36. deny_mapping should arguably be ENODEV, and a low MAP_FIXED should arguably give EPERM.
- **Suggested fix:** Named constants; review those two semantics.
- **Test:** Grep lint.

### L-15 · Unconditional serial logging on exec/fork/futex/memfd paths

- **Where:** `src/kernel/src/syscall_exec.cpp:261`, in `sys_execve`.
- **Severity:** low (confidence high).
- **Problem:** 6 sites, including [PROFILE] fork (not gated by g_hotLogQuiet), a FutexLog atomic per wait, and a MemFdGrowLocked print under a spinlock.
- **Suggested fix:** Gate behind g_hotLogQuiet or diagnostic toggles.
- **Test:** No serial output under g_hotLogQuiet in the fork/exec benchmark.

### L-28 · Raw magic errno numbers (pipe/fifo/dsp/maintenance)

- **Where:** `src/kernel/src/named_fifo.inc:50`, in `NamedFifoAttach`.
- **Severity:** low (confidence high).
- **Problem:** 7 sites: -6, -95 and -16 in DspChangeFormat, -28 in MaintenanceBlockTransfer, -29 twice in pread/pwrite, and SIGPIPE=13 as a literal.
- **Suggested fix:** Named constants.
- **Test:** Grep lint.

### L-31 · Raw magic errno numbers, flags and signals in socket/unix/netlink

- **Where:** `src/kernel/src/syscall_socket.cpp:53`, in `sys_socket`.
- **Severity:** low (confidence high).
- **Problem:** About 40 raw errnos, plus raw MSG flags, AF_NETLINK=16 and SIGPIPE=13.
- **Suggested fix:** Named constants.
- **Test:** Build.

### Info

### I-13 · About 36 raw negative errno literals in VFS-facing files

- **Where:** `src/kernel/src/syscall_open.cpp:67`, in `OpenProcMemFd`.
- **Severity:** info (confidence high).
- **Problem:** Literals include -20 and -40 although the constants exist. AccessAt and stat treat a raw -1 from the VFS as ENOENT.
- **Suggested fix:** Named constants; stop the VFS returning a bare -1.
- **Test:** Grep lint.
