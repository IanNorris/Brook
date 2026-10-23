# Steam's named FIFO implementation plan (completed subset)

Status: the implemented and validated subset is described in
[the FIFO checkpoint](steam-fifo-checkpoint.md). The original plan follows.

The 23 October urandom candidate reaches the native64 Steam client inside
pressure-vessel. It no longer asserts opening `/dev/urandom`. It next asserts
`m_NamedPipe >= 0` in `steamglobalinstance.cpp:384`. Brook currently has anonymous
pipes but no mknod/mknodat FIFO creation. The isolated request is retained in
`build/workloads/urandom-device-20261023/` and its associated private review.
This is not a login-screen success.

Implement named FIFO semantics through real retained filesystem objects rather
than a pathname-keyed synthetic FIFO service or a regular-file placeholder:

1. Add a FIFO-only creation operation to VFS object callbacks and the relevant
   tmpfs/Ext2 backends. Apply ordinary parent search/write permissions, umask,
   exclusive name publication, read-only mounts, ownership and inode lifecycle.
   Reject unsupported device types explicitly. Rebuild modules with the changed
   callback ABI.
2. Associate live pipe state with the exact backing/object identity. Namespace
   copies and bind aliases of the same inode must find the same stream; unrelated
   equal pathnames and an unlinked/recreated FIFO must not. Keep an owned inode
   reference while pipe endpoints exist, and retire registry entries atomically
   with last-endpoint closure. Existing anonymous-pipe storage holds and poll
   subscriptions must not accidentally resurrect an endpoint or suppress EOF.
3. Support RDWR, independent reader/writer opens, nonblocking ENXIO/EAGAIN,
   EOF/EPIPE and signal behavior; blocking open rendezvous must be interruptible.
   Extend dup/fork/SCM_RIGHTS/close bookkeeping for endpoints owning both reader
   and writer references. Poll/select/epoll must subscribe to the real pipe
   state, not report a regular file as perpetually ready. Preserve stat type,
   inode identity, fd flags and ESPIPE.
4. Compare a portable Linux probe, then test Brook in private tmpfs and Ext2.
   Include rename/unlink/recreation, bind/namespace aliases, permissions and
   read-only mount behavior, open/close races, fork/dup lifetimes, readiness,
   signal interruption and allocation-failure cleanup. Re-run anonymous-pipe,
   socket, container and desktop regressions before the actual Steam retry.

Keep the existing client payload, home, diagnostic flags and original job
identities visible in evidence. Use unique append-mode guest log files with
bounded tails over the bridge. Do not authenticate on behalf of the user.
