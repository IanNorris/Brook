# Guest runtime regression probes

Build the C probes in `nix-shell` using `$MUSL_CC -static -O2` (add
`-pthread` for `fdstress.c` and `brkcheck.c`). Keep binaries in ignored `build/`.
Run them inside a disposable Brook VM through `vm_guest_run`:

`sockettypecheck.c` checks send/receive errors and the send-to-write fallback
used by PulseAudio. Linux and Brook must pass 85 checks: valid pipes, eventfds,
memfds and `/dev/null` return ENOTSOCK; absent/closed/O_PATH descriptors return
EBADF. Zero-length calls and 32-bit FD argument normalization are included.
Failed socket operations must preserve pipe data, eventfd counters and file
offsets. Real write fallback and Unix-socket send/receive must still work.

`pifutexcheck.c` checks PI-futex capability reporting. Run `--expect-supported`
on Linux (15 checks) and `--expect-unsupported` on Brook (219 checks). Brook
must reject all six PI operation classes with ENOSYS without changing words,
including null-word capability probes and private/realtime flag combinations.
Ordinary WAIT/WAKE must still work. `--observe` prints the reduced self-lock and
unowned-unlock comparison; the old Brook stubs incorrectly succeeded at both.
This is unsupported-capability acceptance, not priority-inheritance support.

`sigpendingcheck.c` requires `-pthread`. It checks raw `rt_sigpending` sizes
0 through 8, oversized rejection, exact partial writes, null/read-only/unmapped
destinations, repeated non-consuming observation, libc `sigpending`, signal 64,
fork clearing, handler delivery after unblock, blocked POSIX timer signals and
thread-directed isolation. Linux passes 85 checks. Run under a host deadline
because a broken signal/thread implementation can hang. This does not certify
shared process-directed signal routing or ignored-disposition transitions;
Brook's existing ordinary signal storage is per task, and those broader
semantics remain separate work.

`unixrightscheck.c` requires `-pthread`. The default uses one receiver;
`--concurrent` uses four. One sender transfers 512 references to a mapped memfd.
Every one-byte receive must carry exactly one descriptor, every received handle
must read the marker, and total bytes and handles must both equal 512. Run both
modes and require no kernel page-ownership warnings after cleanup. This catches
duplicate installation and rights delivered with the wrong stream bytes.

`fsexitcheck.c` requires `-pthread` and an absolute fixture path. Run once on
ext2 (`/data/FSEXIT.TST`) and once on FAT (`/boot/FSEXIT.TST`). Sixteen rounds
cycle through `exit_group`, parent-directed `SIGKILL`, and process-directed
`SIGKILL` from the leader and a worker, while eight threads read, stat and access
a 131,119-byte fixture. The parent checks the exact contents
after every group exits, forcing a new filesystem acquisition that detects
abandoned locks. After a synced shutdown and reboot, run with the same path
and `--verify`, then require clean offline filesystem checks. The probe uses
the raw Linux `alarm` syscall because musl's wrapper requires `setitimer`;
also enforce a host deadline, since a filesystem deadlock can block even fatal
signal completion. Timing varies the exit point but does not establish that
any particular instruction was interrupted.

`waitmaskcheck.c` must be invoked by absolute path. It checks raw `ppoll`,
`pselect6`, `epoll_pwait` and `sigsuspend` with SIGCHLD blocked outside each wait. A queued
signal and a delayed exec-child exit must invoke the handler and return EINTR
even with SA_RESTART. The monitored pipe keeps its writer open, so EOF cannot
hide missing signal delivery. The handler checks its active mask and saved
ucontext; the caller checks restoration after handler, timeout, readiness and
error paths. Invalid sizes and masks crossing a truly unmapped page are checked,
and SIGKILL/SIGSTOP must remain unblockable. Each case has a 15-second
fork/wait/monotonic-clock watchdog. Expect 32 checks and
`WAITMASKCHECK PASS failures=0` on Linux and Brook.

