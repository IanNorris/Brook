# Process, clone, exec and signals

_11 open entries (8 medium, 3 low). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** `clone`/`clone3` flag validation, `setpgid`, `prctl`, `execve` limits and multi-thread exec, `kill`, signal-pending checks, id range checks, `wait4` flags, and the post-commit exec sanity check.
- **Why it matters:** Linux-compat gaps that surface in real programs (posix_spawn ENOENT is lost, `prctl` always succeeds, `kill(0)` returns ENOSYS). H-07, H-08, H-23 and H-25 in this area are already fixed.
- **Dependencies:** M-15 (execve path length) should share a `PATH_MAX` limit with M-49 in 09. L-02 and M-13 both truncate ids to 16 bits: fix them together.
- **Suggested order:** M-11 and M-12 (clone), M-13/L-02 (ids), M-24, M-14, M-15, M-62, L-16, M-54, L-69.
- **Test approach:** `test_syscall_user_memory/process.cpp` (records clone arguments; exec and wait need the scheduler so are QEMU cases), `test_signal_context`, `test_signal_wait`.
- **Size:** Medium: 11 entries, 1 or 2 sittings. M-54 and L-69 need kernel-side care (QEMU only).

## Entries

### Medium

### M-11 · clone flag-combination validation missing; requested sharing silently degrades (posix_spawn ENOENT lost)

- **Where:** `src/kernel/src/syscall_process.cpp:257`, in `sys_clone`.
- **Severity:** medium (confidence high).
- **Problem:** CLONE_VM|CLONE_VFORK becomes a full copy, so posix_spawn of a missing binary returns 0. Invalid combinations are accepted. CLONE_FILES, CLONE_PARENT, CLONE_PIDFD and the exit signal are ignored.
- **Suggested fix:** An allow-mask plus EINVAL, or implement CLONE_VM vfork sharing.
- **Test:** Host test_clone_flags; QEMU posix_spawn("/nonexistent") gives ENOENT.

### M-12 · clone3 drops exit_signal and ignores pidfd/set_tid/cgroup on the normal path; no size cap or trailing-zero check

- **Where:** `src/kernel/src/syscall_process.cpp:375`, in `sys_clone3`.
- **Severity:** medium (confidence high).
- **Problem:** Only the namespace branch validates. size is unbounded and read directly from user memory.
- **Suggested fix:** Hoist validation above the branch; pass flags|exit_signal.
- **Test:** Host test_clone3_args; QEMU: clone3 with exit_signal=SIGCHLD delivers SIGCHLD.

### M-13 · setpgid has no validation or permission checks; pid arguments truncated to uint16

- **Where:** `src/kernel/src/syscall_process.cpp:545`, in `sys_setpgid`.
- **Severity:** medium (confidence high).
- **Problem:** Any process can move any other process. 65541 truncates to 5. The pointer is unreferenced (a use-after-free window). setsid never returns EPERM for a group leader.
- **Suggested fix:** Linux rules, referenced lookups and a range check.
- **Test:** QEMU test_setpgid_rules.

### M-14 · prctl returns success for every unhandled option; PR_GET_* outputs never written

- **Where:** `src/kernel/src/syscall_process.cpp:632`, in `sys_prctl`.
- **Severity:** medium (confidence high).
- **Problem:** The comment says EINVAL but the code returns 0. GET_NAME leaves the buffer uninitialised. PDEATHSIG is ignored. Two case labels are dead.
- **Suggested fix:** Implement NAME, PDEATHSIG and DUMPABLE; allowlist the rest, EINVAL otherwise.
- **Test:** QEMU test_prctl.

### M-15 · execve path > 255 bytes returns EFAULT instead of ENAMETOOLONG; alias buffers truncate

- **Where:** `src/kernel/src/syscall_exec.cpp:73`, in `sys_execve`.
- **Severity:** medium (confidence high).
- **Problem:** tryPath[128] truncates silently and can open a different file. A raw -36 is used.
- **Suggested fix:** CopyUserCStringResult and ENAMETOOLONG.
- **Test:** QEMU: a 300-char path gives ENAMETOOLONG.

### M-24 · kill with pid <= 0 returns ENOSYS

- **Where:** `src/kernel/src/syscall_signal.cpp:417`, in `sys_kill`.
- **Severity:** medium (confidence high).
- **Problem:** kill(0), kill(-pgid) and kill(-1) are basic job control, and Linux never returns ENOSYS from kill.
- **Suggested fix:** Implement group and broadcast delivery; meanwhile return ESRCH or EPERM.
- **Test:** kill(-pgid) reaps the group.

### M-54 · execve from a non-leader thread does not kill sibling threads

- **Where:** `src/kernel/src/syscall_exec.cpp:409`, in `sys_execve`.
- **Severity:** medium (confidence medium).
- **Problem:** The old group keeps running alongside the new image (BRO-091 class).
- **Suggested fix:** Kill all siblings, leader included; the survivor adopts the TGID.
- **Test:** QEMU test_exec_from_thread.

### M-62 · Signal checks inconsistent: HasPendingSignals vs HasReadInterruptSignal

- **Where:** `src/kernel/src/syscall_rw.cpp:309` (snippet matches 4 lines: 309, 944, 971, 977), in `sys_read / WriteToPinnedFdByType`.
- **Severity:** medium (confidence medium).
- **Problem:** About 9 waits (eventfd, keyboard, UDP, timerfd, tty write, pipe_write.inc) use HasPendingSignals, so a default-ignored SIGCHLD gives a spurious EINTR.
- **Suggested fix:** One helper with HasReadInterruptSignal semantics.
- **Test:** QEMU: eventfd read is not interrupted by a child exiting with SIGCHLD at SIG_DFL.

### Low

### L-02 · Inconsistent id range checks; 64-bit ids truncate and alias valid ones

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:102`, in `sys_brook_wm_signal_dirty`.
- **Severity:** low (confidence high).
- **Problem:** signal_dirty, set_title, pop_input, map_window_vfb and gpu_attach_window cast to uint32 without an upper bound (0x100000001 acts on window 1). The 6 GPU handlers compare `(int32_t)ctx`, so high bits are ignored.
- **Suggested fix:** One ValidWmId() helper; reject ctx > INT32_MAX.
- **Test:** Host test: wmId=(1<<32)|1 and the equivalent ctx give -EINVAL.

### L-16 · wait4 rejects __WALL/__WCLONE/__WNOTHREAD

- **Where:** `src/kernel/src/syscall_process.cpp:451`, in `sys_wait4`.
- **Severity:** low (confidence high).
- **Problem:** strace, gdb, valgrind and supervisors that pass __WALL get EINVAL.
- **Suggested fix:** Accept these bits.
- **Test:** QEMU: waitpid(-1, &s, __WALL).

### L-69 · execve post-commit sanity check kills the process rather than failing pre-commit

- **Where:** `src/kernel/src/syscall_exec.cpp:481`, in `sys_execve`.
- **Severity:** low (confidence low).
- **Problem:** A lazily mapped entry page would kill a valid program.
- **Suggested fix:** A mapping-range check before commit.
- **Test:** test_exec_page_cache with a lazy interpreter.
