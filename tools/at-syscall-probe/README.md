# Guest runtime regression probes

Build the C probes in `nix-shell` using `$MUSL_CC -static -O2` (add
`-pthread` for `fdstress.c` and `brkcheck.c`). Keep binaries in ignored `build/`.
Run them inside a disposable Brook VM through `vm_guest_run`:

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
`pselect6` and `epoll_pwait` with SIGCHLD blocked outside each wait. A queued
signal and a delayed exec-child exit must invoke the handler and return EINTR
even with SA_RESTART. The monitored pipe keeps its writer open, so EOF cannot
hide missing signal delivery. The handler checks its active mask and saved
ucontext; the caller checks restoration after handler, timeout, readiness and
error paths. Invalid sizes and masks crossing a truly unmapped page are checked,
and SIGKILL/SIGSTOP must remain unblockable. Each case has a 15-second
fork/wait/monotonic-clock watchdog. Expect 27 checks and
`WAITMASKCHECK PASS failures=0` on Linux and Brook.

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
is sized with write rather than regular-file ftruncate, which remains a stub.
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
