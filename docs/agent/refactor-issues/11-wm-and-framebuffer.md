# Window manager syscalls and framebuffer

_6 open entries (2 medium, 4 low). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** WM syscall error reporting and window lookup lifetime, framebuffer auto-window creation, `FBIOGET_FSCREENINFO`, input pop details, and a leftover profile dump.
- **Why it matters:** Small, mostly self-contained wins; M-51 is a use-after-free style race on window slots.
- **Dependencies:** M-51 pairs with H-22 (10). Authorisation of these calls is in 02.
- **Suggested order:** M-51, M-05, L-47, L-66, L-48, L-05.
- **Test approach:** `test_syscall_user_memory/input_wm.cpp`; compositor tests `test_compositor_*`.
- **Size:** Small: 6 entries, one sitting.

## Entries

### Medium

### M-05 · WM syscalls report success for non-owned or missing windows; errnos inconsistent

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:92`, in `sys_brook_wm_destroy_window`.
- **Severity:** medium (confidence high).
- **Problem:** 3 sites (destroy_window, signal_dirty, set_title) return 0 for a foreign or stale id because the Wm* functions return void. Other handlers use -EINVAL in some cases and -ENOENT in others for a missing window.
- **Suggested fix:** Make Wm*ById return int and propagate it, with one errno for "not your window".
- **Test:** Host test: B's destroy, dirty and title calls on A's window give -ENOENT and A is unchanged.

### M-51 · WmFindWindowById result used without g_vfbLock; slot or Window* acted on later (TOCTOU)

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:398`, in `sys_brook_wm_set_decoration_mode`.
- **Severity:** medium (confidence medium).
- **Problem:** 7 sites: set_decoration_mode, begin_move, set_maximized, set_minimized, begin_resize, pop_input, and gpu_attach_window in syscall_brook_gpu.cpp. Window.cpp callers hold VfbGuard and these do not. A sibling thread can destroy the window and the slot can be reused by another process between the ownership check and the action.
- **Suggested fix:** Owned, incarnation-checked variants that do lookup and action under the lock (like WmInputPopOwned).
- **Test:** Host test that destroys and recreates the slot between lookup and action: the action must fail.

### Low

### L-05 · Leftover per-call [PROFILE] serial dump in set_title

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:311`, in `sys_brook_wm_set_title`.
- **Severity:** low (confidence high).
- **Problem:** Every title change takes the serial lock and prints 10 counters, and function-local extern declarations hide a dependency on the block cache. It looks like a boot-profiling leftover.
- **Suggested fix:** Remove it, or put it behind a debug flag.
- **Test:** Build plus a QEMU serial-volume check.

### L-47 · Framebuffer auto-window creation races between threads and ignores failures

- **Where:** `src/kernel/src/syscall_ioctl.cpp:298`, in `sys_ioctl (DevFramebuf)`.
- **Severity:** low (confidence medium).
- **Problem:** `!proc->fbVirtual` is checked without framebufferLock, so two threads each create a window. Return values are ignored and the static counter is not atomic.
- **Suggested fix:** Do it under framebufferLock or a once flag, and check the results.
- **Test:** QEMU: two threads doing FBIOGET create exactly one window.

### L-48 · FBIOGET_FSCREENINFO reports the physical framebuffer address as smem_start and mmio_start

- **Where:** `src/kernel/src/syscall_ioctl.cpp:360`, in `sys_ioctl (FBIOGET_FSCREENINFO)`.
- **Severity:** low (confidence medium).
- **Problem:** This leaks the physical address to unprivileged users, and the reported values don't match the per-process VFB.
- **Suggested fix:** smem_start=0 and mmio_start=0/mmio_len=0 for unprivileged callers.
- **Test:** Host test: non-root FSCREENINFO gives smem_start==0.

### L-66 · input_pop reports the pointer position at pop time and drops scroll deltas

- **Where:** `src/kernel/src/syscall_brook_system.cpp:179`, in `sys_brook_input_pop`.
- **Severity:** low (confidence low).
- **Problem:** Every mouse event in a batch gets the current cursor position. deltaX and deltaY are never copied out. The header comment describes the wrong direction.
- **Suggested fix:** Record the position when queueing, and carry the deltas in the reserved bytes (versioned ABI).
- **Test:** Host test: two queued moves and a scroll pop with their own coordinates and the scroll delta.
