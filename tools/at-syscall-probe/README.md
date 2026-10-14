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
