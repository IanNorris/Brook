# Steam PulseAudio and shared-memory checkpoint

The memfd investigation identified a socket error-contract bug and a separate
shared-memory capacity limit. Steam login has not yet passed acceptance.

## PulseAudio diagnosis and fix

A private forwarding probe identifies PulseAudio mempool creation as the source
of the repeated memfd allocations. Linux and Brook reduced context-lifetime
controls both release their descriptors; missing-server controls alone do not
reproduce Steam's accumulation. This does not establish a generic memfd leak.

A real PulseAudio daemon exposes a concrete Brook incompatibility: PulseAudio
first calls send(MSG_NOSIGNAL) on its pipe/eventfd notification descriptors and
falls back to write only for ENOTSOCK. Brook returns EBADF instead. Both sendto
and recvfrom now distinguish a valid non-socket descriptor (ENOTSOCK) from an
invalid, closed or O_PATH descriptor (EBADF). Socket behavior is retained.
The relevant public implementation is PulseAudio 17.0 core-util.c, pa_write.

The new sockettypecheck native probe passes 85 checks on Linux and Brook,
including untouched notification state, actual send-to-write fallback, zero
lengths, high descriptor argument bits and a Unix socket positive control.
The initial O_PATH fixture used /dev/null, which Brook cannot currently open as
O_PATH; that failed receipt is preserved. The corrected fixture uses the valid
O_PATH directory / and still checks EBADF. Synthetic-device O_PATH remains a
separate compatibility gap.

Actual pa_write calls from both installed PulseAudio library builds pass pipe
and eventfd fallback controls. A real UID-1000 PulseAudio daemon with a private
cookie-authenticated Unix socket accepts pactl info. The public diagnostic
helper uses a null sink; this is protocol acceptance, not audible playback.
The optional Steam launcher arguments pass the explicit endpoint and cookie
path through SteamRT without exporting cookie contents.

With the real server, one Steam observation has 94 descriptors and 2 memfds,
rather than approximately 1,000 descriptors and 943 memfds. This is a bounded
startup observation, not long-term leak acceptance. No login window appears.

## Shared-memory capacity

On the socket-fixed kernel, the 16 MiB canonical /dev/shm reaches zero available
space during startup and Steam exits 135. A simultaneous shared-futex control
also receives SIGBUS; all 19 cases pass after Steam releases its allocations.

Tmpfs now accepts a bounded size= mount option (decimal bytes or binary K/M/G
suffixes, rounded to pages), with the existing default of 16 MiB for ordinary
mounts and an existing maximum/global budget of 256 MiB. Invalid or unsupported
options fail before publication. The desktop canonical mount is provisioned
with a larger on-demand quota. No physical memory is reserved up front.

Host tmpfs tests cover parser boundaries, quota exhaustion, statfs reporting,
aggregate budgets and cleanup. A native 64 KiB fixture confirms ENOSPC, reported
capacity and rejection above the maximum. Its first cleanup used unsupported
umount2 flags=0 and failed; the original receipt is retained. Explicit lazy
detach cleanup succeeds, and the separately named corrected fixture passes.

The first 128 MiB Steam run still exhausts the mount and exits 135 after about
115 seconds. The 19 shared-futex cases and actual Intel virgl GLX context/red
pixel readback pass on that same exact kernel. A second attempt also exhausts
the quota and repeatedly restarts webhelpers. During exhaustion, directory
metadata shows three files of 26,214,416 bytes and about 84.6 MB total logical
file size. Tmpfs currently charges both backing storage and canonical cached
mapping pages, so logical file size understates charged memory. The second
attempt requires identity-checked termination. This is not a successful login.

A final temporary 256 MiB mount comparison retains at least 127,442,944 bytes
available in 95 one-second samples, with the global ceiling unchanged. The
quota failure disappears from those samples, but Steam still exits 135 after
about 85 seconds. Its kernel fault report identifies an unmapped first page in
a tracked three-page file mapping. This separates capacity pressure from a
remaining mapping fault; it does not prove every SIGBUS was caused by quota.
The CEF fixed-string snapshot no longer contains the previous shared-memory
allocation failure categories. There is still no login window. The temporary
mount is diagnostic; the shipped desktop quota remains 128 MiB, and this is
not a sufficient Steam capacity policy. The next investigation should identify
the faulting backing object and page-in failure reason, rather than repeatedly
raising quotas.

## Evidence and scope

Private receipts and payloads remain under ignored build/workloads and
build/reviews paths. Raw Steam logs, proprietary binaries, cookies and disk
images are not committed. Per-process FD ownership probes using procfs paths
could not attribute the shared-memory bytes; their zero counts are not evidence
that no shared-memory mappings exist. Directory metadata and statvfs provide
the capacity evidence instead.

The quota candidate frozen ELF SHA-256 is
64a093c727fb3630aefe3edcb2fa6fde982af357fe1ec1df6a1c2286165be974.
Module ABI remains 50. All 102 host tests, tmpfs ASan/UBSan and exact-ELF entry
and scheduler mutation audits pass. Native socket, unsupported-PI, procfs,
packet-pipe and robust-futex controls pass. Earlier failed application runs and
fixture receipts remain distinct from corrected regression passes.

The temporary 256 MiB mount is detached, restoring the original 128 MiB mount.
PulseAudio and Xwayland both exit zero. All original guest jobs have empty
groups and both streams at EOF; unsuccessful Steam exits remain unsuccessful.
Heap verification passes, checked shutdown completes, the original retained
launcher wait returns zero, and boot/data/Nix/temporary disks each pass offline
filesystem checks with exit zero. No VM remains running.
