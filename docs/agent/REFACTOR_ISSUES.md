# Refactor issue log

_Started 15 November 2026 during the pure-move split of `syscall.cpp` (branch `work/syscall-split`, see [syscall-split-plan.md](syscall-split-plan.md)). Nothing listed here was changed by the split. This log drives the separate fix pass, which adds test coverage first._

## Fix pass status (15 November 2026, branch `work/syscall-fixes`)

The fix pass took all 27 high entries plus the wakeup-related medium entries. Each entry now carries a **Status (fix pass)** line. Every high entry was confirmed against the code before it was changed; none was refuted outright, and H-26 is partly refuted (see its entry).

| Outcome | Entries |
|---|---|
| Fixed, with a host test that fails on the previous code | H-01, H-02, H-05, H-06, H-09, H-10, H-11, H-12, H-13, H-14, H-15, H-16, H-17, H-18, H-23, H-24, H-26, H-27, M-18, M-23, M-27 |
| Fixed; the test encodes the fix, but the old tree cannot build against it or the changed path is kernel-only | H-07, H-08, H-19, H-25, L-09 |
| Hardened only (policy still open) | H-04 |
| Partly fixed | M-64, L-60 (single-slot waiters remain on read/write/accept and on /dev/tty) |
| Open: policy decision for Ian | H-03, H-21; see "Authorisation proposal" |
| Deferred, with a design below | H-20, H-22 |

Commits, oldest first:

- `b61a0f73`: raw user pointers in input/WM, getdents64, open and keyboard read;
- `06a10c04`: inet sockets;
- `ae328f7a`: abstract AF_UNIX names;
- `6881a138`: clone TLS, vfork, child TID;
- `cd53b5ac`: read on O_WRONLY;
- `4ef02e1f`: no user copies under spinlocks;
- `d9c2bafa`: /dev/tty pins;
- `863f802a`: sync-file, pipe and eventfd wakeups;
- `196cc80d`: validate-then-dereference in process, time, cred, poll and stat;
- `c5a6cae6`: ioctl and virtio DRM;
- `ea753d08`: input grab hardening.

### Test scaffolding

`src/tests/test_syscall_user_memory/` compiles the real `syscall_*.cpp` files on the host against a fake user address space. There is one translation unit per area, and `--gc-sections` drops whatever a case does not reach.

- **User memory** is an mmap arena with per-page protection. Every address outside it counts as kernel memory, so a raw store through a "kernel" pointer corrupts a canary instead of returning -EFAULT.
- **Raced-unmap mode** marks a range unmapped right after `UserBufferReadable/Writable` succeeds, which is what a sibling `munmap` does. A handler that still dereferences after validating then writes into the "unmapped" page, and the case fails.
- **Hooks** observe every user copy (to assert no pipe or memfd spinlock is held), every wake and every block.
- **Stand-ins:** `fakes.cpp` holds minimal versions of the subsystems underneath (VFS lookups, a fake UDP/TCP socket, GPU ops). The real pipe, AF_UNIX, eventfd, memfd and poll code is compiled in.

### Validate-then-dereference: the design

The fault-tolerant copy already exists:

- `user_copy.S` has one labelled `rep movsb` per direction.
- `UserCopyFaultFixup`, called from the #PF handler, redirects a fault on exactly those instructions to a residual-count return.
- `CopyFromUser`/`CopyToUser` prefault and retry the remaining page, so a raced `munmap` or `mprotect` becomes a short copy and -EFAULT.

Any other kernel-mode fault on an unresolvable user address still panics.

So the rule is: **user memory only through `CopyFromUser`/`CopyToUser`/`CopyUserCString*`**. Results are built in kernel memory and copied once, and never while a spinlock is held. `UserBufferReadable/Writable` remain useful only as an early check before consuming state, for example before popping input events. No new mechanism was needed.

Sites converted in this pass: every site named by H-01, H-02, H-05, H-06, H-08, H-09, H-10, H-13, H-14, H-17, H-18 and H-19, plus the device reads in M-27.

A grep after the pass found these sites still dereferencing user memory directly, for the next round:

- `VfsRead`/`VfsWrite` on regular files (`sys_read`, `sys_write`, `pread64`/`pwrite64`) receive the user pointer and copy inside the filesystem. This is the largest remaining class. It needs a user-aware VFS copy or a bounce buffer in `syscall_rw.cpp`.
- `readv`/`writev` read the iovec array directly (M-28), and `sendmmsg`/`recvmmsg` read the message vector directly.
- `setsockopt`/`getsockopt` values and lengths, and `socketpair`'s result array.
- WM `get_screen_info`, `scanout_map` and `get_windows` (M-02). GPU `submit` and `upload_buffer` pass raw pointers to the driver (M-01).
- The DSP write path, `FUTEX_WAIT` (L-54) and the netlink request staging.

### Authorisation proposal (H-03, H-04, H-21; Ian decides)

Current in-tree users:

- waylandd uses the per-window WM syscalls (506-520, 525), and none of 504, 505 or 521-524.
- The Vulkan renderer (`tools/vulkan-compositor-pkg`) uses `DESKTOP_LAYERS` (0xB19) and the overlay (0xB1A).
- The bridge's frame capture goes through the debug command path and `debug_frame`.

Proposal:

1. **Add one display-server capability.** It is a process-wide flag set by the kernel, never by the process itself.
   - It goes to the processes the boot script starts as the desktop: the registered offscreen renderer (`BdwOffscreenRendererOwnedBy`), waylandd, and root.
   - It survives `exec` only into those binaries, and never passes through `fork` into an unrelated `exec`.
