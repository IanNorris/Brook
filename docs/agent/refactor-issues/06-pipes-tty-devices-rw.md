# Pipes, tty, devices and read/write semantics

_10 open entries (6 medium, 4 low). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** `read`/`write` length handling, pipe atomicity and partial writes, tty stdin/read behaviour, default serial stdout, `/dev/klog`, pread/pwrite errnos, TIOCSPGRP.
- **Why it matters:** Mix of data-corruption (klog cursor size, a tty write fed back into its own input) and compat gaps.
- **Dependencies:** After 01 for anything that touches the read/write copy path. M-47 and M-33 (klog) go together. L-28 (magic errno numbers) is in the errno clean-up, 12.
- **Suggested order:** M-30, M-47 then M-33, M-31, M-34, M-29, L-22, L-27, L-23, L-10. L-28 (magic errno numbers) is in 12.
- **Test approach:** `test_syscall_user_memory/rw.cpp`, `test_pipe_write`, `test_pipe_poll`.
- **Size:** Medium: 10 entries, 1 or 2 sittings.

## Entries

### Medium

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

### M-47 · /dev/klog cursor allocated as 4 bytes but deep-copied as 8

- **Where:** `src/kernel/src/syscall_fd.cpp:34`, in `DevKlogDeepCopy`.
- **Severity:** medium (confidence high).
- **Problem:** Open allocates uint32_t (`auto* cursor = static_cast<uint32_t*>(kmalloc(sizeof(uint32_t)));`), so dup and fork read 8 bytes from a 4-byte allocation (a heap out-of-bounds read).
- **Suggested fix:** One cursor type with sizeof(*src).
- **Test:** Host deep-copy test; QEMU dup and fork of /dev/klog.

### Low

### L-10 · TIOCSPGRP accepts any value, truncates it to uint16 and checks no permissions; dead code in TIOCGPGRP

- **Where:** `src/kernel/src/syscall_ioctl.cpp:497` (snippet matches 2 lines: 497, 614), in `sys_ioctl (TIOCSPGRP, 2 copies)`.
- **Severity:** low (confidence high).
- **Problem:** Negative or nonexistent pgids are accepted and values over 65535 wrap. There is no session check or SIGTTOU. The `*pgrp = 1` branch is unreachable, and ProcessCurrent() is refetched 6 times.
- **Suggested fix:** Validate against the process table and session, returning Linux errnos.
- **Test:** Host test: TIOCSPGRP(-1) gives -EINVAL; a foreign session gives -EPERM.

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

### L-27 · Cooked/raw tty read ignores O_NONBLOCK and loses the rest of a line on a short read

- **Where:** `src/kernel/src/syscall_rw.cpp:853`, in `sys_read (DevKeyboard)`.
- **Severity:** low (confidence high).
- **Problem:** The remainder of a line beyond count is dropped. There is no O_NONBLOCK and no VMIN/VTIME support, and Ctrl+C does not raise SIGINT ("for now" stub).
- **Suggested fix:** Per-tty line buffer; honour 0x800; send SIGINT to the foreground group.
- **Test:** QEMU: type "hello\n" and read 2 then 4 bytes.
