# syscall.cpp split plan

_Written 15 November 2026 by a Claude Code session, before any code moved. Branch `work/syscall-split` from `main` at `aac1d429`._

## Goal and rules

Ian's instruction: "At this point we should aim for just moving code, recording any issues then make actual fixes in a separate pass with additional test coverage."

`src/kernel/src/syscall.cpp` (17,259 lines) is split so that each source file has one responsibility. This pass is a **pure move**:

- no behaviour changes, no renames, no logic edits and no opportunistic fixes;
- no syscall numbers, ABI structs, error codes or locking change;
- anything that looks wrong is recorded in [REFACTOR_ISSUES.md](REFACTOR_ISSUES.md) for a separate fix pass with tests.

The only edits to moved text are the ones a split forces. Each is listed under "Edits the split requires" below.

## How the map was made

The file was segmented mechanically into 950 top-level chunks (functions, variables, types, macros, `#include`s of `.inc` fragments, and namespace/preprocessor structure), with each chunk's leading comment attached to it. Each chunk was assigned to an area by its original line range. A cross-reference pass then found every name used outside its new file, and classified it:

| Kind of shared item | Treatment |
|---|---|
| Type, `static constexpr` constant, macro, `static inline` helper | The definition moves, verbatim, into the owning area's internal header. Internal linkage and inlining are unchanged. |
| `static` function or variable | `static` is dropped. The definition stays in the area's `.cpp` and the internal header declares it. |
| Function already declared in a kernel header (for example `syscall.h`) | Nothing: callers already see the declaration. |
| `sys_*` handler | `static` is dropped and the prototype goes in `syscall_handlers.h`, which the dispatch table includes. |

Items used by only one file keep `static` and stay file-local. A post-build check confirmed that every newly external symbol is referenced from another object file, except `CreateIntelSyncFile`, whose only other caller is compiled with the Dell GPU options.

Include lists were pruned per file. Each candidate removal had to compile in three configurations: the default Release build, every compatible test and Dell option on, and the same set with the SSD installer in place of the AHCI write hooks. For `.cpp` files the object code also had to stay byte-identical in all three. Internal headers were pruned the same way, in dependency order, with syntax-only checks. They keep the headers that declare what they use, rather than whichever header happened to pull them in.

## Edits the split requires

1. **`static` dropped** on 260 handlers, 79 helper functions and 13 variables that are now shared between files. Their parameter continuation lines are re-indented by 7 columns so they still align with the opening parenthesis. The variables become global `B`/`D`/`R` symbols. One of them is `g_syscallTable`, which is now external so that `syscall_strace.cpp` can read it.
2. **Default arguments.** These move from shared `static` definitions to the header declarations, because C++ allows a default to be given only once. `.inc` fragments keep theirs, because host tests include the fragments directly. The exception is `NetlinkReadUser`, whose default moves to `syscall_netlink.h` because `syscall_rw.cpp` relies on it.
3. **Forward declarations** of now-shared functions lose `static`.
4. **DRM types leave the anonymous namespace.** `DrmCtx`, `DrmGemObj`, `DrmResource`, `DrmGpuOwner`, `DrmPrimeObj` and the DRM sysfs helpers were in `namespace {` and are used by other files. They are now `brook::` types in `syscall_drm.h`, which changes the mangled names of the functions that use them.
5. **Type definitions leave three `.inc` fragments**, because their objects are polled, closed or inspected elsewhere. `pipe_poll_types.inc` takes `PipePollNode` and `PipePollGroup`. `named_fifo_types.inc` takes `NamedFifoWait`, `NamedFifoState` and `NamedFifoOpen`. `syscall_netlink_types.inc` takes `NetlinkPacket`, `NetlinkSocket`, `NetlinkWait`, `NetlinkWaitGroup` and `NetlinkVector`. Each original fragment includes its types fragment, so host tests that include a fragment directly are unchanged. `NetlinkRef` and `NetlinkUnref` lose `static`, because `NetlinkWaitGroup`'s inline methods call them.
6. **New file banners** state each file's responsibility and keep the Enkel OS MIT attribution.

## Static initialisation order

Order does not matter. `syscall.cpp` had no dynamic initialisers: its object has no `.init_array`, and neither do any of the new objects. Every global is constant- or zero-initialised. Separately, the kernel never runs `.init_array` at all. Three other objects (`net.cpp`, `apic.cpp` and `serial_writer.cpp`) emit initialisers that only zero `.bss`, so this is harmless today. It is recorded as an issue.

## Codegen effects (not behaviour)

- Calls between files can no longer be inlined, because there is no LTO. The affected helpers are small (`WakeProcess`, `ClockNowNs`, `HasPendingSignals`, `StrEq` and the like). Helpers that were `static inline` moved into headers and still inline.
- Smaller translation units change some inlining decisions. A few static helpers were inlined away (`TimerFdWakeLocked`, `UnixListenWakeEpoll`), and a few header inlines gained per-file out-of-line copies (`MfdLock`, `strncmp`, `PipeBufferDestroy`).
- `IrqSpinLockAcquire` records `__FILE__` and `__LINE__` for lock-holder diagnostics, so those strings now name the new file and line.
- `UNIX_LIFE` records `__builtin_return_address(0)` of the function that expands it, so the diagnostic "caller" values in `unixlife` traces can differ where inlining changed.

## File map

Line counts are for the finished split. Every `syscall_*.cpp` file includes `syscall_internal.h` where it uses shared helpers, the area headers whose items it uses, and `syscall_handlers.h` if it defines or calls handlers.

- **`syscall.cpp`** (761 lines; header `syscall_dispatch.h` (13 lines)): SYSCALL entry/return assembly, dispatch table and dispatcher, not-implemented stubs, user-mode entry and return.
  - Handlers (2): not_implemented, enosys_quiet.
  - Helpers (10): SyscallDispatchC, SyscallSnapshotCurrentC, BrookSyscallDispatcher, SyscallTableInit, SyscallGetTable, SyscallGetTableAddress, SyscallGetEntryPoint, SyscallDispatchInternal, SwitchToUserMode, ReturnToKernel.
