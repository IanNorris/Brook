# Sockets: AF_UNIX, inet and netlink

_17 open entries (9 medium, 8 low). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** Socket creation validation, AF_UNIX datagram semantics, accept/recv flags (`MSG_PEEK`, `MSG_WAITALL`, `MSG_DONTWAIT`), blocking UDP, `sendmsg`/`sockopt` behaviour, errno mapping, netlink details.
- **Why it matters:** Mostly Linux-compat and data-loss bugs (a `MSG_PEEK` that consumes data, datagrams with stream semantics, blocking UDP that spins). H-14 and H-15 are fixed.
- **Dependencies:** After 01 (socket syscall user copies, M-36) so new code uses the helpers. M-63/L-34 (SCM_RIGHTS) are in 03. Blocking-wait items (M-32, M-42) share fixes with 07.
- **Suggested order:** M-41, M-40, M-39 (stream receive/flags), M-37 and M-38 (type validation), M-42/M-32 (blocking UDP), M-43, M-44, then the L entries. L-31 (raw magic numbers in socket/unix/netlink) is in 12.
- **Test approach:** `test_syscall_user_memory/socket.cpp` (fake UDP/TCP) and `unix.cpp`; `test_socket_lifetime`, `test_socket_identity`, `test_unix_stream_vector`, `test_process_socket_metadata`. Netlink has its own host test that includes the fragment.
- **Size:** Large: 17 entries, 2 branches (AF_UNIX/stream and inet/netlink).

## Entries

### Medium

### M-32 · Blocking UDP read busy-spins and never sleeps

- **Where:** `src/kernel/src/syscall_rw.cpp:946`, in `sys_read (Socket, datagram)`.
- **Severity:** medium (confidence high).
- **Problem:** A blocking read on an empty UDP socket spins at 100% CPU, and every error becomes EAGAIN.
- **Suggested fix:** Block on a wait queue; propagate errors.
- **Test:** QEMU: an idle blocking UDP recv leaves the CPU idle; SIGALRM interrupts it.

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

### Low

### L-24 · Datagram write maps every error to ENOTCONN

- **Where:** `src/kernel/src/syscall_rw.cpp:342`, in `WriteToPinnedFdByType (Socket)`.
- **Severity:** low (confidence high).
- **Problem:** EMSGSIZE and ENOMEM are reported as ENOTCONN, and success returns the untruncated count.
- **Suggested fix:** Propagate ret.
- **Test:** QEMU: a 70000-byte UDP write gives EMSGSIZE.

### L-30 · Wrong or flattened errno values in socket syscalls

- **Where:** `src/kernel/src/syscall_socket.cpp:97`, in `sys_socket`.
- **Severity:** low (confidence high).
- **Problem:** About 10 sites: SockCreate gives ENOMEM, publish gives EMFILE, listen gives EINVAL, sendto gives EIO. A non-socket fd gives EBADF rather than ENOTSOCK in GetSockIdx paths and sendmsg. iovlen>1024 gives EINVAL (Linux: EMSGSIZE).
- **Suggested fix:** Propagate errors; a ResolveSocket() helper.
- **Test:** Host errno table with pipe, file and closed fds.

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

### L-61 · Zero-length netlink send returns ENODATA

- **Where:** `src/kernel/src/syscall_netlink.inc:59`, in `NetlinkSend`.
- **Severity:** low (confidence medium).
- **Problem:** Linux returns 0.
- **Suggested fix:** Return 0.
- **Test:** test_netlink_endpoint.

### L-70 · Netlink autobind uses tgid without the pid fallback

- **Where:** `src/kernel/src/syscall_netlink.inc:29`, in `NetlinkBind`.
- **Severity:** low (confidence low).
- **Problem:** Elsewhere the code uses tgid?tgid:pid, so a leader with tgid 0 would bind port 1.
- **Suggested fix:** The same helper.
- **Test:** test_netlink_endpoint getsockname.
