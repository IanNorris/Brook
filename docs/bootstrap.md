# Building Brook inside Brook

Use an isolated, writable experiment clone with the VM bridge enabled, one vCPU,
and silent host audio. Keep the normal prepared checkpoint and source checkout
separate from the test. The development toolchain package and installation
commands are in [tools/bootstrap-pkg](../tools/bootstrap-pkg/README.md).

The test has distinct milestones: guest package installation, a real HTTPS Git
clone and the two required submodules, building the kernel/bootloader/modules
and bare-metal tests in the guest, running hosted tests in Brook, then booting
the resulting guest-built kernel. A host-exported binary SDK supplies tools;
that does not imply that Nix or the compiler was rebuilt inside Brook.

## Compatibility defects found during preparation

The first real Git clone found that BusyBox fallback advertised every unknown
system-bin command as an executable. Git stopped PATH search before reaching
its real remote helper. The fallback now checks the shipped static BusyBox's
actual applet names. `scripts/generate_busybox_applets.py ./busybox_static`
regenerates the allowlist when replacing that binary; rebuild the kernel too.

Git's ordinary failure exit code 128 was also reported as successful: wait4
inferred signal death from every code >=128, turning 128 into zero. Termination
now records Linux wait status explicitly, preserving all eight normal exit bits
and distinguishing signals. The `waitcheck` probe validates raw exit/exit_group
codes 0/1/127/128/255, SIGTERM/SIGKILL and Bash `set -e` external-child behaviour.
The process state release stores also use the field's actual byte width.

The real kernel link command contains over 100 arguments. Previous exec copying
and stack setup silently capped vectors at 64 entries. Shared limits now allow
512 arguments, 256 environment entries, 64 KiB argument strings and 32 KiB environment
strings. Invalid pointers return EFAULT and count/byte overflow returns E2BIG
before image replacement. Shebang argument insertion is checked and private;
it no longer uses a shared scratch buffer or discards trailing arguments.

`execcheck` exercises 128/256 arguments, over 48 KiB strings, exactly 512 argv and 256
env entries, a 20 KB single string, count/byte overflow, unmapped-page pointer and
string boundaries, a NUL at the final mapped byte and shebang preservation.
Both probes passed in isolated Brook regression guests, as did 13 host tests.
Their sources and invocation examples live in [tools/at-syscall-probe](../tools/at-syscall-probe/README.md).

The next clone attempt exposed filesystem metadata defects: path `statx`
invented mode `0644` and inode `1`, and all stat variants dropped ext2's stored
timestamps. Git uses executable mode bits to find remote helpers, while Ninja
needs modification times. Stat variants now share real inode metadata; ext2
updates creation/write/truncation times and supports `utimensat`/`futimens`
with whole-second precision. Old files legitimately retain their old on-disk
timestamps until repaired with a real guest `touch`.

Reporting the actual permissions then exposed a package creation defect:
`open(O_CREAT, mode)` discarded the requested mode, leaving NAR executables
at `0644`. The installer itself already requests `0755` for executable files.
Creation must preserve that mode after applying the guest umask, and mode
updates must dispatch through the file's actual mount rather than the first
ext2 mount. Existing SDK files need their original executable metadata restored;
do not mark the entire Nix store executable.

Always quiesce guest writers and sync before stopping or snapshotting a VM.
Ext2 buffers allocation bitmaps while writing inodes and directory entries
immediately. An unsynced QMP quit can leave allocated inodes marked free, so
the next boot can reuse them and corrupt existing files. Normal bridge stop
and reset must await the synchronous guest sync reply; forced shutdown is
explicit and cannot promise a consistent disk. This flushes guest metadata
caches under normal I/O, not a journaling or power-loss durability guarantee.

The metadata and mode probes passed in Brook, with mode tests on both data
and Nix mounts. All 13 host tests and 26 bridge tests passed. Recovery used
independently `e2fsck -fn`-clean checkpoint copies; damaged disposable images
were retained for diagnosis. The installed SDK's 1,083 executable files were
identified from its original binary closure, rather than guessed from names.

Git's threaded DNS resolver also exposed an indefinite `pselect6` wait after
its worker wrote the eventfd notification. Select/pselect now use poll's
readiness and waiter handling instead of treating eventfds and sockets as
always ready. `selectcheck` passed in Linux and Brook, including delayed
eventfd/pipe writes, TCP accept/data, timeout units, high descriptors and
unmapped-buffer boundaries. Brook's TCP test uses its configured NIC address;
127/8 loopback routing is still missing. The probe uses a separate parent
watchdog because Brook does not implement `alarm` yet.

After object transfer, Git found phantom references: opening a missing refs
directory with a trailing slash fell back to `/boot/lib/`. Basename fallback
is now limited to read-only shared-library names under intended library/Nix
prefixes; missing directories and unrelated names fail normally. `opencheck`
passed in Brook, and a fresh guest `git init` plus `git fsck --full` succeeded
without phantom references. The existing exact `/boot` aliases remain supported.

The main HTTPS clone then succeeded, but submodule update repeatedly registered
URLs and still considered them uninitialized. Raw `access(".git/config")`
looked up the file under `/` instead of the caller's cwd, although absolute
reads and mmap agreed on the valid configuration. Access variants now resolve
cwd/dirfd paths, and classic symlink/rmdir/rename use cwd too. `accesscheck`
passed all 26 assertions in Linux and Brook, including relative rename and
cleanup. Retrying the existing clone fetched both pinned submodules and verified
`vendor/uefi-headers/Include/Uefi.h` and `src/third_party/fatfs/source/ff.c`.

Access permission-bit evaluation and long stored directory-fd paths remain
separate limitations; these checks validate pathname resolution and existence.

CMake's child-process event loop exposed caught SIGCHLD being discarded by
blocking pipe/tty/AF_UNIX reads. Reads now ignore only default/ignored SIGCHLD,
preserve installed handlers and clear their wait registration on interruption.
Epoll checks pending signals before sleeping. The five watchdog-protected
`childsignalcheck` cases passed in Linux and Brook, including exec-child epoll
notification, pipe/socket interruption, SA_RESTART and default-ignored SIGCHLD.

Runtime evidence is retained under ignored `build/experiments/results/bootstrap-20261014/`
and `bootstrap-compat-20261014/`, and the VM registry keeps serial logs and the
actual booted kernel identities. Package payloads, images and logs stay out of git.
