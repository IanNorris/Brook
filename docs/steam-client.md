# Native Steam client checkpoint

The 22 October login-screen attempt executes Valve's native x86-64 Linux
client on Brook, but does not reach a login window. SysV semaphore startup now
passes real guest lifecycle acceptance, and Steam reaches its updater/launcher.
Named FIFO, signal-fd watching and an X display remain observed gaps. No account
credentials are entered. This is not Steam/Proton acceptance.

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
