# Dell Steam software GLX checkpoint

On the XPS 13 9343, Valve's updater completed outside the inner SteamRT
container. Its package moves cross separate mounts inside that container and
fail with `EXDEV`; bypassing the VFS mount boundary is not a suitable fix.
The updated client opted into SteamRT 64-bit after restoring the appropriate
client flag and patching the updated executable interpreters.

The normal software Xwayland session did not expose GLX. An isolated Xephyr
server, running inside that session with `+extension GLX`, `+iglx`, Mesa's
software driver path and the Mesa vendor library available, reported GLX 1.4
and returned a visual from `glXChooseVisual`. Its private mount namespace
provided `/bin/sh` and a disk-backed temporary directory for its keyboard map
and lock; these experiments did not change the system-wide library cache.
Steam's private host library directory also needed the Nix GnuTLS-compatible
Nettle and Hogweed versions to avoid mixing them with older runtime libraries.

One client launched against the nested display got past the earlier GLX and
TLS errors and started its web helper. Shortly afterward the Dell stopped
responding to input and its debug connection was lost. The saved log does not
establish whether the cause was process exhaustion, graphics, or another
failure. A usable login window was not confirmed. Do not repeat this launch
without a bounded way to capture resource use and stop it before another
unrecoverable hang. The subsequent forced power-off required an offline ext2
repair; the SSD and filesystem identities were verified before preen, and a
separate full read-only check passed afterward.

This nested setup is a diagnostic path, not a finished desktop integration.
It uses software rendering, a separate X desktop and incomplete audio/session
integration. The ordinary Xwayland/GLX path and Steam's long-term launcher
still need proper fixes.

## Bounded retry

The Dell's original SSD boot pair was preserved while A/B slots were
initialized. A QEMU-tested kernel with opt-in debug-job task budgets then
booted from the new slot. On the Dell, the four-task acceptance probe passed
fork, unreaped-child, `exec`, `setsid`, thread and reclamation checks.

A single new Steam attempt used a 256-task budget. The last prompt live
sample, 14.6 seconds after launch, showed 35 reservations, a peak of 35,
zero budget refusals, 59 system processes and about 7.55 GB free. Subsequent
statistics requests timed out and the Dell stopped answering ARP. These are
the last observed values, not proof of the state at the eventual hang: a
resource spike or another failure could have occurred between samples. No
further Steam retries should be made until there is a way to capture or
recover from the failure. A task budget does not constrain memory or protect
against a kernel or device deadlock.

## Software-rendered QEMU comparison

A private clone of an older, previously clean Steam review image was booted
with the same kernel as the Dell task-budget candidate, four virtual CPUs and
software rendering. The Steam client/profile and device environment were not
identical to the Dell. The first attempt stopped before launching because the
clone had less than the launcher's required 1 GiB of unreserved space; after a
checked shutdown, the private data filesystem was enlarged offline.

A second attempt reached the client and web helper through a nested GLX-capable
Xephyr display. It was limited to 256 job tasks and paused deliberately at a
30-second host deadline for a QMP register capture. At the last successful
sample, 28.6 seconds in, it had 51 reserved tasks, a peak of 117, and no budget
refusals; the guest was still responding. The captured CPU instruction
pointers all resolved to the scheduler idle loop, a point-in-time observation
rather than evidence of a stall. The VM was resumed and later shut down with
its checked filesystem and device flush.

The client logs include repeated ANGLE/Vulkan initialization failures and,
after further runtime, repeated `Too many open files` errors from attempts to
set up PulseAudio. Brook currently reports an RLIMIT_NOFILE ceiling of 1024;
these messages do not establish whether the process hit that ceiling, leaked
descriptors, or encountered another FD allocation limit. Capture per-process
FD usage and allocation failures before choosing a fix. This run does not
establish the cause of the Dell's lost network or prove that the login works.

### PulseAudio control

A subsequent private-VM run inspected `/proc/<pid>/fd` after Steam's audio
errors. Its `steam` process had 1,024 open descriptors, with descriptors
sampled at 100–109, 400–409 and 1000–1023 all reported as `memfd`. This is
consistent with the earlier PulseAudio investigation in
`steam-pulse-shm-checkpoint.md`; it does not establish a general memfd leak.

In a fresh boot of the same private VM, an authenticated PulseAudio 17 null
sink was checked with `pactl info` before launching the same Steam client.
The Steam process had 91, 94, 93 and 93 open descriptors across four samples
about five seconds apart. This supports using the existing explicit audio
server for further Steam diagnostics rather than merely raising the descriptor
limit. A bounded 60-second run remained responsive, but the nested X display
was black and no login was shown. Its log included shared-memory corruption,
ANGLE/Vulkan initialization errors, and unavailable desktop services. These
are leads rather than a diagnosis, and the older QEMU Steam image is not an
exact reproduction of the Dell. The VM subsequently completed a checked flush
and shutdown.
