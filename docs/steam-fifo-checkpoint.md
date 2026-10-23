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