- **`syscall_common.cpp`** (101 lines; header `syscall_internal.h` (107 lines)): shared syscall helpers: errno values, signal-pending checks, wakeups, fd pin guard, random.
  - Helpers (8): SoftRandU64, RdrandU64, HasPendingSignals, HasReadInterruptSignal, WakeProcess, VnodeRef, VnodeUnref, StrEq.
- **`syscall_signal.cpp`** (439 lines; header `syscall_signal.h` (13 lines)): signal syscalls: sigaction/mask/pending/suspend/timedwait, kill family, sigaltstack, sigreturn, signalfd.
  - Handlers (14): alarm, pause, rt_sigsuspend, rt_sigtimedwait, rt_sigaction, rt_sigpending, rt_sigprocmask, signalfd4, signalfd, sigaltstack, rt_sigreturn, tgkill, tkill, kill.
  - Helpers (3): InstallWaitSignalMask, SyscallTakeWaitSignalMask, SendThreadSignal.
- **`syscall_eventfd.cpp`** (60 lines; header `syscall_eventfd.h` (58 lines)): eventfd objects and eventfd2.
  - Handlers (1): eventfd2.
  - Helpers (5): EventFdInit, EventFdReady, EventFdRef, EventFdUnref, EventFdHandleRef.
- **`syscall_timerfd.cpp`** (150 lines; header `syscall_timerfd.h` (99 lines)): timerfd objects and timerfd_create/settime/gettime.
  - Handlers (3): timerfd_create, timerfd_settime, timerfd_gettime.
  - Helpers (7): TimerFdReady, TimerFdDeadline, TimerFdRef, TimerFdUnref, TimerFdWakeLocked, TimerFdHandleRef, TimerFdSnapshotLocked.
- **`syscall_memfd.cpp`** (312 lines; header `syscall_memfd.h` (85 lines)): memfd objects, memfd_create and memfd-backed named shared memory.
  - Handlers (1): memfd_create.
  - Helpers (19): NextMemFdInode, MfdLock, MfdUnlock, MemFdGrowLocked, MemFdGrow, MemFdGetOrAllocPage, MemFdRef, MemFdUnref, MemFdReleaseMapping, MemFdInstall, MemFdReadAt, MemFdWriteAt, MemFdHandleRef, MemFdHandleUnref, MemFdHandleStat, NamedShmValidate, NamedShmOpen, NamedShmUnlink, SyscallConfigureMemfdTrace.
- **`syscall_dsp.cpp`** (57 lines; header `syscall_dsp.h` (80 lines)): OSS /dev/dsp descriptor state and format helpers.
  - Helpers (4): DspResetStreamLocked, DspRecoverStreamLocked, DspHandleRef, DspChangeFormat.
- **`syscall_pipe.cpp`** (104 lines; header `syscall_pipe.h` (44 lines)): pipes and named FIFOs: pipe/pipe2, pipe end release and wakeups.
  - Handlers (2): pipe, pipe2.
  - Helpers (13): PipeWakeEpollRaw, PipeReleaseEnd, PipeWakeEpoll, CreatePipe, PipePollNotify, NamedFifoWake, NamedFifoClose, NamedFifoAttach, FdPipe, FdNamedFifo, FdPipeRead, FdPipeWrite, NamedFifoReady.
- **`syscall_unix_socket.cpp`** (459 lines; header `syscall_unix_socket.h` (150 lines)): AF_UNIX socket objects: registry, queues, stream I/O and readiness.
  - Helpers (24): UnixCurrentCredentials, UnixCurrentIncarnation, UnixStreamTryWrite, UnixStreamTryRead, UnixFdQueueCount, UnixFdQueueFull, UnixLifeControl, UnixLife, UnixLifeHeap, UnixLifeClaim, UnixLifePublished, UnixFindServer, UnixRegisterServer, UnixSocketHandleRef, UnixFdQueueUnref, UnixDiscardIncoming, UnixPendingRelease, UnixSocketHandleUnref, UnixInstallSocket, UnixShutdownDirection, UnixSocketReady, UnixListenWakeEpoll, UnixSocketWriteSome, UnixSocketReadSome.
- **`syscall_maintenance_block.cpp`** (134 lines; header `syscall_maintenance_block.h` (40 lines)): maintenance-only block device descriptors (write-test and installer builds).
  - Helpers (6): MaintenanceBlockReserve, MaintenanceBlockRelease, MaintenanceBlockDevice, MaintenanceBlockPath, MaintenanceBlockRange, MaintenanceBlockTransfer.
- **`syscall_drm.cpp`** (1095 lines; header `syscall_drm.h` (121 lines)): DRM render-node emulation: sysfs shim, GEM/PRIME objects, virtio-gpu and i915 ioctls, sync files.
  - Helpers (26): SyncFileRef, SyncFileUnref, DrmSysAttrContent, DrmSysIsDir, IntelDrmSysAttrContent, IntelDrmSysIsDir, DrmOwnerRef, DrmOwnerUnref, DrmCaptureComplete, DrmRetirementWorker, DrmEnsureRetirementWorker, DrmResourceLastReference, DrmResourceCreate, DrmPrimeRef, DrmPrimeUnref, DrmGemAlloc, DrmGemAllocImported, DrmGemDrop, DrmGemGet, DrmBackingDiagnosticsRead, DrmFillStr, DrmEnsureBacking, CreateCompletedSyncFile, CreateIntelSyncFile, DrmRenderIoctl, IntelRenderIoctl.
- **`syscall_memory.cpp`** (632 lines; header `syscall_memory.h` (36 lines)): memory syscalls: mmap/munmap/mprotect/mremap/brk/madvise/mincore and mapping faults.
  - Handlers (10): brk, mmap, mprotect, msync, munmap, mremap, mincore, madvise, fadvise64, mbind.
  - Helpers (11): MemFdHandleUserFault, PageCachePreload, ResolveMappingFaultForSpace, ResolveUserMappingFault, FileMapHandleUserFault, ReleaseFileMapping, PrepareAnonymousMapping, PrepareFileMapping, PrepareDeviceMapping, ReleaseDeviceMapping, PrepareUserRemap.
