# Guest runtime regression probes

Build the C probes in `nix-shell` using `$MUSL_CC -static -O2` (add
`-pthread` for `fdstress.c` and `brkcheck.c`). Keep binaries in ignored `build/`.
Run them inside a disposable Brook VM through `vm_guest_run`:

- `probe.c`: relative mkdir/open/symlink/readlink/unlink and invalid dirfd.
- `fdstress.c`: 2,000 concurrent `F_DUPFD_CLOEXEC` operations on a shared memfd.
- `brkcheck.c`: a sibling heap extension preserves an existing page and the
  leader observes the same break.
- `cowcheck.c`: `pipe` writes a valid post-fork COW output buffer; an explicitly
  read-only output buffer still returns `EFAULT`.

For `ext2check.py`, first create `/data/tmp/many` with exactly 800 files whose
names contain 90 characters, then call guest `sync` and stop the VM. Run
`e2fsck -fyD` on **that disposable, stopped** data disk to index the directory.
Check `debugfs -R 'stat /tmp/many' IMAGE` reports `Flags: 0x1000`.
Boot it, run the Python probe, wait for `HTREECHECK PASS`, then stop it and run
`e2fsck -fn IMAGE`. The guest must enumerate every entry, add and remove entries,
clear the obsolete index flag, and persist correct inode/block/free counts.
Never run writable filesystem tools against a live VM disk.

`cwdcheck.c` verifies relative classic mkdir/unlink and mkdirat(AT_FDCWD)
against absolute paths. It leaves three empty fixture directories.
`clockcheck.c` checks a 20 ms absolute monotonic sleep and a 30 ms monotonic
condition-variable timeout in a child, with a one-second parent watchdog so
broken kernels fail instead of leaving an indefinitely sleeping probe.

`sharedcheck.c` checks that writes through a MAP_SHARED memfd remain shared
across both a normal fork and a fork from a thread created before the mapping.
Compile it with `-pthread`. The child must see the leader's current VMAs, and
neither fork may turn object-owned backing pages into private COW pages.

`waitcheck.c` checks both `exit` and `exit_group` for codes 0, 1, 127, 128,
and 255, plus SIGTERM/SIGKILL termination. Run it with the guest Bash absolute
path as its sole argument, using an absolute or explicit relative path for the
probe itself. It also runs external failing commands under Bash `set -e` and
checks that Bash stops with the exact child exit code. Expect `WAITCHECK PASS
failures=0`; the deliberate continuation sentinel for a successful command is
exit code 77. No pthread dependency is needed.

`execcheck.c` must be invoked by its absolute path, optionally followed by a
writable fixture directory (for example `/data/tmp`). It round-trips 128/256
arguments, more than 16 KiB of strings, 128 environment entries, the exact
512-argument/256-environment limits, and one 20,000-byte argument. It checks
count/aggregate-byte overflow returns `E2BIG`, invalid pointer slots and
strings crossing unmapped pages return `EFAULT`, and a NUL at the final mapped
byte works. A temporary shebang fixture checks argument preservation and
interpreter-expansion overflow. Expect `EXECCHECK PASS failures=0`; overflow
tests use Brook's bounded limits and are not expected to pass on Linux.

`metacheck.c` takes a writable directory and optionally an existing executable
path (default: its own argv[0], which should be absolute). It checks path/fd/
statx identity, permissions and timestamps; create/write/O_TRUNC/relatime
updates; explicit `utimensat`/`futimens`, NOW/OMIT and symlink-nofollow handling.
Ext2 timestamps have whole-second precision; explicit subsecond values round
down. Expect `METACHECK PASS failures=0`. Files installed by an older kernel
can legitimately retain zero on-disk timestamps: repair those with `touch`
after booting the new kernel, rather than substituting fictional stat times.

