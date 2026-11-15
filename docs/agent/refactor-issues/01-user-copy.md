# User-memory copies still dereferencing directly

_10 open entries (6 medium, 4 low). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** The remaining places where a handler or driver reads or writes user memory directly, or hands a raw user pointer to a lower layer, instead of using `CopyFromUser`/`CopyToUser`/`CopyUserCString*` (see "Validate-then-dereference: the design" in the index). Covers the `readv`/`writev` iovec array, socket syscall arguments, GPU submit/upload, the WM `get_*` calls, strace string printing, `FUTEX_WAIT`, and the helpers that blur kernel and user pointers.
- **Why it matters:** This is the largest remaining class of crash-the-kernel bugs from a sibling `munmap`. The high-severity direct-dereference entries are already fixed; what remains is medium or low.
- **Known sites without an ID:** Several known sites have **no entry ID** yet; they were found by the post-pass grep and are listed in "Validate-then-dereference: the design" in the index: `VfsRead`/`VfsWrite` on regular files (`sys_read`, `sys_write`, `pread64`/`pwrite64`: needs a user-aware VFS copy or a bounce buffer in `syscall_rw.cpp`), `sendmmsg`/`recvmmsg` message vectors, `setsockopt`/`getsockopt` values and lengths, `socketpair` result array, the DSP write path and netlink request staging. Give them IDs (suggest `H-28`..) when a branch picks them up, and add them to this file.
- **Dependencies:** None. Do this before 05 (sockets) and 06 (pipes/tty/devices), whose fixes should use the same helpers. M-35 should be done together with M-28 (same loop).
- **Suggested order:** M-28 with M-35 (and the `VfsRead`/`VfsWrite` bounce buffer, since they share the iovec loop), then L-29/L-62 (one import helper, no address heuristic), then M-36, then M-01/M-02 (GPU and WM), then M-04, L-26, L-54.
- **Test approach:** Extend `src/tests/test_syscall_user_memory` (raced-unmap mode in `race.cpp`, kernel-pointer canary, copy hooks for lock-held checks). Add one case per handler: kernel-half pointer, unmapped pointer, and unmap after validation. `rw.cpp` and `socket.cpp` are the natural homes. `test_pipe_write` covers the pipe staging.
- **Size:** Medium: about 10 entries, 2 or 3 sittings. The unnumbered VFS read/write bounce buffer is the big piece.

## Entries

### Medium

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

### M-04 · strace prints user path strings with %s straight from user memory

- **Where:** `src/kernel/src/syscall_strace.cpp:247`, in `SyscallDispatchTraced`.
- **Severity:** medium (confidence high).
- **Problem:** open, openat and execve tracing passes a0/a1 straight to SerialPrintf. A kernel pointer leaks kernel memory to serial; an unmapped or unterminated pointer faults the kernel. Only reachable with strace on.
- **Suggested fix:** CopyUserCString into a bounded local buffer and print "<fault>" on failure.
- **Test:** Host test: tracing openat with a kernel or unmapped path does not fault and logs "<fault>".

### M-28 · TOCTOU double-fetch of the iovec array in readv/writev

- **Where:** `src/kernel/src/syscall_rw.cpp:1172`, in `sys_writev`.
- **Severity:** medium (confidence high).
- **Problem:** The O_APPEND "snapshot" is a raw memcpy from user memory. The generic loops read iov_base and iov_len twice, directly from user memory.
- **Suggested fix:** CopyFromUser the iovec array once at entry.
- **Test:** Host iovec-import test; QEMU iovec mutation race.

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

### Low

### L-26 · userSource flag ignored by several write backends

- **Where:** `src/kernel/src/syscall_rw.cpp:289`, in `WriteToPinnedFdByType / AppendUserBuffer`.
- **Severity:** low (confidence high).
- **Problem:** sendfile to an eventfd fails with EFAULT, because the eventfd path always copies from user. AppendUserBuffer always copies from user. UDP and unix paths treat both kinds of pointer as raw memory.
- **Suggested fix:** Thread userSource through every branch.
- **Test:** Host: WriteToPinnedFdByType with a kernel buffer for each type.

### L-29 · iovec import and validation duplicated with inconsistent limits

- **Where:** `src/kernel/src/syscall_rw.cpp:1060`, in `PipeVectorIo`.
- **Severity:** low (confidence high).
- **Problem:** 5 copies: PipeVectorIo, UnixStreamWriteVector, writev O_APPEND, writev TCP and UnixRecordVector. The 2 GiB limit gives EINVAL in some and INT64_MAX is allowed in others.
- **Suggested fix:** One ImportIovec() helper with Linux limits.
- **Test:** Shared iovec-import host test.

### L-54 · FUTEX_WAIT dereferences the user VA directly under the IRQ-off g_futexLock

- **Where:** `src/kernel/src/syscall_futex.cpp:591`, in `sys_futex`.
- **Severity:** low (confidence medium).
- **Problem:** Safe today only because the leaf is pre-checked. With SMAP, or a gap in that check, it faults under the lock.
- **Suggested fix:** A physmap read like PiMappedWord.
- **Test:** test_futex_pool plus a QEMU wait/munmap race.

### L-62 · Kernel and user pointers share one parameter, distinguished by an address heuristic

- **Where:** `src/kernel/src/syscall_unix_socket.cpp:394`, in `UnixSocketWriteSome`.
- **Severity:** low (confidence medium).
- **Problem:** sendmsg passes a kmalloc buffer to sys_sendto. Fixing sendto with CopyFromUser would break this.
- **Suggested fix:** Explicit user and kernel entry points.
- **Test:** Host test of both variants.
