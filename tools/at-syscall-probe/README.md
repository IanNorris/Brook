# Guest runtime regression probes

Build the C probes in `nix-shell` using `$MUSL_CC -static -O2` (add
`-pthread` for `fdstress.c` and `brkcheck.c`). Keep binaries in ignored `build/`.
Run them inside a disposable Brook VM through `vm_guest_run`:

- `sleepcheck.c`: raw nanosleep waits of 0, 1, 10,000, 999,999, 1,000,001
  and 5,000,000 ns never complete early; negative seconds/nanoseconds and
  out-of-range nanoseconds return EINVAL. Expect nine checks and zero failures.
- `filelifecheck.c`: takes an absolute writable directory. Checks independent
  descriptions after unlink, orphan writes and inode reuse, an unfaulted mmap
  surviving last-descriptor close, rename replacement and same-inode rename,
  nonempty-directory rejection, unlink/rmdir type distinctions and directory
  link counts. It also performs 32 rename replacements. Run only on disposable
  disks; after its sync and a clean VM shutdown, require offline fsck to pass.
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
