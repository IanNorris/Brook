Latest client retry (2026-10-23): [canonical urandom device access](urandom-device-checkpoint.md)
gets Steam past its in-container urandom assertion. The next blocker is named
FIFO creation; see [the implementation gate](steam-fifo-next-step.md). No login
window or authentication has been reached in Brook.

Latest container checkpoint: [SteamRT executes its runtime child](steamrt-runtime-checkpoint.md).
The installed pressure-vessel launcher and bundled bubblewrap now complete two
separate default runtime-copy runs as UID 1000. The requested shell prints the
Steam Runtime 3c identity, and its supervisor exits zero with no remaining
children. This uses the existing locally staged installation with Nix ELF
interpreter patches described below; it does not claim pristine-archive,
Steam login, CEF or game acceptance. The older checkpoints below are historical.

# Native Steam client checkpoint

Update (2026-10-22): retained proc-root access, privilege ceilings, mount namespaces
and the bounded [user-namespace foundation](user-namespace-checkpoint.md) have since
passed acceptance. The historical Steam attempt below has not been rerun against
these foundations. The next gate is the remaining mount/root operations with an
unchanged bubblewrap trivial child, before pressure-vessel and Steam. No login
readiness is implied by the foundation tests. The subsequent
[bubblewrap checkpoint](bubblewrap-checkpoint.md) advances past missing overflow-ID
reporting; the accepted [proc namespace-handle checkpoint](proc-namespace-handles.md)
then advances the same executable to mount propagation. The accepted
[private propagation checkpoint](mount-propagation-checkpoint.md) advances it
to tmpfs. The [fresh-instance foundation](tmpfs-foundation-checkpoint.md) and
[bounded tmpfs backend](tmpfs-checkpoint.md) then advance it to the unsupported
recursive newroot bind. Its trivial child has not executed.

The 22 October login-screen attempt executes Valve's native x86-64 Linux
client on Brook, but does not reach a login window. SysV semaphores and signal-fd
watching pass native guest acceptance. Software Xwayland accepts the client's
connection, and explicit Unix credentials now pass D-Bus authentication. The
native64 client then enters its required SteamRT container path and fails opening
`/proc/self/root`. Named FIFO assertions, privilege and namespace requirements
also remain. No account credentials are entered. This is not Steam/Proton acceptance.

## Payload and environment

