# Wait and wakeup correctness (poll, epoll, eventfd, futex)

_15 open entries (10 medium, 5 low). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** Remaining single-slot waiters, epoll semantics (fd-keyed entries, starvation, `EPOLLONESHOT`/`EPOLLET`, locking and allocation cost), `poll` fd re-resolution, futex requeue, `select` details, and eventfd write limits.
- **Why it matters:** Directly relevant to the 60 fps goal: any waiter that falls back to the 10 ms safety tick costs frames, and epoll allocating about 78 KB per walk (M-60) burns time on every wait. H-26, M-18 and the sync-file/pipe/eventfd wakeups are fixed.
- **Dependencies:** M-19 and M-58 should be redone on top of H-20 (03) if that lands first; otherwise key by the fd's handle identity. M-20 (duplicated readiness code) is best done before M-21/M-22.
- **Suggested order:** M-60 (cheap, measurable), M-20, M-21, M-22, M-59, M-19/M-58, then M-64/L-60/M-61 (finish waiter lists), M-55, then the L entries.
- **Test approach:** `test_syscall_user_memory/wait.cpp` (hooks observe every wake and block), `test_pipe_poll`, `test_signal_fd`, `test_signal_wait`, `test_futex_pool`, `test_epoll_diag_guest`. Frame-pacing effects need the Dell measurements (see HANDOVER).
- **Size:** Large: 15 entries, 2 branches (epoll rework; waiter lists).

### Wakeups (H-26, M-18): what changed and what remains

Changed:

- **Sync files:** poll and epoll on an Intel sync_file arm the GPU `Retire` event, which every fence completion signals. A completion between the readiness check and the arm is reported as stale and rechecked.
- **Fallback tick:** prompt GPU wakes are off by default (`gpu wakeups on` enables them). While they are off, a pending fence polls every 1 ms tick instead of every 50 or 100 ms.
- **Pipes:** poll(POLLOUT) on a pipe write end now subscribes to the pipe.
- **Eventfds:** each eventfd keeps a waiter list, so concurrent readers, writers, pollers and epoll waiters all wake.
- **epoll:** pipes and AF_UNIX streams use pipe poll subscriptions instead of the single `epollWaiter` slots.

Remaining:

- **Single-slot waiters:** pipe and AF_UNIX `readerWaiter`/`writerWaiter` (blocking read and write), `acceptWaiter`, the listening socket's `epollWaiter` and the /dev/tty reader. Two blocked readers on one pipe still fall back to the 10 ms tick.
- **10 ms tick:** signals aimed at a different thread from the signalfd poller, and poll() on an epoll fd.

## Entries

### Medium

### M-19 · epoll entries keyed by fd number; close/reuse and fork give wrong events

- **Where:** `src/kernel/src/syscall_epoll.cpp:321`, in `EpollScanReady`.
- **Severity:** medium (confidence high).
- **Problem:** A watch outlives close(). A reused number delivers the new file's events with the old data.u64, and re-ADD gives EEXIST. An epoll fd shared across fork prunes the other process's watches.
- **Suggested fix:** Key entries by open file description and generation; remove them on close.
- **Test:** QEMU: ADD fd 5, close it, reopen as 5, epoll_wait gives no event and re-ADD succeeds.

### M-20 · poll/epoll readiness duplicated four times and drifted

- **Where:** `src/kernel/src/syscall_epoll.cpp:134`, in `EpollLeafReady`.
- **Severity:** medium (confidence high).
- **Problem:** Pipe write end in epoll: no EPOLLERR without readers, so an EPOLLOUT waiter hangs. Read end: EPOLLIN instead of EPOLLHUP, with no end check. DevKeyboard is never epoll-readable. EPOLLRDHUP is reported unrequested. poll reports HUP/ERR only when POLLIN is requested and ignores RDNORM/WRNORM.
- **Suggested fix:** One FdReadiness() and one registration helper.
- **Test:** Table-driven host test poll vs epoll; test_pipe_poll closed-reader EPOLLOUT.

### M-21 · epoll_wait starves later ready entries

- **Where:** `src/kernel/src/syscall_epoll.cpp:318`, in `EpollScanReady`.
- **Severity:** medium (confidence high).
- **Problem:** The scan always starts at entry 0, so with a small maxevents the later level-triggered entries are never returned.
- **Suggested fix:** Rotate the cursor or use a ready list.
- **Test:** 3 always-ready fds with maxevents=1 give 3 distinct fds over 3 calls.

### M-22 · EPOLLONESHOT ignored; EPOLLET only for eventfd/sync_file

- **Where:** `src/kernel/src/syscall_epoll.cpp:56`, in `EpollWalk::watchReady`.
- **Severity:** medium (confidence high).
- **Problem:** ONESHOT never disarms (except for sync files). Sockets and pipes registered with ET behave as LT.
- **Suggested fix:** Disarm ONESHOT on delivery; ET generations, or document LT.
- **Test:** ONESHOT pipe delivers once until MOD.

### M-55 · FUTEX_REQUEUE / FUTEX_CMP_REQUEUE unimplemented (ENOSYS with a debug dump)

- **Where:** `src/kernel/src/syscall_futex.cpp:659`, in `sys_futex`.
- **Severity:** medium (confidence medium).
- **Problem:** musl condvars hang with two or more waiters. The log legend still lists REQUEUE. Magic gs offsets.
- **Suggested fix:** Implement both ops; rate-limit the log.
- **Test:** Host test_futex_requeue; QEMU condvar broadcast with 4 waiters.

