# Steam named FIFO checkpoint — 23 October 2026

Brook now creates real named FIFOs on tmpfs and Ext2. The native Steam client
gets past both its urandom and `m_NamedPipe` assertions inside SteamRT. This is
not a Steam login-screen or authentication acceptance.

FIFO state is keyed by a retained filesystem object and backing identity, not a
pathname. Bind aliases and copied mount namespaces share a stream; unlink and
recreation produce a separate stream. The last open description retires the
stream and discards unread bytes. Duplication, fork and SCM_RIGHTS keep the
existing description alive. Module ABI 47 adds the backend creation callback
and vnode stream ownership fields; all modules must be rebuilt together.

The implemented subset includes FIFO-only mknod/mknodat, umask and parent
permissions, blocking and nonblocking open, RDWR, EOF/EPIPE, interruptible waits,
small atomic writes, FIONREAD/FIONBIO, poll/select/epoll subscriptions and ESPIPE.
Unsupported device-node creation remains explicitly rejected. The existing
restricted FIFO bind subset requires a unique entry. Read-only remount tests
respect Brook's existing NOSUID and monotonic-restriction policy.

Blocking pipe writers now hold exact, scoped poll subscriptions instead of a
raw process pointer. This also fixes interrupted-writer cleanup and allows
multiple blocked writers. Legacy vnode constructors explicitly initialize the
new fields.

## Validation

The portable probe is `tools/at-syscall-probe/fifocheck.c`. Final Brook runs pass
160 fixture checks on tmpfs and 159 on Ext2, plus seven parent checks each.
The exact same final binary also passes on Linux: 161 fixture checks on tmpfs
and 160 on disk, plus seven parent checks each. Both service cgroups finish
empty, and the Linux VM shuts down and passes its offline filesystem check.
These include delayed epoll wakeup, blocking open rendezvous, descriptor passing,
unlink/recreation, mount aliases, interrupted competing writers and last-close
buffer disposal. The hosted suite passes all 101 tests; four relevant sanitizer
tests and the ELF entry/scheduler audits also pass. Hosted tests exercise the
actual backend and pipe write/subscription code; the complete FIFO registry is
exercised in the native VM, not directly in a hosted test.

The unchanged default-copy SteamRT child exits zero, reports glibc 2.42 and
SteamRT 3c. Device, retained-file, namespace, mount, anonymous-pipe, socket and
nine native regression checks pass, as does a desktop launch/close check.
The heap check succeeds. Clean shutdown and all four offline filesystem checks
pass. Full runtime/client logs were extracted from stopped disks and checked
against guest byte counts and SHA-256 hashes.

Evidence is under ignored `build/workloads/fifo-v2-20261023/` and
`build/reviews/fifo-v2-off-gpu-1ms-fixed-mlfq-a-20261023/`. The final accepted probe
source and binary are frozen separately in `final-probe/`; the kernel capsule
contains an earlier probe revision. Earlier failed probe assumptions and their
original jobs remain saved. One overlarge diagnostic inventory lost bridge
output and is excluded from acceptance; a bounded replacement supplied the
needed facts.

## Remaining Steam startup work

The initial outer-client re-exec reports an existing instance and leaves a
helper alive. After saving evidence, that exact helper was terminated and its
tracked group drained. Direct launch through the unchanged SteamRT entry point
advances to the updater. The private 8 GiB data disk genuinely has insufficient
unreserved space. A separate attempt setting the library path inside the
container also gets past the missing `libGL.so.1` loader error and selects the
X11 update UI. It still fails the disk-space gate. Expand a fresh private disk
before retrying; preserve the checked original.

The client payload retains prior Nix ELF-interpreter patches. Neither its
launcher nor pressure-vessel was patched in this checkpoint. 32-bit helpers
remain unsupported. Shared regular-file offsets across fork/dup are separate
kernel work; unique append-mode logs avoid overwriting diagnostic output but do
not fix that ABI gap. General flock semantics and full FIFO POSIX coverage are
not claimed. No paid game files, disks, logs or authentication data belong in git.

## Follow-up: runtime libraries and updater boundary

A new private review clones the checked disks and expands its data disk to
16 GiB offline. The same ABI 47 kernel boots with over 7 GiB of unreserved free
space. Default runtime copying completes, and Steam's graphical X11 updater
appears and extracts packages. Committing that update then fails renaming
`package/tmp/linux64/crashhandler.so_` to `linux64/crashhandler.so` with EXDEV.
This needs a mount-layout comparison with Linux before changing rename rules;
the two directories being on one backing device does not imply they are on the
same mount attachment.

`scripts/workloads/steam_client_check.py` makes the diagnostic client launch
repeatable from inside the guest, as a nonroot user with an existing Xwayland
server. It checks unreserved free space, creates a separate home and unique
append-mode log directory, launches through the installed SteamRT entry point,
and reports log sizes and hashes at wrapper exit with bounded tails. Detached
descendants can append later; wait for the tracked group to drain before
extracting final files, and verify the wrapper snapshot as a prefix. It restores library
paths inside the container: the Nix-patched interpreter needs the container's
standard library directories, including aliases materialized by SteamRT from its
runtime metadata. Client libraries take priority over host exports so Steam's
patched FFmpeg symbols resolve to its bundled libraries.

The optional `--skip-bootstrap` switch explicitly separates the already-staged
client test from updater acceptance. With that diagnostic option, the exact
launcher gets past the missing GTK 2 library and `av_mallocz_tracked` collision,
loads `steamui.so`, and starts the Chromium-based `steamwebhelper`. It then fails
`glXChooseVisual`; the web helper also reports failed shared-memory creation in
its container's `/dev/shm`. These are the next graphics and IPC integration
gates. Keep namespace-local shared memory isolated: do not restore the old
global shared-memory broker through a shadowing container mount.

This is still not a QR-login success. Missing 32-bit helpers, runtime launcher
bus diagnostics, inotify, and NETLINK warnings are also retained in the logs;
they have not all been established as fatal requirements. The new review and
all distinct launch attempts are under ignored
`build/workloads/steam-space-20261023/` and the associated `steam-space` review.

The follow-up also finishes with all 21 tracked jobs terminal, a healthy heap,
a clean guest sync/shutdown, and all four offline filesystem checks passing.
Extracted logs verify the wrapper snapshots as prefixes and retain hashes of
the final complete files. Distinct failed startup attempts and one failed
pre-shutdown argument assertion remain recorded as diagnostics.
