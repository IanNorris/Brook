# Syscall Subsystem Documentation

## Overview

`syscall.cpp` is Brook's monolithic syscall dispatcher at ~11,000 lines. It
implements ~100 Linux-compatible system calls plus Brook-specific extensions
(500-series). Entry is via the SYSCALL instruction (LSTAR MSR →
`BrookSyscallDispatcher`).

## Files

| File | Lines | Purpose |
|------|-------|---------|
| `syscall.cpp` | ~11076 | All syscall implementations + dispatcher |
| `syscall.h` | ~30 | SyscallFrame, SwitchToUserMode declaration |

---

## Entry Path

1. User executes `syscall` instruction
2. CPU: RCX←RIP, R11←RFLAGS, RIP←LSTAR, mask RFLAGS
3. `BrookSyscallDispatcher` (naked asm):
   - `swapgs` → access per-CPU KernelCpuEnv via GS
   - Save user RSP, switch to kernel stack (gs:8)
   - Push all registers (16 pushes = 128 bytes, aligned)
   - Save user context to KernelCpuEnv for fork
   - Call `SyscallDispatchC` → `SyscallDispatchInternal`
   - On return: `SyscallCheckSignals` (signal delivery)
   - Validate RCX is canonical user address (bit 47 check)
   - `swapgs` + `sysret` to user mode

## Validation Helpers

| Function | Purpose |
|----------|---------|
| `UserBufferReadable(addr, len)` | Verify [addr, addr+len) is mapped readable user memory |
| `UserBufferWritable(addr, len)` | Verify [addr, addr+len) is mapped writable user memory |
| `CopyUserCString(addr, out, size)` | Safe copy of NUL-terminated string from user space |
| `CopyToUser(addr, src, len)` | Copy kernel data to validated user buffer |

**⚠️ BRO-004:** Many syscalls do NOT use these helpers. See Known Issues.

---

## Syscall Table

### File Operations
| # | Name | Validation | Notes |
|---|------|------------|-------|
| 0 | read | ✅ UserBufferReadable | Handles pipes, vnodes, eventfd, timerfd, memfd |
| 1 | write | ✅ UserBufferReadable | Handles pipes, vnodes, eventfd, memfd, audio |
| 2 | open | ✅ CopyUserCString | Full VFS open with O_CREAT, O_TRUNC, etc. |
| 3 | close | ✅ FdGet | Handles all fd types with proper cleanup |
| 5 | fstat | ✅ FdGet | Returns LinuxStat for fd |
| 4 | stat | ❌ No validation | BRO-004 |
| 6 | lstat | ❌ No validation | BRO-004 |
| 8 | lseek | ✅ FdGet | SEEK_SET/CUR/END |
| 16 | ioctl | ❌ Weak `< 0x1000` | BRO-004, 30+ sub-cases |
| 17 | pread64 | ❌ No buf validation | BRO-004 |
| 18 | pwrite64 | ❌ No buf validation | BRO-004 |
| 19 | readv | ❌ No iov validation | BRO-004 |
| 20 | writev | ❌ No iov validation | BRO-004 |
| 22 | pipe | ❌ NULL check only | BRO-004 |
| 32 | dup | ✅ FdGet | |
| 33 | dup2 | ✅ FdGet + bounds | |
| 40 | sendfile | Stub | No offset support |
| 72 | fcntl | ✅ FdGet | F_GETFL, F_SETFL, F_GETFD, F_SETFD, F_DUPFD |
| 77 | ftruncate | ✅ FdGet | |
| 79 | getcwd | ❌ No validation | BRO-004 |
| 82 | rename | ❌ No path validation | BRO-004 |
| 83 | mkdir | ❌ No path validation | BRO-004 |
| 87 | unlink | ❌ No path validation | BRO-004 |
| 88 | symlink | ❌ No path validation | BRO-004 |
| 89 | readlink | ✅ CopyUserCString + CopyToUser | |
| 217 | getdents64 | ❌ No buf validation | BRO-004 |
| 257 | openat | ❌ No path validation | BRO-004 |
| 293 | pipe2 | ❌ NULL check only | BRO-004 |
| 319 | memfd_create | ⚠️ Weak check | |
| 405 | close_range | ✅ Bounds checked | |