- **`syscall_time.cpp`** (216 lines; header `syscall_time.h` (20 lines)): clock, sleep and POSIX timer syscalls.
  - Handlers (11): clock_gettime, clock_getres, gettimeofday, time, nanosleep, clock_nanosleep, timer_create, timer_settime, timer_gettime, timer_getoverrun, timer_delete.
  - Helpers (1): ClockNowNs.
- **`syscall_poll.cpp`** (641 lines; header `syscall_poll.h` (23 lines)): poll/ppoll/select/pselect6 and shared wait-deadline helpers.
  - Handlers (4): poll, ppoll, select, pselect6.
  - Helpers (6): WaitDeadlineNs, WaitWakeupTick, PollWait, ParseWaitTimeout, PollUser, SelectWait.
- **`syscall_epoll.cpp`** (655 lines; header `syscall_epoll.h` (55 lines)): epoll instances and epoll_create/ctl/wait.
  - Handlers (5): epoll_create, epoll_create1, epoll_ctl, epoll_wait, epoll_pwait.
  - Helpers (14): EpollFdRef, EpollFdUnref, EpollFdHandleRef, EpollLeafReady, EpollDiagEnable, EpollDiagEnabled, EpollDiagTargetAvailable, EpollDiagRead, EpollDiagFor, EpollDiagAdd, EpollScanReady, EpollPollReady, epoll_create_impl, epoll_wait_impl.
- **`syscall_fd.cpp`** (639 lines; header `syscall_fd.h` (18 lines)): descriptor table syscalls: close/close_range/dup/lseek/fcntl and descriptor teardown.
  - Handlers (7): close, close_range, dup, dup2, dup3, lseek, fcntl.
  - Helpers (10): DevKlogDeepCopy, DevTtyDeepCopy, FinalizeClosedFd, CloseProcessFd, FdReleaseOwnedEntry, CloseProcessFdForced, FdBumpRefcount, FdRetainCopy, DuplicateFd, FileRecordLock.
- **`syscall_open.cpp`** (593 lines; header `syscall_open.h` (97 lines)): open/openat/creat path resolution and descriptor creation.
  - Handlers (3): open, creat, openat.
  - Helpers (2): OpenProcMemFd, OpenAtInternal.
- **`syscall_rw.cpp`** (1348 lines; header `syscall_rw.h` (67 lines)): read/write family: read/write/readv/writev/pread64/pwrite64/sendfile.
  - Handlers (7): write, read, readv, writev, sendfile, pwrite64, pread64.
  - Helpers (9): IsTerminalFd, AppendUserBuffer, WriteToPinnedFdByType, WriteToFdByType, ReceiveTcpToUser, PipeReadSome, UnixRecordVectorIo, PipeVectorIo, UnixStreamWriteVector.
- **`syscall_stat.cpp`** (800 lines): stat family, statx, statfs, access/faccessat.
  - Handlers (10): access, stat, lstat, fstat, newfstatat, statx, statfs, fstatfs, faccessat, faccessat2.
  - Helpers (10): AccessAt, FillStat, BusyboxStatFallback, DrmStatFill, DrmAccessResult, do_stat_internal, do_lstat_internal, FstatKernel, FillCurrentDirectoryStat, CopySpaceResult.
- **`syscall_dir.cpp`** (110 lines): directory listing: getdents64.
  - Handlers (1): getdents64.
- **`syscall_path.cpp`** (401 lines): path namespace syscalls: cwd, mkdir, unlink, rename, link, symlink, readlink, umask.
  - Handlers (20): getcwd, readlink, readlinkat, symlink, symlinkat, umask, chdir, fchdir, rmdir, unlink, rename, mkdir, mknodat, mknod, mkdirat, unlinkat, renameat, renameat2, linkat, link.
  - Helpers (4): DoReadlink, DoUnlink, DoRename, DoMkdir.
- **`syscall_file_ops.cpp`** (317 lines): per-file operations: truncate, fallocate, flock, chmod/chown, utimensat, sync/fsync.
  - Handlers (14): sync, fsync, ftruncate, truncate, flock, chmod, fchmod, fchmodat, chown, fchown, lchown, fchownat, utimensat, fallocate.
  - Helpers (1): ChownAt.
- **`syscall_mount.cpp`** (19 lines): mount namespace syscalls: mount/umount2/pivot_root.
  - Handlers (3): mount, umount2, pivot_root.
- **`syscall_ioctl.cpp`** (887 lines): ioctl dispatch for terminals, framebuffer, OSS, DRM and sockets.
  - Handlers (1): ioctl.
- **`syscall_socket.cpp`** (1601 lines; header `syscall_socket.h` (13 lines)): socket syscalls (inet, AF_UNIX and netlink), SCM_RIGHTS and msghdr I/O.
  - Handlers (18): socket, bind, connect, sendto, recvfrom, setsockopt, getsockopt, getsockname, getpeername, shutdown, listen, accept, accept4, socketpair, sendmsg, recvmsg, sendmmsg, recvmmsg.
  - Helpers (20): GetSockIdx, UnixSocketName, AcceptSocket, CmsgAlign, UnixFdSnapFrom, UnixFdSnapInstall, UnixFdSnapRelease, NetlinkGetIdentity, NetlinkRef, NetlinkUnref, NetlinkReady, NetlinkAvailable, NetlinkBind, NetlinkDestination, NetlinkCopyName, NetlinkSend, NetlinkReceive, NetlinkVectorIo, NetlinkReadUser, NetlinkInterfaceIoctl.
- **`syscall_process.cpp`** (647 lines): process lifecycle syscalls: fork/clone/exit/wait, ids, process groups, arch_prctl, prctl.
  - Handlers (20): arch_prctl, exit, exit_group, getpid, getppid, fork, clone, vfork, clone3, wait4, waitid, set_tid_address, getpgrp, getpgid, setpgid, getsid, setsid, gettid, prctl, rseq.
  - Helpers (3): CopyWaitUsage, ChildWaitInterrupted, WaitChildEvent.
