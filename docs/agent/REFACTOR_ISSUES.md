# Refactor issue log (index)

_Started 15 November 2026 during the pure-move split of `syscall.cpp` (branch `work/syscall-split`, see [syscall-split-plan.md](syscall-split-plan.md)). Nothing listed here was changed by the split. The log drives the separate fix pass, which adds test coverage first._

The log used to be one 1,700-line file. It is now split into work chunks under [refactor-issues/](refactor-issues/), so one theme can be taken on per sitting or per branch. **Entry IDs (H-xx, M-xx, L-xx, I-xx) are unchanged**; each appears exactly once, either in a chunk (open or partly fixed) or in [done.md](refactor-issues/done.md) (fixed). Other docs and commits that cite an ID can find it with `grep -rn "### M-45" docs/agent/refactor-issues`.

## Counts

| Severity | Total | Fixed (done.md) | Open or partly fixed (chunks) |
|---|---|---|---|
| High | 27 | 22 | 5 |
| Medium | 67 | 3 | 64 |
| Low | 71 | 1 | 70 |
| Info | 17 | 0 | 17 |
| **All** | **182** | **26** | **156** |

Open includes entries only partly fixed (M-64, L-60), hardened (H-04), deferred (H-20, H-22) and awaiting a decision (H-03, H-21). Entries marked confidence "low" are the reviewer's suspicion and need confirming before anyone fixes them.

## Chunks

Open counts are H/M/L/I. File numbers group related themes and are not the work order; see "Suggested order across chunks" below.

| File | Theme | Open H/M/L/I | Total | Depends on / coordinate with | Needs Ian | Tackle first |
|---|---|---|---|---|---|---|
| [01-user-copy.md](refactor-issues/01-user-copy.md) | User-memory copies still dereferencing directly | 0/6/4/0 | 10 | none; before 05, 06 | no | M-28 (with M-35) |
| [02-authorisation.md](refactor-issues/02-authorisation.md) | Brook syscall authorisation (display-server capability) | 3/0/3/0 | 6 | none | **yes** | H-03 (decision) |
| [03-file-descriptions-and-fd-table.md](refactor-issues/03-file-descriptions-and-fd-table.md) | Open-file descriptions (H-20), fd publish races | 1/7/6/0 | 14 | coordinate with 05, 07 | no | M-45 |
| [04-process-clone-exec-signals.md](refactor-issues/04-process-clone-exec-signals.md) | clone, exec, setpgid, prctl, signals | 0/8/3/0 | 11 | M-15 with 09 (M-49) | no | M-11 |
| [05-sockets.md](refactor-issues/05-sockets.md) | AF_UNIX, inet, netlink semantics | 0/9/8/0 | 17 | 01, 03 (M-63) | no | M-41 |
| [06-pipes-tty-devices-rw.md](refactor-issues/06-pipes-tty-devices-rw.md) | Pipes, tty, klog, read/write semantics | 0/6/4/0 | 10 | 01 | no | M-30 |
| [07-wait-and-wakeup.md](refactor-issues/07-wait-and-wakeup.md) | poll, epoll, eventfd, futex, waiters | 0/10/5/0 | 15 | 03 (M-19, M-58) | no | M-60 |
| [08-memory-time-sysinfo.md](refactor-issues/08-memory-time-sysinfo.md) | mmap, memfd, madvise, time, rlimit | 0/6/5/0 | 11 | none | no | M-17 |
| [09-vfs-stat-paths.md](refactor-issues/09-vfs-stat-paths.md) | open flags, paths, stat, synthetic files | 0/4/8/0 | 12 | M-49 with 04 (M-15) | no | M-46 |
| [10-drm-gpu-ioctl.md](refactor-issues/10-drm-gpu-ioctl.md) | DRM/virtio ioctls, GPU, H-22 lifetime | 1/5/11/0 | 17 | H-22 with 11 (M-51) | no (H-22 needs virgl check) | M-06 |
| [11-wm-and-framebuffer.md](refactor-issues/11-wm-and-framebuffer.md) | WM syscalls, framebuffer | 0/2/4/0 | 6 | M-51 with 10 (H-22) | no | M-51 |
| [12-errno-and-diagnostics.md](refactor-issues/12-errno-and-diagnostics.md) | Raw errno literals, strace, serial logging | 0/1/8/1 | 10 | run early or per file; see chunk | no | L-03 |
| [13-hygiene-and-split-artefacts.md](refactor-issues/13-hygiene-and-split-artefacts.md) | Dead code, stale comments, split leftovers, L-37 | 0/0/1/16 | 17 | do last | no | L-37 |
| [14-single-responsibility-refactors.md](refactor-issues/14-single-responsibility-refactors.md) | Follow-on file splits (net.cpp, compositor.cpp, window.cpp, ext2_vfs.cpp, scheduler.cpp, syscall_* leftovers) | no IDs | 0 | after the bug chunk touching the same file | no | `net.cpp` debug channel move |
| [done.md](refactor-issues/done.md) | Fixed entries, grouped by fix commit | n/a | 26 | n/a | no | n/a |

