# Native Steam client checkpoint

The 22 October login-screen attempt executes Valve's native x86-64 Linux
client on Brook, but does not reach a login window. No account credentials are
entered. This is a compatibility failure, not Steam/Proton acceptance.

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

## Observed blockers

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
