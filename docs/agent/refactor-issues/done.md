# Done: fixed entries

Entries from the syscall-split issue log that the fix pass (branch `work/syscall-fixes`) and later branches fixed. They are copied verbatim, with their original IDs and **Status** lines, grouped by fix commit (oldest first; entries fixed across several commits come last). The open entries are in the chunk files, see [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md).

Entries that are only partly fixed (M-64, L-60) or hardened (H-04) stay in their chunk with the done part noted in the Status line.

Note on H-26: the sync-file and pipe POLLOUT parts are fixed. The single-slot waiters and the 10 ms tick that remain are tracked by M-64, L-60 and the "Wakeups" section of [07-wait-and-wakeup.md](07-wait-and-wakeup.md).

## `b61a0f73`: raw user pointers in input/WM, getdents64, open and keyboard read

### H-01 · Unvalidated user output pointers written directly (arbitrary kernel write)

- **Where:** `src/kernel/src/syscall_brook_system.cpp:156`, in `sys_brook_input_pop`.
- **Severity:** high (confidence high).
- **Problem:** 4 sites write through raw user pointers with no range check: input_pop (up to 256x16 bytes); sys_brook_wm_create_window and sys_brook_wm_resize_vfb (`auto* uo = reinterpret_cast<BrookWmCreateOut*>(outPtr);`); sys_brook_wm_pop_input (WmInputPop writes `out[produced++]`). There is no SMAP, so a kernel address is accepted and written with partly caller-controlled data. An unmapped address faults in kernel mode. On a failed write, events are lost or the new window leaks.
- **Suggested fix:** Build the result in a kernel buffer, then CopyToUser. For pop_input, pop into a local array (WmInputPopOwned). Roll back window create/resize if the copy fails.
- **Test:** Host test per handler with a kernel-half address and an unmapped address: -EFAULT and no kernel write. QEMU guest passing 0xffff800000000000 to syscalls 504, 506, 510 and 511.
- **Status (fix pass):** fixed in `b61a0f73`. Test: host test in `test_syscall_user_memory` (input_wm.cpp).

