# Memory, mmap, time and system information

_11 open entries (6 medium, 5 low). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** `mmap` of memfds and files, `mincore`, `madvise`/`fadvise`, named shm and `memfd_create` arguments, `times`, `getrandom`, `gettimeofday`, and argument validation / stub syscalls for time, rlimit, scheduling.
- **Why it matters:** Compat and robustness. M-56/M-57 change how faults on file mappings behave, so they deserve care on the desktop path.
- **Dependencies:** None. L-14 (errno literals in memory) is in 12.
- **Suggested order:** M-17, M-56, M-16, M-57, L-55, L-53, M-25, M-26, L-18, L-19, L-20.
- **Test approach:** `test_memfd_seals`, `test_syscall_user_memory/memfd.cpp` and `race.cpp`; fault behaviour (SIGBUS, mmap of unpinnable files) needs QEMU.
- **Size:** Medium: 11 entries, 1 or 2 sittings.

## Entries

### Medium

### M-16 · mincore faults pages in, fails on PROT_NONE ranges, and calls the same resolver twice

- **Where:** `src/kernel/src/syscall_memory.cpp:583`, in `sys_mincore`.
- **Severity:** medium (confidence high).
- **Problem:** Probing a sparse 700 MiB memfd allocates it all. PROT_NONE gives ENOMEM. Both names wrap one resolver.
- **Suggested fix:** Walk mapping ranges without faulting.
- **Test:** QEMU test_mincore.

### M-17 · Writable MAP_PRIVATE memfd mappings rejected; private memfd maps can never be made writable

- **Where:** `src/kernel/src/syscall_memory.cpp:378`, in `sys_mmap`.
- **Severity:** medium (confidence high).
- **Problem:** Linux supports CoW here. mprotect(PROT_WRITE) gives EACCES. The seal check duplicates MemFdAdmitMapping.
- **Suggested fix:** CoW private memfd mappings.
- **Test:** QEMU test_memfd_private_cow.

### M-25 · times() reports ms ticks while AT_CLKTCK is 100; child times system-wide

- **Where:** `src/kernel/src/syscall_sysinfo.cpp:216`, in `sys_times`.
- **Severity:** medium (confidence high).
- **Problem:** Values are 10 times too large. Reaped ticks are global, so children of other processes are counted. Uptime uses ApicTickCount, which drifts.
- **Suggested fix:** Convert to USER_HZ; keep reaped ticks per parent; ClockNowNs for uptime.
- **Test:** test_runtime_accounting: a 1 s busy loop gives user ≈ 100.

### M-26 · getrandom silently falls back to a TSC-based PRNG

- **Where:** `src/kernel/src/syscall_sysinfo.cpp:95`, in `SoftRandU64 (used by sys_getrandom)`. Also in: syscall_common.cpp.
- **Severity:** medium (confidence high).
- **Problem:** Without RDRAND (QEMU qemu64), the output is predictable rdtsc-xorshift-LCG data presented as cryptographic randomness.
- **Suggested fix:** A ChaCha20 CSPRNG seeded at boot; EAGAIN under GRND_NONBLOCK until seeded.
- **Test:** Host test with RDRAND masked.

### M-56 · Access past EOF on a memfd mapping raises SIGSEGV instead of SIGBUS; memfd OOM reported as Unhandled

- **Where:** `src/kernel/src/syscall_memory.cpp:234`, in `ResolveMappingFaultForSpace`.
- **Severity:** medium (confidence medium).
- **Problem:** The file branch returns EndOfFile (SIGBUS) but the memfd branch returns Unhandled (SIGSEGV), breaking wl_shm clients. The two LockFaultRange failures return different results.
- **Suggested fix:** EndOfFile and OutOfMemory.
- **Test:** QEMU test_memfd_sigbus.

### M-57 · Unpinnable file mmap eagerly allocates and reads the whole length; pages beyond EOF read as zeros

- **Where:** `src/kernel/src/syscall_memory.cpp:497`, in `sys_mmap / PrepareFileMapping`.
- **Severity:** medium (confidence medium).
- **Problem:** Mapping 1 GiB of a 4 KiB file allocates 1 GiB, and there is no SIGBUS past EOF.
- **Suggested fix:** Cap at the file size; fault the tail to EndOfFile.
- **Test:** QEMU test_mmap_unpinned_file.

### Low

### L-18 · gettimeofday(NULL, tz) fails with EFAULT

- **Where:** `src/kernel/src/syscall_time.cpp:73`, in `sys_gettimeofday`.
- **Severity:** low (confidence high).
- **Problem:** Linux skips a NULL tv and fills tz; tz is ignored here.
- **Suggested fix:** Skip NULL tv; zero tz.
- **Test:** gettimeofday(NULL,&tz) returns 0.

### L-19 · Missing argument validation (epoll_create1 flags, epoll_create size, getrandom flags, getrusage who, rlimit resource, priority policy, epoll_ctl bits, clocks 8/9, timerfd CLOCK_BOOTTIME)

- **Where:** `src/kernel/src/syscall_epoll.cpp:395`, in `sys_epoll_create1`.
- **Severity:** low (confidence high).
- **Problem:** Invalid arguments are accepted where Linux returns EINVAL. timerfd lacks CLOCK_BOOTTIME, which clock_nanosleep accepts.
- **Suggested fix:** Linux-equivalent checks.
- **Test:** Host invalid-argument errno table.

### L-20 · Stubs that silently succeed (setaffinity, setparam, setscheduler, setpriority, prlimit set); getaffinity reports 8 CPUs

- **Where:** `src/kernel/src/syscall_sched.cpp:60`, in `sys_sched_setaffinity / sys_sched_getaffinity`.
- **Severity:** low (confidence high).
- **Problem:** These do nothing but succeed. pid is ignored (no ESRCH). The fake CPU count mis-sizes thread pools.
- **Suggested fix:** Real online mask; ESRCH; EPERM or EINVAL for unsupported requests.
- **Test:** getaffinity popcount equals the SMP count.

### L-53 · madvise hints never check the range is mapped; common advice values return EINVAL; fadvise64/mbind are silent stubs

- **Where:** `src/kernel/src/syscall_memory.cpp:611`, in `sys_madvise`.
- **Severity:** low (confidence medium).
- **Problem:** DONTDUMP and similar give EINVAL (OpenSSL and Chromium use them). fadvise64 doesn't validate the fd.
- **Suggested fix:** Validate mapping coverage; accept harmless advice values.
- **Test:** QEMU test_madvise_hints.

### L-55 · Named shm and memfd_create argument handling diverges from Linux

- **Where:** `src/kernel/src/syscall_memfd.cpp:211`, in `NamedShmOpen / sys_memfd_create`.
- **Severity:** low (confidence medium).
- **Problem:** O_TRUNC always gives EOPNOTSUPP. The seal default differs from Linux. mode is ignored. memfd_create doesn't validate the name. MFD_NOEXEC_SEAL and MFD_EXEC give EINVAL. There are 3 divergent MemFdData construction sites.
- **Suggested fix:** MemFdCreate(); allow O_TRUNC when unmapped; validate the name.
- **Test:** test_memfd_seals; QEMU shm_open O_TRUNC.