### Memory Management
| # | Name | Validation | Notes |
|---|------|------------|-------|
| 9 | mmap | ✅ | MAP_ANONYMOUS, MAP_PRIVATE, MAP_SHARED, MAP_FIXED |
| 10 | mprotect | ✅ | |
| 11 | munmap | ✅ | |
| 12 | brk | ✅ | 128MB program break limit |
| 25 | mremap | ⚠️ Race in copy | Direct user VA memcpy |
| 27 | mincore | ❌ No vec validation | BRO-004 |

### Process Management
| # | Name | Validation | Notes |
|---|------|------------|-------|
| 39 | getpid | ✅ | |
| 56 | clone | ❌ PARENT_SETTID write | BRO-004 |
| 57 | fork | ✅ | |
| 58 | vfork | ✅ | Treated as fork |
| 59 | execve | ❌ argv/envp arrays | BRO-004 |
| 60 | exit | ✅ | |
| 61 | wait4 | ❌ status write | BRO-004 |
| 62 | kill | ✅ | |
| 110 | getppid | ✅ | |
| 158 | arch_prctl | ❌ ARCH_GET_FS write | BRO-004 |
| 186 | gettid | ✅ | |
| 218 | set_tid_address | ⚠️ Stored, validated later | |
| 231 | exit_group | ✅ | |
| 435 | clone3 | ❌ No args validation | BRO-004 |

### Signal Handling
| # | Name | Validation | Notes |
|---|------|------------|-------|
| 13 | rt_sigaction | ❌ No validation | BRO-004 |
| 14 | rt_sigprocmask | ❌ No validation | BRO-004 |
| 15 | rt_sigreturn | ✅ | |
| 34 | pause | ✅ | |
| 37 | alarm | ✅ | |
| 128 | rt_sigtimedwait | ✅ UserBufferReadable | |
| 130 | rt_sigsuspend | ❌ No validation | BRO-004 |
| 131 | sigaltstack | ❌ No validation | BRO-004 |

### Time & Sleep
| # | Name | Validation | Notes |
|---|------|------------|-------|
| 35 | nanosleep | ✅ UserBufferReadable | rem uses wrong check (Readable not Writable) |
| 96 | gettimeofday | ❌ No validation | BRO-004 |
| 201 | time | ❌ No validation | BRO-004 |
| 228 | clock_gettime | ❌ No validation | BRO-004 |
| 229 | clock_getres | ❌ No validation | BRO-004 |

### I/O Multiplexing
| # | Name | Validation | Notes |
|---|------|------------|-------|
| 7 | poll | ❌ No fds validation | BRO-004; waiter limit = 16 fds |
| 23 | select | ❌ No fd_set validation | BRO-004 |
| 213 | epoll_create | ✅ | |
| 232 | epoll_wait | ❌ Weak `< 0x1000` | BRO-004 |
| 233 | epoll_ctl | ❌ Weak `< 0x1000` | BRO-004 |
| 285 | timerfd_create | ✅ | |
| 287 | timerfd_settime | ❌ Weak `< 0x1000` | BRO-004 |
| 288 | timerfd_gettime | ❌ Weak `< 0x1000` | BRO-004 |
| 290 | eventfd2 | ✅ | |

### Info / Identity
| # | Name | Validation | Notes |
|---|------|------------|-------|
| 63 | uname | ❌ No validation | BRO-004 |
| 97 | getrlimit | ❌ No validation | BRO-004 |
| 98 | getrusage | ❌ No validation | BRO-004 |
| 99 | sysinfo | ❌ No validation | BRO-004 |
| 102-108 | uid/gid getters | ✅ | Return constants |
| 118 | getresuid | ❌ No validation | BRO-004 |
| 120 | getresgid | ❌ No validation | BRO-004 |
| 125 | capget | ❌ No validation | BRO-004 |
| 137 | statfs | ❌ No validation | BRO-004 |
| 204 | sched_getaffinity | ❌ No validation | BRO-004 |
| 302 | prlimit64 | ❌ No validation | BRO-004 |
| 318 | getrandom | ❌ No validation | BRO-004 |
| 332 | statx | ❌ No validation | BRO-004 |