### M-58 · poll re-resolves fds by number per phase; registrations survive close/dup2 during sleep

- **Where:** `src/kernel/src/syscall_poll.cpp:464`, in `PollWait`.
- **Severity:** medium (confidence medium).
- **Problem:** Cleanup can't reach an object whose fd was closed while the caller slept, leaving a stale Process* that a later wake uses, guarded only by the magic check. epoll keeps its pins across the sleep and avoids this.
- **Suggested fix:** Pin objects for the whole wait.
- **Test:** QEMU: shared eventfd, fd closed during the poll, poller exits, child writes; no stale wake.

### M-59 · Global epoll graph mutex held across CopyToUser and leaf readiness checks

- **Where:** `src/kernel/src/syscall_epoll.cpp:341`, in `EpollScanReady`.
- **Severity:** medium (confidence medium).
- **Problem:** g_epollGraphLock is system-wide, so one process faulting stalls everyone's epoll and poll-on-epoll.
- **Suggested fix:** Per-instance lock; snapshot, unlock, then copy.
- **Test:** Two processes; a slow-page epoll_wait doesn't stall epoll_ctl.

### M-60 · Every epoll walk allocates and memsets about 78 KB

- **Where:** `src/kernel/src/syscall_epoll.cpp:269`, in `EpollWalkOwner::EpollWalkOwner`.
- **Severity:** medium (confidence medium).
- **Problem:** This happens per epoll_wait iteration, poll-on-epoll and epoll_ctl, plus the 1024 FdPut slots and a 24 KB clearMemo per scan, on the 60 fps hot path.
- **Suggested fix:** Clear only the touched entries; size by maxevents.
- **Test:** Cycles-per-epoll_wait perf test with 4 fds.

### M-61 · Interrupted keyboard read leaves a stale global input waiter

- **Where:** `src/kernel/src/syscall_rw.cpp:773`, in `sys_read (DevKeyboard)`.
- **Severity:** medium (confidence medium).
- **Problem:** The EINTR paths never call InputRemoveWaiter and there is no exit cleanup, so a later key press wakes an exited or reused Process. Stale entries also fill WAITER_MAX.
- **Suggested fix:** An RAII waiter guard.
- **Test:** QEMU: signal a tty reader, let it exit, press a key; no crash.

### M-64 · Stale, unlocked single-slot waiter pointers on AF_UNIX pipes

- **Where:** `src/kernel/src/syscall_unix_socket.cpp:450`, in `UnixSocketReadSome`.
- **Severity:** medium (confidence medium).
- **Problem:** The pointer is set and cleared without the lock and is not cleared after a timeout followed by success, so a peer can wake an exited Process. A second waiter overwrites the first. acceptWaiter is single-slot too.
- **Suggested fix:** Wait queues with ProcessRef.
- **Test:** Two concurrent readers both wake.
- **Status (fix pass):** partly fixed in `863f802a`: epoll no longer uses the rx/tx single slots. read/write and accept still use readerWaiter/writerWaiter/acceptWaiter.

### Low

### L-17 · epoll_wait rejects maxevents above 1024

- **Where:** `src/kernel/src/syscall_epoll.cpp:626`, in `sys_epoll_wait`.
- **Severity:** low (confidence high).
- **Problem:** Loops that use 4096 get EINVAL. epfd is validated before maxevents, the reverse of Linux.
- **Suggested fix:** Clamp the internal batch.
- **Test:** maxevents=4096 works.

### L-21 · Leftover diagnostics that user code can trigger (epoll_wait bad fd, rt_sigreturn)

- **Where:** `src/kernel/src/syscall_epoll.cpp:612`, in `sys_epoll_wait`.
- **Severity:** low (confidence high).
- **Problem:** Each epoll_wait on a bad fd prints an unconditional SerialPrintf (UART flood, interrupts off). rt_sigreturn has a similar gs:48 DbgPrintf.
- **Suggested fix:** Put them behind a debug flag.
- **Test:** 10k epoll_wait(-1) calls give bounded serial output.

### L-25 · eventfd write rejects count > 8

- **Where:** `src/kernel/src/syscall_rw.cpp:287`, in `WriteToPinnedFdByType (EventFd)`.
- **Severity:** low (confidence high).
- **Problem:** Linux accepts any count >= 8.
- **Suggested fix:** `count < 8`.
- **Test:** QEMU: write(efd, buf, 16) returns 8.

### L-56 · select rejects tv_usec >= 1e6; pselect6 doesn't write back remaining time

- **Where:** `src/kernel/src/syscall_poll.cpp:508`, in `ParseWaitTimeout`.
- **Severity:** low (confidence medium).
- **Problem:** Linux normalises tv_usec for select.
- **Suggested fix:** Normalise microseconds.
- **Test:** select {0,1500000} waits about 1.5 s.

### L-60 · /dev/tty and eventfd single-waiter fields set without a lock

- **Where:** `src/kernel/src/syscall_rw.cpp:1005`, in `sys_read (DevTty, EventFd)`.
- **Severity:** low (confidence medium).
- **Problem:** Waiters are registered outside the lock and concurrent readers overwrite each other, so wakeups fall back to the 10 ms timeout (latency that matters for the 60 fps goal).
- **Suggested fix:** Register under the lock with wait groups.
- **Test:** QEMU: two eventfd readers; one write wakes a reader within 1 ms.
- **Status (fix pass):** partly fixed in `863f802a` (eventfd). The /dev/tty readerWaiter remains.
