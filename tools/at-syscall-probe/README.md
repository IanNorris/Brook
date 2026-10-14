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
