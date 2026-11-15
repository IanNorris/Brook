# Single-responsibility refactors (follow-on, no issue IDs)

_Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). These are structural moves, not bugs, so they carry no IDs._

- **Scope:** pure moves of code that is still in the wrong file, inside the split `syscall_*.cpp` files and in `net.cpp`, `compositor.cpp`, `window.cpp`, `ext2_vfs.cpp`, `scheduler.cpp`, `process.cpp`.
- **Dependencies:** do each move after the bug-fix chunk that edits the same file, so fixes are not rebased across a move. `sys_ioctl` device extraction goes after 10; `OpenAtInternal` and the synthetic-node registry after 09 (L-39); the `net.cpp` debug move is independent. `DebugHandleCommand` splitting is not a pure move and needs its own review.
- **Suggested order:** `net.cpp` debug channel/commands (pure move, independent), `sys_ioctl` device drivers, `sys_read` readers, misplaced helpers, then `compositor.cpp`, `window.cpp`, `scheduler.cpp` (careful: `audit_scheduler_start.py`), `process.cpp`, `ext2_vfs.cpp` (lowest value).
- **Test approach:** build plus the existing host tests; `src/tests/test_socket_lifetime/main.cpp` includes all of `net.cpp`, so re-check its stubs after the debug move. No behaviour change allowed in a pure move.
- **Size:** each file is one sitting; `net.cpp` and `compositor.cpp` are the largest.

## Candidates

Inside the split files (follow-up moves, same pure-move rules):

- `sys_ioctl` still contains whole device drivers. These would go to their owning files:
  - OSS → `syscall_dsp.cpp`;
  - termios/TIOC* → a tty-ioctl file;
  - FBIO* and VFB auto-creation → a framebuffer file;
  - DMA_BUF and sync-file ioctls → `syscall_drm.cpp` or a sync-file module;
  - BLK* → `syscall_maintenance_block.cpp`.
- `OpenAtInternal` (about 500 lines) embeds device dispatch, the DRM sysfs shim, synthetic `/etc` files and `/boot` aliases. A synthetic-node registry shared with stat, statx and access would remove four drifting copies.
- `sys_read` holds the tty, keyboard and klog readers (about 350 lines), and `WriteToPinnedFdByType` holds the DSP write loop.
- These items sit in the wrong place:
  - the SCM_RIGHTS snapshot code (`UnixFdSnap*`) is in `syscall_socket.cpp`, but is AF_UNIX-only and used by `syscall_rw.cpp`;
  - `NetlinkInterfaceIoctl` (SIOCGIF*) is not netlink;
  - netlink could be its own `.cpp` once its host test includes a header instead of the fragment;
  - `IntelRenderIoctl` (i915) and `DrmRenderIoctl` (virtio-gpu) share a file;
  - `WaitDeadlineNs`/`WaitWakeupTick` live in poll, and `TimerWaitGroup` in the timerfd header, though both are generic wait helpers;
  - `sys_brook_gpu_window_validate`/`peer_recovery` are AF_UNIX credential checks;
  - `syscall_brook_system.cpp` is a grab-bag.
- Robust-list cleanup could leave `syscall_futex.cpp`. The mapping-fault resolver and `FilePageCache` preloader in `syscall_memory.cpp` belong under `memory/`.

Elsewhere in the kernel:

- **`net.cpp` (7,504 lines).**
  - Lines 4177–4357 are the debug channel transport (TCP socket to the host, reply sockets, send and poll).
  - Lines 4360–7502 are the debug-command system, including `DebugHandleCommand`, a single 2,623-line function. It is an if/else chain over command strings.
  - Moving both regions out of `net.cpp` is a pure move: `debug_channel.cpp` and `debug_commands.cpp`.
  - Splitting `DebugHandleCommand` per area (scheduler, GPU, audio, memory and so on) is **not** a pure move, because each branch must become a function. Do it as its own reviewed change.
  - `src/tests/test_socket_lifetime/main.cpp` includes the whole of `net.cpp`, so moving the debug code out also changes what that test compiles. Its stubs need re-checking.
  - Not done in this pass (see the plan).
- **`compositor.cpp` (4,768 lines)** mixes several things:
  - software composition;
  - GPU (BLIT) composition, and a GPU textured-quad present path;
  - hardware cursor rendering;
  - the clock overlay;
  - the user-mode compositor scanout interface (Phase 1);
  - the window-state query API (Phase 2).
  
  Natural files: `compositor_gpu.cpp`, `compositor_cursor.cpp`, `compositor_scanout_api.cpp` (the scanout map/flip and window-list syscalls' backend, see H-03), and the clock overlay next to `clock_overlay.cpp`.
- **`window.cpp` (3,991 lines)** mixes window state and API, chrome rendering, the taskbar, the app launcher popup and the per-window VFB API. Chrome, taskbar and launcher are UI rendering and can move out (`window_chrome.cpp`, `taskbar.cpp`, `app_launcher.cpp`), leaving window management.
- **`ext2_vfs.cpp` (4,816 lines)** is already sectioned:
  - on-disk structures;
  - device I/O and caches;
  - allocation;
  - directory manipulation;
  - inode operations;
  - path resolution;
  - vnode and fs ops.
  
  Candidates: `ext2_cache.cpp` (block, inode and dirent caches plus metadata write-back), `ext2_alloc.cpp`, `ext2_dir.cpp`, with path resolution and the vnode ops staying in `ext2_vfs.cpp`. This is lower value than the others because it is one subsystem.
- **`scheduler.cpp` (3,706 lines)** mixes the scheduler lock, per-CPU state, reschedule IPIs, ready-queue policy delegation, idle tasks, the context-switch logic and BRO-176 diagnostic rings. The diagnostic rings (`AsLiveEvent` and similar) and the idle/IPI plumbing could move out (`scheduler_diag.cpp`, `scheduler_idle.cpp`). Context-switch logic should stay with the core. Treat it with care: `audit_scheduler_start.py` checks compiled startup code.
- **`process.cpp` (2,160 lines)** holds `ProcessFork`, exec image commit and thread creation. It is the natural home for the clone/vfork fixes (H-07, H-23, H-25), but it is not urgent to split.
