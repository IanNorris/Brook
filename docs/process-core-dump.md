# Live user-process core dumps

Run `proc dump PID /data/name.core` over the normal debug channel. Either the
thread-group leader or a live thread ID selects the group. The destination
must be a new absolute path. The file is created with mode `0600`. For remote
capture, use the existing boot-pinned hardware session and bulk transfer.

```sh
python3 scripts/dell_proc_dump.py SESSION PID ./capture.core
```

The helper uses the destination's parent directory for private staging, checks
the downloaded length, and prints a GDB backtrace command. Install the
matching guest executable and libraries on the host, or use `--exe` to
specify the matching executable. GDB's sysroot is `/` so absolute
`/nix/store` names resolve to the corresponding host paths. The session needs
the compressed bulk helper and its existing key. Cores on the guest remain
under `/data/.brook-transfer` for retrieval and disposal.

The reply includes threads, missing register samples, readable mappings,
logical ELF bytes, holes in pages, and elapsed milliseconds. Both interfaces
accept `--full` to include readable file pages and shared RAM that the
default selection skips. Device mappings are excluded. The logical output
limit is 1 GiB, matching the existing transfer limit. There are also bounds
on ELF segments and recorded file paths. An I/O error can leave an incomplete
file at the chosen path.

This is a live snapshot. Threads can continue running during collection.
Syscall entry saves the user register state, while timer and reschedule
interrupts save snapshots of threads executing in user mode. A thread with
no usable sample from the selected address space is counted in
`missing_registers`. The ELF contains integer registers and the original
exec auxiliary vector. It does not capture vector registers or fault in
nonresident pages. Dirty or writable file-backed data is captured when
resident. GDB can reconstruct omitted clean named pages when it can find the
matching files. Unnamed mappings are captured from resident RAM. Filesystem
changes, process exit, and concurrent memory writes can make the snapshot
incomplete or inconsistent.

The host test checks Linux x86-64 note and register layouts. The optional
`check_dell_image_qemu.py --proc-dump-qa` fixture launches one user thread
stuck in a pipe read and one executing userspace, downloads the core over a
private loopback bulk port, and verifies both backtraces in GDB.