- **`syscall_exec.cpp`** (526 lines): execve.
  - Handlers (1): execve.
  - Helpers (1): CopyExecVector.
- **`syscall_sched.cpp`** (137 lines): scheduler syscalls: yield, affinity, policy, priority, membarrier.
  - Handlers (12): sched_yield, membarrier, sched_getaffinity, sched_setaffinity, sched_setparam, sched_getparam, sched_setscheduler, sched_getscheduler, sched_get_priority_max, sched_get_priority_min, getpriority, setpriority.
- **`syscall_futex.cpp`** (668 lines): futex, PI futexes and robust lists.
  - Handlers (3): set_robust_list, get_robust_list, futex.
  - Helpers (26): FutexAllocWaiter, FutexFreeWaiter, FutexLog, FutexDumpWaiters, FutexHash, FutexWake, FutexRemoveWaiter, FutexRearmWaiter, FutexCancelProcess, FutexTimeoutDeadline, FutexMappedKey, FutexResolveKey, RobustEnsurePage, RobustRead, RobustWord, RobustFutexCleanup, PiId, PiFutexQueued, PiPin, PiSyncPins, PiOrdinaryQueued, PiGraphError, PiFutexCancelProcess, PiMappedWord, PiFutexOperation, PiFutexOwnerExit.
- **`syscall_cred.cpp`** (212 lines): credential syscalls: uid/gid, groups, capabilities.
  - Handlers (16): getuid, getgid, geteuid, getegid, setuid, setgid, setreuid, setregid, setresuid, setresgid, capget, capset, getgroups, setgroups, getresuid, getresgid.
  - Helpers (5): ChangeIds, EmptyCapabilitiesSupported, CapabilityWords, SortGroupIds, GetResIds.
- **`syscall_sysinfo.cpp`** (247 lines): system information syscalls: uname, sysinfo, getrusage, times, rlimits, getrandom.
  - Handlers (7): uname, prlimit64, getrandom, getrusage, sysinfo, times, getrlimit.
  - Helpers (2): StrCopyN, ReportedResourceLimit.
- **`syscall_sysv_ipc.cpp`** (76 lines): System V semaphore syscalls.
  - Handlers (4): semget, semop, semtimedop, semctl.
  - Helpers (1): SemUserOperation.
- **`syscall_brook_system.cpp`** (243 lines): Brook system syscalls: profiler, crash entry, input, system messages, paging, audio, AHCI export.
  - Handlers (9): brook_system_message, brook_pageable_memory, brook_profile, brook_set_crash_entry, brook_crash_complete, brook_input_pop, brook_input_grab, brook_audio_control, brook_ahci_export.
- **`syscall_brook_wm.cpp`** (647 lines): Brook window-manager syscalls.
  - Handlers (35): brook_wm_create_window, brook_wm_destroy_window, brook_wm_signal_dirty, brook_wm_publish_source_commit, brook_wm_begin_source_write, brook_wm_query_source_commit, brook_wm_present_gres, brook_wm_place_window, brook_wm_window_state, brook_wm_refresh_apps, brook_wm_theme, brook_wm_appearance, brook_pointer_speed, brook_wm_taskbar_metrics, brook_wm_scale, brook_wm_set_client_socket, brook_wm_set_app_id, brook_wm_set_title, brook_wm_pointer_capture, brook_wm_pop_input, brook_wm_resize_vfb, brook_wm_set_decoration_mode, brook_wm_set_window_properties, brook_wm_begin_move, brook_wm_move_relative, brook_wm_set_maximized, brook_wm_set_minimized, brook_wm_begin_resize, brook_wm_set_cursor_visible, brook_wm_set_cursor_image, brook_wm_get_screen_info, brook_wm_scanout_map, brook_wm_scanout_flip, brook_wm_get_windows, brook_wm_map_window_vfb.
- **`syscall_brook_gpu.cpp`** (408 lines): Brook GPU syscalls: output buffers, handoff control, window validation, per-process virgl.
  - Handlers (13): brook_gpu_output_alloc, brook_gpu_buffer_try_cpu_read, brook_gpu_buffer_snapshot_fence, brook_gpu_handoff_ctl, brook_gpu_window_validate, brook_gpu_window_peer_recovery, brook_gpu_ctx_create, brook_gpu_resource_create, brook_gpu_attach_window, brook_gpu_upload_buffer, brook_gpu_submit, brook_gpu_transfer, brook_gpu_ctx_destroy.
- **`syscall_brook_desktop.cpp`** (500 lines): Brook desktop syscalls: layer capture, scene graph, debug frame, overlay.
  - Handlers (5): brook_desktop_layers, brook_scene_create, brook_scene_ctl, brook_debug_frame, brook_desktop_overlay.
  - Helpers (2): CaptureDesktopChrome, CaptureDesktopBackdrop.
- **`syscall_strace.cpp`** (341 lines; header `syscall_strace.h` (16 lines)): syscall tracing (strace) and its controls.
  - Helpers (6): SyscallCategory, SyscallName, SyscallDispatchTraced, StraceEnablePid, StraceEnableName, StraceEnableAll.

Internal headers (none are public kernel API):

- `syscall_internal.h`: errno constants, profiling counters, `KernelOperationGuard`, `FdRef`, `VnodeRef`/`VnodeUnref` and the common helpers.
- `syscall_handlers.h`: one prototype for each `sys_*` handler, grouped by file.
- `syscall_<area>.h`: only what that area shares (types, constants and declarations). `syscall_netlink.h` and `syscall_pipe.h` wrap the `*_types.inc` fragments.

## Commit sequence

Each step is one commit. Each commit builds Release, passes the host tests that compile kernel sources, and is compile-checked in all three option configurations.