## Suggested order across chunks

1. **02 authorisation:** raise the decision with Ian now; it blocks only its own chunk, so the rest can proceed.
2. **01 user copies**, then **12 errno clean-up** as a quick mechanical pass (merge it before the other branches touch the same files).
3. **04 process/exec**, **09 VFS/paths** (M-15 and M-49 share one path limit), **06 pipes/tty/devices**.
4. **03 fd table** (the small entries), **05 sockets**, **07 wait/wakeup** (the last needs the 60 fps measurements; M-60 is a cheap start).
5. **08 memory**, **10 DRM/GPU** and **11 WM** (do H-22 with M-51).
6. **H-20** (shared open-file description) on its own branch with a QEMU shell test.
7. **13 hygiene** last, then **14 single-responsibility** moves, each after the bug chunk that touches the same file.

The original ordering, kept for reference: test scaffolding, then the no-validation kernel read/write entries, authorisation, spinlock-held copies and `CLONE_SETTLS`, the validate-then-dereference class, then Linux-semantics entries. The first four steps are done.

## Where the designs live

- Authorisation proposal (H-03, H-04, H-21): [02-authorisation.md](refactor-issues/02-authorisation.md)
- Shared open-file description (H-20): [03-file-descriptions-and-fd-table.md](refactor-issues/03-file-descriptions-and-fd-table.md)
- Wakeups changed and remaining (H-26, M-18): [07-wait-and-wakeup.md](refactor-issues/07-wait-and-wakeup.md)
- GPU_ATTACH_WINDOW lifetime (H-22): [10-drm-gpu-ioctl.md](refactor-issues/10-drm-gpu-ioctl.md)
- Split artefacts: [13-hygiene-and-split-artefacts.md](refactor-issues/13-hygiene-and-split-artefacts.md)
- Single-responsibility candidates: [14-single-responsibility-refactors.md](refactor-issues/14-single-responsibility-refactors.md)

## Fix pass status (15 November 2026, branch `work/syscall-fixes`)

The fix pass took all 27 high entries plus the wakeup-related medium entries. Each entry now carries a **Status (fix pass)** line. Every high entry was confirmed against the code before it was changed; none was refuted outright, and H-26 is partly refuted (see its entry).

| Outcome | Entries |
|---|---|
| Fixed, with a host test that fails on the previous code | H-01, H-02, H-05, H-06, H-09, H-10, H-11, H-12, H-13, H-14, H-15, H-16, H-17, H-18, H-23, H-24, H-26, H-27, M-18, M-23, M-27 |
| Fixed; the test encodes the fix, but the old tree cannot build against it or the changed path is kernel-only | H-07, H-08, H-19, H-25, L-09 |
| Hardened only (policy still open) | H-04 |
| Partly fixed | M-64, L-60 (single-slot waiters remain on read/write/accept and on /dev/tty) |
| Open: policy decision for Ian | H-03, H-21; see the authorisation proposal in [02-authorisation.md](refactor-issues/02-authorisation.md) |
| Deferred, with a design in its chunk file | H-20 ([03](refactor-issues/03-file-descriptions-and-fd-table.md)), H-22 ([10](refactor-issues/10-drm-gpu-ioctl.md)) |

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

## How this was collected

Seven read-only reviewers each took one group of the new files. Each reviewer read the code in full, checked callees where severity depended on them, and reported every finding as file, function and an exact code snippet. The snippets were located mechanically in the split files, so every **Where** line points at the post-split location on this branch.

I spot-checked the high-severity entries against the code: getdents64, the open kernel-pointer branch, `input_pop`, the abstract AF_UNIX name copy and `CLONE_SETTLS`. Entries marked confidence "low" are the reviewer's suspicion and need confirming before anyone fixes them. Where a reviewer found the same pattern in many places, it is one entry with a count. So the totals count distinct problems, not lines.

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