### H-02 · Window title read byte by byte from an unvalidated user pointer (kernel memory disclosure)

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:302`, in `sys_brook_wm_set_title`.
- **Severity:** high (confidence high).
- **Problem:** 2 sites (set_title, and create_window's `char c = utp[i];`). A kernel address supplied as the title copies kernel memory into a title that WM_GET_WINDOWS or the chrome then shows. An unmapped pointer faults the kernel. set_app_id already does this correctly with CopyUserCString.
- **Suggested fix:** Use CopyUserCString(titlePtr, buf, sizeof(buf)) and return -EFAULT, through one shared helper.
- **Test:** Host test: a kernel pointer and an unmapped title pointer both give -EFAULT; titles of 63, 64 and 65 bytes are terminated correctly.
- **Status (fix pass):** fixed in `b61a0f73`. Test: host test in `test_syscall_user_memory` (input_wm.cpp).

### H-13 · Cooked-tty Ctrl+C writes to an unvalidated user pointer when count==0

- **Where:** `src/kernel/src/syscall_rw.cpp:790`, in `sys_read (DevKeyboard cooked)`.
- **Severity:** high (confidence high).
- **Problem:** UserBufferWritable is skipped for count==0, but the Ctrl+C branch still stores buf[0] and returns 1. read(fd, kernel address, 0) followed by Ctrl+C writes 0x0A into the kernel, and the call returns more than count.
- **Suggested fix:** Return 0 for count==0 and use CopyToUser.
- **Test:** QEMU: read(tty, 0xffff800000001000, 0) with an injected Ctrl+C returns 0.
- **Status (fix pass):** fixed in `b61a0f73`. Test: host test in `test_syscall_user_memory` (rw.cpp).

### H-17 · getdents64 writes directory entries to an unchecked user address

- **Where:** `src/kernel/src/syscall_dir.cpp:67`, in `sys_getdents64`.
- **Severity:** high (confidence high).
- **Problem:** Neither the vnode branch nor the /dev/dri branch (`auto* obuf = reinterpret_cast<uint8_t*>(bufAddr);`) validates bufAddr or count. User-named dirents are written straight to that address, so a kernel address corrupts the kernel and an unmapped one faults in kernel mode.
- **Suggested fix:** Kernel bounce buffer plus CopyToUser, as sys_fstat does.
- **Test:** QEMU: getdents64 with a kernel, NULL, unmapped or straddling buffer returns -EFAULT and does not panic.
- **Status (fix pass):** fixed in `b61a0f73`. Test: host test in `test_syscall_user_memory` (dir_open.cpp).

### H-18 · open/openat dereference a kernel-half path pointer supplied by the user

- **Where:** `src/kernel/src/syscall_open.cpp:94`, in `OpenAtInternal`.
- **Severity:** high (confidence high).
- **Problem:** Any upper-half address is used as the path, letting a user read kernel memory or trigger a page-fault DoS. No internal kernel callers exist.
- **Suggested fix:** Remove the kernel-pointer branch and return -EFAULT. If needed later, add a separate OpenAtKernel().
- **Test:** QEMU: open((char*)0xffff800000000000,0) returns -EFAULT.
- **Status (fix pass):** fixed in `b61a0f73`. Test: host test in `test_syscall_user_memory` (dir_open.cpp).

## `06a10c04`: inet sockets

### H-14 · Inet socket paths pass raw, unvalidated user pointers to the net stack or write through them (arbitrary kernel read/write)

- **Where:** `src/kernel/src/syscall_socket.cpp:446`, in `sys_recvfrom`.
- **Severity:** high (confidence high).
- **Problem:** About 9 sites do no range check at all, so a kernel address is accepted. SockRecvFrom writes the datagram and source address directly, and a fault leaves the RX tail half-advanced. Others: sys_bind (`auto* addr = reinterpret_cast<const SockAddrIn*>(addrVal);`, which leaks through getsockname), sys_sendto UDP, sys_recvmsg UDP (`void* buf = msg->msg_iov[0].iov_base;`, `*dst = srcAddr;`), getsockname/getpeername inet (`__builtin_memcpy(uaddr, &local, copyLen);`), AcceptSocket inet (`*userAddr = peerAddr;`, which ignores addrlen and overflows) and AcceptSocket AF_UNIX (an addrlen store guarded only by >=0x1000).
- **Suggested fix:** Stage every address and payload in the kernel; CopyFromUser into a SockAddrIn (check sin_family); CopyToUser min(*addrlen, actual) plus the length; user-aware UDP variants.
- **Test:** Host test with stubbed user_copy rejecting kernel-range pointers: -EFAULT for bind, sendto, recvfrom, recvmsg, getsockname, getpeername and accept. QEMU probe with 0xffff800000000000.
- **Status (fix pass):** fixed in `06a10c04`. Test: host test in `test_syscall_user_memory` (socket.cpp, with a fake network stack).

## `ae328f7a`: abstract AF_UNIX names

### H-15 · Abstract AF_UNIX names not distinguished; every abstract connect matches the first abstract listener

- **Where:** `src/kernel/src/syscall_unix_socket.cpp:222`, in `UnixFindServer`.
- **Severity:** high (confidence high).
- **Problem:** bind and connect copy the name with `for (int i = 0; i < 107 && path[i]; ++i)`, which stops at the leading NUL, so every abstract path is "". A connect to "@foo" can reach "@bar": wrong peer, and a security problem. There is no EADDRINUSE for abstract names, sun_path is cut to 107 bytes, and addrlen==2 binds an empty name rather than autobinding.
- **Suggested fix:** memcmp the full abstract address with its length; enforce uniqueness under g_unixNameLock; implement autobind.
- **Test:** Host: listeners "\0a" and "\0b"; connecting to "\0b" pins b; a second bind of "\0a" gives EADDRINUSE.
- **Status (fix pass):** fixed in `ae328f7a` (also EADDRINUSE and Linux autobind). Test: host test in `test_syscall_user_memory` (unix.cpp).

## `6881a138`: clone TLS, vfork, child TID

### H-07 · vfork parent can block forever (lost wakeup); parent pointer unowned

- **Where:** `src/kernel/src/syscall_process.cpp:306`, in `sys_clone`.
- **Severity:** high (confidence high).
- **Problem:** vforkParent is set after SchedulerAddProcess, and an unconditional [PROFILE] fork print widens the window. A child that exits or execs first never wakes the parent. SchedulerBlock returns on any wake. The raw pointer holds no reference, so a killed parent leaves the child unblocking a freed task.
- **Suggested fix:** Publish a referenced vforkParent before SchedulerAddProcess; wait on a completion flag in a loop; use one release helper for exit, exit_group and exec.
- **Test:** QEMU test_vfork_race: 10k vfork iterations with immediate _exit, and exec-ENOENT then _exit.
- **Status (fix pass):** fixed in `6881a138`. Test: host test in `test_syscall_user_memory` (process.cpp). The old tree does not build against the test (new release helper); the publication-order assertion encodes the bug.

### H-23 · CLONE_SETTLS accepts a non-canonical or kernel FS base (wrmsr #GP in context switch)

- **Where:** `src/kernel/src/syscall_process.cpp:246`, in `sys_clone`.
- **Severity:** high (confidence medium).
- **Problem:** arch_prctl validates the base but clone does not, and context_switch.S wrmsrs it: any user can crash the kernel. context_switch.S also skips the FS write when fsBase==0, so a task inherits the previous FS base after ARCH_SET_FS 0.
- **Suggested fix:** ValidUserSegmentBase() in clone and clone3; always write FS base.
- **Test:** QEMU: clone with tls=0x8000000000000000 gives -EPERM and the kernel survives.
- **Status (fix pass):** fixed in `6881a138` (clone/clone3 validate the base; context_switch.S always writes FS). Test: host test in `test_syscall_user_memory` (process.cpp). The context-switch half is kernel-only; the QEMU check boots it.

### H-25 · CLONE_CHILD_SETTID never implemented; CHILD_CLEARTID ignored for fork-style clones

- **Where:** `src/kernel/src/syscall_process.cpp:252`, in `sys_clone`.
- **Severity:** high (confidence medium).
- **Problem:** glibc _Fork relies on CHILD_SETTID, so a forked child's TCB tid stays the parent's. PI unlock then gives EPERM, and robust-list cleanup skips the child's mutexes. PARENT_SETTID is honoured only with CLONE_THREAD.
- **Suggested fix:** Write the child TID into the child's address space before it runs; honour CLEARTID and PARENT_SETTID for forks.
- **Test:** QEMU test_fork_tid with a PI mutex.
- **Status (fix pass):** fixed in `6881a138`. Test: host test in `test_syscall_user_memory` (process.cpp records the TID addresses; the ForkChildTrampoline store is kernel-only).

## `cd53b5ac`: read on O_WRONLY

### H-12 · read()/pread64() on an O_WRONLY regular file returns its contents

- **Where:** `src/kernel/src/syscall_rw.cpp:526`, in `sys_read, sys_pread64`.
- **Severity:** high (confidence high).
- **Problem:** The Vnode branches never check (statusFlags&3)==1, while the MemFd, BlockDevice and sendfile paths do. A mode-0200 file opened write-only can be read, bypassing permissions. Linux returns EBADF.
- **Suggested fix:** Return -EBADF for write-only descriptors; use a shared FdReadable/FdWritable helper.
- **Test:** QEMU: create a mode-0200 file, open O_WRONLY; read and pread give EBADF.
- **Status (fix pass):** fixed in `cd53b5ac`. Test: host test in `test_syscall_user_memory` (rw.cpp).

## `4ef02e1f`: no user copies under spinlocks

### H-11 · User memory copied while a spinlock is held (pipe ring, memfd, serial)

- **Where:** `src/kernel/src/syscall_rw.cpp:463` (snippet matches 2 lines: 463, 996), in `sys_read / WriteToPinnedFdByType`.
- **Severity:** high (confidence high).
- **Problem:** PipeBuffer::read/write memcpy under pipe->lock, and several callers hand them raw user pointers: /dev/tty read and write, pipe and FIFO write via pipe_write.inc, and AF_UNIX stream read and write. MemFdReadAt and MemFdWriteAt memcpy under MfdLock (a spinlock). DebugSerial and DevKeyboard write `buf[i]` under SerialLock. About 10 sites. A raced munmap, mprotect or fork-CoW faults inside a spinlock, giving a panic or deadlock.
- **Suggested fix:** Stage through bounded kernel buffers with Copy*User outside the lock, as PipeReadSome and ReceiveTcpToUser already do.
- **Test:** Host test_pipe_write asserting the callback never sees a user address; QEMU munmap and fork race on pipe, tty and memfd writes.
- **Status (fix pass):** fixed in `4ef02e1f`. Test: host test in `test_syscall_user_memory` (memfd.cpp: no lock held across any user copy) and `test_pipe_write` (CopyFromUser staging, faulting source).

### H-16 · AF_UNIX stream I/O copies raw user memory while holding the pipe spinlock

- **Where:** `src/kernel/src/syscall_unix_socket.cpp:67`, in `UnixStreamTryWrite`.
- **Severity:** high (confidence high).
- **Problem:** Only SOCK_SEQPACKET is staged. For streams, user pointers from sendto, sendmsg, recvmsg, read and write reach memcpy under pipe->lock (UnixStreamTryRead too), so a demand fault sleeps under a spinlock and a raced munmap oopses the kernel.
- **Suggested fix:** Stage stream payloads in bounded kernel chunks; copy outside the lock.
- **Test:** QEMU: socketpair send/recv while a sibling munmaps the buffer.
- **Status (fix pass):** fixed in `4ef02e1f`. Test: host test in `test_syscall_user_memory` (memfd.cpp, AF_UNIX stream case).

### H-24 · memfd read/write memcpy to/from user buffer under the memfd spinlock (self-deadlock via MAP_SHARED fault)

- **Where:** `src/kernel/src/syscall_memfd.cpp:128`, in `MemFdReadAt / MemFdWriteAt`.
- **Severity:** high (confidence medium).
- **Problem:** read(fd, its own unfaulted MAP_SHARED mapping) faults into MemFdGetOrAllocPage, which takes MfdLock again and deadlocks. A raced munmap faults under the spinlock. Large writes call PmmAllocPage repeatedly under the lock.
- **Suggested fix:** Resolve pages under the lock, then copy unlocked (or bounce).
- **Test:** QEMU test_memfd_self_read.
- **Status (fix pass):** fixed in `4ef02e1f`. Test: host test in `test_syscall_user_memory` (memfd.cpp, including a fault back into the memfd during the copy).

## `d9c2bafa`: /dev/tty pins

### H-27 · /dev/tty open borrows stdin/stdout pipe handles without pinning them

- **Where:** `src/kernel/src/syscall_open.cpp:235`, in `OpenAtInternal`.
- **Severity:** high (confidence medium).
- **Problem:** fds 0 and 1 are read without FdRef or a lock, and the refcounts are bumped after publishing. A sibling's close can free the PipeBuffer first (use-after-free) or underflow the counts.
- **Suggested fix:** Pin fds 0 and 1, retain both ends, then publish with FdTablePublishOwned.
- **Test:** QEMU: open("/dev/tty") racing dup2 and close on fds 0 and 1; check pipe counts afterwards.
- **Status (fix pass):** fixed in `d9c2bafa` (also honours O_CLOEXEC, part of M-45). Test: host test in `test_syscall_user_memory` (dir_open.cpp).

## `863f802a`: sync-file, pipe and eventfd wakeups

### H-26 · No wakeup registration for SyncFile, SignalFd, pipe POLLOUT or nested epoll; waits rely on the 50/100 ms safety tick

- **Where:** `src/kernel/src/syscall_poll.cpp:354`, in `PollWait`.
- **Severity:** high (confidence medium).
- **Problem:** Readiness for these types is only noticed at the next safety tick (50 ms in poll; 100 ms in epoll: `uint64_t safetyMs = timerWaiters.allocationFailed ? 5 : 100;`). A missed GPU fence wake costs several frames at 60 fps.
- **Suggested fix:** Waiter groups for BdwGpuEvent (sync_file), signalfd, pipe writerWaiter and nested epoll; meanwhile lower safetyMs for these types.
- **Test:** QEMU: poll and epoll on a sync_file signalled after 2 ms wake within 5 ms.
- **Status (fix pass):** fixed in `863f802a` for sync files (GPU Retire event, or a 1 ms tick while prompt wakes are off) and pipe POLLOUT. Partly refuted: signal delivery already wakes the target thread even for a masked signal, and epoll flattens nested epolls onto their leaves. What remains (signals aimed at another thread, poll() on an epoll fd) now uses a 10 ms tick. Test: host test in `test_syscall_user_memory` (wait.cpp).

### M-18 · Single-slot waiter fields overwritten; lost wakeups

- **Where:** `src/kernel/src/syscall_poll.cpp:372`, in `PollWait`.
- **Severity:** medium (confidence high).
- **Problem:** poll overwrites the eventfd readerWaiter that a blocked read() uses (even for a POLLOUT-only poll). The epollWaiter slots are shared too. With two or more waiters, the last one wins and the others fall back to the timeout. No lock is held.
- **Suggested fix:** Per-object waiter lists like PipePollGroup.
- **Test:** Two pollers and a reader on one eventfd: one write wakes all within 1 ms.
- **Status (fix pass):** fixed in `863f802a`: eventfds keep a waiter list (EventFdWaitGroup); epoll uses pipe poll subscriptions for pipes and AF_UNIX rx/tx. Test: host test in `test_syscall_user_memory` (wait.cpp).

## `196cc80d`: validate-then-dereference in process, time, cred, poll and stat

### H-09 · User memory validated then dereferenced directly (37 sites in poll/cred/time/signal/sysinfo/sched/epoll)

- **Where:** `src/kernel/src/syscall_poll.cpp:523`, in `PollUser`.
- **Severity:** high (confidence high).
- **Problem:** 37 sites: poll 9 (ParseWaitTimeout, PollUser in and out, SelectWait, the pselect6 argument), cred 9, time 8 (nanosleep, clock_nanosleep, timer_*), signal 5, sysinfo 5, sched 2 and epoll_ctl 1. A raced munmap faults the kernel (a user-triggerable DoS). timer_create writes the id after the timer exists, with no failure path, so the timer leaks. The UserAccessScope in clock_gettime is redundant.
- **Suggested fix:** CopyFromUser/CopyToUser everywhere; delete the timer when the id copy fails.
- **Test:** QEMU munmap race on poll, select, nanosleep rem, getgroups and getresuid gives EFAULT.
- **Status (fix pass):** fixed in `196cc80d` (all 37 sites; timer_create deletes the timer when the id copy fails). Test: host test in `test_syscall_user_memory` (race.cpp: raced-unmap mode).

### H-10 · sched_getparam checks readability then writes

- **Where:** `src/kernel/src/syscall_sched.cpp:90`, in `sys_sched_getparam`.
- **Severity:** high (confidence high).
- **Problem:** A read-only page passes the check and is then written, which faults or (with WP off) modifies a read-only or CoW page. NULL or a bad pointer returns 0.
- **Suggested fix:** CopyToUser; -EINVAL for NULL and -EFAULT on failure.
- **Test:** sched_getparam on a PROT_READ page gives -EFAULT.
- **Status (fix pass):** fixed in `196cc80d` (NULL is -EINVAL, a failed store -EFAULT). Test: host test in `test_syscall_user_memory` (race.cpp: read-only page).

### H-19 · User memory dereferenced directly after a validate-only check (stat, statx, F_GETLK, utimensat)

- **Where:** `src/kernel/src/syscall_stat.cpp:278` (snippet matches 2 lines: 278, 328), in `do_stat_internal`.
- **Severity:** high (confidence high).
- **Problem:** 5 sites: do_stat_internal and do_lstat_internal, sys_statx (`auto* stx = reinterpret_cast<linux_statx*>(bufAddr);`), FileRecordLock in syscall_fd.cpp, and sys_utimensat (`UserTime value = userTimes[i];`). Each writes during VFS work, so a raced munmap faults the kernel.
- **Suggested fix:** Fill kernel-local structs and use Copy*User.
- **Test:** QEMU munmap race against stat, statx, F_GETLK and utimensat.
- **Status (fix pass):** fixed in `196cc80d`. Test: covered by the checked-copy rule; no dedicated host case (stat needs the VFS lookups).

### M-23 · rt_sigprocmask writes oldset before reading set and validating how

- **Where:** `src/kernel/src/syscall_signal.cpp:242`, in `sys_rt_sigprocmask`.
- **Severity:** medium (confidence high).
- **Problem:** When set and oldset alias, SIG_BLOCK is a no-op. An invalid how still clobbers oldset.
- **Suggested fix:** Copy set in, validate how, then write oldset.
- **Test:** sigprocmask(SIG_BLOCK,&s,&s); how=7 leaves oldset untouched.
- **Status (fix pass):** fixed in `196cc80d`. Test: host test in `test_syscall_user_memory` (race.cpp).

## `c5a6cae6`: ioctl and virtio DRM

### H-05 · GET_CAPS zero-fills a user buffer directly and passes the raw user pointer to the GPU driver

- **Where:** `src/kernel/src/syscall_drm.cpp:531`, in `DrmRenderIoctl (DRM_NR_VIRTGPU_GET_CAPS)`.
- **Severity:** high (confidence high).
- **Problem:** The address is only range-checked with UserBufferWritable (no UserAccessScope). The code then writes it byte by byte, and GetCapset writes up to 64 KiB into user memory from driver context. A sibling munmap or mprotect makes the kernel fault.
- **Suggested fix:** kmalloc and zero a kernel buffer, GetCapset into it, then CopyToUser.
- **Test:** QEMU virgl race test (GET_CAPS while a sibling munmaps); host test: an unmapped address gives -EFAULT.
- **Status (fix pass):** fixed in `c5a6cae6`. Test: host test in `test_syscall_user_memory` (ioctl.cpp: the fake driver must receive a kernel buffer).

### H-06 · Direct user-pointer dereference after range validation only (repeated pattern)

- **Where:** `src/kernel/src/syscall_ioctl.cpp:269`, in `DrmRenderIoctl, sys_ioctl`. Also in: syscall_drm.cpp, syscall_ioctl.cpp.
- **Severity:** high (confidence high).
- **Problem:** About 30 sites: 13 more in DRM (VERSION, GET_CAP, GETPARAM, RESOURCE_CREATE/INFO, TRANSFER, EXECBUFFER, PRIME, GEM_CLOSE, MAP) and 16 in ioctl (FIONBIO, FIONREAD, FBIO*, termios/TIOC*, TCGETS2). Examples: `DrmVirtgpuExecbuffer eb = *reinterpret_cast<DrmVirtgpuExecbuffer*>(arg);` and `auto* info = reinterpret_cast<FbVarScreeninfo*>(arg);`. A concurrent munmap makes the kernel fault, and partial in-place writes leave torn output.
- **Suggested fix:** CopyFromUser into a kernel local, fill it, and CopyToUser once. Use one termios/winsize helper.
- **Test:** Host test per ioctl family with an unmapped or read-only arg giving -EFAULT; QEMU munmap race against TCGETS, FBIOGET and RESOURCE_CREATE.
- **Status (fix pass):** fixed in `c5a6cae6` (also L-09: one set of termios/pgrp/winsize helpers). Test: host test in `test_syscall_user_memory` (ioctl.cpp, including the raced-unmap mode).

### L-09 · termios/TIOC handling duplicated (keyboard vs tty, and TCGETS2)

- **Where:** `src/kernel/src/syscall_ioctl.cpp:400`, in `sys_ioctl`.
- **Severity:** low (confidence high).
- **Problem:** TCGETS, TCSETS*, TIOCG/SPGRP and TIOCGWINSZ are implemented twice and the c_cc table three times; the copies already diverge.
- **Suggested fix:** TtyFillTermios() and TtyIoctl() helpers.
- **Test:** Host test: byte-compare TCGETS from the keyboard and a tty; TCGETS2 equals TCGETS plus speeds.
- **Status (fix pass):** fixed in `c5a6cae6`. Test: host test in `test_syscall_user_memory` (ioctl.cpp).

## `6adbe3bc`: shared open file descriptions (branch `work/open-file`)

### H-20 · dup() and fork() copies get their own file offset and status flags (no shared open file description)

- **Where:** `src/kernel/src/syscall_fd.cpp:362`, in `DuplicateFd`.
- **Severity:** high (confidence high).
- **Problem:** seekPos and statusFlags are copied per slot. With `cmd >f 2>&1` and `{ a; b; } >f`, writers overwrite each other, and F_SETFL O_NONBLOCK on one alias doesn't affect the others.
- **Suggested fix:** A refcounted open-file-description object holding the offset and flags; lock the offset read-modify-write.
- **Test:** QEMU: dup then write "a" and "b" gives "ab"; same for fork.
- **Status (`work/open-file`):** fixed in `6adbe3bc`: refcounted `OpenFile` (offset, status flags, sleeping `posLock`) shared by dup, dup2/dup3, F_DUPFD, fork and SCM_RIGHTS; design as built in [03-file-descriptions-and-fd-table.md](03-file-descriptions-and-fd-table.md). Test: `test_fd_table`, `test_syscall_user_memory` (rw.cpp, socket.cpp) and QEMU `scripts/test_open_file_qemu.py` (dup "ab", fork offsets, child F_SETFL O_NONBLOCK reaching the parent, BusyBox `cmd >f 2>&1` and `{ a; b; } >f`).
- **Regression (`work/pipe-poll-uaf`):** `6adbe3bc` changed `MemFdInstall` to take the descriptor's memfd reference before publishing and drop it on failure, but `memfd_create` still created the object with refcount 0 and freed it itself on failure. Every `memfd_create` that failed with EMFILE freed the `MemFdData` twice; when another CPU reused the 64-byte block in between, the second `kfree` released a live object. That is the Steam gen 415 panic (`PipePollGroup::~PipePollGroup` walking a freed `PipePollNode`). Fixed by giving `memfd_create` its own reference (refcount 1, dropped after the install); `pipe()`/`socketpair()` now publish both ends atomically, `accept()` no longer allocates under the registry lock, and pipes/poll nodes carry live markers. Test: `test_syscall_user_memory` (memfd.cpp `MemfdCreateAtFullTable` fails on the old code) and QEMU `scripts/test_pipe_poll_stress_qemu.py` (old code: double frees logged and a trap on a freed pipe; fixed: pass).

### M-45 · Publish-then-mutate fd allocation ignores O_CLOEXEC and other open flags (11 branches)

- **Where:** `src/kernel/src/syscall_open.cpp:175`, in `OpenAtInternal`.
- **Severity:** medium (confidence high).
- **Problem:** Affected branches: O_PATH, fb0, keyboard, tty, renderD128, /dev/dri, DRM sysfs, synthetic /etc and klog. Flags are set after FdAlloc publishes the fd, or not at all, so fds leak across exec. open("/etc/hostname", O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC) succeeds.
- **Suggested fix:** One FdEntry builder plus FdTablePublishOwned; reject write and O_EXCL on read-only synthetic nodes.
- **Test:** QEMU: per synthetic path, F_GETFD after O_CLOEXEC; O_WRONLY on /etc/passwd fails.
- **Status (`work/open-file`):** fixed in `6adbe3bc`: every branch publishes through `FdAlloc`/`FdPublishNew` with status and FD_CLOEXEC preset; read-only synthetic files refuse write, O_TRUNC and O_CREAT|O_EXCL (EACCES/EEXIST), `/dev/dri` refuses write (EISDIR). Test: `test_syscall_user_memory` (dir_open.cpp keyboard fallback).

### M-50 · F_GETFL reports open-only flags; F_SETFL accepts any bit

- **Where:** `src/kernel/src/syscall_fd.cpp:626`, in `sys_fcntl`.
- **Severity:** medium (confidence high).
- **Problem:** F_GETFL returns O_CREAT, O_EXCL, O_TRUNC and O_CLOEXEC. F_SETFL can set O_PATH or O_TRUNC.
- **Suggested fix:** Strip open-only flags at open; apply only the settable mask in F_SETFL.
- **Test:** QEMU: F_GETFL after open(O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC) equals O_RDWR.
- **Status (`work/open-file`):** fixed in `6adbe3bc`: call-only flags are stripped when the description is created and F_SETFL applies only Linux SETFL_MASK. Test: `test_fd_table`, QEMU `scripts/test_open_file_qemu.py`.

### L-57 · timerfd_create and epoll_create publish the fd and then patch its flags

- **Where:** `src/kernel/src/syscall_timerfd.cpp:84`, in `sys_timerfd_create`.
- **Severity:** low (confidence medium).
- **Problem:** A sibling fork+exec can see the fd without CLOEXEC.
- **Suggested fix:** FdTablePublishOwned with the flags preset (as eventfd2 and signalfd4 do).
- **Test:** Fork+exec race never inherits a TFD_CLOEXEC fd.
- **Status (`work/open-file`):** fixed in `6adbe3bc`: timerfd_create and epoll_create publish with O_NONBLOCK/FD_CLOEXEC preset.

### L-59 · CreatePipe publishes descriptors before setting endpoint flags

- **Where:** `src/kernel/src/syscall_pipe.cpp:74`, in `CreatePipe`.
- **Severity:** low (confidence medium).
- **Problem:** The flags, status flags and CLOEXEC are written after FdAlloc and without the fd lock, so a sibling's dup or fork can see a write end with flags=0 or no CLOEXEC. Every error is flattened to EMFILE.
- **Suggested fix:** Publish the entry with its flags preset (FdTablePublishOwned).
- **Test:** Host: FdAlloc with initial flags.
- **Status (`work/open-file`):** fixed in `6adbe3bc`: both pipe ends publish complete with flags, status and FD_CLOEXEC; the real error is returned. Test: QEMU `scripts/test_open_file_qemu.py` (pipe ends are separate descriptions).

### L-63 · lseek succeeds on sockets, ttys, keyboard and epoll fds

- **Where:** `src/kernel/src/syscall_fd.cpp:456`, in `sys_lseek`.
- **Severity:** low (confidence medium).
- **Problem:** Should be ESPIPE. A VfsStat error becomes EINVAL, and seekPos is updated unlocked.
- **Suggested fix:** A seekable-type allowlist; propagate errors.
- **Test:** QEMU: lseek on socketpair, tty and epoll gives ESPIPE.
- **Status (`work/open-file`):** fixed in `6adbe3bc`: ESPIPE for sockets (inet, unix, netlink), /dev/tty, keyboard and serial stdio; eventfd/timerfd/epoll/signalfd return 0 as Linux noop_llseek does (not ESPIPE as suggested); VfsStat errors propagate; the offset is updated under the description's posLock. Test: QEMU `scripts/test_open_file_qemu.py`.

## Several commits: `b61a0f73`, `06a10c04`

### M-27 · Raw dereference of user buffers (non-fault-safe) in device read paths

- **Where:** `src/kernel/src/syscall_rw.cpp:554`, in `sys_read`.
- **Severity:** medium (confidence high).
- **Problem:** About 9 sites: urandom, klog, SyntheticMem and the keyboard modes. UDP read and write go through SockRecvFrom and SockSendTo byte loops, so a fault mid-datagram corrupts the RX ring. The urandom and klog `>= 0x0000800000000000ULL` checks are dead.
- **Suggested fix:** Kernel staging buffers plus Copy*User; user-aware UDP variants.
- **Test:** QEMU: munmap race during urandom, klog, SyntheticMem and UDP reads gives EFAULT.
- **Status (fix pass):** fixed in `b61a0f73` (keyboard, urandom, klog, synthetic files) and `06a10c04` (UDP read/write). Test: host test in `test_syscall_user_memory` (rw.cpp, socket.cpp).

## Several commits: `6881a138`, `196cc80d`

### H-08 · Direct user-pointer dereference after validation only (exec vectors, clone3, clone tid, wait4/waitid, arch_prctl, mincore)

- **Where:** `src/kernel/src/syscall_exec.cpp:40`, in `CopyExecVector`.
- **Severity:** high (confidence high).
- **Problem:** 11 sites. Exec slots and strings; clone3 `__builtin_memcpy(&copied,reinterpret_cast<const void*>(argsPtr)`; clone parent tid; wait4 status (`*reinterpret_cast<int32_t*>(statusAddr) = event.status`, after the child is already reaped); CopyWaitUsage; waitid; arch_prctl GET_FS/GET_GS; mincore `vec[i] = 1`. A raced munmap gives a kernel #PF.
- **Suggested fix:** Copy*User throughout.
- **Test:** QEMU test_usercopy_race; host CopyExecVector test.
- **Status (fix pass):** fixed: arch_prctl GET_FS/GS in `6881a138`, the rest (exec vectors, clone3, wait4/waitid, rusage, mincore) in `196cc80d`. Test: covered by the checked-copy rule; no dedicated host case for exec/wait (they need the scheduler); clone3 is exercised by process.cpp.