1. This plan.
2. Prep: move the shared type definitions out of `pipe_poll.inc`, `named_fifo.inc` and `syscall_netlink.inc` into `*_types.inc` fragments (item 5 above).
3. `syscall_common.cpp`: shared syscall helpers: errno values, signal-pending checks, wakeups, fd pin guard, random.
4. `syscall_signal.cpp`: signal syscalls: sigaction/mask/pending/suspend/timedwait, kill family, sigaltstack, sigreturn, signalfd.
5. `syscall_eventfd.cpp`: eventfd objects and eventfd2.
6. `syscall_timerfd.cpp`: timerfd objects and timerfd_create/settime/gettime.
7. `syscall_memfd.cpp`: memfd objects, memfd_create and memfd-backed named shared memory.
8. `syscall_dsp.cpp`: OSS /dev/dsp descriptor state and format helpers.
9. `syscall_pipe.cpp`: pipes and named FIFOs: pipe/pipe2, pipe end release and wakeups.
10. `syscall_unix_socket.cpp`: AF_UNIX socket objects: registry, queues, stream I/O and readiness.
11. `syscall_maintenance_block.cpp`: maintenance-only block device descriptors (write-test and installer builds).
12. `syscall_drm.cpp`: DRM render-node emulation: sysfs shim, GEM/PRIME objects, virtio-gpu and i915 ioctls, sync files.
13. `syscall_memory.cpp`: memory syscalls: mmap/munmap/mprotect/mremap/brk/madvise/mincore and mapping faults.
14. `syscall_time.cpp`: clock, sleep and POSIX timer syscalls.
15. `syscall_poll.cpp`: poll/ppoll/select/pselect6 and shared wait-deadline helpers.
16. `syscall_epoll.cpp`: epoll instances and epoll_create/ctl/wait.
17. `syscall_fd.cpp`: descriptor table syscalls: close/close_range/dup/lseek/fcntl and descriptor teardown.
18. `syscall_open.cpp`: open/openat/creat path resolution and descriptor creation.
19. `syscall_rw.cpp`: read/write family: read/write/readv/writev/pread64/pwrite64/sendfile.
20. `syscall_stat.cpp`: stat family, statx, statfs, access/faccessat.
21. `syscall_dir.cpp`: directory listing: getdents64.
22. `syscall_path.cpp`: path namespace syscalls: cwd, mkdir, unlink, rename, link, symlink, readlink, umask.
23. `syscall_file_ops.cpp`: per-file operations: truncate, fallocate, flock, chmod/chown, utimensat, sync/fsync.
24. `syscall_mount.cpp`: mount namespace syscalls: mount/umount2/pivot_root.
25. `syscall_ioctl.cpp`: ioctl dispatch for terminals, framebuffer, OSS, DRM and sockets.
26. `syscall_socket.cpp` (with `syscall_netlink.h` for the netlink fragment): socket syscalls (inet, AF_UNIX and netlink), SCM_RIGHTS and msghdr I/O.
27. `syscall_process.cpp`: process lifecycle syscalls: fork/clone/exit/wait, ids, process groups, arch_prctl, prctl.
28. `syscall_exec.cpp`: execve.
29. `syscall_sched.cpp`: scheduler syscalls: yield, affinity, policy, priority, membarrier.
30. `syscall_futex.cpp`: futex, PI futexes and robust lists.
31. `syscall_cred.cpp`: credential syscalls: uid/gid, groups, capabilities.
32. `syscall_sysinfo.cpp`: system information syscalls: uname, sysinfo, getrusage, times, rlimits, getrandom.
33. `syscall_sysv_ipc.cpp`: System V semaphore syscalls.
34. `syscall_brook_system.cpp`: Brook system syscalls: profiler, crash entry, input, system messages, paging, audio, AHCI export.
35. `syscall_brook_wm.cpp`: Brook window-manager syscalls.
36. `syscall_brook_gpu.cpp`: Brook GPU syscalls: output buffers, handoff control, window validation, per-process virgl.
37. `syscall_brook_desktop.cpp`: Brook desktop syscalls: layer capture, scene graph, debug frame, overlay.
38. `syscall_strace.cpp`: syscall tracing (strace) and its controls.
39. Prune the residual `syscall.cpp` include list to what dispatch needs.

## Old line ranges → new location (for rebasing other branches)

The ranges refer to `src/kernel/src/syscall.cpp` at `aac1d429`. Each range includes the comment block above its first item. "→ header" means the definition moved verbatim into that header.

To rebase a branch that edits `syscall.cpp`:

1. Find the hunk's old line range below.
2. Apply it to the new file. Usually only `static` differs: shared definitions lost it, and header prototypes carry any default arguments.
3. If the change adds a new cross-file use of a `static` helper, drop `static` and declare it in that area's header, as the split did.

