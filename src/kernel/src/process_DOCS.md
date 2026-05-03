# Process Management Documentation

## Overview

The process subsystem manages process lifecycle (create, fork, exec, destroy),
file descriptors, signal handling, and thread creation. It is the largest
single-file component after syscall.cpp.

## Files

| File | Lines | Purpose |
|------|-------|---------|
| `process.h` | ~600 | Process struct, FdEntry, signal types, API declarations |
| `process.cpp` | ~2055 | Process lifecycle implementation |

---

## Key Data Structures

### Process (process.h)

The central kernel data structure. ~2.5KB per instance (heap-allocated).
Uses `PROCESS_MAGIC` sentinel for use-after-free detection.

**Identity:**
- `pid` (uint16_t): Unique process ID, 1-255
- `parentPid`, `pgid`, `sid`: Process group, session
- `tgid`: Thread group ID (= leader's PID)
- `threadLeader`: Pointer to thread group leader

**Scheduling:**
- `state` (ProcessState): Ready, Running, Blocked, Stopped, Terminated
- `schedNext/Prev`: Circular doubly-linked ready queue
- `runningOnCpu`: CPU index (-1 = not running)
- `cpuAffinity`: Pinned CPU (-1 = unset, required because no TLB shootdown)
- `wakeupTick`: Timer-based wakeup
- `syncNext`, `pendingWakeup`: Mutex/sync wait queue linkage

**Memory:**
- `pageTable` (PageTable): Per-process PML4
- `programBreak`: Current brk() position
- `mmapNext`: Next free user mmap virtual address
- `kernelStackBase/Top`: Per-process kernel stack (256KB with guard pages)
- `memfdMaps[256]`, `fileMaps[4096]`, `fbMaps[4]`: Lazy mapping records

**File Descriptors:**
- `fds` (FdEntry*): Heap-allocated array of MAX_FDS (256) entries
- Shared across CLONE_FILES threads (all threads point to leader's table)
- fork() allocates a new independent copy

**Signal State:**
- `sigMask`, `sigPending`, `sigSavedMask`: Bitmasks
- `sigAltstackSp/Size/Flags`: Alternate signal stack
- `inSignalHandler`, `sigReturnPending`: Delivery state

**Threading:**
- `isThread`: True for non-leader threads
- `clearChildTid`: CLONE_CHILD_CLEARTID address
- `isForkChild`: Fork child trampoline state
- `forkReturn{Rip,Rsp,Rflags,Rbx,Rbp,R12-R15,Rdi,Rsi,Rdx,R8-R10}`: Full saved register set

### FdEntry

| Field | Type | Description |
|-------|------|-------------|
| `type` | FdType | None, Vnode, Pipe, Socket, DevNull, MemFd, UnixSocket, etc. |
| `flags` | uint8_t | O_NONBLOCK, pipe direction |
| `fdFlags` | uint8_t | FD_CLOEXEC (bit 0) |
| `refCount` | uint32_t | Reference count |
| `statusFlags` | uint32_t | O_* flags from open |
| `handle` | void* | Backend-specific handle |
| `seekPos` | uint64_t | File offset |
| `dirPath[64]` | char | Directory path for openat |

### Address Space Layout (User)

| Range | Purpose |
|-------|---------|
| `0x400000` (USER_LOAD_BASE) | ELF binary load address |
| `0x10000000..0x700000000000` | mmap region |
| `0x7F0000000000` (INTERP_LOAD_BASE) | Dynamic linker |
| `0x7FFFFFFFE000` (USER_STACK_TOP) | Stack top (grows down, 8MB) |

---

## Functions

### Process Lifecycle

| Function | Contract |
|----------|----------|
| `ProcessCreate(elfData, elfSize, argc, argv, envc, envp, stdFds)` | Creates a new process from an ELF binary. Allocates pid, kernel stack, page table, loads ELF + interpreter, sets up user stack with Linux ABI (argc/argv/envp/auxv), initializes TLS, wires fd 0/1/2. Returns Process* or null. |
| `KernelThreadCreate(name, fn, arg, priority)` | Creates a ring-0 kernel thread. Uses kernel page table, no user stack. fn/arg stored on kernel stack for trampoline. |
| `ProcessFork(parent, userRip, userRsp, userRflags)` | Copy-on-write fork. Writable pages are eagerly copied (no SMP TLB shootdown yet). Read-only pages are shared with PmmRefPage. Duplicates fd table with proper refcounting for pipes, vnodes, sockets, memfds, eventfds, epollfd, timerfds, unix sockets. |
| `ProcessCreateThread(parent, userRip, userRsp, userRflags, tlsBase)` | CLONE_VM\|CLONE_FILES thread. Shares page table, fd table, tgid. Gets own kernel stack and PID (TID). |
| `CreateRemoteThread(target, entry, stackSize, argBytes, argLen)` | Injects a new thread into target's address space. Allocates stack in target's mmap region, copies arg data. Used for crash dump writers. |
| `ProcessExec(proc, elfData, elfSize, argc, argv, envc, envp, outStackPtr)` | Replaces address space. Frees old page table, loads new ELF, resets mmap/brk, closes FD_CLOEXEC fds, resets signal handlers (SIG_IGN preserved). |
| `ProcessDestroy(proc)` | Full teardown: force-unlock ext2 mutexes, close all fds (leader only), unregister from compositor/WM, destroy page table, free pages, free kernel stack, clear signal handlers. |

### File Descriptor Operations

| Function | Contract |
|----------|----------|
| `FdAlloc(proc, type, handle) → int` | Find first free slot, return fd index. Returns -EMFILE if full. |
| `FdFree(proc, fd)` | Zero-fill the slot. |
| `FdGet(proc, fd) → FdEntry*` | Bounds-check + None-check, return pointer or null. |
| `ProcessCloseAllFds(proc)` | Close all fds (process exit). Delegates to CloseProcessFd. |
| `ProcessCloseCloexecFds(proc)` | Close FD_CLOEXEC fds (execve). |

### Signal Delivery

| Function | Contract |
|----------|----------|
| `ProcessSendSignal(proc, signum) → int` | SIGKILL: immediate terminate. SIGSTOP: immediate stop. SIGCONT: resume + deliver. SIGTSTP/TTIN/TTOU: stop if default action. Otherwise: set pending bit, wake blocked process. Drops ignored signals. |
| `ProcessSendSignalToGroup(pgid, signum) → int` | Send to all processes with matching pgid. |
| `SyscallCheckSignals(frame, syscallResult) → int64_t` | Called from asm syscall return path. Handles rt_sigreturn (restore frame from ucontext). Delivers pending signals by building SignalFrame on user stack and redirecting SyscallFrame to handler. Supports SA_RESTART for read(). |

---

## Known Issues

| Bug | Severity | Description |
|-----|----------|-------------|
| **BRO-081** | medium | Error paths in ProcessCreate/Fork leak kernel stack and fd table |
| **BRO-082** | high | SyscallCheckSignals reads/writes user memory without validation |

### Additional Observations

1. **Duplicate declaration**: `FdGet` is declared twice in process.h (lines 575 and 581). Harmless but should be cleaned up.

2. **Deterministic AT_RANDOM and stack canary**: `SimpleRand` uses a hardcoded seed (`0xDEADBEEFCAFEBABE`). Stack canary is always `0x57a0000012345678`. Both are predictable. Should use RDRAND if available (the RNG is already available via VirtIO RNG / /dev/urandom).

3. **Self-copy in ProcessCreateThread**: Line 1344 copies `g_sigHandlers[parent->tgid]` to `g_sigHandlers[thread->tgid]`, but `thread->tgid == parent->tgid`, making this a no-op self-copy. Wasted work.

4. **No lock on fd table**: FdAlloc/FdFree/FdGet have no synchronization. With CLONE_FILES threads sharing one fd table, concurrent open/close can race. Currently mitigated by SMP CPU affinity pinning, but will need a lock for true SMP.

5. **ProcessSendSignal SIGKILL race**: Directly sets `state = Terminated` without holding any scheduler lock. If target is running on another CPU, the state change races with the scheduler's timer tick check.

6. **MAX_PROCESSES = 256**: PID space is 8-bit effective range. `g_sigHandlers[256][64]` = 128KB static. Adequate for current workload but could be tight for complex app stacks (Ladybird spawns ~10 processes).

7. **ProcessFork copies writable pages eagerly**: No COW for writable pages because Brook lacks SMP TLB shootdown. This means fork() of a large process (like Ladybird) copies its entire writable address space. Documented as intentional limitation.

8. **TLS setup is duplicated**: Identical TLS allocation + initialization code appears in both ProcessCreate (~line 598-662) and ProcessExec (~line 1630-1687). Should be factored into a shared helper.

---

## Init Sequence

1. `SchedulerAllocPid()` — get unique PID
2. `AllocFdTable()` — heap-allocate fd array
3. `VmmAllocKernelStack()` — kernel stack with guard pages
4. `VmmCreateUserPageTable()` — PML4 with shared kernel half
5. `ElfLoad()` — load binary into user page table
6. `LoadInterpreter()` — load ld-linux if PT_INTERP
7. Stack allocation (user-space pages, guard page at bottom)
8. `SetupUserStack()` — Linux ABI: argc/argv/envp/auxv
9. TLS setup (variant II, self-pointer, stack canary)
10. `SchedulerAddProcess()` — enqueue for scheduling

## Teardown Sequence (ProcessDestroy)

1. `Ext2ForceUnlockForPid()` — release held ext2 mutexes
2. `ProcessCloseAllFds()` — close all file descriptors (leader only)
3. `kfree(proc->fds)` — free fd table (leader only)
4. `CompositorUnregisterProcess()` — remove from compositor
5. `WmDestroyWindowForProcess()` — destroy WM windows
6. `ProcessClearLazyMappings()` — release memfd/file VMA references
7. `VmmDestroyUserPageTable()` — free user page table pages
8. `VmmKillPid()` — free VMALLOC + PMM pages
9. `VmmFreeKernelStack()` — free kernel stack
10. `SchedulerRemoveProcess()` — remove from scheduler
11. Clear `g_sigHandlers[tgid]` — prevent stale handler pointers
12. `FreeProcessStruct()` — zero magic + kfree