### Networking (via net.cpp)
| # | Name | Notes |
|---|------|-------|
| 41 | socket | Delegates to NetSocket |
| 42 | connect | Delegates to NetConnect |
| 43 | accept | Delegates to NetAccept |
| 44 | sendto | Delegates to NetSendTo |
| 45 | recvfrom | Delegates to NetRecvFrom |
| 46 | sendmsg | Delegates to NetSendMsg |
| 47 | recvmsg | Delegates to NetRecvMsg |
| 49 | bind | Delegates to NetBind |
| 50 | listen | Delegates to NetListen |
| 51 | getsockname | Delegates to NetGetSockName |
| 52 | getpeername | Stub |
| 53 | socketpair | Delegates to NetSocketPair |
| 54 | setsockopt | Delegates to NetSetSockOpt |
| 55 | getsockopt | Delegates to NetGetSockOpt |
| 48 | shutdown | Delegates to NetShutdown |

### Brook-Specific (500-series)
| # | Name | Validation | Notes |
|---|------|------------|-------|
| 500 | brook_profile | ✅ | Profiler control |
| 502 | brook_set_crash_entry | ✅ | Store crash handler |
| 503 | brook_crash_complete | ✅ | Crash dump done |
| 504 | brook_input_pop | ❌ No buf validation | BRO-004 |
| 505 | brook_input_grab | ✅ | |
| 506 | brook_wm_create_window | ❌ No validation | BRO-004 |
| 507 | brook_wm_destroy_window | ✅ | |
| 508 | brook_wm_signal_dirty | ✅ | |
| 509 | brook_wm_set_title | ❌ No path validation | BRO-004 |
| 510 | brook_wm_pop_input | ❌ No buf validation | BRO-004 |
| 511 | brook_wm_resize_vfb | ❌ No validation | BRO-004 |
| 512-518 | wm control | ✅ | No user buffers |

---

## Known Issues

| Bug | Severity | Description |
|-----|----------|-------------|
| **BRO-004** | high | ~60+ unvalidated user pointer dereferences across ~40 syscalls |
| **BRO-082** | high | SyscallCheckSignals reads/writes signal frame without validation |
| **BRO-083** | medium | sys_writev drops partial writes; no iovcnt bounds check |
| **BRO-084** | low | sys_poll only registers waiters on first 16 fds |

### Validation Pattern Summary

Three patterns exist in the codebase:
1. **Proper validation** (✅): `UserBufferReadable/Writable` + `CopyUserCString` — used by sys_read, sys_write, sys_open, sys_readlink
2. **Weak heuristic** (⚠️): `if (arg < 0x1000) return -EFAULT` — catches NULL but not unmapped/kernel addresses
3. **No validation** (❌): Direct `reinterpret_cast` and dereference — most common pattern, ~40 syscalls

### Recommended Fix Strategy

Add macros at the top of the file:
```cpp
#define VALIDATE_USER_READ(addr, len) \
    do { if (!UserBufferReadable((addr), (len))) return -EFAULT; } while(0)
#define VALIDATE_USER_WRITE(addr, len) \
    do { if (!UserBufferWritable((addr), (len))) return -EFAULT; } while(0)
```
Then systematically add to each affected syscall. Priority order:
1. Syscalls that write to user memory (arbitrary write → exploit)
2. Syscalls that read user strings (path traversal, kernel crash)
3. Syscalls that read user structs (info leak, crash)

---

## Internal Data Structures

### EventFdData
```cpp
struct EventFdData {
    volatile uint64_t counter;
    uint32_t flags;         // EFD_SEMAPHORE, EFD_NONBLOCK
    volatile uint32_t refCount;
    Process* readerWaiter;
};
```

### TimerFdData
```cpp
struct TimerFdData {
    volatile uint64_t expiryCount;
    uint64_t intervalNs;
    uint64_t nextExpiry;     // absolute LAPIC tick
    int clockId;
    volatile uint32_t refCount;
    bool armed;
    Process* waiter;
};
```

### EpollInstance
```cpp
struct EpollInstance {
    EpollEntry entries[EPOLL_MAX_FDS]; // 64 max watched fds
    int count;
    volatile uint32_t refCount;
    Process* waiter;
};
```

### MemFd
Sparse/lazy page-backed anonymous file. Pages allocated on first write.
Max size tracked per-memfd. Supports mmap with lazy fault handling.