| Old lines | Destination | First … last item |
|---|---|---|
| 1–130 | `syscall.cpp (includes; copied to each new file, then pruned)` | ReturnToKernel … CopyUserCString |
| 131–159 | `syscall_common.cpp` | SoftRandU64 … RdrandU64 |
| 160–377 | `syscall.cpp` | SyscallDispatchInternal … BrookSyscallDispatcher |
| 378–385 | `syscall_common.cpp` | g_profOpenCount … g_profMmapCount |
| 386–386 | `syscall_internal.h` | g_profFaultCount |
| 387–389 | `syscall_common.cpp` | g_profCacheHit … g_profPreloadPages |
| 395–419 | `syscall_internal.h` | ENOENT … ELOOP |
| 420–460 | `syscall_signal.cpp` | WaitSignalMask … SyscallTakeWaitSignalMask |
| 461–496 | `syscall_common.cpp` | HasPendingSignals … WakeProcess |
| 497–498 | `syscall_pipe.cpp` |  |
| 499–501 | `syscall_drm.cpp` | SyncFileRef … SyncFileUnref |
| 502–514 | `syscall_eventfd.h` | EventFdData |
| 515–522 | `syscall_eventfd.cpp` | g_eventFdIdentity … EventFdInit |
| 523–531 | `syscall_eventfd.h` | EventFdGuard … EventFdSnapshot |
| 532–538 | `syscall_eventfd.cpp` | EventFdReady |
| 539–539 | `syscall_eventfd.h` | EFD_SEMAPHORE |
| 540–540 | `syscall_eventfd.cpp` | EFD_CLOEXEC |
| 541–541 | `syscall_eventfd.h` | EFD_NONBLOCK |
| 542–548 | `syscall_timerfd.cpp` | TFD_NONBLOCK … WaitWakeupTick |
| 549–562 | `syscall_timerfd.h` | TimerWaitNode … TimerFdGuard |
| 563–579 | `syscall_timerfd.cpp` | TimerFdReady … TimerFdDeadline |
| 580–589 | `syscall_epoll.cpp` | EpollEvent |
| 590–615 | `syscall_epoll.h` | EPOLL_MAX_FDS … EpollInstance |
| 616–619 | `syscall_memfd.cpp` | MFD_CLOEXEC … MFD_ALLOW_SEALING |
| 620–640 | `syscall_memfd.h` | MemFdData |
| 641–652 | `syscall_memfd.cpp` | NextMemFdInode |
| 653–665 | `syscall_memfd.h` | MfdLock … MfdUnlock |
| 666–725 | `syscall_memfd.cpp` | MemFdGrowLocked … MemFdGetOrAllocPage |
| 726–745 | `syscall_memfd.h` | MemFdRef … MemFdUnref |
| 746–753 | `syscall_memfd.cpp` | MemFdReleaseMapping |
| 754–759 | `syscall_internal.h` | KernelOperationGuard |
| 760–785 | `syscall_brook_system.cpp` | sys_brook_system_message … sys_brook_pageable_memory |
| 786–853 | `syscall_memfd.cpp` | MemFdInstall … MemFdWriteAt |
| 854–879 | `syscall_internal.h` | VnodeRef … VnodeUnref |
| 880–885 | `syscall_memory.cpp` | MemFdHandleUserFault … FileMapHandleUserFault |
| 886–904 | `syscall_memfd.cpp` | MemFdHandleRef … MemFdHandleStat |
| 905–920 | `syscall_eventfd.h` | EventFdRef … EventFdUnref |
| 921–984 | `syscall_timerfd.h` | TimerFdRef … TimerWaitGroup |
| 985–987 | `syscall_timerfd.cpp` | TimerFdWakeLocked |
| 988–1003 | `syscall_epoll.h` | EpollFdRef … EpollFdUnref |
| 1004–1005 | `syscall_eventfd.cpp` | EventFdHandleRef |
| 1006–1006 | `syscall_epoll.cpp` | EpollFdHandleRef |
| 1007–1007 | `syscall_timerfd.cpp` | TimerFdHandleRef |
| 1008–1033 | `syscall_dsp.h` | DspState … DspIoGuard |
| 1034–1054 | `syscall_dsp.cpp` | DspResetStreamLocked … DspHandleRef |
| 1055–1079 | `syscall_fd.cpp` | DevKlogDeepCopy … DevTtyDeepCopy |
| 1080–1313 | `syscall_memory.cpp` | FileMappingBacking … FileMapHandleUserFault |
| 1320–1322 | `syscall_unix_socket.h` | AF_UNIX … UNIX_SOCK_CLOEXEC |
| 1323–1323 | `syscall_unix_socket.cpp` | UNIX_MAX_SERVERS |
| 1324–1337 | `syscall_unix_socket.h` | UNIX_ACCEPT_QUEUE … UnixFdSnap |
| 1338–1348 | `syscall_unix_socket.cpp` | UnixFdSnapRelease … UnixCurrentIncarnation |
| 1349–1364 | `syscall_unix_socket.h` | UnixFdQueue |
| 1365–1486 | `syscall_unix_socket.cpp` | UnixStreamTryWrite … UnixFdQueueFull |
| 1487–1538 | `syscall_unix_socket.h` | UnixPendingConn … SockAddrUn |
| 1539–1579 | `syscall_unix_socket.cpp` | g_unixRegistryLock … UnixLifeHeap |
| 1580–1582 | `syscall_unix_socket.h` | UNIX_LIFE |
| 1583–1599 | `syscall_unix_socket.cpp` | UnixLifeClaim … g_unixNameLock |
| 1600–1603 | `syscall_unix_socket.h` | UnixNameGuard |
| 1604–1659 | `syscall_unix_socket.cpp` | UnixFindServer … UnixFdQueueUnref |
| 1660–1670 | `syscall_pipe.h` | PipeWakeEpollRaw |
| 1671–1695 | `syscall_pipe.cpp` | PipeReleaseEnd |
| 1696–1768 | `syscall_unix_socket.cpp` | UnixDiscardIncoming … UnixInstallSocket |
| 1769–1778 | `syscall_pipe.h` | PipeWakeEpoll |
| 1779–1817 | `syscall_unix_socket.cpp` | UnixShutdownDirection … UnixSocketReady |
| 1818–1829 | `syscall_unix_socket.h` | UnixListenWakeEpoll |
| 1835–1865 | `syscall_dsp.h` | SNDCTL_DSP_RESET … DSP_BUFFER_SIZE |
| 1866–1883 | `syscall_dsp.cpp` | DspChangeFormat |
| 1884–1906 | `syscall_internal.h` | FdRef |
| 1907–1984 | `syscall_unix_socket.cpp` | UnixSocketStage … UnixSocketReadSome |
| 1985–1994 | `syscall_rw.cpp` | IsTerminalFd |
| 1995–2042 | `syscall_open.cpp` | OpenProcMemFd |
| 2043–2052 | `syscall_rw.cpp` | AppendStageLimit |
| 2053–2055 | `syscall_rw.h` | VnodeWriteLimit |
| 2057–2065 | `syscall_maintenance_block.h` | MaintenanceBlockFd |
| 2066–2179 | `syscall_maintenance_block.cpp` | maintenanceBlockLock … MaintenanceBlockTransfer |
| 2181–2192 | `syscall_rw.cpp` | AppendUserBuffer |
| 2193–2194 | `syscall_socket.cpp` |  |
| 2195–3156 | `syscall_rw.cpp` | WriteToPinnedFdByType … sys_read |
| 3157–3167 | `syscall_common.cpp` | StrEq |
| 3168–3230 | `syscall_memfd.cpp` | NamedShmEntry … NamedShmUnlink |
| 3231–3274 | `syscall_drm.cpp` | DrmSysAttr … g_drmDriDirTag |
| 3275–3276 | `syscall_drm.h` | DRM_RENDER_RDEV |
| 3277–3293 | `syscall_drm.cpp` | DrmSysAttrContent … IntelDrmSysIsDir |
| 3294–3308 | `syscall_drm.h` | DRM_MAX_GEM |
| 3309–3310 | `syscall_drm.cpp` | g_drmMapTokens … g_drmBackingDiagnostics |
| 3311–3313 | `syscall_drm.h` | DrmGpuOwner |
| 3314–3322 | `syscall_drm.cpp` | DrmOwnerRef … DrmOwnerUnref |
| 3323–3338 | `syscall_drm.h` | DrmResource |
| 3339–3418 | `syscall_drm.cpp` | DrmCaptureComplete … DrmResourceCreate |
| 3419–3445 | `syscall_drm.h` | DrmGemObj … DrmMappingGuard |
| 3446–3488 | `syscall_drm.cpp` | DrmGemAlloc … DrmBackingDiagnosticsRead |
| 3489–3569 | `syscall_open.h` | AtLookup |
| 3570–4083 | `syscall_open.cpp` | OpenAtInternal … sys_creat |
| 4084–4285 | `syscall_fd.cpp` | FinalizeClosedFd … sys_close_range |
| 4286–4334 | `syscall_pipe.cpp` | CreatePipe … sys_pipe |
| 4335–4564 | `syscall_fd.cpp` | FdBumpRefcount … sys_lseek |
| 4565–4573 | `syscall_memory.cpp` | sys_brk |
| 4579–4594 | `syscall_memory.h` | MmapFlags … MmapProt |
| 4595–4602 | `syscall_memory.cpp` | DeviceMappingBacking |
| 4603–4606 | `syscall_memory.h` | MappingBackingGuard |
| 4607–4860 | `syscall_memory.cpp` | ReleaseFileMapping … sys_mremap |
| 4861–4974 | `syscall_process.cpp` | ARCH_SET_GS … sys_exit_group |
| 4980–5024 | `syscall_rw.h` | iovec … UnixRecordVector |
| 5025–5203 | `syscall_rw.cpp` | UnixRecordVectorIo … sys_writev |
| 5204–5261 | `syscall_sysv_ipc.cpp` | sys_semget … sys_semctl |
| 5262–5296 | `syscall_sysinfo.cpp` | utsname … sys_uname |
| 5297–5663 | `syscall_process.cpp` | sys_getpid … sys_waitid |
| 5664–6170 | `syscall_exec.cpp` | CopyExecVector … sys_execve |
| 6171–6183 | `syscall_process.cpp` | sys_set_tid_address |
| 6189–6194 | `syscall_time.h` | timespec |
| 6195–6262 | `syscall_time.cpp` | timeval … sys_time |
| 6263–6296 | `syscall_sched.cpp` | sys_sched_yield … sys_membarrier |
| 6297–6422 | `syscall_time.cpp` | sys_nanosleep … sys_timer_delete |
| 6423–6550 | `syscall_signal.cpp` | sys_alarm … sys_rt_sigtimedwait |
| 6551–6613 | `syscall_stat.cpp` | BusyboxStatFallback … sys_access |
| 6614–6672 | `syscall_ioctl.cpp` | FBIOGET_VSCREENINFO … FbFixScreeninfo |
| 6691–6692 | `syscall_drm.h` | DRM_IOCTL_TYPE |
| 6693–6883 | `syscall_drm.cpp` | DRM_NR_VERSION … CreateIntelSyncFile |
| 6884–6893 | `syscall_drm.h` | SyncFilePublicationPin |
| 6894–7535 | `syscall_drm.cpp` | DrmRenderIoctl … IntelRenderIoctl |
| 7536–8330 | `syscall_ioctl.cpp` | sys_ioctl |
| 8331–8857 | `syscall_stat.cpp` | LinuxStat … sys_newfstatat |
| 8858–8954 | `syscall_dir.cpp` | LinuxDirent64 … sys_getdents64 |
| 8955–8960 | `syscall_file_ops.cpp` | sys_sync |
| 8961–8961 | `syscall_mount.cpp` |  |
| 8962–8984 | `syscall_file_ops.cpp` | sys_fsync |
| 8985–9160 | `syscall_cred.cpp` | sys_getuid … sys_setgroups |
| 9161–9241 | `syscall_signal.cpp` | sys_rt_sigaction … sys_rt_sigprocmask |
| 9242–9300 | `syscall_sysinfo.cpp` | rlimit64 … sys_getrandom |
| 9301–9309 | `syscall_open.cpp` | sys_openat |
| 9310–9323 | `syscall_path.cpp` | sys_getcwd |
| 9324–9336 | `syscall_fd.cpp` | F_DUPFD … FD_CLOEXEC |
| 9337–9340 | `syscall_fd.h` | O_NONBLOCK … O_CLOEXEC |
| 9341–9479 | `syscall_fd.cpp` | LinuxFlock … sys_fcntl |
| 9485–9491 | `syscall_poll.h` | pollfd … POLLIN |
| 9492–10012 | `syscall_poll.cpp` | POLLPRI … sys_ppoll |
| 10013–10161 | `syscall_path.cpp` | DoReadlink … sys_symlinkat |
| 10162–10173 | `syscall_pipe.cpp` | sys_pipe2 |
| 10174–10193 | `syscall_signal.cpp` | sys_signalfd4 … sys_signalfd |
| 10194–10222 | `syscall_eventfd.cpp` | sys_eventfd2 |
| 10223–10839 | `syscall_epoll.cpp` | EPOLLIN … sys_epoll_pwait |
| 10840–10946 | `syscall_timerfd.cpp` | CLOCK_REALTIME … sys_timerfd_gettime |
| 10947–11007 | `syscall_memfd.cpp` | memfdTraceEnabled … sys_memfd_create |
| 11008–11055 | `syscall_rw.cpp` | sys_sendfile |
| 11056–11182 | `syscall_sysinfo.cpp` | sys_getrusage … sys_times |
| 11183–11393 | `syscall_path.cpp` | sys_umask … sys_linkat |
| 11394–11595 | `syscall_file_ops.cpp` | sys_ftruncate … sys_utimensat |
| 11596–11670 | `syscall_sched.cpp` | sys_sched_getaffinity … sys_sched_get_priority_min |
| 11671–11781 | `syscall_stat.cpp` | linux_statx_timestamp … sys_statx |
| 11782–11848 | `syscall_file_ops.cpp` | sys_fallocate |
| 11849–11928 | `syscall_memory.cpp` | sys_mincore … sys_fadvise64 |
| 11929–11943 | `syscall_sched.cpp` | sys_getpriority … sys_setpriority |
| 11944–11954 | `syscall_memory.cpp` | sys_mbind |
| 11955–12003 | `syscall_rw.cpp` | sys_pwrite64 |
| 12004–12013 | `syscall_path.cpp` | sys_link |
| 12014–12083 | `syscall_signal.cpp` | SS_ONSTACK … sys_rt_sigreturn |
| 12084–12160 | `syscall_process.cpp` | sys_getpgrp … sys_gettid |
| 12161–12237 | `syscall_signal.cpp` | SendThreadSignal … sys_kill |
| 12238–12249 | `syscall_sysinfo.cpp` | sys_getrlimit |
| 12250–12308 | `syscall_stat.cpp` | LinuxStatFs … sys_fstatfs |
| 12309–12400 | `syscall_poll.cpp` | SelectWait … sys_pselect6 |
| 12401–12561 | `syscall_brook_system.cpp` | sys_brook_profile … sys_brook_input_grab |
| 12562–12740 | `syscall_brook_wm.cpp` | BrookWmCreateOut … sys_brook_wm_window_state |
| 12741–12931 | `syscall_brook_gpu.cpp` | sys_brook_gpu_output_alloc … sys_brook_gpu_handoff_ctl |
| 12932–13261 | `syscall_brook_desktop.cpp` | g_chromeCaptureStats … sys_brook_scene_ctl |
| 13262–13288 | `syscall_brook_wm.cpp` | sys_brook_wm_refresh_apps … sys_brook_wm_appearance |
| 13289–13297 | `syscall_brook_system.cpp` | sys_brook_audio_control |
| 13298–13311 | `syscall_brook_wm.cpp` | sys_brook_pointer_speed … sys_brook_wm_taskbar_metrics |
| 13312–13457 | `syscall_brook_desktop.cpp` | sys_brook_debug_frame … sys_brook_desktop_overlay |
| 13458–13484 | `syscall_brook_system.cpp` | sys_brook_ahci_export |
| 13485–13524 | `syscall_brook_wm.cpp` | sys_brook_wm_scale … sys_brook_wm_set_client_socket |
| 13525–13581 | `syscall_brook_gpu.cpp` | sys_brook_gpu_window_validate … sys_brook_gpu_window_peer_recovery |
| 13582–13723 | `syscall_brook_wm.cpp` | sys_brook_wm_set_app_id … sys_brook_wm_set_window_properties |
| 13724–13855 | `syscall_brook_gpu.cpp` | sys_brook_gpu_ctx_create … sys_brook_gpu_ctx_destroy |
| 13856–14078 | `syscall_brook_wm.cpp` | sys_brook_wm_begin_move … sys_brook_wm_map_window_vfb |
| 14079–14109 | `syscall.cpp` | sys_not_implemented … sys_enosys_quiet |
| 14110–14130 | `syscall_cred.cpp` | GetResIds … sys_getresgid |
| 14131–14168 | `syscall_rw.cpp` | sys_pread64 |
| 14169–14213 | `syscall_process.cpp` | sys_prctl |
| 14214–14229 | `syscall_stat.cpp` | sys_faccessat … sys_faccessat2 |
| 14230–14255 | `syscall_futex.cpp` | sys_set_robust_list … sys_get_robust_list |
| 14256–14266 | `syscall_process.cpp` | sys_rseq |
| 14267–14885 | `syscall_futex.cpp` | FUTEX_WAIT … sys_futex |
| 14886–16454 | `syscall_socket.cpp` | sys_socket … sys_recvmmsg |
| 16455–16801 | `syscall.cpp` | g_syscallTable … SyscallGetEntryPoint |
| 16802–17079 | `syscall_strace.cpp` | StraceCategory … SyscallDispatchTraced |
| 17080–17174 | `syscall.cpp` | SyscallDispatchInternal … SwitchToUserMode |
| 17177–17226 | `syscall_strace.cpp` | StraceEnablePid … StraceEnableAll |
| 17227–17259 | `syscall.cpp` | ReturnToKernel |

## Verification plan

1. **Symbols.** `nm -C --defined-only` on `BROOK.elf` before and after. Expect only:
   - local→global changes for the shared items;
   - the DRM anonymous-namespace renames;
   - inlining-driven appearance or disappearance of static helpers.
2. **Dispatch table.** Decode `SyscallTableInit` in both ELFs into (slot → function symbol) pairs, and require them to be identical.
3. **Text.** Compare normalised lines of the original against the new files (ignoring `static`, includes and generated declarations). Use `git diff --color-moved=zebra` to inspect the moves.
4. **Builds.** Release and Debug builds; host tests; the Dell image build with `BROOK_BDW_SCANOUT_FLIP`, `BROOK_BDW_PERSISTENT_SCANOUT_MAP`, `BROOK_BDW_ZERO_COPY_SCANOUT` and `BROOK_BDW_VIDEO_ENGINE`; and its `--qemu-check` boot to the desktop.