`symlinkcheck.c DIRECTORY [MOUNT_ROOT]` creates or reopens links with target lengths around
the ext2 inline boundary. It checks exact lstat size, readlink bytes without a
copied terminator. Inspect the inodes offline to require data-block storage for
targets of 60 bytes or more (Brook's generic stat block count is estimated). Keep
the links through checked shutdown and offline fsck, then run again after boot
to check persistence. Missing paths must return ENOENT, regular files/directories
EINVAL; relative and dirfd reads of dangling links retain their target bytes,
including truncated buffers. An optional mount root must
also report EINVAL (thirteen checks with that argument). Expect
`SYMLINKCHECK PASS failures=0`.

- `devicecheck.c`: takes two different mount roots (Brook: `/data /nix`;
  Linux: `/ /proc`). Verifies device identity across stat/fstat/lstat/fstatat
  and path/fd statx, then requires the mounts to have distinct devices. This
  catches false coreutils input/output alias detection when inode numbers match.
- `sleepcheck.c`: raw nanosleep waits of 0, 1, 10,000, 999,999, 1,000,001
  and 5,000,000 ns never complete early; negative seconds/nanoseconds and
  out-of-range nanoseconds return EINVAL. Expect nine checks and zero failures.
- `filelifecheck.c`: takes an absolute writable directory. Checks independent
  descriptions after unlink, orphan writes and inode reuse, an unfaulted mmap
  surviving last-descriptor close, rename replacement and same-inode rename,
  nonempty-directory rejection, unlink/rmdir type distinctions and directory
  link counts. Also checks directory cache reuse and moves, symlink storage,
  forked descriptors and an 8 MiB orphan crossing double-indirect blocks. Pass
  `--require-reuse` after the directory argument in Brook to require actual
  inode reuse; omit it on Linux, whose allocator need not reuse immediately.
  It also performs 32 rename replacements. Run only on disposable disks; after its sync and a clean VM shutdown, require offline fsck to pass.
- `probe.c`: relative mkdir/open/symlink/readlink/unlink and invalid dirfd.
- `fdstress.c`: 2,000 concurrent `F_DUPFD_CLOEXEC` operations on a shared memfd.
- `brkcheck.c`: a sibling heap extension preserves an existing page and the
  leader observes the same break.
- `brkshrinkcheck.c`: raw break shrink/regrowth returns zeroed full pages,
  preserves the live partial page, and drops a fork child's COW references
  without changing its parent's heap. It uses no allocator or stdio while
  borrowing the break. Run alongside `brkcheck.c`; neither test establishes
  the cause of an application heap crash.
- `cowcheck.c`: `pipe` writes a valid post-fork COW output buffer; an explicitly
  read-only output buffer still returns `EFAULT`.

`stdfdcheck.c` takes an absolute writable directory and an absolute Python
executable. It checks regular files, closed descriptors, anonymous pipes and
`/dev/null` reused as each of fd 0, 1 and 2: metadata follows the object, terminal
ioctls return `ENOTTY`, and closed descriptors return `EBADF`. Python then reads
a 40 KiB script from a regular file on stdin; exact output, exit status and empty
stderr distinguish script execution from an accidental interactive REPL. Child
checks have a 30-second fork/wait/monotonic-clock watchdog. Expect 20 checks and
`STDFDCHECK PASS failures=0`. Explicitly terminal-marked shell pipes retain their
terminal behavior; descriptor numbers alone do not identify a terminal.

`mmaplifecheck.c` takes an absolute writable directory and must itself be invoked
by its absolute path. It creates a 16 MiB readonly mapping fixture and faults
every page in eight exit children and eight exec children, deliberately leaving
unmapping to process teardown. In an otherwise idle Brook guest, `sysinfo`
free-memory samples after reaper/quarantine settling must lose less than 16 MiB
per 128 MiB phase. The old leak loses about 128 MiB per phase. It also verifies a
parent's inherited mapping survives child exec/exit. Allow at least 512 MiB free;
children have 30-second watchdogs. Use `--no-accounting` for a busy Linux host,
where unrelated global memory changes make the free-memory assertion unsuitable;
that mode still checks mapping contents and process lifecycle behavior. Expect
`MMAPLIFECHECK PASS failures=0`; this tests page reclamation, not the cause of
every earlier compiler fault.

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
exact `/boot` prefixes and read-only legacy library aliases still work.
Missing Nix store library paths must return ENOENT, so compiler search paths
cannot substitute boot-disk musl for glibc or another package's contents.
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

`mmapcachecheck.c --diagnose PATH` compares every byte of `pread` and a read-only
private mmap of an existing file (1 byte through 64 MiB). It prints inode/size,
FNV-1a digests, mismatch count and the first differing offset/bytes. Run it on
a suspect library before reboot clears the global cache. It performs no writes,
but mapping may populate that cache; keep the backing file unchanged throughout.

For the namespace regression, pass two file paths on **different ext2 mounts**
with equal inode numbers and different content at overlapping offsets. The
probe rejects unsuitable inode/content pairs, primes the first file, verifies
the second twice, and reopens the first to verify stable identity. Brook still
reports zero `st_dev`, so mount separation must be established by the test setup.
Expect `MMAPCACHECHECK PASS failures=0`. This does not test write, truncation or
inode-reuse invalidation, which remain separate page-cache limitations.

A reproducible fixture uses two new, otherwise identical disposable ext2
images: format them with the same size/options, then use `debugfs -w` to write
one 64 KiB payload at `/cachecheck` in each. Use distinct payloads (for example
all `A` versus all `B`) and verify `debugfs -R 'stat /cachecheck' IMAGE` reports
the same inode on both. Attach and mount both images in an isolated test VM,
then invoke the probe with both `/cachecheck` paths. Never modify a live VM's
image through debugfs. The fixed kernel logs each mount's page-cache namespace;
the high 32 bits of an ext2 cacheId are that namespace and the low 32 bits are
the actual inode. Filesystem `stat` continues to expose the actual inode.

Writable ext2 regular files currently opt out of the global mmap page cache:
the namespace identity alone cannot keep pages coherent across writes,
truncation or inode reuse. Demand paging and ext2's block cache remain enabled,
but cross-process mmap-page sharing and preload read-ahead are disabled. A
future coherent cache or mapping-local read-ahead can restore those benefits.
Fault dumps show cacheId zero for these files, so that field cannot identify
their backing inode; use filesystem metadata separately.

`mmapmutatecheck.c` takes an absolute writable ext2 directory. It primes and
unmaps a partial final page, appends six bytes into the old EOF padding, performs
an in-place pwrite, replaces contents with O_TRUNC, then unlinks/recreates the
file. Every fresh mapping must equal both pread and known expected bytes. The
ext2 fixture must report `reused=1` to demonstrate actual inode-reuse coverage;
the probe requires it by default. For host filesystems whose allocator chooses
a different inode, `--allow-no-reuse` explicitly skips that assertion. Expect
`MMAPMUTATECHECK PASS failures=0`. The probe closes/unmaps before unlink; it does
not claim coherent updates to existing private mappings or correct lifetime
of open-unlinked files.

`sharedfilecheck.c` takes an absolute writable directory. This Brook-specific
probe expects writable regular-file MAP_SHARED (including SHARED_VALIDATE) to
fail with EOPNOTSUPP until file writeback is implemented. It verifies private
writable changes do not persist, private read-only mappings still read the
file, and two shared memfd mappings still see each other's writes. The file
is sized with write; resizing is covered separately by truncatecheck.
Expect `SHAREDFILECHECK PASS failures=0`; the rejection checks intentionally
fail on Linux, where writable shared file mappings are supported.

LLVM 18's [FileOutputBuffer implementation](https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-18.1.8/llvm/lib/Support/FileOutputBuffer.cpp)
falls back to an in-memory buffer followed by ordinary file writes when its
output mmap fails. Its preceding file-resize failure is fatal, so this does
not establish that unsupported ftruncate can simply start failing without
additional caller work. Validate the actual guest linker output as a nonempty,
parseable ELF after testing the rejection; a successful linker exit alone is
insufficient. No shared regular-file persistence is claimed by this fallback.

`syscallstatecheck.c` checks all 16 XMM registers and, when CPUID/XGETBV permit,
all 16 YMM registers across raw Linux syscall instructions. Register loads,
syscall and stores are in a single inline-assembly block, so C calling
conventions cannot explain a mismatch. It covers getpid, mmap zero-fill, brk
growth/restoration, pipe write/read, clock_gettime and a short blocking
nanosleep. Compile normally without `-mavx`; AVX instructions are explicitly
gated. Expect `SYSCALLSTATECHECK PASS failures=0`; failures report a register
bitmask and first differing byte. This establishes a syscall register-state
defect, not the cause of any particular application crash. Signal-handler FP
restoration and fork-state inheritance need separate coverage.

The syscall-entry fix uses the existing IRQ XSAVE/XRSTOR convention (x87,
SSE and AVX, mask 7). It reserves at most 1215 bytes below the unchanged saved
GPR frame on the 256 KiB kernel stack. Callee-saved registers retain both
frame pointers through blocking/context switches; signal-return processing
still receives the original GPR frame, and the final RAX is preserved across
XRSTOR's mask setup. Fresh exec and fork-child trampoline paths do not return
through the parent's saved syscall frame. Re-run waitcheck, execcheck and
childsignalcheck alongside the vector sentinel probe after changing this path.

`faultisolationcheck.c` forks a child that performs a raw NULL load. The parent
must reap SIGSEGV, then successfully emit `FAULTISOLATIONCHECK SERIAL AFTER USER
FAULT` through Brook's inherited debug fd 3. Confirm the marker in UART logs
and query bridge procs/stats afterward: write success alone is insufficient.
Host runs use `--no-serial`. This verifies fatal user faults preserve the
ordinary serial ticket lock; kernel panic/double-fault handling is unchanged.

`threadpagecheck.c` checks that user pages outlive the thread that allocated
them. Compile with `-pthread` (static musl is suitable), then run an absolute
executable path with an absolute writable ext2 directory and `--require-reuse`:
`/boot/threadpagecheck /data/tmp --require-reuse`. A worker allocates anonymous
pages, commits PROT_NONE pages with mprotect, faults a private file mapping,
and triggers CPU COW faults. After joining it, an independent peer created
before those allocations forks/execs/reaps 48 children in three batches of 16.
Each batch reserves all child PIDs behind a pipe before releasing them to exec,
so repeated reuse of a lower free PID cannot hide the worker's TID. At least
one child must reuse the worker's TID. All four mappings must preserve their complete byte
patterns, and the peer must retain its original COW data. The victim never forks
after worker allocation: doing so could share/disown its frames and conceal
the defect. A separate parent enforces a 30-second watchdog. On Linux, omit
`--require-reuse`, since its PID allocator need not promptly reuse the TID.
Expect `THREADPAGECHECK PASS failures=0` plus `observed=1 required=1` on Brook.

The motivating bootstrap failure preserved Git's arena VA `0x24000000`
mapping physical frame `0x16efe000`; serial recorded that exact frame being
freed by PmmKillPid with owner PID 19, refCount 1 and mapCount 1. Its captured
contents subsequently contained machine code while Git remained asleep on
the arena's futex at offset 0x30. Allocating shared-address-space backing under
the stable thread-group ID prevents an unrelated process reusing an exited
worker's PID from reclaiming those pages. Page-table pages remain kernel-owned;
individual kernel stacks retain thread ownership and explicit teardown.

`exitgroupcheck.c` stresses exit_group against eight workers doing futex
wait/wake (timed and untimed), nanosleep, and blocked pipe reads. Build with
`-pthread`; run `/boot/BIN/EXITGRP [rounds]` (default 128, range 8–4096).
Every eighth child joins its workers normally to exercise ordinary waiter
reference removal; the remaining children exit with live workers. Each child
must actually be reaped by waitpid with the expected exit status within five
seconds. The parent fails rather than blocking indefinitely on a broken group.
Unexpected futex errors fail the child. Expect `EXITGROUPCHECK PASS rounds=128`.

For kernel acceptance, also inspect the process and futex diagnostics after
completion: no worker from the test may remain and no futex waiter may retain
its process. A host pass validates the probe, not Brook's scheduler. The probe
covers FD pin teardown through blocked reads, but does not prove lifetime
safety of every raw pipe/eventfd/socket waiter pointer or address-reuse race.

`posixtimercheck.c` validates Linux x86-64 POSIX timer structures against the
build toolchain headers, then checks real signal delivery, SI_TIMER metadata,
independent timers sharing a signal, periodic overruns, disarm/rearm, relative
and absolute monotonic/realtime deadlines, invalid buffers/arguments, targeted
thread delivery and TID reuse isolation, fork/exec/exit cleanup, and bounded
resource recovery. Its final test uses libc `timer_create(SIGEV_THREAD)` and
requires 16 actual callback executions with the original value. Build and run
**dynamically with glibc from the same closure as Vim**, with `-pthread -lrt`;
a musl/raw-only test does not establish glibc helper compatibility. Use an
absolute executable path (the test re-execs itself), and pass `--require-reuse`
on Brook to require reuse of a dead target TID. Omit that option on Linux.
A parent enforces a 30-second watchdog. Expect `POSIXTIMERCHECK PASS failures=0`
and `glibc_callbacks=16`, then inspect process/futex diagnostics for leftovers.

The kernel timer registry is bounded at 128 timers system-wide and returns
EAGAIN on exhaustion. It supports CLOCK_REALTIME/CLOCK_MONOTONIC and
SIGEV_NONE/SIGEV_SIGNAL/Linux SIGEV_THREAD_ID; libc implements SIGEV_THREAD on
top of the latter. Unsupported clocks and raw SIGEV_THREAD return EINVAL.
Timer notification via SIGKILL, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, or SIGTTOU
returns EOPNOTSUPP: IRQ-return job-control signal delivery is not implemented.
Other asynchronous handlers are delivered at syscall return, so a CPU-only
loop without syscalls is not a conforming asynchronous-handler acceptance
case. The blocked glibc helper receives its signal through rt_sigtimedwait.
Deleting a timer discards its queued notification; disarming preserves one
already queued. Like the tested Linux host, disarming clears the reported
interval, settime resets overrun accounting, and settime ignores flag bits
other than TIMER_ABSTIME. rt_sigtimedwait dequeues before copying siginfo, so
an invalid output pointer consumes the selected signal and returns EFAULT.
The probe also requires ignored timers not to interrupt nanosleep, and requires
a surviving sibling to use/create timers after its leader calls raw SYS_exit.
The sole Brook-specific test requirement is `--require-reuse`; without it the
probe is intended to pass on Linux with glibc. Timers carry owner/target
incarnation tokens and no persistent
process references. Process layout/module ABI are unchanged. These explicit
scope limits must not be reported as full POSIX timer or signal conformance.

The host Linux SIGEV_NONE optimization can report its previous countdown after
disarming, although the interval is cleared. Its `common_timer_get` bypasses
the inactive-timer return for SIGEV_NONE and reads the old hrtimer expiry
([Linux source](https://github.com/torvalds/linux/blob/v6.12/kernel/time/posix-timers.c#L600)).
The portable probe checks interval clearing for SIGEV_NONE, and checks both
zero interval and zero remaining time plus absence of delivery on SIGEV_SIGNAL.
Brook returns zero remaining time for either disarmed mode; it does not emulate
the stale-countdown optimization.


### Live task directories (`proctaskcheck.c`)

Build with `cc -std=c11 -Wall -Wextra -Werror -pthread proctaskcheck.c -o proctaskcheck`.
Run on Linux as a reference and then in Brook. The test checks the Chromium
`fstatat(proc_fd, "self/task/", ...)` contract: `st_nlink == 2 + live threads`,
thread appearance/disappearance, TGID-correct `self` versus `thread-self`, task
status identity, exclusion of a foreign process, and held-directory enumeration
and fstat across 32 subsequent process births. `observed_reuse` reports actual
PID reuse; a run without reuse does not prove reuse isolation.

Task enumeration and fstat are tied to the group/process incarnation and cannot
retarget a reused numeric PID. **Unverified and not fixed here:** relative
`openat`/`fstatat` through an old directory FD still use the generic saved-path
resolver, which does not carry the vnode generation into path traversal. That
requires a separate VFS/dirfd fix. The existing pid file generators are reused;
this does not make their approximate memory maps a complete Linux maps view.
The stale-directory fstat check permits Brook's existing EBADF translation of a
failed vnode stat, as well as ENOENT/ESRCH or an unchanged original inode; it
checks that the FD never acquires the reused process's identity.

Chromium's primary implementation is
[thread_helpers.cc](https://raw.githubusercontent.com/chromium/chromium/main/sandbox/linux/services/thread_helpers.cc).
Brook inotify calls currently return ENOSYS; no inotify sysctl limits are exposed
because there is no backed watch/instance/event capacity to report.


`largepayload.c` + `largepayload.S` create a 40 MiB file-backed executable (not
just BSS); build both together. Run it directly through the kernel shell, then
run `largeexeccheck /path/to/largepayload` to check execve and seven malformed
images while a live sibling remains intact. The checker needs `-pthread`.
Payload success intentionally exits 37 after printing its checksum.

`fileidentitycheck /tmp` and `fileidentitycheck /data` compare independent open
file descriptions, path stats, cached 128 KiB files, dup/fork and distinct memfd
objects. The directory argument must be writable; fixtures are removed. FAT
rename/recreate persistent inode semantics are outside this probe's contract.

`wmoverflowcheck [count]` is a Brook-only visual fixture creating 20 windows by
default (2–28 allowed). It holds them for at most 300 seconds so the taskbar's
hidden-window navigation can be clicked and observed; SIGTERM can end it early.
It is not an automated UI acceptance test.

`truncatecheck PATH [--brook-gate]` checks actual regular-file resizing on the
filesystem containing PATH: shrink, zero-filled growth (including a retained
partial block), independent-open sizes/read caches, unchanged OFD position,
invalid/negative/read-only/directory calls, and sparse markers around direct,
single and double indirect boundaries for 1/2/4 KiB ext2 blocks. Run on both
`/boot/TRUNC.TST` (FAT) and `/data/TRUNC.TST` (ext2). It retains a 131119-byte
zero-filled fixture; after checked sync/reboot, `truncatecheck PATH --verify`
checks persistence. The portable mode also runs on Linux. `--brook-gate` is
explicitly Brook-specific: resize of a live lazy FileMap returns EBUSY until
all mapping/fault pins have gone, including serialized mprotect/munmap splits
and fork. It also runs 16 bounded kill/fault-loop rounds with readiness pipes,
requires SIGKILL termination, and requires resize after each reap. This stress
does not prove cancellation at a specific blocked-I/O instruction. Per-thread
active fault ownership is consumed once by normal return or off-CPU teardown;
fork/clone do not inherit that transient ownership.

This is a tested serialized restriction, not Linux's mapped-truncate/SIGBUS
behavior or full concurrent VMA correctness. Required next work: synchronize
all partial unmap/protection/publication changes and fault PTE installation;
the existing late-fault/orphan-PTE race is not fixed by the resize registry.
Eager private copies retain their existing copied-EOF semantics.

The truncate implementation appends a VnodeOps method and requires module ABI 7;
all modules must be rebuilt, including the new per-thread Process fault slot. FAT and ext2 support resize up to 0xffffffff bytes;
FAT growth writes zeros, ext2 grows sparsely. Ext4 returns EOPNOTSUPP: vendored
lwext4 only shrinks, and its cached sizes/read-ahead require a separate coherence
change before enabling resize. FAT rejects rename/unlink of open FILs (and
conservatively directory mutation while files are open), since directory-slot
identities and backing clusters otherwise become stale. This includes mapped
files and handles opened before a later mmap. FAT rename crash atomicity and
unlink-open semantics are not claimed. Ext2's existing inode holders retain
unlinked backing. An ext2 metadata I/O error can leave detached allocations
requiring fsck; resize never intentionally frees blocks still referenced by
successfully committed metadata. Full filesystem fault injection and all ext2
block-size variants require actual guest/backend tests; the host ext2 structure
tests do not exercise this implementation.

`exepathcheck.c /data/EXEPATH` copies its ELF into a fixture and executes it
through relative and absolute executable symlinks, a symlinked parent directory,
and a symlinked script whose shebang interpreter is itself a symlink. Each child
compares `/proc/self/exe` with the physical ELF pathname; the script case also
checks that interpreter argv retains the script spelling. The same probe passes
on Linux. Build it statically and run on the old kernel first: unresolved exePath
storage should fail these comparisons, rather than merely checking exec success.
After the fixed kernel passes, also launch through Brook's shell directly:
`run /data/EXEPATH/bin/relative --report /data/EXEPATH/lib/payload` and
`run /data/EXEPATH/script-link /data/EXEPATH/lib/payload`.
This checks initial executable pathname resolution only; it does not claim
rename/unlink tracking for `/proc/self/exe`. Brook's existing executable pathname
storage is bounded; the executable opener rejects a resolved name that cannot
fit instead of returning a truncated name. General VFS lookup remains separate
from this strict executable-open resolver.

`procfdcheck.c` verifies that reopening a memfd through `/proc/self/fd/N` or
`/proc/<current-pid>/fd/N` opens its backing object with an independent offset
and requested access mode. It checks inode/device equality, CLOEXEC, sequential
and positional I/O, read-only write/truncate/shared-mmap/mprotect rejection,
shared mapping coherence, close-original survival, inherited access rights
through dup/fork, and surviving mappings after all descriptors close. It also
checks new `/dev/shm` descriptor access modes and namespace ownership after the
creator closes. Linux passes the same assertions. The probe does not assert
shared OFD offsets after dup/fork or shm unlink behavior.

Brook currently supports procfd reopening only for memfds in the current fd
table; foreign-process paths and other descriptor types fail instead of opening
synthetic link-description text. O_PATH/O_TRUNC/O_CREAT procfd modes are outside
this scope. Memfd creation returns O_RDWR; read-only reopened mappings carry a
maximum permission ceiling (module ABI9). Writable MAP_PRIVATE memfd requests
remain unsupported rather than sharing writable pages silently. Concurrent VMA
mutation, mapped memfd shrink/SIGBUS semantics, full private-copy semantics and
real file seals remain separate work. Mapping/remapping no longer grows logical
file size. The existing bounded shm namespace now retains its own backing
reference and serializes publication; shm unlink/reclamation still needs work,
and shm O_TRUNC is rejected because freeing mapped pages is unsafe.

The procfd probe also changes a cold three-page memfd mapping's middle page to
PROT_NONE/PROT_READ, checks that the outside pages retain their permissions,
checks expected child faults inside, restores access, and unmaps all fragments.
`--brook-capacity` additionally creates 768 memfd mappings and verifies that
protection splits succeed beyond the retired 256-entry mapping-table limit.
It checks the protected middle page, both outside pages, restored access and
cleanup; the larger coverage run is Brook-specific and is not run on Linux.
Partial protection/unmap metadata is split with backing references retained;
the capacity probe alone does not establish concurrent metadata/PTE correctness.
Remapping a range spanning distinct memfd fragments is explicitly rejected.
F_ADD_SEALS/F_GET_SEALS and unsupported memfd_create flag bits return EINVAL;
no immutability or cross-sandbox isolation claim is made. Firefox experiments can
use its upstream-supported MOZ_SHM_NO_SEALS=1 setting while sealing remains absent.

`mappingracecheck.c` requires an explicitly configured test kernel with
`-DBROOK_MAPPING_TEST_HOOKS=ON` (default OFF), and runs as guest root. Build the
probe with `-pthread`; invoke it with a disposable FAT or ext2 file pathname.
The test-only scalar syscall `0xB30` arms a barrier for the caller's address-space
identity and VA interval, queries arrivals/waiters, or releases waiting faults.
The query/release controls are global so a parent can cancel its child. Neither
the control syscall nor barrier waits are included in ordinary kernels.

The actual file/memfd resolver stops outside all mapping/backing locks, either
after retaining its range snapshot or after preparing pages before publication.
The probe requires both competing faults to arrive before release, checks
same-page and adjacent-page loads, replaces the mapping with MAP_FIXED and
verifies replacement bytes survive, and requires SIGSEGV after unmap or write
permission removal. Cancellation cases require EBUSY while a stopped file fault
holds its pin, SIGKILL completion after release, and successful truncate after
consuming wait. A missed barrier or timeout fails. This covers cancellation at
the preparation boundaries, not cancellation while a disk request is blocked.
It does not establish regular-file MAP_SHARED write coherence or writeback.

`unixfdcheck.c` is a bounded pthread/fork socket ownership regression. Four
threads repeatedly duplicate and close one socket endpoint, including racing
`dup2`/`dup3` replacement of descriptor 200, while the main thread forks 24
children which close inherited endpoints. The final original and duplicate must
still transfer exact bytes through the peer. Build statically with `-pthread`,
run `UNIXCHK` under a host deadline, and require `UNIXFDCHECK PASS`. The raw
45-second alarm is advisory; it cannot recover a kernel panic or deadlock. Linux
passes the same probe. Brook may return EBUSY when replacing a descriptor with an
active slot pin; failure leaves both existing descriptors unchanged.

The actual fd-table host tests deterministically reproduce premature last
release using the old publish-before-retain sequence, then test owned publication,
pinned replacement rejection and fork snapshot retention against source close.
Unix socket creation, accept and SCM_RIGHTS installation now publish complete
owned descriptors. Existing process strace (including errors-only mode) enables
up to 512 `UNIXLIFE` allocation/retain/release records for diagnostic launches.
The historical Firefox socket UAF was observed directly; its precise original
release interleaving remains unproven until lifecycle tracing attributes it.
Listener registry lookup now pins before dropping its lock. Concurrent pending
accept queues and SCM_RIGHTS queue mutation/cancellation are still separate
limitations; this checkpoint does not claim all Unix socket synchronization or
shared OFD offset semantics are complete.

Common process fd-table wrappers and finalization now enter cooperative kernel
operation guards before touching plain fd locks or claiming resource ownership.
This prevents fatal cancellation from abandoning the lock or a stack-owned
retirement. The latch lasts through the syscall safe boundary; an unrelated
blocking operation later in that syscall can therefore defer fatal delivery.
Pending terminal cancellation is included in socket interrupt checks, but a
complete audit of all blocking subsystems remains separate work.

`fallocatecheck.c` verifies real memfd reservation: mode0 grows a 2304-byte
cursor-sized object, stat reports physical allocation before first touch, and
its first shared-mapping write reads back correctly. Unaligned allocation,
KEEP_SIZE reservation beyond EOF (observable through allocated st_blocks), zero
contents, retained existing data, unchanged file position, low32 fd ABI and
error cases share one Linux/Brook probe. Run `FALLOC --brook` to additionally
require EOPNOTSUPP with unchanged contents/size on FAT and ext2 regular files;
those backends do not yet implement reservation. The default mode passes Linux.

Memfd fallocate currently supports mode0 and KEEP_SIZE only. It allocates real
zeroed physical pages under the backing lock; an allocation failure frees the
private staged pages before returning ENOSPC and does not advance logical size.
Index growth may remain after failed reservation. No shrinking, hole punching,
seals, or concurrent VMA invalidation is claimed. The 16-GiB existing memfd index
limit is enforced as EFBIG for fallocate. The old success-only syscall is detected
by size0 after reported success; the probe exits before touching the EOF mapping.
OOM rollback is reviewed by construction, not claimed as fault-injection tested.

`socketoptcheck.c` verifies connected AF_UNIX SO_SNDBUF/SO_RCVBUF, SO_TYPE and
SO_ERROR, all optlen truncations from zero through eight bytes with canaries,
bad descriptors, non-sockets, unsupported levels/options, invalid pointers and
negative lengths. Linux passes the default probe. Brook's `--brook` mode also
fills the nonblocking socket to its reported usable capacity and requires the
next byte to return EAGAIN; unattached buffer queries require ENOTCONN.

Unix buffer results are the attached PipeBuffer capacity minus the ring's one
reserved byte. No disconnected buffer default is invented. SO_TYPE preserves
the creation type across accept; preexisting DGRAM/SEQPACKET byte-stream behavior
is not fixed or endorsed by this query implementation. Unix SO_ERROR is zero
because current connect failures are synchronous. Unknown SOL_SOCKET options
return ENOPROTOOPT; an unsupported Unix option level returns EOPNOTSUPP, matching
the Linux reference. Existing INET SO_ERROR is retained, without claiming other
INET options or the separate setsockopt stub are implemented.

`peercredcheck.c` checks Unix SO_PEERCRED's Linux 12-byte ucred ABI, every
0–16-byte result length with canaries, and unconnected {0, -1, -1} identity.
Socketpair credentials remain the creator's after fork, peer close and credential
changes; a pair created by a pthread reports TGID. Path connections capture
client effective UID/GID at connect and listener identity at first listen,
including accepting through an inherited listener. Run `PEERCRED --require-root`
to require the root-server / UID-and-GID-1000-client case; an unprivileged host
run tests same-user connections and explicitly reports root_case=0.
Snapshots contain values, not live Process pointers, and survive PID recycling
without retargeting. They do not add SCM_CREDENTIALS or fix the separately
tracked pending-connection/ancillary-queue concurrency and cancellation gaps.
Semantics reference: https://man7.org/linux/man-pages/man7/unix.7.html#SOCKET_OPTIONS

`unixpendingcheck.c` tests a stream client sending a retained memfd and closing
before server accept, with a parent/child barrier and allocation churn. It runs
32 rounds, checks the received file's marker and EOF, listener abandonment, and
an `EMFILE` accept followed by retry without losing the pending connection.
The pending slot owns its pipe ends and ancillary queues until successful accept
transfers them or listener teardown releases them. This fixes early-close
lifetime ownership; concurrent ancillary producers/consumers and general Unix
socket cancellation semantics remain separate work.

`gsbasecheck.c` exercises `ARCH_SET_GS`/`ARCH_GET_GS` using actual `gs:0` loads,
four threads and 8,000 scheduling yields, thread/fork inheritance, exec reset,
zero-base restoration, unmapped low bases, rejected high/noncanonical bases,
and invalid output pointers. Build with `-static -O2 -pthread` and run as
`/boot/GSCHECK` in Brook. The default probe also runs on x86-64 Linux with
`ARCH_SET_GS` enabled. This does not enable the FSGSBASE instructions. Brook
keeps the active kernel GS CPU-local and saves the inactive user GS per thread;
module ABI 10 requires matching rebuilt modules.

`cachecowcheck.c` warms a 1 MiB file, then runs eight rounds of four forked
children with four threads each. Three threads read into inherited CoW buffers;
the fourth repeatedly faults private file mappings. It checks every returned
byte, reads spanning the 64 KiB kernel bounce limit, short reads at EOF and final
offsets, and the parent's unchanged memory. Build with `-static -O2 -pthread`
and run `COWCHK /data/CACHECOW.TST`. The initial core probe reproduced the old
ext2 cache/TLB deadlock; the expanded probe passed the fixed kernel. This is a
concurrency stress regression, not proof of every possible interleaving.

Ext2 reads to user addresses now stage through at most 64 KiB of resident kernel
memory. The filesystem read completes and releases its locks before copyout can
fault. Kernel-buffer reads retain the direct coalesced path. Cache insertion
allocates missing buffers outside the cache lock and disposes of unused racing
allocations after unlocking. Invalid user-buffer recovery and concurrent VMA
invalidation remain separate compatibility work.


`brkexeccheck.c` is a Brook-specific nonleader-exec isolation check. Compile with
`musl-gcc -static -O2 -pthread brkexeccheck.c -o brkexeccheck` and run it in the
guest under a host watchdog. It checks the current detached nonleader-exec
model, separate new-image heap growth and continued sharing by old-AS tasks;
it does not assert Linux exec semantics. Internal pipe polls have five-second
bounds. It passes both the pre-transaction baseline and transaction candidate.


`wmfilewaitcheck.c` runs 32 native-window exits with a private file mapping,
consuming wait and immediate truncate. Its optional `--delayed-renderer` mode
requires the opt-in mapping test kernel and keeps the compositor parked at frame
entry through wait and truncate; state checks reject early release or timeout.
Run against both `/data/WMWAIT.TST` and `/boot/WMWAIT.TST`. It does not test an
old compositor pixel snapshot or in-flight GPU submission.

`terminalstresscheck.c` must replace the actual native terminal's owner using
`exec /boot/TERMCHK /data/TERMCHK.OK`. Compile static with musl, `-O2 -Wall
-Wextra -Werror`. It requires the opt-in mapping test kernel for epoch queries,
checks the retained source immediately after exec, writes exactly 256 KiB,
then prompts for two GUI resizes. Choose sizes whose pixel byte lengths leave
at least four bytes of page padding. An old alias canary must survive the second
resize and three epochs, while the new allocation's padding must be zero.
Initial dimensions must differ from fallback 640×480. The durable result file
records completion after the terminal closes; a launch acknowledgement is not
success. A 150-second POSIX timer bounds the run, including blocked output.


`filesharedcheck.c` is a Linux-contract probe for writable regular-file sharing.
Compile static with musl and `-O2 -Wall -Wextra -Werror`, and supply a fresh path
(created O_EXCL). It checks aliases across separate opens and fork, ordinary I/O
coherence, private CoW, msync/fsync/reopen, truncate/EOF SIGBUS, shrink/regrow
zeroing and lifetime after unlink/close. A 60-second POSIX timer bounds the run.
Run the Linux reference with core dumps disabled because one child deliberately
accesses a whole page beyond EOF. It currently passes Linux; Brook's unsupported
shared-write path is an expected failing baseline, not a permitted success.
This does not prove simultaneous same-page misses or durability across reboot.
See `docs/file-shared-mappings.md` for the implementation and remaining gates.


`framealiascheck.c` is a Brook framebuffer regression for managed frame sharing.
Compile static with musl and `-O2 -Wall -Wextra -Werror`, then run `/boot/FRMALIAS`.
It maps the first framebuffer page 512 times, closes the fd, verifies a shared
canary through every alias and removes aliases while checking the survivors.
The final alias is unmapped before exit. A 60-second POSIX timer bounds the run.
This exercises real guest mappings above the old 255-reference limit, not file
writeback. The separate hosted `test_pmm_refs` uses actual PMM code to verify
70,000 references/mapping counts and final freeing with exact count assertions.


`filecoherencecheck.c` checks the ordinary-I/O stage of ext2 coherent mappings.
Build static musl with `-O2 -Wall -Wextra -Werror -pthread` and run
`/boot/FILECOH /data/FILECOH.TST` against a fresh path (O_EXCL). It checks pwrite
visibility through read-only shared aliases across opens/threads, private CoW,
file-backed user buffers, an untouched mapped write source, unmapped truncate/
regrow invalidation, and unlink/close lifetime. It passes Linux and Brook. The
60-second timer bounds the run. This does not test writable shared stores, mapped
truncate, msync, or deterministic simultaneous kernel misses. Brook's syscall
validation populates the untouched source before the file write operation.


`futexexitcheck.c` reproduces protected-operation cancellation with four untimed
private futex waiters and no normal waker. Each child touches 32 MiB, observes all
four workers sleeping through procfs, then invokes exit_group. The parent requires
each group to be reaped within five seconds for 32 rounds and checks free-memory
recovery after warming caches (one footprint of tolerance). Build static musl with
`-O2 -Wall -Wextra -Werror -pthread`; run `/boot/FUTXCHK` on an otherwise idle guest.
Memory is a bounded growth check, not an exact PMM-reference audit. A host run
sanity-checks probe logic; only the actual Brook guest demonstrates cancellation.
The pre-fix kernel 2582aece fails on round zero with retained off-CPU futex threads.


`namedshmcheck.c` exercises /dev/shm with unique disposable names and a 60-second
watchdog. It holds 96 names simultaneously, checks independent opens and stable
backing identity/CLOEXEC, unlink/recreate isolation while old mappings stay valid,
fork sharing after descriptor close, and an eight-thread exclusive-create race.
Require `NAMEDSHMCHECK PASS`; errors are failures rather than an accepted fallback.
This checks named-open/unlink and descriptor/mapping lifetime, not a complete
tmpfs implementation (directory enumeration, path stat and permission ownership
remain separate). The initrd provides /dev/shm for fresh application discovery.

`modecheck.c` additionally checks creat's write-only access, umask and existing
inode truncation/mode preservation; mkdir/mkdirat check private, zero and sticky
modes, umask and EEXIST for occupied names. Run the permissions probe on ext2.
FAT and the experimental lwext4 adapter do not offer ext2's creation-mode contract.


`exepathcheck.c` now includes direct execution of /proc/self/exe and a second
self-reexec after launching through a symlink to a copied ELF. All six cases
require the resolved executable identity from readlink. The self-reexec supplies
a custom argv[0]. Brook currently resolves the recorded
pathname for exec; this is not an inode-pinned magic link surviving unlink or
rename, and does not implement opening /proc/self/exe as an ordinary descriptor.


`eventfdcheck.c` has a 60-second watchdog and checks invalid eventfd2 flags,
nonblocking empty reads, semaphore reads, descriptor/status flags, close-on-exec
through a real self-reexec, and concurrent producers while the counter is drained.
Require `EVENTFDCHECK PASS`; this does not establish overflow or every blocking
waiter behavior. Chromium's Mojo eventfd feature probe intentionally supplies
invalid flags, so a successful descriptor there is a startup-fatal false result.

`passcredcheck.c` checks automatic SO_PASSCRED/SCM_CREDENTIALS delivery on a
connected Unix stream. Ninety-six forked writers use an inherited endpoint;
the receiver requires each child's actual PID after it has exited, including
partial reads, rather than the connection-time SO_PEERCRED PID. It also checks
enable/disable and getsockopt, short setsockopt values, simultaneous memfd rights
with CLOEXEC, and truncated ancillary output. Build static musl with
`-O2 -Wall -Wextra -Werror`; expect 887 checks and zero failures on Linux and
Brook. Automatic credentials use the sender's real UID/GID. Explicit user-supplied
SCM_CREDENTIALS remains EOPNOTSUPP. The bounded metadata ring backpressures
writers after 63 pending distinct credential spans; adjacent identical senders
coalesce. This does not establish datagram/SEQPACKET record boundaries or
credential-namespace translation. Pipe descriptor passing remains unsupported;
the rights case deliberately uses a supported memfd.

`discardcheck.c` requires `-pthread` and checks private anonymous
MADV_DONTNEED: rounded lengths, untouched neighboring pages, zeroed read-only
and PROT_NONE pages after restoring access, fork isolation, invalid arguments,
and a sparse 1-GiB reservation. Eight synchronized readers warm translations
before each of 32 discards and require zero bytes after return. Expect
`DISCARDCHECK PASS checks=60 failures=0 reader_errors=0 rounds=32` on Linux and
Brook. Brook currently replaces populated frames eagerly; this verifies content
and lifetime, not RSS reclamation. File/shared/device discard is unsupported.
The hosted VMM test separately covers pre-publication OOM rollback, hidden
leaves, bounded address collection and retirement after a delayed TLB ACK.

`fdcapacitycheck.c` requires RLIMIT_NOFILE >=1024, opens 512 eventfds, registers
all of them with one epoll instance, writes each, and requires all 512 readiness
notifications through poll and epoll without duplicates. It drains all counters,
checks readiness clears, and verifies F_DUPFD_CLOEXEC at descriptor 900. Build
static musl with `-O2 -Wall -Wextra -Werror`; expect
`FDCAPACITYCHECK PASS descriptors=512 poll=512 epoll=512 high_fd=900`.
This checks capacity and descriptor lifecycle, not whether a browser leaks
descriptors over time.

`shutdowncheck.c` requires `-pthread`. It compares connected AF_UNIX stream
shutdown with Linux: buffered bytes and SCM_RIGHTS survive shutdown until read,
SHUT_WR preserves reverse traffic, SHUT_RD rejects peer writes, and poll/epoll
report readable EOF and RDHUP without a premature HUP on half-close. Eight rounds
wake four blocked readers plus poll/epoll waiters, and four blocked writers on a
full queue; SIGPIPE delivery and MSG_NOSIGNAL suppression are checked. One case closes the pinned writing descriptor before shutting down
through a duplicate. Require `SHUTDOWNCHECK PASS checks=722 rounds=8`, then normal
browser cleanup, heap integrity and clean offline filesystem checks. A 60-second
alarm bounds the probe. Internet sockets and unbound/listening Unix shutdown
remain outside Brook's connected-stream subset and return EOPNOTSUPP.

`faultgroupcheck.c` requires `-pthread` and x86-64. Nine cases exercise actual
worker SIGSEGV, SIGILL and SIGFPE faults, a leader SIGSEGV, a handled worker
SIGSEGV, and a worker fault after raw leader `SYS_exit(0)`. Additional cases
put the leader or a peer in an indefinite raw `rt_sigtimedwait` for blocked
SIGUSR1 while another worker faults; external SIGKILL also cancels an entire
group waiting in that syscall. Other workers block in futex and pipe reads.
The parent waits for readiness, triggers the fault,
then uses blocking waitpid with an alarm deadline. It checks the exact fatal
signal and pipe EOF after group teardown; the handled control must remain alive
until explicit SIGKILL. Readiness waits retry EINTR, including SIGCHLD from a
raw leader exit. Failure output identifies the stage and distinguishes the
observed wait result from cleanup. Run `faultgroupcheck 8` for 72 cases, with
an external host deadline as well. The Linux and Brook controls pass 72/72
(Brook session `ac8a974dcc5f4a64b0e87e2163b2bf29`), returning to nine service
tasks. The old kernel reproduces a stuck leader in the new signal-wait case;
the fix checks terminal cancellation and uses an interruptible scheduler wait.
The same fixed session passes all 85 pending-signal checks and the glibc
POSIX-timer suite with 16 callbacks and required TID reuse. The original six
cases and separate fatal-signal probe previously passed 48/48 each in session
`72f2260bb6cf4742bfff86664d5fbe5b`. Follow guest
acceptance with task-count recovery, checked shutdown and offline fsck. This
tests fault containment, not a fix for VLC's underlying invalid proxy access.

`fatalgroupcheck.c` requires `-pthread`. It creates a child with futex, pipe,
epoll and timed-sleep workers. Twelve cases cover default SIGTERM to the leader
and a nonleader, nonleader `tgkill(SIGKILL)`, raw `alarm` SIGALRM, masked TERM,
handled/ignored TERM, raw leader `SYS_exit`, racing thread creation, default
SIGUSR1, masked SIGUSR2 and real-time signal 34. Parent waitpid is bounded;
failing cases request process-wide SIGKILL and use a second bounded wait.
Handled/ignored controls must exit normally, fatal cases must report the exact
signal, and raw leader exit must leave peers alive until explicitly released.
It does not establish complete process-wide routing or stop/continue semantics.

```sh
mkdir -p build/io-bench
nix-shell --run '"$MUSL_CC" -O2 -Wall -Wextra -Werror -static -pthread tools/at-syscall-probe/fatalgroupcheck.c -o build/io-bench/fatalgroupcheck'
```

Stage the ignored binary on a disposable VM disk while stopped, then run
`run /data/fatalgroupcheck 4` through `vm_guest_run`. Expect
`FATALGROUPCHECK PASS rounds=4 cases=48 failures=0`, no remaining test tasks,
heap integrity OK and no kernel ownership warnings. Follow with checked guest
shutdown and offline filesystem checks. The same binary is a Linux control.


`cursorcheck.c` is Brook-only: it uses custom syscall 519, so do not run it as
a Linux control. It checks invalid dimensions, null/protected source memory,
an unaligned 16 KiB image across five mappings, and 500 uploads racing 200
unmap/remap cycles. Expect `CURSORCHECK PASS ... failures=0`; both accepted and
EFAULT results are valid during the race. `show` publishes a small teal cursor
with a nonzero hotspot; `reset` restores the default arrow. Use these modes
while launching an app to inspect busy-badge preservation and reset.

```sh
nix-shell --run '"$MUSL_CC" -O2 -Wall -Wextra -Werror -static -pthread tools/at-syscall-probe/cursorcheck.c -o build/io-bench/cursorcheck'
```

Stage the ignored executable only on a stopped disposable VM disk. Run the
probe through `vm_guest_run`, inspect its completion marker and kernel logs,
then use checked shutdown and offline filesystem checks.


`wmterminalresizecheck.c` is Brook-only and must be launched from a native
Terminal, not directly through `vm_guest_run`. It creates a GUI as a bash
descendant, maximizes/restores it twelve times, and verifies that its parent
terminal's character grid never changes. Expect
`WMTERMINALRESIZECHECK PASS rounds=12 parent=<rows>x<cols>`. The older incorrect
controlling-terminal lookup changes the parent grid on the first maximize.

```sh
nix-shell --run '"$MUSL_CC" -O2 -Wall -Wextra -Werror -static tools/at-syscall-probe/wmterminalresizecheck.c -o build/io-bench/wmterminalresizecheck'
```

Stage on a stopped disposable disk, open Terminal, and execute the staged path.
Compare the previous and candidate kernels, capture the visible result, and
finish each run with checked shutdown and offline filesystem checks.


`wmdesktopscalecheck.c` is Brook-only (custom syscall `0xB04`). With no argument
it checks current requested/applied scale and invalid requests; with `100`, `150`
or `200` it requests that native desktop scale and waits up to five seconds for
application. Run while the compositor is active, and inspect `SCALECHECK PASS`
in guest/serial output. It does not save settings or test toolkit content scale.

```sh
nix-shell --run '"$MUSL_CC" -O2 -Wall -Wextra -Werror -static tools/at-syscall-probe/wmdesktopscalecheck.c -o build/io-bench/wmdesktopscalecheck'
```

Stage only on a stopped disposable VM disk. Use Settings for save/reboot
acceptance, and check window controls against fresh screenshot coordinates.


`drmbindcheck.c` verifies Brook's retained DRM bridge using the public render-node
ABI: device access/stat/statx (including descriptor metadata), DRM_VERSION,
virtual sysfs attributes, genuine directory binds, namespace copies, fake-device
masking, `nodev`, retained open descriptors, and pivot-root/detach.

Compile with the usual static musl toolchain. Run `--brook` as a nonroot user in
a disposable Brook VM; it creates its own user/mount namespaces. `--linux-root`
is for a disposable Linux control VM with namespace privileges. `--native`
performs only the baseline metadata/open checks. Keep execution inside test VMs;
no host mount tests are needed. Successful Brook output includes
`DRMBIND_FIXTURE_PASS` and `DRMBIND_PARENT_PASS`.

The initrd must be regenerated with real `/dev/dri` and `/sys/dev/char`
directories. Broker visibility follows their retained identity, including genuine
binds; replacing them with unrelated directories must hide the broker. `nodev`
still denies new device opens without revoking descriptors already opened.

`sharedcontrolcheck.c` tests a small named control channel with an independently
exec'd helper. It uses an 8,208-byte object (including the final 16-byte partial
page), separate opens, shared read/write and read-only aliases, and closes file
descriptors while mappings remain live. Eight pipe-ordered exchanges isolate
basic coherence; 64 acquire/release exchanges then exercise process-shared
futex wait/wake after unlinking and replacing the pathname. The helper uses a
fixed free virtual range distinct from the parent, and at least one successful
blocked wait and nonzero wake result must be observed. The replacement
must remain separate from the still-live old channel. A 30-second watchdog
bounds the test, and the parent cleans up its owned child and pathname.

Build static musl with `-std=c11 -O2 -Wall -Wextra -Werror`. Run the binary without
arguments as the desktop user, with writable `/dev/shm`, working proc-exe and
permission to execute the binary again. Require both `SHAREDCONTROLCHECK
pipe-coherence PASS` and the final `SHAREDCONTROLCHECK PASS` with exit zero and
no live descendants. It prints fixed stages/counts, never application contents.
This tests known-data coherence and ordinary shared-futex handshakes; it does
not establish Steam protocol correctness, owner-liveness interpretation, or
all possible shared-memory races.

`loopbackcheck.c` checks TCP on `127.0.0.1` and `127.0.0.2`, wildcard
listeners, blocking and nonblocking connect with poll/SO_ERROR, bidirectional
8,193-byte transfers in both directions, FIN/EOF, accepted local/peer addresses,
measured close time, and a UDP round trip with
source verification. It uses a raw alarm watchdog. This probe waits for listener
readiness before accept; it does not certify blocking accept before connection,
full UDP demultiplexing, or concurrent close semantics. Close timing is reported,
not asserted against a scheduling-sensitive threshold.

`acceptcheck.c` requires `-pthread`. It checks delayed blocking accept, accept4
flags versus listener flags, an empty nonblocking listener, poll and epoll
registered before connection, handled-signal interruption, accepted connection
bytes, and closing the accepting descriptor while a duplicate remains live.
The close case permits either completing the original accept or EBADF, then
requires the duplicate listener to accept a fresh connection. Linux passes 28
checks. Parent and forked children use raw alarm watchdogs; also retain the
whole guest job until all descendants drain. The test does not directly inspect
kernel waiter pointers or establish wake latency under load.