`modecheck.c` takes one or more absolute writable directories. Pass directories
on both the data and Nix ext2 mounts to verify mount routing. It checks 0755
creation, umask producing 0640, mode-0000 creation, preservation of an existing
inode's mode, path/fd/cwd/dirfd chmod, followed symlinks, and error propagation.
When run as root, a child drops its uid/gid to verify non-owner fchmod fails.
Expect `MODECHECK PASS failures=0`. Existing SDK executables created by the old
kernel require a targeted permission repair from their NAR executable metadata;
changing all store files to executable would corrupt the intended modes.

`selectcheck.c` requires `-pthread`. A separate parent process uses monotonic
time, `waitpid(WNOHANG)` and `SIGKILL` as a 15-second watchdog to bound infinite
`pselect` tests for delayed resolver-style eventfd notification, pipe data and
TCP accept/data readiness. The optional argument is a local IPv4 address;
default is `127.0.0.1` on Linux. In the current QEMU guest, pass its configured
NIC address (`10.0.2.15`) to exercise Brook's supported self-address route:
127/8 loopback routing remains unimplemented. It also checks empty/zero/finite waits,
timeval versus small timespec fractions, fd 200, read/write result bit counts,
exception sets, invalid fds/counts/timeouts, and fdset/poll buffers at an
unmapped page boundary. The guard page is removed with `munmap`: Brook's
user-buffer validation does not yet consistently reject `PROT_NONE`, and its
`alarm` syscall is unavailable. Expect `SELECTCHECK PASS failures=0`. Brook bounds
poll/select to its 256-fd table and rounds positive waits up to milliseconds.
Temporary pselect/ppoll signal-mask handling remains a separate task.

`opencheck.c` takes an absolute writable directory and an existing regular
library path under `/boot/lib/` (for example `/boot/lib/libdl.so.2`, when
installed). It checks that missing Git refs directories, trailing slashes and
unrelated file names cannot become `/boot/lib` or one of its files. It also
checks directory/write requests cannot use library basename aliases, while
exact `/boot` prefixes and read-only legacy/Nix library aliases still work.
Expect `OPENCHECK PASS failures=0`. This is a Brook compatibility probe;
ordinary Linux does not provide these boot-library aliases.

`accesscheck.c` takes an absolute writable directory (use `/data/tmp` to keep
directory-fd paths within Brook's existing 63-byte stored-path limit). It calls
raw `access`, `faccessat` and `faccessat2`, checking relative `.git/config` after
`chdir`, absolute paths, directory fds with a different cwd, synthetic-file
existence, dangling symlinks, EMPTY_PATH/EACCESS flags, bad fds/modes/flags,
oversize paths and strings crossing an unmapped page. Expect `ACCESSCHECK PASS
failures=0`. Permission-bit and real-versus-effective credential evaluation
remain unimplemented in Brook; this probe checks pathname/existence semantics.

`childsignalcheck.c` must be invoked by its absolute path. Five separately
watched child processes check SIGCHLD from an exec'd child, a signal handler
notifying an epoll event loop through eventfd, caught signals during pipe and
AF_UNIX reads, SA_RESTART, and default-ignored SIGCHLD during a blocking read.
The handler also supplies read data, so delivery just before the read begins
does not introduce a timing-dependent hang. Each case has a 15-second
fork/waitpid/monotonic-clock watchdog; no alarm or pthread support is required.
Expect `CHILDSIGNALCHECK PASS failures=0`. Temporary epoll_pwait masks remain
outside this probe's scope.

`fd3check.c` must be invoked by its absolute path, with an absolute writable
directory. Start it with Brook's inherited debug-serial fd 3. It verifies
closed-fd errors, file reuse with binary write/writev/pwrite and persisted
content, dup2 replacement, pipe/eventfd reuse, and a child that closes fd 3
before exec and opens its output there. Verify both `FD3CHECK SERIAL ORIGINAL`
and `FD3CHECK SERIAL DUPLICATE` appear in the UART log; successful write returns
alone cannot prove serial delivery. Expect `FD3CHECK PASS failures=0`.
`--no-serial` skips the Brook-specific inherited-device checks for host runs.