The official stable
[steamrt64 manifest](https://client-update.steamstatic.com/steam_client_steamrt64)
reports version `1792618285`. All 27 selected packages, totaling 674,556,018
compressed bytes, are size/SHA-256 checked against that manifest. The old Scout
runtime package is omitted. Original packages and manifests remain private.

The new client is ELF64 and can execute without the old 32-bit bootstrapper.
Its `steamrt64` CEF links still target assets under the legacy-named
`ubuntu12_64` directory; those assets are required despite that directory's
name. Private staging normalizes archive backslashes, preserves internal links,
sets executable modes and patches x86-64 ELF interpreters for Brook's existing
Nix glibc loader. Original package files are not modified.

The visible Intel VM uses the checked ABI 34 kernel/module capsule
`3d1c279d005174f4ffe0fe23837b75578570f1686ef15473f43821a73167f818`,
eight CPUs/eight GiB and unchanged MLFQ/audio settings. Steam runs as UID 1000
with a fresh private home and no host Steam data. Direct launch selects Wayland,
the existing Nix library map and bundled platform libraries; runtime-container
launch is not established. Bootstrap update/file verification is disabled only
for this locally patched, hash-verified diagnostic installation. CEF sandbox/GPU
flags are diagnostic settings, not accepted launch defaults.

The first archive transfer fills the private 128-MiB `/tmp` image and exits
with ENOSPC. That request is retained as failed with no live descendants. The
125,566,976-byte partial archive is recorded and removed. Separate tracked jobs
install the verified main archive and CEF supplement into persistent storage,
removing each archive afterward. Both install jobs exit zero.

## Initial observed blockers

Steam's requirement checker reports that user namespaces are required; bubblewrap
fails at `prctl(PR_SET_NO_NEW_PRIVS)`. The client continues past that failure,
then asserts during thread synchronization, semaphore creation and its global
instance named pipe. These are startup failures before a usable window.

A separate disposable guest probe confirms:

| Interface | Observed result |
| --- | --- |
| Private SysV `semget` | ENOSYS |
| `prctl(PR_SET_NO_NEW_PRIVS, 1)` | EINVAL |
| `unshare(CLONE_NEWUSER)` | ENOSYS |
| `mkfifo` in the private writable home | ENOSYS |

The main executable imports `semget`, `semop`, `semctl` and `mkfifo`, consistent
with the observed errors. Supporting these must provide actual object lifetime,
blocking/wakeup and access semantics. Returning success for unsupported namespace
or privilege operations would not establish a working runtime container.

The initial cleanup command is rejected by automatic approval review because it
targets a fixed PID without identity validation. The corrected cleanup recovers
the original tracked job, checks its guest PID/incarnation and exact `/proc/PID/exe`,
then rechecks the incarnation before SIGTERM. The original Steam job reaches
completed state with zero live tasks. No replacement client is launched.

## Next gate

Implement and test the minimum IPC interfaces, then rerun the existing client
before deciding how much runtime-container support is needed for login. Retain
the native 64-bit path and original client payload. User login/approval remains
a manual gate after a visible login screen exists. This checkpoint does not
claim container support, a working CEF window, authentication or game launching.

Evidence and the installed private VM image are under
`build/reviews/steam-login-off-gpu-1ms-fixed-mlfq-a-20261022`; host payloads and
staging helpers are under `build/workloads/steam-20261022`. Neither Steam
binaries nor account data belongs in git. The user shuts down through the
launcher; the original VM owner exits zero and all four offline filesystem
checks pass. The installed private image is retained as a clean reusable seed.


## SysV semaphore implementation and native acceptance

The kernel now provides native x86-64 `semget`, `semop`, `semtimedop` and the
basic `semctl` operations: IPC_STAT/SET/RMID, GETVAL/ALL/PID/NCNT/ZCNT and
SETVAL/ALL. Sets have permissions, creator/owner metadata, atomic ordered
operation arrays, blocking/interruptible waits and monotonic finite deadlines.
Identifiers are not reused within a boot. Unsupported IPC_INFO/SEM_INFO and
namespace support are not claimed.

SEM_UNDO uses PID/incarnation aliases to shared undo lists. CLONE_SYSVSEM shares
independently of CLONE_THREAD; ordinary fork starts empty, exec preserves the
association, and the final alias applies adjustments once. SETVAL/SETALL clear
applicable adjustments across all groups. Removal invalidates outstanding undo
and wakes already-blocked operations with EIDRM, including removal during the
unlocked retry gap. Final-exit cleanup runs outside scheduler locks before
termination drops its temporary process reference. It does not allocate or sleep.
User copying and allocation precede the service lock; process wakeups happen
after unlocking. The process/module ABI remains 34.

Astra's integration review caught and corrected an unpublished-thread rollback
that could decrement a live sibling's address-space membership, an EIDRM retry
race, and chown-style restrictions that differ from Linux IPC_SET. The rollback
review also exposed inherited legacy GPU context ownership at fork: fork births
now clear that ID, and unpublished cleanup does not retire parent graphics
registrations. The actual finalizer has a hosted rollback regression.

`test_sysv_sem_ops` compares ordered transactions with Linux's actual private
semaphore sets; `test_sysv_sem_service` executes the actual service with bounded
host scheduler/heap boundaries and competing threads. `test_process_destroy`
executes the real finalizer with counted resource backends. The final full host
inventory passes 89/89 serially, and the affected tests plus compiled
entry/start mutation audits pass. The strict
entry audit still lists its four previously unsupported return families; the
supported-wrapper audit is not a proof of every syscall return path. Bootstrap
acceptance's previously stale inventory is updated to 89 tests/88 executables.

The first guest probe passes creation/control/ownership, then exposes that this
glibc routes semop through syscall 220 (`semtimedop`, even with a null timeout).
That missing entry is implemented with real finite timeout semantics, retaining
one original deadline through retries. Production wake-tick conversion has
fractional-millisecond, expired-deadline and saturation coverage.

The expanded native probe in `tools/sysv-sem-probe-pkg` passes as UID 1000 in
Brook: atomic rollback, ordered zero waits, permissions metadata and creator
ownership transfer, direct semop, timed waits under unrelated wakes, waiter
counts, stale IDs/removal wake, interrupted waits, CLONE_SYSVSEM before first
undo use, last-alias exit, exec preservation, fatal blocked-waiter cleanup and
undo visibility before WNOWAIT observes death. Heap integrity passes. The same
probe passes on Linux; ownership transfer is explicitly skipped there because
the tool's host user namespace maps only one user and group. Brook executes
that case rather than skipping it.

Evidence is `steam-semtimed-off-gpu-1ms-fixed-mlfq-a-20261022/native-sysv-probe`,
using kernel `883aa37d0c7e89dd22fe74a752211e5a2f0e5d5be1ffb040d8bc3b5d2855e24e`.
The earlier `steam-sysv` probe VM stops cleanly, original launcher exits zero
and all four filesystem checks pass.

The unchanged verified Steam client then loses the semaphore assertions and
reaches updater verification and subprocess launch. It exits 125, with zero
live tracked tasks, after reporting the remaining named-pipe assertion,
XOpenDisplay failure and launcher-service signal-watching ENOSYS. The installed
Xwayland package is being assessed as a real X11 compatibility path, not a fake
display or a replacement login web page. Valve's [native64 requirements](https://github.com/ValveSoftware/steam-for-linux/blob/master/README.beta.md)
include X11/Xwayland, user/mount namespaces and a session bus; no runtime-container
support is established by the semaphore work.


The existing guest `nix-install xwayland` installs version 24.1.10 and its
closure successfully. Software rootful Xwayland starts a real Wayland surface
and accepts the unprivileged Steam X11 connection. The normal unprivileged
X-server launch fails at its hard-link lockfile (Brook link/linkat remain
unsupported). The bounded diagnostic runs only that X server as guest root
with `-nolock`, `-nolisten tcp`, `-shm` and no host display access. This is not an
accepted desktop default or accelerated/rootless Xwayland acceptance.

With DISPLAY=:0, the same Steam launch reaches `Create window` without the
previous XOpenDisplay failure, then still exits 125 on launcher-service signal
watching. Its original tracked group reaches zero live tasks. Named FIFO
assertions remain, as do the separate runtime-container requirements. Evidence
is the `xwayland-install`, `xwayland-first`, `xwayland-nolock`,
`xwayland-root-nolock` and `steam-client-x11` subdirectories of that review.

## Signal-fd implementation and native acceptance

Native `signalfd`/`signalfd4` now create a refcounted shared-mask description with
real reads, nonblocking/CLOEXEC flags, mask updates, poll/epoll readiness and
fork/dup/SCM_RIGHTS ownership. Reads consume the calling task's signals rather
than the creator's; SIGKILL/SIGSTOP are excluded. The 128-byte record carries
available sender, timer and child status information. Signal delivery already
wakes masked targets; existing bounded wait rechecks cover shared mask updates.
This is standard-signal acceptance, not realtime queue or edge-triggered epoll
acceptance. Generic kill-to-thread-group routing retains its earlier model.

Astra identified two necessary companion fixes. Kernel-generated SIGCHLD now
has process-shared pending state, separate from thread-private signals. A worker
can consume its group's child exit without stealing a leader's private tgkill.
Child status/sender information is retained through reap with incarnation checks.
Blocked signals are retained even when their default or explicit disposition is
ignore, making them available to synchronous readers. Ordinary kill also now
supplies SI_USER sender metadata.

The exec handoff is one scheduler commit: ready lock, registry lock, old runtime
charge/address-space membership release, then signal-locked identity and shared
child-record transfer, followed by runtime rebind. This prevents notification or
signal readers seeing a half-written group identity and keeps the old leader
protected until the handoff. A hosted interleaving pauses exactly at that point
and verifies both readers and parent notification remain excluded. This does not
claim broader Linux nonleader-exec identity equivalence.

All 90 host tests pass. The compiled supported-entry and startup mutation audits
pass. The native `tools/signalfd-probe-pkg` passes on Linux and as UID 1000 in
Brook, including worker-thread SIGCHLD consumption, private tgkill isolation,
blocked ignored signals, sender/status data, fork reader identity, poll/epoll,
blocking reads, mask sharing, CLOEXEC and final alias cleanup. The expanded
semaphore probe also passes against this kernel, and heap integrity passes.
Kernel/module ABI remains 34; the FD type is appended without shifting existing
values or changing the process layout.

Evidence is `steam-signalfd-off-gpu-1ms-fixed-mlfq-a-20261022`, kernel
`85f65c8a42e58ce723e36fd52a6b847c45db628257eed040f925ad115276762a`.
The same UID-1000 Steam/Xwayland launch no longer fails signal watching; its
launcher reaches D-Bus authentication and reports unsupported explicit Unix
credentials during sendmsg. The tracked Steam group exits 125 with zero live
tasks. Named FIFO assertions and the separate runtime-container requirements
remain unresolved. No login window or account authentication is claimed.

The signalfd review stops cleanly after graceful Xwayland group exit; the
original launcher exits zero, final heap integrity passes, and all four
offline filesystem checks pass. The checked image is retained as the next seed.

## Explicit Unix credentials and the runtime-container boundary

Unix-stream sendmsg now accepts an explicitly supplied SCM_CREDENTIALS record
when its PID is the sender's TGID, its UID is one of the sender's real/effective/
saved UIDs, and its GID is one of the equivalent GIDs. Invalid IDs and malformed
records are rejected. This is the unprivileged subset of
[Linux credential validation](https://github.com/torvalds/linux/blob/v6.12/net/core/scm.c);
CAP_SYS_ADMIN/SETUID/SETGID overrides are not implemented or claimed.

The message header, ancillary headers, credential record and descriptor numbers
are copied before use. Validated credentials are attached to the actual accepted
byte span; SO_PASSCRED still controls delivery. Rights are privately retained
until bytes commit and released on control parsing, credential validation or
write failure. Connection-time SO_PEERCRED identity is unchanged. The older
iovec/payload copying path is not redesigned by this change.

The expanded `tools/at-syscall-probe/passcredcheck.c`, built separately with
`tools/unix-credentials-probe-pkg`, passes on Linux and as UID 1000 in Brook.
It tests automatic and explicit credentials, child sender identity, combined
credentials/rights, malformed records, rejected identity claims, and exact
pipe-writer-reference rollback after invalid credentials, full nonblocking
socket EAGAIN and inaccessible control data EFAULT. The last case verifies
failure without leaked rights; Brook's whole-control preflight can reject it
before acquiring the first right. Linux reports 1,030 checks and Brook 974;
the full-socket test counts each fill/drain operation, so counts vary with socket
capacity. Both report zero failures. All 90 host tests and supported compiled
entry/start mutation audits pass. The native signalfd regression also passes
against this kernel; heap integrity passes. ABI remains 34.

Evidence is `steam-credentials-off-gpu-1ms-fixed-mlfq-a-20261022`, kernel
`cf48cffbd8e2fb5c88df01b75df2a9be5736886d5d9be3a33472b4d4d8797cb5`.
The original Steam job 122 and subsequent corrected-environment job 152 each
exit 2 with zero live tasks. The D-Bus credential error is gone. The first run
then fails because STEAM_RUNTIME=0 incorrectly identifies an existing runtime.
The corrected run removes that variable, matching the retained native64
steam.sh branch and Valve's [launcher guidance](https://github.com/ValveSoftware/steam-for-linux/issues/13597).
It reaches pressure-vessel-wrap and reports `opendir(/proc/self/root): No such
file or directory`.

Astra's bounded script review finds no supported native64 non-container path:
pv-run.sh calls srt-client-container, and the runtime entry point ultimately
executes pressure-vessel-wrap. SDK/custom-runtime options still use containers.
The legacy STEAM_RUNTIME=0 branch is explicitly unsupported and does not provide
a native64 escape hatch. Implementing `/proc/self/root` alone would not establish
the separately missing no_new_privs, user/mount namespace and mount semantics.
Full runtime-container support needs a separate scope decision; no checks or
security operations are replaced with successful stubs. No login page exists yet.

Xwayland exits through semantic close with its exact registered group gone.
The final heap check passes and shutdown acknowledges the checked filesystem
and device flush. The original launcher exits zero and all four offline
filesystem checks pass; their receipts remain with the review image.

The separately approved bounded [SteamRT feasibility investigation](steam-runtime-feasibility.md)
captures the unchanged runtime in a private Linux guest through container setup,
then observes the real login UI after detaching tracing before CEF. It records
the actual user/mount namespace and filesystem operations, source-confirmed
i386/helper and hardlink fallbacks, and the reviewed object/lifetime design.
Both comparison guests stop cleanly. This does not change Brook's container
support or establish a Brook login window; implementation needs a separate scope.

Retained proc root acceptance (2026-10-22): `/proc/self/root` now follows the
exact retained FsContext root, with the same incarnation and thread-group
checks as cwd. Native proc-root checks pass (34 Brook, 33 Linux), together
with all seven filesystem/credentials/IPC regressions, 96 host tests and the
compiled entry/scheduler audits. Unsupported clone namespace flags now fail
explicitly instead of creating an ordinary process.

Kernel `64ae922e1b0f6cbdfa1d5a9f8c2a234a3038fa843aa8a2f45a5f4e0012e70874`
(ABI 38), review `steam-root-off-gpu-1ms-fixed-mlfq-a-20261022`, reaches
`bwrap: prctl(PR_SET_NO_NEW_PRIVS) failed: Invalid argument`. The original
Steam job exits 1 with no live tasks; no Brook login UI is established.
The proc-FD directory warning remains separate. Heap integrity, checked
shutdown, original wrapper exit and all four offline filesystem checks pass.

The bounded no_new_privs slice is also accepted (2026-10-22). PR_SET_NO_NEW_PRIVS
validates its arguments and irreversibly sets a per-task bit; GET reports it.
Fork/clone inherit the caller's bit, existing sibling tasks remain independent,
and both successful and failed exec preserve it. Brook currently grants no
set-ID or file-capability privileges at exec; future exec privilege grants must
honor this bit. This is not namespace, seccomp, or capability implementation.
See the [Linux contract](https://docs.kernel.org/userspace-api/no_new_privs.html).

ABI 39 kernel `3a42b8030011780dcabb26002de05c4ce5857aecb1333cef03827e52746e33f7`
passes the fresh native NNP probe (34 checks plus exec child) and all eight
retained filesystem/credentials/IPC regressions. The host process already has
NNP set: its 25-check subset explicitly skips initial transition and sibling
isolation. The exec probe uses the UID-1000 fixture. These tests do not claim
to test set-ID/file-capability suppression, since Brook does not implement
those exec privilege gains. Both compiled entry/start audits pass in their
supported scope. Hosted tests pass 95/96 in parallel; the timed-out
audio-delivery test passes separately in 8.30 seconds.

The unchanged Steam retry in `steam-nnp-off-gpu-1ms-fixed-mlfq-a-20261022`
passes the prior prctl failure, then reports a missing
`/proc/sys/kernel/overflowuid` while checking user namespace requirements.
The proc-FD directory warning remains. The separately captured Linux setup
requires real CLONE_NEWUSER/CLONE_NEWNS, ID mappings, mount isolation and root
transitions. No Brook QR login is reached. Continuing past these small
prerequisites needs the container implementation scope described in the
[feasibility result](steam-runtime-feasibility.md), rather than successful
namespace stubs.

The original Steam and Xwayland jobs both finish with zero live tasks. Heap
integrity passes; checked flush/stop completes, the retained original wrapper
exits zero and all four offline filesystem checks pass. Private launch and
acceptance receipts are in `build/workloads/steam-nnp-20261022` and the review
directory; no Steam payload, authentication or account data is committed.