2. **Gate these syscalls on it**, returning -EPERM otherwise:
   - `input_pop` (504) and `input_grab` (505);
   - the legacy compositor calls 521-524 (scanout map and flip, window list, mapping any window's VFB);
   - `DESKTOP_LAYERS` (0xB19), and the full-frame ops 0 and 2 of `DESKTOP_OVERLAY` (0xB1A);
   - `DESKTOP_CLIENT_FRAME`;
   - the cursor image set and reset (L-44).
3. **Retire 521-524** if Ian confirms nothing out of tree uses them. There is no in-tree caller.
4. **Keep `debug_frame` root-only**, and give it its own capture slot so that a desktop capture cannot starve it.

Already done, with no policy change: `input_grab` refuses to replace a live grabber unless the caller is that grabber or root (`ea753d08`).

### Shared open-file description (H-20)

Today `FdEntry` holds `seekPos` and `statusFlags` per slot, and `dup`, `dup2`, `fork` and SCM_RIGHTS copy them.

A correct fix gives each open a refcounted `OpenFile { refs; lock; offset; statusFlags; type; handle; }`. `FdEntry` then keeps only `fdFlags` (CLOEXEC) and a pointer to it.

- **Reference counting:** `FdTableDuplicate*`, `FdTableForkSnapshot`, `UnixFdSnapFrom`/`Install` and `FinalizeClosedFd` retain and release the `OpenFile` rather than the handle. The per-type handle refcount moves into the `OpenFile`'s final release.
- **Offsets:** `read`, `write`, `lseek`, `getdents64`, `sendfile` and `copy_file_range` update the offset under `OpenFile::lock`. That lock must be a sleeping lock, because VFS I/O can block. Pipes and sockets ignore the offset and skip the lock.
- **Status flags:** `F_GETFL`/`F_SETFL` and `FIONBIO` act on the shared flags.

This touches every fd type's lifetime, so it needs its own branch and a QEMU shell test. The test should show interleaved rather than overwritten output for `cmd >f 2>&1` and `{ a; b; } >f`, and should show a forked child's `F_SETFL O_NONBLOCK` reaching the parent.

### GPU_ATTACH_WINDOW lifetime (H-22)

`sys_brook_gpu_attach_window` (0xB12) hands the window's kernel VFB address to the virtio-gpu driver as resource backing. `WM_RESIZE_VFB` and window destroy then retire those pages through `CompositorRetireVfb`, while the host resource stays attached until `CtxDestroy`. Only the virtio demo apps (`gltri`, `glgears`) call it.

Proposed fix:

1. At attach, record `(ctx, resId)` on the `Window` under `g_vfbLock`, and do the window lookup under the same lock.
2. In resize and destroy, before the old VFB is retired, call `GpuAppOps::RetireResource(ctx, resId)` (detach plus host unref) and clear the record. A later use of that resource by the app then fails instead of writing freed pages.

It was not done in this pass, for two reasons:

- The driver op must not run under `g_vfbLock` if the compositor's virtio path can take that lock while holding the virtqueue. The call therefore has to move outside the lock, with a retirement pin.
- The QEMU check does not exercise virgl, so the change could not be verified.

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


## How this was collected

Seven read-only reviewers each took one group of the new files. Each reviewer read the code in full, checked callees where severity depended on them, and reported every finding as file, function and an exact code snippet. The snippets were located mechanically in the split files, so every **Where** line points at the post-split location on this branch.

I spot-checked the high-severity entries against the code: getdents64, the open kernel-pointer branch, `input_pop`, the abstract AF_UNIX name copy and `CLONE_SETTLS`. Entries marked confidence "low" are the reviewer's suspicion and need confirming before anyone fixes them. Where a reviewer found the same pattern in many places, it is one entry with a count. So the totals below count distinct problems, not lines.

**Totals: 182 entries: 27 high, 67 medium, 71 low, 17 info.**

One correction to a common assumption: `user_access_scope.h` says a `UserAccessScope` is "lexical eviction exclusion only, NOT … protection against raced munmap/mprotect". A live scope therefore does not make a raw dereference of user memory safe.

## Cross-cutting themes

These cut across the whole syscall layer and are better fixed as one design change than entry by entry.

1. **User memory accessed without fault-safe copies.**
   - Some handlers write or read through a user pointer with *no* range check at all, so an unprivileged process can read or write arbitrary kernel memory (there is no SMAP): H-01, H-02, H-13, H-14, H-17 and H-18.
   - About 100 further sites validate with `UserBuffer*` and then dereference directly: H-06, H-08, H-09, H-10, H-19 and the per-area medium entries. Racing a sibling `munmap` with any of them faults the kernel.
   - Fix: one rule, user memory only through `CopyFromUser`/`CopyToUser`, plus a host `user_copy` stub that rejects kernel-range pointers, so each handler can be tested.
2. **User memory copied while holding a spinlock**: pipes, AF_UNIX streams, memfd and serial (H-11, H-16, H-24). A demand fault would sleep under the lock, and a raced unmap oopses the kernel. memfd can self-deadlock on its own `MAP_SHARED` mapping.
3. **Brook-specific syscalls have no authorisation**. Any process can grab all input, map the compositor back buffer, read every window, capture the desktop, or replace the cursor (H-03, H-04, H-21 and several low entries). Decide on a display-server capability once and gate them all on it.
4. **Descriptor publication races.** Many paths publish an fd with `FdAlloc` and then patch flags, or close by number on a copy-out failure. This loses `O_CLOEXEC` and can close a sibling's reused fd. `FdTablePublishOwned` and `FdTablePublishOwnedPinned` already exist and should be used everywhere.
5. **Single-slot waiter fields** (`readerWaiter`, `epollWaiter`, `acceptWaiter`) are overwritten and set without locks. Several fd types also have no wakeup source at all, so waiters fall back to 10–100 ms safety ticks (H-26 and the medium poll/epoll entries). This is directly relevant to the 60 fps goal: a missed sync-file wake costs frames.
6. **Raw errno literals.** There are about 200 `return -NN` sites even where named constants exist. Several are wrong for Linux (`ENOSYS` from `kill` and unknown ioctls, `EFAULT` for long paths). Add the missing constants to `syscall_internal.h`, then a grep lint.
7. **No shared open-file description** (H-20). Offsets and status flags are copied on `dup` and `fork`, so shell redirections that share an offset overwrite each other's output.

## Suggested order for the fix pass

1. Test scaffolding:
   - a host harness that compiles a split file against a fault-injecting `user_copy` stub;
   - a QEMU guest "munmap race" probe that the high entries can share.
2. The no-validation kernel read/write entries: H-01, H-02, H-13, H-14, H-17 and H-18. Each is small.
3. Authorisation: H-03, H-04 and H-21.
4. Spinlock-held user copies (H-11, H-16, H-24) and the `CLONE_SETTLS` crash (H-23).
5. The validate-then-dereference class, area by area, with the same harness.
6. Linux-semantics entries (medium and low), each with its own small test.

## Split artefacts

These come from the move itself and are safe to clean up early, without behaviour risk:

- orphaned section banners and comments that now sit above the wrong function, or describe "the file" as the old monolith;
- the leftover declarations in `syscall.cpp` (see the info entries);
- `syscall_futex_pi.inc` and `syscall_dsp.h` comments that still refer to `syscall.cpp`;
- `syscall_mount.cpp` existing only to include `syscall_mount_namespace.inc`.

Reviewers who saw about 110 includes in some files were reading an earlier snapshot. The committed files carry pruned include lists.

## Single-responsibility candidates (not moved in this pass)

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

## High

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

### H-03 · Legacy user-mode compositor syscalls 521-524 lack any privilege check

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:608`, in `sys_brook_wm_scanout_map`.
- **Severity:** high (confidence high).
- **Problem:** Any process can map the compositor backbuffer read/write, push rows to MMIO (scanout_flip), list every window (get_windows) and map any window's VFB (map_window_vfb, where WmMapWindowVfb only checks that the caller is non-null). That allows cross-process pixel leaks and screen spoofing. compositor.h says "privileged userspace process", but nothing enforces it. 521 and 524 also skip the ProcessCurrent() null check.
- **Suggested fix:** Gate on a compositor capability (root, the registered input grabber or the display server), or retire the syscalls if unused. Otherwise return -EPERM.
- **Test:** QEMU test: an unprivileged process calling 521-524 gets -EPERM.
- **Status (fix pass):** open: policy decision for Ian; proposal under "Authorisation proposal" below. Confirmed: 521-524 have no in-tree caller.

### H-04 · Any process can become the global input grabber (keylogger, waylandd DoS)

- **Where:** `src/kernel/src/syscall_brook_system.cpp:203`, in `sys_brook_input_grab`.
- **Severity:** high (confidence high).
- **Problem:** CompositorSetInputGrabber replaces the current grabber unconditionally, so any process receives all keyboard and mouse input and the display server stops getting it.
- **Suggested fix:** Require privilege, and refuse while a live grabber is registered unless the caller is that grabber.
- **Test:** QEMU test: non-root 505 gives -EPERM and the existing grabber keeps receiving input.
- **Status (fix pass):** hardened in `ea753d08` (no silent takeover of a live grabber; root may take over). Policy still open, see "Authorisation proposal". Test: host test of `InputGrabTakeoverAllowed` (grab.cpp).

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

### H-07 · vfork parent can block forever (lost wakeup); parent pointer unowned

- **Where:** `src/kernel/src/syscall_process.cpp:306`, in `sys_clone`.
- **Severity:** high (confidence high).
- **Problem:** vforkParent is set after SchedulerAddProcess, and an unconditional [PROFILE] fork print widens the window. A child that exits or execs first never wakes the parent. SchedulerBlock returns on any wake. The raw pointer holds no reference, so a killed parent leaves the child unblocking a freed task.
- **Suggested fix:** Publish a referenced vforkParent before SchedulerAddProcess; wait on a completion flag in a loop; use one release helper for exit, exit_group and exec.
- **Test:** QEMU test_vfork_race: 10k vfork iterations with immediate _exit, and exec-ENOENT then _exit.
- **Status (fix pass):** fixed in `6881a138`. Test: host test in `test_syscall_user_memory` (process.cpp). The old tree does not build against the test (new release helper); the publication-order assertion encodes the bug.

### H-08 · Direct user-pointer dereference after validation only (exec vectors, clone3, clone tid, wait4/waitid, arch_prctl, mincore)

- **Where:** `src/kernel/src/syscall_exec.cpp:40`, in `CopyExecVector`.
- **Severity:** high (confidence high).
- **Problem:** 11 sites. Exec slots and strings; clone3 `__builtin_memcpy(&copied,reinterpret_cast<const void*>(argsPtr)`; clone parent tid; wait4 status (`*reinterpret_cast<int32_t*>(statusAddr) = event.status`, after the child is already reaped); CopyWaitUsage; waitid; arch_prctl GET_FS/GET_GS; mincore `vec[i] = 1`. A raced munmap gives a kernel #PF.
- **Suggested fix:** Copy*User throughout.
- **Test:** QEMU test_usercopy_race; host CopyExecVector test.
- **Status (fix pass):** fixed: arch_prctl GET_FS/GS in `6881a138`, the rest (exec vectors, clone3, wait4/waitid, rusage, mincore) in `196cc80d`. Test: covered by the checked-copy rule; no dedicated host case for exec/wait (they need the scheduler); clone3 is exercised by process.cpp.

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

### H-11 · User memory copied while a spinlock is held (pipe ring, memfd, serial)

- **Where:** `src/kernel/src/syscall_rw.cpp:463` (snippet matches 2 lines: 463, 996), in `sys_read / WriteToPinnedFdByType`.
- **Severity:** high (confidence high).
- **Problem:** PipeBuffer::read/write memcpy under pipe->lock, and several callers hand them raw user pointers: /dev/tty read and write, pipe and FIFO write via pipe_write.inc, and AF_UNIX stream read and write. MemFdReadAt and MemFdWriteAt memcpy under MfdLock (a spinlock). DebugSerial and DevKeyboard write `buf[i]` under SerialLock. About 10 sites. A raced munmap, mprotect or fork-CoW faults inside a spinlock, giving a panic or deadlock.
- **Suggested fix:** Stage through bounded kernel buffers with Copy*User outside the lock, as PipeReadSome and ReceiveTcpToUser already do.
- **Test:** Host test_pipe_write asserting the callback never sees a user address; QEMU munmap and fork race on pipe, tty and memfd writes.
- **Status (fix pass):** fixed in `4ef02e1f`. Test: host test in `test_syscall_user_memory` (memfd.cpp: no lock held across any user copy) and `test_pipe_write` (CopyFromUser staging, faulting source).

### H-12 · read()/pread64() on an O_WRONLY regular file returns its contents

- **Where:** `src/kernel/src/syscall_rw.cpp:526`, in `sys_read, sys_pread64`.
- **Severity:** high (confidence high).
- **Problem:** The Vnode branches never check (statusFlags&3)==1, while the MemFd, BlockDevice and sendfile paths do. A mode-0200 file opened write-only can be read, bypassing permissions. Linux returns EBADF.
- **Suggested fix:** Return -EBADF for write-only descriptors; use a shared FdReadable/FdWritable helper.
- **Test:** QEMU: create a mode-0200 file, open O_WRONLY; read and pread give EBADF.
- **Status (fix pass):** fixed in `cd53b5ac`. Test: host test in `test_syscall_user_memory` (rw.cpp).

### H-13 · Cooked-tty Ctrl+C writes to an unvalidated user pointer when count==0

- **Where:** `src/kernel/src/syscall_rw.cpp:790`, in `sys_read (DevKeyboard cooked)`.
- **Severity:** high (confidence high).
- **Problem:** UserBufferWritable is skipped for count==0, but the Ctrl+C branch still stores buf[0] and returns 1. read(fd, kernel address, 0) followed by Ctrl+C writes 0x0A into the kernel, and the call returns more than count.
- **Suggested fix:** Return 0 for count==0 and use CopyToUser.
- **Test:** QEMU: read(tty, 0xffff800000001000, 0) with an injected Ctrl+C returns 0.
- **Status (fix pass):** fixed in `b61a0f73`. Test: host test in `test_syscall_user_memory` (rw.cpp).

### H-14 · Inet socket paths pass raw, unvalidated user pointers to the net stack or write through them (arbitrary kernel read/write)

- **Where:** `src/kernel/src/syscall_socket.cpp:446`, in `sys_recvfrom`.
- **Severity:** high (confidence high).
- **Problem:** About 9 sites do no range check at all, so a kernel address is accepted. SockRecvFrom writes the datagram and source address directly, and a fault leaves the RX tail half-advanced. Others: sys_bind (`auto* addr = reinterpret_cast<const SockAddrIn*>(addrVal);`, which leaks through getsockname), sys_sendto UDP, sys_recvmsg UDP (`void* buf = msg->msg_iov[0].iov_base;`, `*dst = srcAddr;`), getsockname/getpeername inet (`__builtin_memcpy(uaddr, &local, copyLen);`), AcceptSocket inet (`*userAddr = peerAddr;`, which ignores addrlen and overflows) and AcceptSocket AF_UNIX (an addrlen store guarded only by >=0x1000).
- **Suggested fix:** Stage every address and payload in the kernel; CopyFromUser into a SockAddrIn (check sin_family); CopyToUser min(*addrlen, actual) plus the length; user-aware UDP variants.
- **Test:** Host test with stubbed user_copy rejecting kernel-range pointers: -EFAULT for bind, sendto, recvfrom, recvmsg, getsockname, getpeername and accept. QEMU probe with 0xffff800000000000.
- **Status (fix pass):** fixed in `06a10c04`. Test: host test in `test_syscall_user_memory` (socket.cpp, with a fake network stack).

### H-15 · Abstract AF_UNIX names not distinguished; every abstract connect matches the first abstract listener

- **Where:** `src/kernel/src/syscall_unix_socket.cpp:222`, in `UnixFindServer`.
- **Severity:** high (confidence high).
- **Problem:** bind and connect copy the name with `for (int i = 0; i < 107 && path[i]; ++i)`, which stops at the leading NUL, so every abstract path is "". A connect to "@foo" can reach "@bar": wrong peer, and a security problem. There is no EADDRINUSE for abstract names, sun_path is cut to 107 bytes, and addrlen==2 binds an empty name rather than autobinding.
- **Suggested fix:** memcmp the full abstract address with its length; enforce uniqueness under g_unixNameLock; implement autobind.
- **Test:** Host: listeners "\0a" and "\0b"; connecting to "\0b" pins b; a second bind of "\0a" gives EADDRINUSE.
- **Status (fix pass):** fixed in `ae328f7a` (also EADDRINUSE and Linux autobind). Test: host test in `test_syscall_user_memory` (unix.cpp).

### H-16 · AF_UNIX stream I/O copies raw user memory while holding the pipe spinlock

- **Where:** `src/kernel/src/syscall_unix_socket.cpp:67`, in `UnixStreamTryWrite`.
- **Severity:** high (confidence high).
- **Problem:** Only SOCK_SEQPACKET is staged. For streams, user pointers from sendto, sendmsg, recvmsg, read and write reach memcpy under pipe->lock (UnixStreamTryRead too), so a demand fault sleeps under a spinlock and a raced munmap oopses the kernel.
- **Suggested fix:** Stage stream payloads in bounded kernel chunks; copy outside the lock.
- **Test:** QEMU: socketpair send/recv while a sibling munmaps the buffer.
- **Status (fix pass):** fixed in `4ef02e1f`. Test: host test in `test_syscall_user_memory` (memfd.cpp, AF_UNIX stream case).

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

### H-19 · User memory dereferenced directly after a validate-only check (stat, statx, F_GETLK, utimensat)

- **Where:** `src/kernel/src/syscall_stat.cpp:278` (snippet matches 2 lines: 278, 328), in `do_stat_internal`.
- **Severity:** high (confidence high).
- **Problem:** 5 sites: do_stat_internal and do_lstat_internal, sys_statx (`auto* stx = reinterpret_cast<linux_statx*>(bufAddr);`), FileRecordLock in syscall_fd.cpp, and sys_utimensat (`UserTime value = userTimes[i];`). Each writes during VFS work, so a raced munmap faults the kernel.
- **Suggested fix:** Fill kernel-local structs and use Copy*User.
- **Test:** QEMU munmap race against stat, statx, F_GETLK and utimensat.
- **Status (fix pass):** fixed in `196cc80d`. Test: covered by the checked-copy rule; no dedicated host case (stat needs the VFS lookups).

### H-20 · dup() and fork() copies get their own file offset and status flags (no shared open file description)

- **Where:** `src/kernel/src/syscall_fd.cpp:362`, in `DuplicateFd`.
- **Severity:** high (confidence high).
- **Problem:** seekPos and statusFlags are copied per slot. With `cmd >f 2>&1` and `{ a; b; } >f`, writers overwrite each other, and F_SETFL O_NONBLOCK on one alias doesn't affect the others.
- **Suggested fix:** A refcounted open-file-description object holding the offset and flags; lock the offset read-modify-write.
- **Test:** QEMU: dup then write "a" and "b" gives "ab"; same for fork.
- **Status (fix pass):** deferred: confirmed; needs a refcounted open-file description through the fd table, fork and SCM_RIGHTS. Design under "Shared open-file description (H-20)" below.

### H-21 · Desktop capture/overlay syscalls expose window pixels and full frames to any process

- **Where:** `src/kernel/src/syscall_brook_desktop.cpp:165`, in `sys_brook_desktop_layers / sys_brook_desktop_overlay`.
- **Severity:** high (confidence medium).
- **Problem:** The only gate is BdwRenderExperimentEnabled(). Any process can export layer pixels, chrome and backdrop, or request and read a full composed frame (overlay op 0/2). That bypasses debug_frame's root-only check and shares its single capture slot (a DoS for root screenshots). BROOK_DESKTOP_CLIENT_FRAME also lets any process call WmNoteClientFrame for any id.
- **Suggested fix:** Require the registered offscreen renderer (BdwOffscreenRendererOwnedBy) or root for every op.
- **Test:** QEMU test with the experiment on: an unrelated process gets -EPERM on 0xB19 and 0xB1A.
- **Status (fix pass):** open: policy decision for Ian; proposal under "Authorisation proposal" below. Not hardened: the Vulkan renderer and bridge capture tools use 0xB19/0xB1A.

### H-22 · gpu_attach_window gives the host a kernel VFB address with no lifetime link

- **Where:** `src/kernel/src/syscall_brook_gpu.cpp:336`, in `sys_brook_gpu_attach_window`.
- **Severity:** high (confidence medium).
- **Problem:** The VFB pages become host scatter-gather backing. WM_RESIZE_VFB or window destroy frees them while the resource stays attached until CtxDestroy, so later host transfers write freed or reused pages. Also: the lookup is not locked, and resId is not checked against the ctx.
- **Suggested fix:** Hold a backing reference for the attachment, or detach on resize and destroy. Do the lookup under VfbGuard.
- **Test:** QEMU test: attach, resize, then GPU_TRANSFER dir=1 must not corrupt foreign pages (freed-page poison check).
- **Status (fix pass):** deferred: confirmed by inspection; virtio-gpu demo path only (gltri, glgears). Design under "GPU_ATTACH_WINDOW lifetime (H-22)" below.

### H-23 · CLONE_SETTLS accepts a non-canonical or kernel FS base (wrmsr #GP in context switch)

- **Where:** `src/kernel/src/syscall_process.cpp:246`, in `sys_clone`.
- **Severity:** high (confidence medium).
- **Problem:** arch_prctl validates the base but clone does not, and context_switch.S wrmsrs it: any user can crash the kernel. context_switch.S also skips the FS write when fsBase==0, so a task inherits the previous FS base after ARCH_SET_FS 0.
- **Suggested fix:** ValidUserSegmentBase() in clone and clone3; always write FS base.
- **Test:** QEMU: clone with tls=0x8000000000000000 gives -EPERM and the kernel survives.
- **Status (fix pass):** fixed in `6881a138` (clone/clone3 validate the base; context_switch.S always writes FS). Test: host test in `test_syscall_user_memory` (process.cpp). The context-switch half is kernel-only; the QEMU check boots it.

### H-24 · memfd read/write memcpy to/from user buffer under the memfd spinlock (self-deadlock via MAP_SHARED fault)

- **Where:** `src/kernel/src/syscall_memfd.cpp:128`, in `MemFdReadAt / MemFdWriteAt`.
- **Severity:** high (confidence medium).
- **Problem:** read(fd, its own unfaulted MAP_SHARED mapping) faults into MemFdGetOrAllocPage, which takes MfdLock again and deadlocks. A raced munmap faults under the spinlock. Large writes call PmmAllocPage repeatedly under the lock.
- **Suggested fix:** Resolve pages under the lock, then copy unlocked (or bounce).
- **Test:** QEMU test_memfd_self_read.
- **Status (fix pass):** fixed in `4ef02e1f`. Test: host test in `test_syscall_user_memory` (memfd.cpp, including a fault back into the memfd during the copy).

### H-25 · CLONE_CHILD_SETTID never implemented; CHILD_CLEARTID ignored for fork-style clones

- **Where:** `src/kernel/src/syscall_process.cpp:252`, in `sys_clone`.
- **Severity:** high (confidence medium).
- **Problem:** glibc _Fork relies on CHILD_SETTID, so a forked child's TCB tid stays the parent's. PI unlock then gives EPERM, and robust-list cleanup skips the child's mutexes. PARENT_SETTID is honoured only with CLONE_THREAD.
- **Suggested fix:** Write the child TID into the child's address space before it runs; honour CLEARTID and PARENT_SETTID for forks.
- **Test:** QEMU test_fork_tid with a PI mutex.
- **Status (fix pass):** fixed in `6881a138`. Test: host test in `test_syscall_user_memory` (process.cpp records the TID addresses; the ForkChildTrampoline store is kernel-only).

### H-26 · No wakeup registration for SyncFile, SignalFd, pipe POLLOUT or nested epoll; waits rely on the 50/100 ms safety tick

- **Where:** `src/kernel/src/syscall_poll.cpp:354`, in `PollWait`.
- **Severity:** high (confidence medium).
- **Problem:** Readiness for these types is only noticed at the next safety tick (50 ms in poll; 100 ms in epoll: `uint64_t safetyMs = timerWaiters.allocationFailed ? 5 : 100;`). A missed GPU fence wake costs several frames at 60 fps.
- **Suggested fix:** Waiter groups for BdwGpuEvent (sync_file), signalfd, pipe writerWaiter and nested epoll; meanwhile lower safetyMs for these types.
- **Test:** QEMU: poll and epoll on a sync_file signalled after 2 ms wake within 5 ms.
- **Status (fix pass):** fixed in `863f802a` for sync files (GPU Retire event, or a 1 ms tick while prompt wakes are off) and pipe POLLOUT. Partly refuted: signal delivery already wakes the target thread even for a masked signal, and epoll flattens nested epolls onto their leaves. What remains (signals aimed at another thread, poll() on an epoll fd) now uses a 10 ms tick. Test: host test in `test_syscall_user_memory` (wait.cpp).

### H-27 · /dev/tty open borrows stdin/stdout pipe handles without pinning them

- **Where:** `src/kernel/src/syscall_open.cpp:235`, in `OpenAtInternal`.
- **Severity:** high (confidence medium).
- **Problem:** fds 0 and 1 are read without FdRef or a lock, and the refcounts are bumped after publishing. A sibling's close can free the PipeBuffer first (use-after-free) or underflow the counts.
- **Suggested fix:** Pin fds 0 and 1, retain both ends, then publish with FdTablePublishOwned.
- **Test:** QEMU: open("/dev/tty") racing dup2 and close on fds 0 and 1; check pipe counts afterwards.
- **Status (fix pass):** fixed in `d9c2bafa` (also honours O_CLOEXEC, part of M-45). Test: host test in `test_syscall_user_memory` (dir_open.cpp).


## Medium

### M-01 · GPU submit/upload pass raw user pointers to the driver (double fetch, munmap race)

- **Where:** `src/kernel/src/syscall_brook_gpu.cpp:372`, in `sys_brook_gpu_submit`.
- **Severity:** medium (confidence high).
- **Problem:** 2 sites (submit, and upload_buffer's `ga->BufferUpload`). Only UserBufferReadable is checked, yet the GpuAppOps contract says the data is "already validated/copied by the caller". The command stream can change after validation, or be unmapped and fault the kernel.
- **Suggested fix:** CopyFromUser into a kmalloc buffer (bounded, or chunked for uploads) and pass the kernel copy.
- **Test:** Host test with a fake GpuAppOps that asserts it receives a kernel pointer; QEMU munmap race during submit.

### M-02 · Validate-then-dereference user outputs, one of them under the compositor lock

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:590`, in `sys_brook_wm_get_screen_info`.
- **Severity:** medium (confidence high).
- **Problem:** 3 sites (get_screen_info, scanout_map, get_windows). scanout_map's result is written inside CompositorScanoutMap while g_scanoutMappingLock is held. get_windows writes user memory and maps VFBs per window, and maxCount*sizeof is unchecked and truncated to uint32. That is harmless today only because of the 32-entry cap.
- **Suggested fix:** Fill kernel structs, drop the locks, then CopyToUser. Clamp maxCount to WM_MAX_WINDOWS before doing the size math.
- **Test:** Host test: a CopyToUser failure gives -EFAULT and no lock is held across the copy.

### M-03 · strace name table has wrong numbers and lacks names for special-cased syscalls

- **Where:** `src/kernel/src/syscall_strace.cpp:122`, in `SyscallName`.
- **Severity:** medium (confidence high).
- **Problem:** statfs and fstatfs are listed as 134 and 135, but SYS_STATFS=137 and SYS_FSTATFS=138, so personality(135) prints as fstatfs. Names are missing for futex(202), the epoll_* calls, accept4, mount/umount2/pivot_root, sync, mknod*, capget/capset, memfd_create, timerfd_* and others, so the always-log errors list prints "?". SyscallCategory also omits accept4 from SC_NET, epoll_pwait/epoll_create1 from SC_IO, and faccessat, mknodat, fsync, statfs and fchdir from SC_FILE.
- **Suggested fix:** Generate the name and category tables from one X-macro list keyed by the SYS_* constants.
- **Test:** Host test: every non-default slot of g_syscallTable has a non-null name, and SyscallName(SYS_STATFS)=="statfs".

### M-04 · strace prints user path strings with %s straight from user memory

- **Where:** `src/kernel/src/syscall_strace.cpp:247`, in `SyscallDispatchTraced`.
- **Severity:** medium (confidence high).
- **Problem:** open, openat and execve tracing passes a0/a1 straight to SerialPrintf. A kernel pointer leaks kernel memory to serial; an unmapped or unterminated pointer faults the kernel. Only reachable with strace on.
- **Suggested fix:** CopyUserCString into a bounded local buffer and print "<fault>" on failure.
- **Test:** Host test: tracing openat with a kernel or unmapped path does not fault and logs "<fault>".

### M-05 · WM syscalls report success for non-owned or missing windows; errnos inconsistent

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:92`, in `sys_brook_wm_destroy_window`.
- **Severity:** medium (confidence high).
- **Problem:** 3 sites (destroy_window, signal_dirty, set_title) return 0 for a foreign or stale id because the Wm* functions return void. Other handlers use -EINVAL in some cases and -ENOENT in others for a missing window.
- **Suggested fix:** Make Wm*ById return int and propagate it, with one errno for "not your window".
- **Test:** Host test: B's destroy, dirty and title calls on A's window give -ENOENT and A is unchanged.

### M-06 · DRM ioctl size and direction bits are ignored, so fixed-size copies over-read and over-write user structs

- **Where:** `src/kernel/src/syscall_ioctl.cpp:280`, in `sys_ioctl (DevDri) -> DrmRenderIoctl`. Also in: syscall_ioctl.cpp / syscall_drm.cpp.
- **Severity:** medium (confidence high).
- **Problem:** Only type and nr reach the handler, so sizeof(kernel struct) is always copied. An old 40-byte execbuffer gets 64 bytes read, and with FENCE_FD_OUT 64 bytes written back, corrupting 24 bytes of the caller's stack. Linux copies min(user, kernel) and zero-fills. The Intel path does check ioctlSize.
- **Suggested fix:** Pass the full cmd and honour _IOC_SIZE and _IOC_DIR.
- **Test:** Host test: EXECBUFFER with _IOC_SIZE=40 and a canary after the struct; the canary is untouched.

### M-07 · Virtio PRIME_HANDLE_TO_FD ignores and doesn't validate flags (DRM_CLOEXEC, DRM_RDWR)

- **Where:** `src/kernel/src/syscall_drm.cpp:837`, in `DrmRenderIoctl (DRM_NR_PRIME_HANDLE_TO_FD)`.
- **Severity:** medium (confidence high).
- **Problem:** dmabuf fds requested as CLOEXEC survive exec, RDWR is dropped, and unknown bits are accepted. The Intel path validates `flags&~(0x80000u|2u)`.
- **Suggested fix:** Validate the flags and derive fdFlags/statusFlags from them, sharing the Intel helper.
- **Test:** QEMU exec-inherit test for DRM_CLOEXEC; host test: flags=0x4 gives -EINVAL.

### M-08 · User-controlled rc.size drives an unbounded, uncharged backing allocation, zeroed byte by byte under the DRM lock

- **Where:** `src/kernel/src/syscall_drm.cpp:421`, in `DrmRenderIoctl (RESOURCE_CREATE) / DrmEnsureBacking`.
- **Severity:** medium (confidence high).
- **Problem:** A 1x1 texture with size=0xffffffff passes and allocates up to 4 GiB of Device pages for KernelPid, uncharged, and the client may hold 8192 handles. The zeroing loop runs under dctx->mappingLock. That is an unprivileged memory-exhaustion and stall DoS.
- **Suggested fix:** Bound the size by the geometry, add a per-file or per-process budget, and use memset.
- **Test:** Host test: a 1x1 texture with a huge size gives -EINVAL; QEMU repeated creates hit an -ENOMEM budget.

### M-09 · Virgl EXECBUFFER doesn't validate flags and ignores FENCE_FD_IN, ring_idx and syncobjs

- **Where:** `src/kernel/src/syscall_drm.cpp:760`, in `DrmRenderIoctl (DRM_NR_VIRTGPU_EXECBUFFER, non-Venus path)`.
- **Severity:** medium (confidence high).
- **Problem:** The Venus branch rejects unknown flags and syncobjs; the virgl branch accepts anything and never waits on the in-fence, which breaks cross-device ordering with a pending Intel sync file.
- **Suggested fix:** Validate the flags, wait for (or reject) a pending FENCE_FD_IN, and reject syncobj counts.
- **Test:** Host test: flags=0x80 and num_in_syncobjs=1 give -EINVAL; QEMU: a pending Intel fence is not submitted early.

### M-10 · Unknown ioctls return -ENOSYS or -EINVAL instead of -ENOTTY

- **Where:** `src/kernel/src/syscall_ioctl.cpp:884`, in `sys_ioctl`.
- **Severity:** medium (confidence high).
- **Problem:** The final fallthrough returns ENOSYS, which libraries read as "ioctl missing". The fb and dsp unknown-cmd branches return EINVAL before the 'T' catch-all, so isatty() on /dev/fb0 or /dev/dsp gives EINVAL.
- **Suggested fix:** -ENOTTY for unknown commands; let 'T' commands reach the ENOTTY path.
- **Test:** Host table test: TCGETS on fb, dsp, a file and a socket gives -ENOTTY.

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

### M-18 · Single-slot waiter fields overwritten; lost wakeups

- **Where:** `src/kernel/src/syscall_poll.cpp:372`, in `PollWait`.
- **Severity:** medium (confidence high).
- **Problem:** poll overwrites the eventfd readerWaiter that a blocked read() uses (even for a POLLOUT-only poll). The epollWaiter slots are shared too. With two or more waiters, the last one wins and the others fall back to the timeout. No lock is held.
- **Suggested fix:** Per-object waiter lists like PipePollGroup.
- **Test:** Two pollers and a reader on one eventfd: one write wakes all within 1 ms.
- **Status (fix pass):** fixed in `863f802a`: eventfds keep a waiter list (EventFdWaitGroup); epoll uses pipe poll subscriptions for pipes and AF_UNIX rx/tx. Test: host test in `test_syscall_user_memory` (wait.cpp).

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

### M-23 · rt_sigprocmask writes oldset before reading set and validating how

- **Where:** `src/kernel/src/syscall_signal.cpp:242`, in `sys_rt_sigprocmask`.
- **Severity:** medium (confidence high).
- **Problem:** When set and oldset alias, SIG_BLOCK is a no-op. An invalid how still clobbers oldset.
- **Suggested fix:** Copy set in, validate how, then write oldset.
- **Test:** sigprocmask(SIG_BLOCK,&s,&s); how=7 leaves oldset untouched.
- **Status (fix pass):** fixed in `196cc80d`. Test: host test in `test_syscall_user_memory` (race.cpp).

### M-24 · kill with pid <= 0 returns ENOSYS

- **Where:** `src/kernel/src/syscall_signal.cpp:417`, in `sys_kill`.
- **Severity:** medium (confidence high).
- **Problem:** kill(0), kill(-pgid) and kill(-1) are basic job control, and Linux never returns ENOSYS from kill.
- **Suggested fix:** Implement group and broadcast delivery; meanwhile return ESRCH or EPERM.
- **Test:** kill(-pgid) reaps the group.

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

### M-27 · Raw dereference of user buffers (non-fault-safe) in device read paths

- **Where:** `src/kernel/src/syscall_rw.cpp:554`, in `sys_read`.
- **Severity:** medium (confidence high).
- **Problem:** About 9 sites: urandom, klog, SyntheticMem and the keyboard modes. UDP read and write go through SockRecvFrom and SockSendTo byte loops, so a fault mid-datagram corrupts the RX ring. The urandom and klog `>= 0x0000800000000000ULL` checks are dead.
- **Suggested fix:** Kernel staging buffers plus Copy*User; user-aware UDP variants.
- **Test:** QEMU: munmap race during urandom, klog, SyntheticMem and UDP reads gives EFAULT.
- **Status (fix pass):** fixed in `b61a0f73` (keyboard, urandom, klog, synthetic files) and `06a10c04` (UDP read/write). Test: host test in `test_syscall_user_memory` (rw.cpp, socket.cpp).

### M-28 · TOCTOU double-fetch of the iovec array in readv/writev

- **Where:** `src/kernel/src/syscall_rw.cpp:1172`, in `sys_writev`.
- **Severity:** medium (confidence high).
- **Problem:** The O_APPEND "snapshot" is a raw memcpy from user memory. The generic loops read iov_base and iov_len twice, directly from user memory.
- **Suggested fix:** CopyFromUser the iovec array once at entry.
- **Test:** Host iovec-import test; QEMU iovec mutation race.

### M-29 · Anonymous pipes do not give PIPE_BUF write atomicity

- **Where:** `src/kernel/src/pipe_write.inc:33` (snippet matches 2 lines: 33, 60), in `WriteToPinnedFdByType pipe branch`.
- **Severity:** medium (confidence high).
- **Problem:** Atomic mode needs flag 0x10, which only named-FIFO endpoints set (CreatePipe sets flags=1). Writes of 4096 bytes or less can be split, or come back short in nonblocking mode, so concurrent writers interleave.
- **Suggested fix:** Every pipe write of 4096 bytes or less is atomic.
- **Test:** test_pipe_write: a 100-byte write to a nearly full anonymous pipe gives EAGAIN or blocks.

### M-30 · write() to a terminal shell's stdin read end is accepted and fed back into its own input

- **Where:** `src/kernel/src/pipe_write.inc:5`, in `WriteToPinnedFdByType pipe branch`.
- **Severity:** medium (confidence high).
- **Problem:** The terminal stdin read end has flags=0x04 (the TTY marker), which passes the ==0 check, so writes to fd 0 inject input.
- **Suggested fix:** Check the write bit (`flags&1`), keeping the socketpair flags==2 case, or reuse FdPipeWrite.
- **Test:** test_pipe_write: flags=0x04 gives EBADF.

### M-31 · Default serial stdout only works on fd numbers 1 and 2

- **Where:** `src/kernel/src/syscall_rw.cpp:116`, in `WriteToPinnedFdByType`.
- **Severity:** medium (confidence high).
- **Problem:** A null-handle Vnode is recognised only by fd number, so dup(1) gives EBADF. It also enqueues the raw user pointer.
- **Suggested fix:** Branch on the entry type, and stage the data.
- **Test:** QEMU: write(dup(1)) succeeds.

### M-32 · Blocking UDP read busy-spins and never sleeps

- **Where:** `src/kernel/src/syscall_rw.cpp:946`, in `sys_read (Socket, datagram)`.
- **Severity:** medium (confidence high).
- **Problem:** A blocking read on an empty UDP socket spins at 100% CPU, and every error becomes EAGAIN.
- **Suggested fix:** Block on a wait queue; propagate errors.
- **Test:** QEMU: an idle blocking UDP recv leaves the CPU idle; SIGALRM interrupts it.

### M-33 · /dev/klog read drops lines and reports EOF to blocking readers

- **Where:** `src/kernel/src/syscall_rw.cpp:586`, in `sys_read (DevKlog)`.
- **Severity:** medium (confidence high).
- **Problem:** The cursor advances past lines that did not fit. A blocking reader sleeps 100 ticks once, without checking signals, then returns 0.
- **Suggested fix:** Read line by line or rewind; block in a loop with signal checks.
- **Test:** Host klog reader with 1-byte reads; QEMU blocking klog waits.

### M-34 · Read/write lengths truncated or clamped inconsistently

- **Where:** `src/kernel/src/syscall_rw.cpp:527`, in `sys_read / sys_pread64 / UDP paths`.
- **Severity:** medium (confidence high).
- **Problem:** read casts count to uint32, so count=0x100000000 reads 0 (EOF). pread passes an unclamped count into the int-returning VfsRead. UDP truncates the length but reports the full count. Only write clamps to VnodeWriteLimit.
- **Suggested fix:** Clamp once at entry, as Linux does with MAX_RW_COUNT.
- **Test:** QEMU: read with count=0x100000001 returns the file size.

### M-35 · readv/writev generic loop blocks after a full first iovec and splits records

- **Where:** `src/kernel/src/syscall_rw.cpp:1118`, in `sys_readv`.
- **Severity:** medium (confidence high).
- **Problem:** TCP, tty, eventfd, timerfd and AF_UNIX stream: after the first iovec fills, a second sys_read blocks (Linux returns), and the eventfd and timerfd counters are consumed twice. writev splits pipe and tty writes.
- **Suggested fix:** Stage and scatter as PipeVectorIo does.
- **Test:** QEMU: readv with two iovecs on TCP holding exactly len(iov0) bytes doesn't block.

### M-36 · Validated-then-dereferenced user memory (TOCTOU/double fetch) in socket syscalls

- **Where:** `src/kernel/src/syscall_socket.cpp:516`, in `sys_getsockopt`.
- **Severity:** medium (confidence high).
- **Problem:** About 12 sites: the sendmsg/recvmsg AF_UNIX loops read msg->msg_iov[i] directly; recvmsg writes cmsg headers and fd slots (`header->cmsg_len = copied;`); getsockopt; setsockopt SO_PASSCRED; the recvfrom addrlen store; socketpair `sv[0] = fdA;`; sendmmsg/recvmmsg msg_len. The recvfrom UserAccessScope doesn't protect against a raced munmap.
- **Suggested fix:** Copy the iovec array once; Copy*User everywhere; build the control buffer in kernel memory.
- **Test:** Host user_copy stub counting violations; QEMU munmap race on recvmsg with SCM_RIGHTS.

### M-37 · AF_UNIX SOCK_DGRAM silently gets stream semantics

- **Where:** `src/kernel/src/syscall_socket.cpp:77`, in `sys_socket`.
- **Severity:** medium (confidence high).
- **Problem:** Only type 5 is a record type, so DGRAM merges message boundaries, and connect to a bound datagram peer fails.
- **Suggested fix:** Route DGRAM through the record path, or return ESOCKTNOSUPPORT until it does.
- **Test:** socketpair DGRAM: sends of 3 and 5 bytes give two recvs of 3 and 5.

### M-38 · socket()/socketpair() don't validate type, flags or protocol for AF_UNIX/AF_INET

- **Where:** `src/kernel/src/syscall_socket.cpp:917`, in `sys_socketpair`.
- **Severity:** medium (confidence high).
- **Problem:** Any type&0xf, any extra flags and any protocol are accepted. AF_INET masks 0xFF and maps every failure to ENOMEM.
- **Suggested fix:** A shared ParseSocketType() returning EINVAL, ESOCKTNOSUPPORT or EPROTONOSUPPORT.
- **Test:** Host errno table.

### M-39 · AF_UNIX accept() ignores O_NONBLOCK set via fcntl

- **Where:** `src/kernel/src/syscall_socket.cpp:821`, in `AcceptSocket`.
- **Severity:** medium (confidence high).
- **Problem:** F_SETFL never updates usd->nonblock for AF_UNIX, so an event loop hangs on a spurious wake.
- **Suggested fix:** Also check statusFlags&0x800.
- **Test:** After fcntl O_NONBLOCK, accept with no client gives EAGAIN.

### M-40 · recvfrom flips the shared socket nonblock flag to implement MSG_DONTWAIT

- **Where:** `src/kernel/src/syscall_socket.cpp:424`, in `sys_recvfrom`.
- **Severity:** medium (confidence high).
- **Problem:** Other threads and fds see the change: a concurrent blocking reader gets a spurious EAGAIN, and FIONBIO is overwritten on restore.
- **Suggested fix:** Pass the nonblock flag per call to UnixSocketReadSome.
- **Test:** A blocking recv in A never sees EAGAIN while B loops recv(MSG_DONTWAIT).

### M-41 · recv()/recvfrom silently ignore MSG_PEEK/MSG_WAITALL/MSG_OOB on stream sockets (data consumed)

- **Where:** `src/kernel/src/syscall_socket.cpp:425`, in `sys_recvfrom`.
- **Severity:** medium (confidence high).
- **Problem:** recv(MSG_PEEK) dequeues the data and sendto ignores flags, while recvmsg rejects the same flags with EOPNOTSUPP.
- **Suggested fix:** Shared allowed-flags masks; EOPNOTSUPP or an implementation.
- **Test:** Peek then read return the same bytes, or the peek fails.

### M-42 · Blocking UDP recvfrom/recvmsg never blocks; all errors become EAGAIN

- **Where:** `src/kernel/src/syscall_socket.cpp:1536`, in `sys_recvmsg`.
- **Severity:** medium (confidence high).
- **Problem:** Only iov[0] is used, the length is truncated to uint32, MSG_TRUNC is missing and flags are ignored.
- **Suggested fix:** A wait loop; propagate errors; scatter across iovecs.
- **Test:** A blocking UDP recv with a delayed sender blocks, then returns the datagram.

### M-43 · sendmsg on TCP rejects multi-iovec sends over 65507 bytes and zero-length sends

- **Where:** `src/kernel/src/syscall_socket.cpp:1338`, in `sys_sendmsg`.
- **Severity:** medium (confidence high).
- **Problem:** The UDP limit is applied to streams. An empty send gives EINVAL (Linux gives 0). sendto UDP returns the untruncated length and maps failures to EIO.
- **Suggested fix:** SockSendUserVector for streams; EMSGSIZE for DGRAM only.
- **Test:** TCP sendmsg of 200 KiB in 3 iovecs; an empty sendmsg returns 0.

### M-44 · setsockopt silently succeeds for every unknown option on inet and AF_UNIX

- **Where:** `src/kernel/src/syscall_socket.cpp:487`, in `sys_setsockopt`.
- **Severity:** medium (confidence high).
- **Problem:** SO_RCVTIMEO and SO_SNDTIMEO are ignored, so a timed recv can block forever. getsockopt gives ENOPROTOOPT for the same options. Netlink rejects everything except SO_RCVBUF, including SO_SNDBUF, which iproute2 needs (medium confidence).
- **Suggested fix:** An explicit ignore allowlist; ENOPROTOOPT otherwise; implement the timeouts.
- **Test:** Host setsockopt/getsockopt symmetry table.

### M-45 · Publish-then-mutate fd allocation ignores O_CLOEXEC and other open flags (11 branches)

- **Where:** `src/kernel/src/syscall_open.cpp:175`, in `OpenAtInternal`.
- **Severity:** medium (confidence high).
- **Problem:** Affected branches: O_PATH, fb0, keyboard, tty, renderD128, /dev/dri, DRM sysfs, synthetic /etc and klog. Flags are set after FdAlloc publishes the fd, or not at all, so fds leak across exec. open("/etc/hostname", O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC) succeeds.
- **Suggested fix:** One FdEntry builder plus FdTablePublishOwned; reject write and O_EXCL on read-only synthetic nodes.
- **Test:** QEMU: per synthetic path, F_GETFD after O_CLOEXEC; O_WRONLY on /etc/passwd fails.

### M-46 · O_TRUNC truncates the file before the O_DIRECTORY check rejects it

- **Where:** `src/kernel/src/syscall_open.cpp:545`, in `OpenAtInternal`.
- **Severity:** medium (confidence high).
- **Problem:** open(file, O_WRONLY|O_TRUNC|O_DIRECTORY) erases the file, then returns ENOTDIR. Opening a directory for writing is not rejected with EISDIR (low confidence).
- **Suggested fix:** Pass the directory requirement into VfsOpenAt; return EISDIR and EINVAL as Linux does.
- **Test:** QEMU: the size is unchanged after ENOTDIR.

### M-47 · /dev/klog cursor allocated as 4 bytes but deep-copied as 8

- **Where:** `src/kernel/src/syscall_fd.cpp:34`, in `DevKlogDeepCopy`.
- **Severity:** medium (confidence high).
- **Problem:** Open allocates uint32_t (`auto* cursor = static_cast<uint32_t*>(kmalloc(sizeof(uint32_t)));`), so dup and fork read 8 bytes from a 4-byte allocation (a heap out-of-bounds read).
- **Suggested fix:** One cursor type with sizeof(*src).
- **Test:** Host deep-copy test; QEMU dup and fork of /dev/klog.

### M-48 · flock() is a stub that always succeeds

- **Where:** `src/kernel/src/syscall_file_ops.cpp:117`, in `sys_flock`.
- **Severity:** medium (confidence high).
- **Problem:** No EBADF or EINVAL checks and no exclusion, so git, nix and package managers believe they hold locks. Record-lock machinery already exists.
- **Suggested fix:** Whole-file locks owned by the open file description, at least with validation and EWOULDBLOCK.
- **Test:** test_record_locks flock cases; QEMU: a second LOCK_EX|LOCK_NB gives EWOULDBLOCK.

### M-49 · Too-long paths return EFAULT; limits vary between 256 and 512 bytes (33 sites)

- **Where:** `src/kernel/src/syscall_stat.cpp:376` (snippet matches 3 lines: 376, 384, 664), in `sys_stat`.
- **Severity:** medium (confidence high).
- **Problem:** mkdir fails on a 300-byte path while mkdirat succeeds. PATH_MAX is 4096, and a too-long path should give ENAMETOOLONG.
- **Suggested fix:** A shared CopyUserPath() with a PATH_MAX buffer, using CopyUserCStringResult.
- **Test:** QEMU: 300- and 4000-byte paths succeed; a 5000-byte path gives ENAMETOOLONG.

### M-50 · F_GETFL reports open-only flags; F_SETFL accepts any bit

- **Where:** `src/kernel/src/syscall_fd.cpp:626`, in `sys_fcntl`.
- **Severity:** medium (confidence high).
- **Problem:** F_GETFL returns O_CREAT, O_EXCL, O_TRUNC and O_CLOEXEC. F_SETFL can set O_PATH or O_TRUNC.
- **Suggested fix:** Strip open-only flags at open; apply only the settable mask in F_SETFL.
- **Test:** QEMU: F_GETFL after open(O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC) equals O_RDWR.

### M-51 · WmFindWindowById result used without g_vfbLock; slot or Window* acted on later (TOCTOU)

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:398`, in `sys_brook_wm_set_decoration_mode`.
- **Severity:** medium (confidence medium).
- **Problem:** 7 sites: set_decoration_mode, begin_move, set_maximized, set_minimized, begin_resize, pop_input, and gpu_attach_window in syscall_brook_gpu.cpp. Window.cpp callers hold VfbGuard and these do not. A sibling thread can destroy the window and the slot can be reused by another process between the ownership check and the action.
- **Suggested fix:** Owned, incarnation-checked variants that do lookup and action under the lock (like WmInputPopOwned).
- **Test:** Host test that destroys and recreates the slot between lookup and action: the action must fail.

### M-52 · Unpinned fd publish, then CloseProcessFd(fd) on copyout failure, can close a sibling's descriptor

- **Where:** `src/kernel/src/syscall_drm.cpp:1065`, in `DrmRenderIoctl (PRIME_HANDLE_TO_FD), IntelRenderIoctl`.
- **Severity:** medium (confidence medium).
- **Problem:** 4 sites: virtio PRIME export (`int fd = FdAlloc(proc, FdType::DrmPrime, p);`), Intel PRIME export, Intel syncobj export and cache-probe alloc. Each publishes unpinned and closes by number if the copy fails, so a reused number gets closed. The sync-file paths avoid this with a pinned publish. 3 sites also flatten the publish error to -EMFILE.
- **Suggested fix:** Pinned publish, then close via the pin's identity; propagate the real error.
- **Test:** Host fd-table test: an fd closed and reopened by a sibling survives the forced copyout failure.

### M-53 · Intel GEM_MMAP callback re-resolves the render fd by number (fd-reuse TOCTOU)

- **Where:** `src/kernel/src/syscall_drm.cpp:958`, in `IntelRenderIoctl (user.mapBuffer lambda)`.
- **Severity:** medium (confidence medium).
- **Problem:** sys_ioctl pins the FdEntry, but the callback calls sys_mmap with the raw number, which a sibling can dup2 over in the meantime.
- **Suggested fix:** Pass the pinned BdwGpuFile to an internal mmap helper.
- **Test:** QEMU: GEM_MMAP racing a dup2 over the same number.

### M-54 · execve from a non-leader thread does not kill sibling threads

- **Where:** `src/kernel/src/syscall_exec.cpp:409`, in `sys_execve`.
- **Severity:** medium (confidence medium).
- **Problem:** The old group keeps running alongside the new image (BRO-091 class).
- **Suggested fix:** Kill all siblings, leader included; the survivor adopts the TGID.
- **Test:** QEMU test_exec_from_thread.

### M-55 · FUTEX_REQUEUE / FUTEX_CMP_REQUEUE unimplemented (ENOSYS with a debug dump)

- **Where:** `src/kernel/src/syscall_futex.cpp:659`, in `sys_futex`.
- **Severity:** medium (confidence medium).
- **Problem:** musl condvars hang with two or more waiters. The log legend still lists REQUEUE. Magic gs offsets.
- **Suggested fix:** Implement both ops; rate-limit the log.
- **Test:** Host test_futex_requeue; QEMU condvar broadcast with 4 waiters.

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

### M-62 · Signal checks inconsistent: HasPendingSignals vs HasReadInterruptSignal

- **Where:** `src/kernel/src/syscall_rw.cpp:309` (snippet matches 4 lines: 309, 944, 971, 977), in `sys_read / WriteToPinnedFdByType`.
- **Severity:** medium (confidence medium).
- **Problem:** About 9 waits (eventfd, keyboard, UDP, timerfd, tty write, pipe_write.inc) use HasPendingSignals, so a default-ignored SIGCHLD gives a spurious EINTR.
- **Suggested fix:** One helper with HasReadInterruptSignal semantics.
- **Test:** QEMU: eventfd read is not interrupted by a child exiting with SIGCHLD at SIG_DFL.

### M-63 · Received SCM_RIGHTS fds published before their numbers reach userspace; a fault leaks them

- **Where:** `src/kernel/src/syscall_socket.cpp:1464`, in `sys_recvmsg`.
- **Severity:** medium (confidence medium).
- **Problem:** When the cmsg or msghdr copy faults, the installed fds stay unreported. socketpair has the same order, and its rollback `CloseProcessFd(proc, fdA)` closes by number, which can hit a reused fd.
- **Suggested fix:** Pinned publish (FdTablePublishOwnedPinned), copy out, then commit; close by identity.
- **Test:** QEMU: a recvmsg control buffer made read-only; the fd count is unchanged after EFAULT.

### M-64 · Stale, unlocked single-slot waiter pointers on AF_UNIX pipes

- **Where:** `src/kernel/src/syscall_unix_socket.cpp:450`, in `UnixSocketReadSome`.
- **Severity:** medium (confidence medium).
- **Problem:** The pointer is set and cleared without the lock and is not cleared after a timeout followed by success, so a peer can wake an exited Process. A second waiter overwrites the first. acceptWaiter is single-slot too.
- **Suggested fix:** Wait queues with ProcessRef.
- **Test:** Two concurrent readers both wake.
- **Status (fix pass):** partly fixed in `863f802a`: epoll no longer uses the rx/tx single slots. read/write and accept still use readerWaiter/writerWaiter/acceptWaiter.

### M-65 · /boot alias fallback can create or modify files under /boot and ignores O_NOFOLLOW

- **Where:** `src/kernel/src/syscall_open.cpp:472`, in `OpenAtInternal`.
- **Severity:** medium (confidence medium).
- **Problem:** The ENOENT retry keeps O_CREAT, O_TRUNC and write access, so open("/etc/x/y", O_CREAT) creates /boot/etc/x/y. followFinal is dropped.
- **Suggested fix:** Use the alias only for read-only opens without O_CREAT or O_TRUNC, and honour O_NOFOLLOW.
- **Test:** QEMU: O_CREAT under a missing directory gives ENOENT and nothing is created under /boot.

### M-66 · O_TMPFILE treated as O_DIRECTORY; opens the directory itself

- **Where:** `src/kernel/src/syscall_open.cpp:445`, in `OpenAtInternal`.
- **Severity:** medium (confidence medium).
- **Problem:** The 0x400000 bit is never checked, so open(dir, O_TMPFILE|O_RDWR) returns a writable directory fd and glibc and systemd skip their fallbacks.
- **Suggested fix:** Return -EOPNOTSUPP while O_TMPFILE is unsupported.
- **Test:** QEMU: open("/tmp", O_TMPFILE|O_RDWR) gives EOPNOTSUPP.

### M-67 · close_range accepts CLOSE_RANGE_UNSHARE but ignores it; sets CLOEXEC without a lock

- **Where:** `src/kernel/src/syscall_fd.cpp:250`, in `sys_close_range`.
- **Severity:** medium (confidence medium).
- **Problem:** Threads sharing the table are affected, and the unlocked write races publish.
- **Suggested fix:** Implement UNSHARE or return EINVAL; set CLOEXEC under ProcessFdLock.
- **Test:** test_fd_table range-CLOEXEC under the lock.


## Low

### L-01 · StraceEnableName/StraceEnableAll scan only PIDs 1..255

- **Where:** `src/kernel/src/syscall_strace.cpp:311` (snippet matches 2 lines: 311, 334), in `StraceEnableName / StraceEnableAll`.
- **Severity:** low (confidence high).
- **Problem:** TaskIdCapacity is 1024, so pids of 256 and above are never traced. ProcessFindByPid returns an unreferenced pointer that is then written while the process may be exiting.
- **Suggested fix:** Iterate the process table under its lock or while holding a reference.
- **Test:** Host test: StraceEnableName matches pid 700.

### L-02 · Inconsistent id range checks; 64-bit ids truncate and alias valid ones

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:102`, in `sys_brook_wm_signal_dirty`.
- **Severity:** low (confidence high).
- **Problem:** signal_dirty, set_title, pop_input, map_window_vfb and gpu_attach_window cast to uint32 without an upper bound (0x100000001 acts on window 1). The 6 GPU handlers compare `(int32_t)ctx`, so high bits are ignored.
- **Suggested fix:** One ValidWmId() helper; reject ctx > INT32_MAX.
- **Test:** Host test: wmId=(1<<32)|1 and the equivalent ctx give -EINVAL.

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

### L-05 · Leftover per-call [PROFILE] serial dump in set_title

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:311`, in `sys_brook_wm_set_title`.
- **Severity:** low (confidence high).
- **Problem:** Every title change takes the serial lock and prints 10 counters, and function-local extern declarations hide a dependency on the block cache. It looks like a boot-profiling leftover.
- **Suggested fix:** Remove it, or put it behind a debug flag.
- **Test:** Build plus a QEMU serial-volume check.

### L-06 · VIRTGPU_WAIT on guest-backed BOs succeeds without waiting or checking flags

- **Where:** `src/kernel/src/syscall_drm.cpp:813`, in `DrmRenderIoctl (DRM_NR_VIRTGPU_WAIT)`.
- **Severity:** low (confidence high).
- **Problem:** A silent-success stub that relies on unrecorded synchronous Transfer3D/Submit3D. Unknown flags are accepted here, while the blob path rejects them with a raw -95.
- **Suggested fix:** Validate the flags (NOWAIT only), and add real idle tracking or assert the synchronous invariant.
- **Test:** Host test: WAIT with flags=0x2 on a legacy BO gives -EINVAL.

### L-07 · ioctl stubs that silently succeed

- **Where:** `src/kernel/src/syscall_ioctl.cpp:645`, in `sys_ioctl`.
- **Severity:** low (confidence high).
- **Problem:** Every unknown DevKeyboard cmd returns 0. TCXONC and TCFLSH succeed on any fd. TIOCSCTTY and TIOCSWINSZ are ignored. TCSETS* drop VMIN/VTIME. FIONREAD on an INET socket always gives 0. FBIOPUT_VSCREENINFO returns 0 with no VFB and never writes back. The private cmd==1 nonblock ioctl changes fde->flags without the fd lock.
- **Suggested fix:** ENOTTY for non-tty fds and unknown keyboard cmds; implement FIONREAD for TCP/UDP; write back FBIOPUT.
- **Test:** Host test: TCFLSH on a regular file gives -ENOTTY; QEMU: FIONREAD on TCP with data pending is > 0.

### L-08 · FIONBIO, FIOCLEX and FIONCLEX are handled only for some fd types

- **Where:** `src/kernel/src/syscall_ioctl.cpp:228`, in `sys_ioctl`.
- **Severity:** low (confidence high).
- **Problem:** Linux handles these generically; Brook returns ENOTTY for eventfd, timerfd, signalfd, epoll, files and DRM (libuv uses FIONBIO). The UnixSocket nonblock store is not atomic.
- **Suggested fix:** Generic handling through statusFlags/fdFlags plus per-type hooks.
- **Test:** Host test: FIONBIO on an eventfd makes read return -EAGAIN; FIOCLEX sets FD_CLOEXEC.

### L-09 · termios/TIOC handling duplicated (keyboard vs tty, and TCGETS2)

- **Where:** `src/kernel/src/syscall_ioctl.cpp:400`, in `sys_ioctl`.
- **Severity:** low (confidence high).
- **Problem:** TCGETS, TCSETS*, TIOCG/SPGRP and TIOCGWINSZ are implemented twice and the c_cc table three times; the copies already diverge.
- **Suggested fix:** TtyFillTermios() and TtyIoctl() helpers.
- **Test:** Host test: byte-compare TCGETS from the keyboard and a tty; TCGETS2 equals TCGETS plus speeds.
- **Status (fix pass):** fixed in `c5a6cae6`. Test: host test in `test_syscall_user_memory` (ioctl.cpp).

### L-10 · TIOCSPGRP accepts any value, truncates it to uint16 and checks no permissions; dead code in TIOCGPGRP

- **Where:** `src/kernel/src/syscall_ioctl.cpp:497` (snippet matches 2 lines: 497, 614), in `sys_ioctl (TIOCSPGRP, 2 copies)`.
- **Severity:** low (confidence high).
- **Problem:** Negative or nonexistent pgids are accepted and values over 65535 wrap. There is no session check or SIGTTOU. The `*pgrp = 1` branch is unreachable, and ProcessCurrent() is refetched 6 times.
- **Suggested fix:** Validate against the process table and session, returning Linux errnos.
- **Test:** Host test: TIOCSPGRP(-1) gives -EINVAL; a foreign session gives -EPERM.

### L-11 · DRM_IOCTL_VERSION ignores string copyout failures

- **Where:** `src/kernel/src/syscall_drm.cpp:383`, in `DrmFillStr`.
- **Severity:** low (confidence high).
- **Problem:** A bad name pointer is skipped silently; Linux returns -EFAULT.
- **Suggested fix:** Return bool and propagate -EFAULT.
- **Test:** Host test: VERSION with a bad name pointer gives -EFAULT.

### L-12 · Duplicated GPU-context and fence plumbing with inconsistent details

- **Where:** `src/kernel/src/syscall_ioctl.cpp:205`, in `DrmRenderIoctl, sys_ioctl`. Also in: syscall_drm.cpp, syscall_ioctl.cpp.
- **Severity:** low (confidence high).
- **Problem:** Lazy CtxCreate appears 3 times, one passing config while the others pass {}. The fence-out publish appears 3 times. The 5 s wait loops differ: the DrmPrime one does unchecked addition and no terminalPending check, while the IntelPrime one uses TimeoutAddSafe and checks terminalPending.
- **Suggested fix:** DrmEnsureContext, DrmPublishFenceOut and one interruptible wait helper.
- **Test:** Host test: the DrmPrime export wait returns -EINTR on a fatal signal.

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

### L-16 · wait4 rejects __WALL/__WCLONE/__WNOTHREAD

- **Where:** `src/kernel/src/syscall_process.cpp:451`, in `sys_wait4`.
- **Severity:** low (confidence high).
- **Problem:** strace, gdb, valgrind and supervisors that pass __WALL get EINVAL.
- **Suggested fix:** Accept these bits.
- **Test:** QEMU: waitpid(-1, &s, __WALL).

### L-17 · epoll_wait rejects maxevents above 1024

- **Where:** `src/kernel/src/syscall_epoll.cpp:626`, in `sys_epoll_wait`.
- **Severity:** low (confidence high).
- **Problem:** Loops that use 4096 get EINVAL. epfd is validated before maxevents, the reverse of Linux.
- **Suggested fix:** Clamp the internal batch.
- **Test:** maxevents=4096 works.

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

### L-21 · Leftover diagnostics that user code can trigger (epoll_wait bad fd, rt_sigreturn)

- **Where:** `src/kernel/src/syscall_epoll.cpp:612`, in `sys_epoll_wait`.
- **Severity:** low (confidence high).
- **Problem:** Each epoll_wait on a bad fd prints an unconditional SerialPrintf (UART flood, interrupts off). rt_sigreturn has a similar gs:48 DbgPrintf.
- **Suggested fix:** Put them behind a debug flag.
- **Test:** 10k epoll_wait(-1) calls give bounded serial output.

### L-22 · Blocking pipe and tty writes return after at most 4096 bytes

- **Where:** `src/kernel/src/pipe_write.inc:44`, in `WriteToPinnedFdByType pipe branch`.
- **Severity:** low (confidence high).
- **Problem:** This is a deliberate partial-write policy that diverges from Linux, which completes blocking writes.
- **Suggested fix:** Loop until done, except on a signal or EPIPE.
- **Test:** test_pipe_write: a blocking 64 KiB write returns 65536.

### L-23 · pread/pwrite errnos on non-seekable fds differ from Linux; raw -29

- **Where:** `src/kernel/src/syscall_rw.cpp:1292`, in `sys_pwrite64 / sys_pread64`.
- **Severity:** low (confidence high).
- **Problem:** Only pipes get ESPIPE (as a raw -29); other types give EINVAL or EBADF. There is no offset+count overflow check.
- **Suggested fix:** -ESPIPE for every non-positional fd, plus an overflow check.
- **Test:** QEMU: pread and pwrite on a socket, tty and eventfd give ESPIPE.

### L-24 · Datagram write maps every error to ENOTCONN

- **Where:** `src/kernel/src/syscall_rw.cpp:342`, in `WriteToPinnedFdByType (Socket)`.
- **Severity:** low (confidence high).
- **Problem:** EMSGSIZE and ENOMEM are reported as ENOTCONN, and success returns the untruncated count.
- **Suggested fix:** Propagate ret.
- **Test:** QEMU: a 70000-byte UDP write gives EMSGSIZE.

### L-25 · eventfd write rejects count > 8

- **Where:** `src/kernel/src/syscall_rw.cpp:287`, in `WriteToPinnedFdByType (EventFd)`.
- **Severity:** low (confidence high).
- **Problem:** Linux accepts any count >= 8.
- **Suggested fix:** `count < 8`.
- **Test:** QEMU: write(efd, buf, 16) returns 8.

### L-26 · userSource flag ignored by several write backends

- **Where:** `src/kernel/src/syscall_rw.cpp:289`, in `WriteToPinnedFdByType / AppendUserBuffer`.
- **Severity:** low (confidence high).
- **Problem:** sendfile to an eventfd fails with EFAULT, because the eventfd path always copies from user. AppendUserBuffer always copies from user. UDP and unix paths treat both kinds of pointer as raw memory.
- **Suggested fix:** Thread userSource through every branch.
- **Test:** Host: WriteToPinnedFdByType with a kernel buffer for each type.

### L-27 · Cooked/raw tty read ignores O_NONBLOCK and loses the rest of a line on a short read

- **Where:** `src/kernel/src/syscall_rw.cpp:853`, in `sys_read (DevKeyboard)`.
- **Severity:** low (confidence high).
- **Problem:** The remainder of a line beyond count is dropped. There is no O_NONBLOCK and no VMIN/VTIME support, and Ctrl+C does not raise SIGINT ("for now" stub).
- **Suggested fix:** Per-tty line buffer; honour 0x800; send SIGINT to the foreground group.
- **Test:** QEMU: type "hello\n" and read 2 then 4 bytes.

### L-28 · Raw magic errno numbers (pipe/fifo/dsp/maintenance)

- **Where:** `src/kernel/src/named_fifo.inc:50`, in `NamedFifoAttach`.
- **Severity:** low (confidence high).
- **Problem:** 7 sites: -6, -95 and -16 in DspChangeFormat, -28 in MaintenanceBlockTransfer, -29 twice in pread/pwrite, and SIGPIPE=13 as a literal.
- **Suggested fix:** Named constants.
- **Test:** Grep lint.

### L-29 · iovec import and validation duplicated with inconsistent limits

- **Where:** `src/kernel/src/syscall_rw.cpp:1060`, in `PipeVectorIo`.
- **Severity:** low (confidence high).
- **Problem:** 5 copies: PipeVectorIo, UnixStreamWriteVector, writev O_APPEND, writev TCP and UnixRecordVector. The 2 GiB limit gives EINVAL in some and INT64_MAX is allowed in others.
- **Suggested fix:** One ImportIovec() helper with Linux limits.
- **Test:** Shared iovec-import host test.

### L-30 · Wrong or flattened errno values in socket syscalls

- **Where:** `src/kernel/src/syscall_socket.cpp:97`, in `sys_socket`.
- **Severity:** low (confidence high).
- **Problem:** About 10 sites: SockCreate gives ENOMEM, publish gives EMFILE, listen gives EINVAL, sendto gives EIO. A non-socket fd gives EBADF rather than ENOTSOCK in GetSockIdx paths and sendmsg. iovlen>1024 gives EINVAL (Linux: EMSGSIZE).
- **Suggested fix:** Propagate errors; a ResolveSocket() helper.
- **Test:** Host errno table with pipe, file and closed fds.

### L-31 · Raw magic errno numbers, flags and signals in socket/unix/netlink

- **Where:** `src/kernel/src/syscall_socket.cpp:53`, in `sys_socket`.
- **Severity:** low (confidence high).
- **Problem:** About 40 raw errnos, plus raw MSG flags, AF_NETLINK=16 and SIGPIPE=13.
- **Suggested fix:** Named constants.
- **Test:** Build.

### L-32 · listen() marks Listening before registering; failure leaves the socket inconsistent; full backlog gives EAGAIN on blocking connect

- **Where:** `src/kernel/src/syscall_socket.cpp:734`, in `sys_listen`.
- **Severity:** low (confidence high).
- **Problem:** Only 32 server slots exist system-wide, and a failed registration leaves the state Listening. Backlog is ignored and listen on a non-stream socket is allowed.
- **Suggested fix:** Register first, then flip the state; make blocking connect wait.
- **Test:** Filling the 32 slots makes listen fail with the state still Unbound.

### L-33 · Netlink SO_RCVBUF minimum below one packet's accounting, so every send gets ENOBUFS

- **Where:** `src/kernel/src/syscall_socket.cpp:474`, in `sys_setsockopt`.
- **Severity:** low (confidence high).
- **Problem:** The minimum of 8192 is less than sizeof(NetlinkPacket), about 8208.
- **Suggested fix:** Clamp to at least sizeof(NetlinkPacket).
- **Test:** test_netlink_endpoint: SO_RCVBUF=1 then RTM_GETLINK succeeds.

### L-34 · SCM_RIGHTS snapshot ref/unref duplicates FdBumpRefcount/FinalizeClosedFd and has drifted

- **Where:** `src/kernel/src/syscall_socket.cpp:1019`, in `UnixFdSnapFrom`.
- **Severity:** low (confidence high).
- **Problem:** BlockDevice, DevTty and DevDsp are rejected here but supported by FdBumpRefcount. Passing a socket over itself creates an uncollectable cycle (no GC).
- **Suggested fix:** Use FdBumpRefcount and FinalizeClosedFd.
- **Test:** Host per-FdType refcount balance.

### L-35 · recvmmsg ignores timeout and always behaves as MSG_WAITFORONE

- **Where:** `src/kernel/src/syscall_socket.cpp:1579`, in `sys_recvmmsg`.
- **Severity:** low (confidence high).
- **Problem:** The comment is misleading, and DONTWAIT is forced after the first message.
- **Suggested fix:** Implement the timeout; apply DONTWAIT only with WAITFORONE.
- **Test:** recvmmsg with vlen 4 and 2 queued.

### L-36 · Duplicated socket setup and decode logic

- **Where:** `src/kernel/src/syscall_socket.cpp:254`, in `sys_connect`.
- **Severity:** low (confidence high).
- **Problem:** The pipe-pair setup appears twice, UnixSocketData init 4 times, the sockIdx decode about 10 times and the SO_* tables 3 times.
- **Suggested fix:** Helpers.
- **Test:** Existing tests.

### L-37 · Kernel never runs .init_array, but three objects emit global constructors

- **Where:** `src/kernel/src/net.cpp:40`, in `(global initialisers)`.
- **Severity:** low (confidence high).
- **Problem:** linker.ld has no .init_array rule and nothing walks it. BROOK.elf nevertheless carries a 0x18-byte .init_array with entries from net.cpp, apic.cpp and serial_writer.cpp (`_GLOBAL__sub_I_*`). Today those constructors only zero .bss, which is already zero, so nothing is lost. But any future global with a non-trivial constructor would silently stay unconstructed. syscall.cpp and every split file have none (checked).
- **Suggested fix:** Either run .init_array from KernelMain before anything uses globals, or add a link-time check that .init_array is empty and make those globals constinit/constexpr.
- **Test:** A build step (like audit_entry_state.py) that fails if .init_array is non-empty, or a boot assertion that each constructor ran.

### L-38 · Legacy wrappers duplicate their *at() counterparts and have drifted

- **Where:** `src/kernel/src/syscall_path.cpp:258`, in `DoRename`.
- **Severity:** low (confidence high).
- **Problem:** Pairs: rename/renameat, mkdir/mkdirat, unlink/unlinkat, symlink/symlinkat and stat/lstat, which differ in buffer size. FinalizeClosedFd open-codes VfsRelease with `prev <= 1`, hiding underflow.
- **Suggested fix:** Delegate each to the *at form with AT_FDCWD.
- **Test:** QEMU legacy vs *at comparison.

### L-39 · Synthetic /etc and /proc file list copied four times and drifted

- **Where:** `src/kernel/src/syscall_stat.cpp:311` (snippet matches 2 lines: 311, 359), in `do_stat_internal`.
- **Severity:** low (confidence high).
- **Problem:** Only open serves /etc/os-release. Sizes are a fake 64. statx lacks the synthetic files.
- **Suggested fix:** One {path, content} table.
- **Test:** QEMU: stat, statx, access and open agree; st_size equals the bytes read.

### L-40 · fstat returns EBADF for valid fds and masks backend errors

- **Where:** `src/kernel/src/syscall_stat.cpp:419`, in `FstatKernel`.
- **Severity:** low (confidence high).
- **Problem:** DevKlog, DrmPrime, CompositorScene and IntelSyncObj give EBADF. Anon-inode fds report S_IFREG.
- **Suggested fix:** Cover every FdType; propagate errors.
- **Test:** fstat succeeds on every fd type.

### L-41 · getdents64 d_ino does not match st_ino

- **Where:** `src/kernel/src/syscall_dir.cpp:90`, in `sys_getdents64`.
- **Severity:** low (confidence high).
- **Problem:** find -inum, du and the getcwd fallback misbehave. The DRM branch returns 0 rather than EINVAL for a buffer that is too small.
- **Suggested fix:** Carry the backend inode in DirEntry.
- **Test:** QEMU: d_ino equals lstat st_ino on tmpfs and ext2.

### L-42 · readlink("/proc/PID/exe") truncates the PID to 16 bits and skips permission checks

- **Where:** `src/kernel/src/syscall_path.cpp:100`, in `DoReadlink`.
- **Severity:** low (confidence high).
- **Problem:** 65537 wraps to pid 1, and there is no PTRACE_MODE_READ-style check.
- **Suggested fix:** Parse into uint64 with a bound, and add a uid check.
- **Test:** QEMU: readlink("/proc/65537/exe") gives ENOENT.

### L-43 · query_source_commit lacks a caller/ownership check; bad pointer returns -EINVAL

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:125`, in `sys_brook_wm_query_source_commit`.
- **Severity:** low (confidence medium).
- **Problem:** This is the only source-commit syscall without proc. Any window can be read by slot, gated only by the incarnation. An unwritable output returns -EINVAL, not -EFAULT. Cross-process access may be intended (scene service).
- **Suggested fix:** Document it as capability-by-incarnation or add a provider check; return -EFAULT for a bad pointer.
- **Test:** Host test: an unwritable output gives -EFAULT.

### L-44 · Cursor image set/reset lacks authorisation; hand-rolled user copy

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:539`, in `sys_brook_wm_set_cursor_image`.
- **Severity:** low (confidence medium).
- **Problem:** Any process can replace or reset the global cursor (the reset runs before the ProcessCurrent check). The hotspot is unbounded. A page-walk memcpy under addressSpace->mutation duplicates what CopyFromUser would do.
- **Suggested fix:** Gate on the cursor owner or grabber, bound the hotspot, and use CopyFromUser.
- **Test:** Host test: an unprivileged caller gets -EPERM; an out-of-image hotspot gives -EINVAL.

### L-45 · SCANOUT_ADOPT rollback closes fds by number (may close a sibling's replacement)

- **Where:** `src/kernel/src/syscall_brook_gpu.cpp:145`, in `sys_brook_gpu_handoff_ctl`.
- **Severity:** low (confidence medium).
- **Problem:** A sibling can close and reopen an fd number before the CopyToUser failure, so the rollback closes an unrelated descriptor. scene_create already handles this with an FdRef identity compare.
- **Suggested fix:** Pin each fd and verify type and handle before closing, through a helper shared with scene_create.
- **Test:** Host fd-table test: a number replaced between publish and rollback is not closed.

### L-46 · Profiler and crash-entry syscalls lack authorisation and atomicity

- **Where:** `src/kernel/src/syscall_brook_system.cpp:57`, in `sys_brook_profile`.
- **Severity:** low (confidence medium).
- **Problem:** Any process can start or stop the system-wide profiler. set_crash_entry's `if (leader->crashEntry != 0) return -EINVAL;` is a non-atomic check-then-set, accepts kernel-half addresses, and returns EINVAL rather than EBUSY. crash_complete is allowed with no crash in progress.
- **Suggested fix:** Root (or a capability) for profile; a CAS on crashEntry; validate it as a user address; gate crash_complete on crashInProgress.
- **Test:** Host test: concurrent set_crash_entry has exactly one success; a kernel address gives -EFAULT.

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

### L-49 · Per-file DRM mutex held across whole ioctls, including blocking GPU waits

- **Where:** `src/kernel/src/syscall_drm.cpp:455`, in `DrmRenderIoctl`.
- **Severity:** low (confidence medium).
- **Problem:** mappingLock is held across PresentBarrier, submits, host creation, 1 MiB user copies and the zeroing loop, and it also serializes DRM mmap and the WM present and scene lookups. One GPU wait stalls the whole process.
- **Suggested fix:** Lock only the GEM-table lookups and mutations; snapshot with lifetime references.
- **Test:** QEMU: EXECBUFFER blocked in one thread doesn't block mmap in another.

### L-50 · GET_CAP and unknown-nr semantics differ from Linux

- **Where:** `src/kernel/src/syscall_drm.cpp:484`, in `DrmRenderIoctl (DRM_NR_GET_CAP / default)`.
- **Severity:** low (confidence medium).
- **Problem:** Every capability other than PRIME succeeds with 0, including TIMESTAMP_MONOTONIC (Linux gives 1) and unknown ones (Linux gives -EINVAL). An unknown nr returns ENOTTY, where Linux gives EINVAL.
- **Suggested fix:** TIMESTAMP_MONOTONIC=1, -EINVAL for unknown capabilities, and a decision on the unknown-nr errno.
- **Test:** Host test: GET_CAP(6)=1 and GET_CAP(0xffff) gives -EINVAL.

### L-51 · PRIME_FD_TO_HANDLE imports into the host before reserving a GEM slot; bad fd gives EINVAL

- **Where:** `src/kernel/src/syscall_drm.cpp:884`, in `DrmRenderIoctl (DRM_NR_PRIME_FD_TO_HANDLE)`.
- **Severity:** low (confidence medium).
- **Problem:** When the GEM table is full, the host import is never undone. A missing fd returns -EINVAL where Linux returns -EBADF.
- **Suggested fix:** Reserve the slot first or unimport on failure; return -EBADF for a missing fd.
- **Test:** Host fake-GpuAppOps test: a full table leaves ImportResource balanced; fd 999 gives -EBADF.

### L-52 · DevIntelDri dispatch doesn't null-check the handle

- **Where:** `src/kernel/src/syscall_ioctl.cpp:273`, in `sys_ioctl`.
- **Severity:** low (confidence medium).
- **Problem:** The DevDri path returns -EBADF on a null handle; the Intel path passes null on.
- **Suggested fix:** `if(!fde->handle) return -EBADF;`
- **Test:** Host test with a null-handle DevIntelDri entry.

### L-53 · madvise hints never check the range is mapped; common advice values return EINVAL; fadvise64/mbind are silent stubs

- **Where:** `src/kernel/src/syscall_memory.cpp:611`, in `sys_madvise`.
- **Severity:** low (confidence medium).
- **Problem:** DONTDUMP and similar give EINVAL (OpenSSL and Chromium use them). fadvise64 doesn't validate the fd.
- **Suggested fix:** Validate mapping coverage; accept harmless advice values.
- **Test:** QEMU test_madvise_hints.

### L-54 · FUTEX_WAIT dereferences the user VA directly under the IRQ-off g_futexLock

- **Where:** `src/kernel/src/syscall_futex.cpp:591`, in `sys_futex`.
- **Severity:** low (confidence medium).
- **Problem:** Safe today only because the leaf is pre-checked. With SMAP, or a gap in that check, it faults under the lock.
- **Suggested fix:** A physmap read like PiMappedWord.
- **Test:** test_futex_pool plus a QEMU wait/munmap race.

### L-55 · Named shm and memfd_create argument handling diverges from Linux

- **Where:** `src/kernel/src/syscall_memfd.cpp:211`, in `NamedShmOpen / sys_memfd_create`.
- **Severity:** low (confidence medium).
- **Problem:** O_TRUNC always gives EOPNOTSUPP. The seal default differs from Linux. mode is ignored. memfd_create doesn't validate the name. MFD_NOEXEC_SEAL and MFD_EXEC give EINVAL. There are 3 divergent MemFdData construction sites.
- **Suggested fix:** MemFdCreate(); allow O_TRUNC when unmapped; validate the name.
- **Test:** test_memfd_seals; QEMU shm_open O_TRUNC.

### L-56 · select rejects tv_usec >= 1e6; pselect6 doesn't write back remaining time

- **Where:** `src/kernel/src/syscall_poll.cpp:508`, in `ParseWaitTimeout`.
- **Severity:** low (confidence medium).
- **Problem:** Linux normalises tv_usec for select.
- **Suggested fix:** Normalise microseconds.
- **Test:** select {0,1500000} waits about 1.5 s.

### L-57 · timerfd_create and epoll_create publish the fd and then patch its flags

- **Where:** `src/kernel/src/syscall_timerfd.cpp:84`, in `sys_timerfd_create`.
- **Severity:** low (confidence medium).
- **Problem:** A sibling fork+exec can see the fd without CLOEXEC.
- **Suggested fix:** FdTablePublishOwned with the flags preset (as eventfd2 and signalfd4 do).
- **Test:** Fork+exec race never inherits a TFD_CLOEXEC fd.

### L-58 · VnodeRef check-then-act race; VnodeUnref doesn't detect underflow; orphaned VnodeTryRef comment

- **Where:** `src/kernel/src/syscall_internal.h:62`, in `VnodeRef / VnodeUnref`.
- **Severity:** low (confidence medium).
- **Problem:** A vnode can be revived from zero. prev==0 leads to a second VfsClose. The "Try to take a reference..." comment has no function under it.
- **Suggested fix:** A CAS-based VnodeTryRef; treat prev==0 as a BUG.
- **Test:** Host fake-vnode underflow test.

### L-59 · CreatePipe publishes descriptors before setting endpoint flags

- **Where:** `src/kernel/src/syscall_pipe.cpp:74`, in `CreatePipe`.
- **Severity:** low (confidence medium).
- **Problem:** The flags, status flags and CLOEXEC are written after FdAlloc and without the fd lock, so a sibling's dup or fork can see a write end with flags=0 or no CLOEXEC. Every error is flattened to EMFILE.
- **Suggested fix:** Publish the entry with its flags preset (FdTablePublishOwned).
- **Test:** Host: FdAlloc with initial flags.

### L-60 · /dev/tty and eventfd single-waiter fields set without a lock

- **Where:** `src/kernel/src/syscall_rw.cpp:1005`, in `sys_read (DevTty, EventFd)`.
- **Severity:** low (confidence medium).
- **Problem:** Waiters are registered outside the lock and concurrent readers overwrite each other, so wakeups fall back to the 10 ms timeout (latency that matters for the 60 fps goal).
- **Suggested fix:** Register under the lock with wait groups.
- **Test:** QEMU: two eventfd readers; one write wakes a reader within 1 ms.
- **Status (fix pass):** partly fixed in `863f802a` (eventfd). The /dev/tty readerWaiter remains.

### L-61 · Zero-length netlink send returns ENODATA

- **Where:** `src/kernel/src/syscall_netlink.inc:59`, in `NetlinkSend`.
- **Severity:** low (confidence medium).
- **Problem:** Linux returns 0.
- **Suggested fix:** Return 0.
- **Test:** test_netlink_endpoint.

### L-62 · Kernel and user pointers share one parameter, distinguished by an address heuristic

- **Where:** `src/kernel/src/syscall_unix_socket.cpp:394`, in `UnixSocketWriteSome`.
- **Severity:** low (confidence medium).
- **Problem:** sendmsg passes a kmalloc buffer to sys_sendto. Fixing sendto with CopyFromUser would break this.
- **Suggested fix:** Explicit user and kernel entry points.
- **Test:** Host test of both variants.

### L-63 · lseek succeeds on sockets, ttys, keyboard and epoll fds

- **Where:** `src/kernel/src/syscall_fd.cpp:456`, in `sys_lseek`.
- **Severity:** low (confidence medium).
- **Problem:** Should be ESPIPE. A VfsStat error becomes EINVAL, and seekPos is updated unlocked.
- **Suggested fix:** A seekable-type allowlist; propagate errors.
- **Test:** QEMU: lseek on socketpair, tty and epoll gives ESPIPE.

### L-64 · AT_EMPTY_PATH handling duplicated and inconsistent (7 sites)

- **Where:** `src/kernel/src/syscall_stat.cpp:52`, in `AccessAt`.
- **Severity:** low (confidence medium).
- **Problem:** O_PATH fds are rejected inconsistently, and utimensat(AT_FDCWD, "", AT_EMPTY_PATH) gives EBADF.
- **Suggested fix:** AtLookup::InitEmpty() shared by all.
- **Test:** QEMU matrix of dirfd kind × *at syscall.

### L-65 · Unsupported cases return wrong errno or a stub result (fstatfs ENOSYS, sync errors, umount2 flags 0)

- **Where:** `src/kernel/src/syscall_stat.cpp:778`, in `sys_fstatfs`.
- **Severity:** low (confidence medium).
- **Problem:** fstatfs on a pipe gives ENOSYS. sync propagates errors. umount2(path, 0) gives EINVAL.
- **Suggested fix:** Pseudo-fs magic numbers; sync returns 0; a documented errno for umount2.
- **Test:** QEMU checks.

### L-66 · input_pop reports the pointer position at pop time and drops scroll deltas

- **Where:** `src/kernel/src/syscall_brook_system.cpp:179`, in `sys_brook_input_pop`.
- **Severity:** low (confidence low).
- **Problem:** Every mouse event in a batch gets the current cursor position. deltaX and deltaY are never copied out. The header comment describes the wrong direction.
- **Suggested fix:** Record the position when queueing, and carry the deltas in the reserved bytes (versioned ABI).
- **Test:** Host test: two queued moves and a scroll pop with their own coordinates and the scroll delta.

### L-67 · RESOURCE_CREATE_BLOB failure after Submit3D leaks a host virgl object

- **Where:** `src/kernel/src/syscall_drm.cpp:599`, in `DrmRenderIoctl (RESOURCE_CREATE_BLOB)`.
- **Severity:** low (confidence low).
- **Problem:** The blob_id create command was already submitted, and the drop path skips RetireResource when gres==0.
- **Suggested fix:** Submit a matching destroy, or reorder the calls.
- **Test:** Host fake-GpuAppOps test asserting cleanup.

### L-68 · TRANSFER and WAIT on an imported GEM run on the exporter's GPU context

- **Where:** `src/kernel/src/syscall_drm.cpp:701`, in `DrmRenderIoctl (TRANSFER_TO/FROM_HOST, WAIT)`.
- **Severity:** low (confidence low).
- **Problem:** The importer submits into the exporter's ctx (ordering and isolation), and the box is not bounds-checked. It may be intentional.
- **Suggested fix:** Use dctx->owner->ctxId and bound the box.
- **Test:** Host fake-GpuAppOps test: an import-then-transfer submits on the importer's ctx.

### L-69 · execve post-commit sanity check kills the process rather than failing pre-commit

- **Where:** `src/kernel/src/syscall_exec.cpp:481`, in `sys_execve`.
- **Severity:** low (confidence low).
- **Problem:** A lazily mapped entry page would kill a valid program.
- **Suggested fix:** A mapping-range check before commit.
- **Test:** test_exec_page_cache with a lazy interpreter.

### L-70 · Netlink autobind uses tgid without the pid fallback

- **Where:** `src/kernel/src/syscall_netlink.inc:29`, in `NetlinkBind`.
- **Severity:** low (confidence low).
- **Problem:** Elsewhere the code uses tgid?tgid:pid, so a leader with tgid 0 would bind port 1.
- **Suggested fix:** The same helper.
- **Test:** test_netlink_endpoint getsockname.

### L-71 · Handle retention and allocation while holding the per-process fd spinlock

- **Where:** `src/kernel/src/syscall_fd.cpp:363`, in `DuplicateFd`.
- **Severity:** low (confidence low).
- **Problem:** Calls kmalloc and per-type Ref functions under ProcessFdLock, with an undocumented lock order. Not confirmed whether they can sleep.
- **Suggested fix:** Pin, copy, unlock, then retain.
- **Test:** dup/fork stress with lock debugging.


## Info

### I-01 · Dispatch table uses magic slot numbers where named constants exist; no duplicate-slot check

- **Where:** `src/kernel/src/syscall.cpp:553`, in `SyscallTableInit`.
- **Severity:** info (confidence high).
- **Problem:** No duplicate or overwritten slots and no unregistered handlers were found. But 0xB08 and 0xB09 use literals despite BROOK_SYS_WM_WINDOW_STATE and BROOK_SYS_WM_PLACE_WINDOW. Many other slots are bare numbers (0xB00-0xB07, 0xB0A, 0xB10-0xB16, 0xB40, 324, 435, 113/114/117/119, 253-255, 294). Uniqueness is checked only by scattered static_asserts, one of which lives in syscall_brook_gpu.cpp.
- **Suggested fix:** Name every slot and build the table from a constexpr {num, fn} array with a uniqueness check.
- **Test:** static_assert or host test that registered numbers are distinct.

### I-02 · Misplaced or orphaned comments

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:228`, in `sys_brook_pointer_speed`.
- **Severity:** info (confidence high).
- **Problem:** The 0xB0C taskbar comment sits on pointer_speed. The 0xB01 WM_SET_APP_ID comment sits above set_client_socket. set_crash_entry refers to "commit d" and files/crash-dump-plan.md. Both misplacements predate the split.
- **Suggested fix:** Move the comments and drop the stale references.
- **Test:** None.

### I-03 · Stale or misleading comments (memory/memfd/futex/process)

- **Where:** `src/kernel/src/syscall_memory.cpp:525`, in `sys_mremap (header comment)`.
- **Severity:** info (confidence high).
- **Problem:** Stale comments: mremap does honour FIXED; the PageCachePreload size is 16 MB, not 4; the MemFdHandleUserFault description; "SyntheticMem" in memfd_create; FdAlloc; "handled in idt.cpp"; futex_pi.inc says it is included by syscall.cpp (a split artefact); the prctl comment. retryBudget=0 is unbounded.
- **Suggested fix:** Update the comments.
- **Test:** none.

### I-04 · Dead code and duplicated logic (fork/clone, vfork wake, bin-prefix tables)

- **Where:** `src/kernel/src/syscall_process.cpp:177` (snippet matches 2 lines: 177, 275), in `sys_fork / sys_clone`.
- **Severity:** info (confidence high).
- **Problem:** sys_fork duplicates sys_clone's register block. The vfork wake appears 3 times. tgidExiting is cleared twice. There are 2 bin-prefix tables. FileMappingBacking::size is unused, and some checks are redundant.
- **Suggested fix:** Route fork through clone; shared helpers.
- **Test:** Existing fork/exec tests.

### I-05 · Orphaned or misleading comments and redundant declarations after the split

- **Where:** `src/kernel/src/syscall_common.cpp:91`, in `StrEq`.
- **Severity:** info (confidence high).
- **Problem:** The "Error codes" banner sits over signal helpers and "sys_open (2)" over StrEq in common. In poll, "Regular files are always ready" is above the Path POLLNVAL line, EpollPollReady is declared again, SC_IO is hard-coded as 1u<<5, and logPoll has no prefix. In timerfd, "using LAPIC tick count" is stale and ClockNowNs/WaitWakeupTick are declared again. EpollInstance::waiter is unused.
- **Suggested fix:** Clean up.
- **Test:** none.

### I-06 · DspHandleRef dead and duplicated; stale DSP header comments

- **Where:** `src/kernel/src/syscall_dsp.cpp:27`, in `DspHandleRef`.
- **Severity:** info (confidence high).
- **Problem:** DspHandleRef is never called; syscall_fd.cpp inlines the same increment. syscall_dsp.h comments ("further below", "defined earlier in the file") describe the monolith (a split artefact). DSP_HW_RATE and MIXER_HW_RATE are parallel constants.
- **Suggested fix:** Use DspHandleRef or delete it; fix the comments.
- **Test:** Dup/fork refcount check.

### I-07 · Misleading or orphaned comments in syscall_rw.cpp

- **Where:** `src/kernel/src/syscall_rw.cpp:1214`, in `sys_sendfile / WriteToPinnedFdByType`.
- **Severity:** info (confidence high).
- **Problem:** sendfile is not a stub. The "Shared write dispatch" comment sits above AppendUserBuffer. "The pin above" refers to the caller. The eventfd comments sit above the SignalFd and SyncFile lines.
- **Suggested fix:** Move or fix the comments.
- **Test:** n/a

### I-08 · Dead code and orphaned or misleading comments in socket files

- **Where:** `src/kernel/src/syscall_socket.cpp:89`, in `sys_socket`.
- **Severity:** info (confidence high).
- **Problem:** An empty AF_INET6 branch, 3 empty blocks, `(void)` casts after real uses, a wrong lease comment in recvfrom, UnixFdSnapRelease declared in two places and an unused UnixFdQueueFull.
- **Suggested fix:** Clean up.
- **Test:** Build.

### I-09 · Random helpers became external symbols in the global namespace

- **Where:** `src/kernel/src/syscall_common.cpp:15`, in `SoftRandU64 / RdrandU64`.
- **Severity:** info (confidence high).
- **Problem:** Both were file-static at global scope in syscall.cpp. They are shared now (getrandom and the rw /dev/urandom path), so the split dropped `static` and they are `::SoftRandU64`/`::RdrandU64`, the only split-created globals outside `namespace brook`. Moving them was out of scope (it would be a rename).
- **Suggested fix:** Move them into `brook` (or a random module, see the getrandom CSPRNG entry) in the fix pass.
- **Test:** Build only.

### I-10 · Leftover forward declarations and using-declarations in syscall.cpp after the split

- **Where:** `src/kernel/src/syscall.cpp:28`, in `(file scope)`.
- **Severity:** info (confidence high).
- **Problem:** The residual dispatcher still declares MemFdHandleUserFault/FileMapHandleUserFault and the `using brook::UserBufferReadable/...` lines from the monolith's preamble. Nothing in syscall.cpp uses them now. They were kept to keep the move pure.
- **Suggested fix:** Delete them.
- **Test:** Build only.

### I-11 · Maintenance-block reservation state is exported as raw globals

- **Where:** `src/kernel/src/syscall_maintenance_block.h:27`, in `maintenanceBlockLock et al.`.
- **Severity:** info (confidence high).
- **Problem:** open (BLK publish) and stat (temporary holders) touch the lock and counters directly, so the split had to export them. maintenanceBlockOwner stays static. The reservation protocol is now spread over three files with no single owner. This applies only to write-test and installer builds.
- **Suggested fix:** Wrap it in MaintenanceBlockPublish/MaintenanceBlockTemporaryHold helpers inside syscall_maintenance_block.cpp and make the globals static again.
- **Test:** Existing AHCI write-test QEMU path (BROOK_AHCI_WRITE_TEST_HOOKS).

### I-12 · DRM types were in an anonymous namespace but are used by six files

- **Where:** `src/kernel/src/syscall_drm.h:87`, in `DrmCtx / DrmGemObj / DrmResource`.
- **Severity:** info (confidence high).
- **Problem:** The split moved them to `brook::` (in syscall_drm.h) because memory, fd, ioctl, socket, wm and desktop all dereference them. syscall_drm.h is therefore a wide internal API (GEM table, mapping guard, PRIME, capture completion) and nothing is encapsulated.
- **Suggested fix:** Give the consumers narrow accessors (for example DrmLookupForMap, DrmPrimeRetain) and shrink the header.
- **Test:** Existing DRM/virgl QEMU tests.

### I-13 · About 36 raw negative errno literals in VFS-facing files

- **Where:** `src/kernel/src/syscall_open.cpp:67`, in `OpenProcMemFd`.
- **Severity:** info (confidence high).
- **Problem:** Literals include -20 and -40 although the constants exist. AccessAt and stat treat a raw -1 from the VFS as ENOENT.
- **Suggested fix:** Named constants; stop the VFS returning a bare -1.
- **Test:** Grep lint.

### I-14 · Orphaned and misleading comments left by the split (VFS files)

- **Where:** `src/kernel/src/syscall_open.cpp:26`, in `OpenProcMemFd`.
- **Severity:** info (confidence high).
- **Problem:** "sys_write (1)" sits above OpenProcMemFd. "openat delegates to sys_open" is wrong. The renderD128 comment sits on the 129 branch. The mkdirat header is above mknodat. The access header is above forward declarations. The CloseProcessFdForced comment sits above FdReleaseOwnedEntry.
- **Suggested fix:** Move or delete the comments.
- **Test:** none.

### I-15 · Dead code and unused declarations in syscall.cpp

- **Where:** `src/kernel/src/syscall.cpp:195`, in `BrookSyscallDispatcher / file scope`.
- **Severity:** info (confidence medium).
- **Problem:** Nothing jumps to .Lsyscall_invalid. The MemFdHandleUserFault/FileMapHandleUserFault forward declarations and the `using brook::...` lines are unused after the split. `mov %%gs:16, %%r12` appears unused later (low confidence).
- **Suggested fix:** Delete them after confirming the r12 load.
- **Test:** Build plus QEMU boot.

### I-16 · Duplicated debug-frame/overlay and GEM-capture logic; inconsistent owner identity

- **Where:** `src/kernel/src/syscall_brook_desktop.cpp:363`, in `sys_brook_debug_frame`.
- **Severity:** info (confidence medium).
- **Problem:** debug_frame and desktop_overlay repeat ops 0-3 nearly verbatim. debug_frame keys the owner by thread incarnation, while 5 other sites use the leader incarnation, so a request on one thread and a read on another gets ESTALE. GEM capture validation is duplicated between wm_present_gres and scene_ctl CAPTURE.
- **Suggested fix:** DebugFrameOps(), an OwnerIncarnation(proc) helper and ValidateCaptureGem().
- **Test:** Host test: debug-frame request and read from two threads of one process both succeed.

### I-17 · Misleading comments and dead code in DRM/ioctl

- **Where:** `src/kernel/src/syscall_drm.cpp:259`, in `(file scope), DrmRenderIoctl`. Also in: syscall_drm.cpp, syscall_drm.h, syscall_ioctl.cpp.
- **Severity:** info (confidence medium).
- **Problem:** The comment claims Linux ignores the size bits; it doesn't. The "Per-fd DRM render-node state" comment in syscall_drm.h now sits above DRM_MAX_GEM rather than DrmCtx (a split artefact). The "Intel file type is not published yet" comment looks stale. DrmVirtgpuContextSetParam is unused, and GpuContextParam layout has no static_assert. The `gobj &&` null check is inconsistent with the later unchecked use. The UserBufferReadable before CopyFromUser is redundant. The refusal counters are not atomic.
- **Suggested fix:** Correct the comments, add static_asserts and remove the redundancies.
- **Test:** Compile-only.
