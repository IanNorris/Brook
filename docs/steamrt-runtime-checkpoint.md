# SteamRT executes its runtime child

The installed Steam Runtime 3c, build 3c.0.20260824.257593, now
completes pressure-vessel setup and executes its own `/bin/sh` child on Brook.
Two distinct default copy-runtime runs completed with status 0 in approximately
29.85 seconds each. Both printed the child marker and runtime `/etc/os-release`;
pv-adverb reported command status 0 and no remaining children. No Steam
login, authenticated client, game, or arbitrary container compatibility is claimed.

The final gaps were O_PATH fstatfs, mount-label capacity, and `/dev/null` after
binding the canonical device directory into the container. The first two are
described in the adjacent path-statfs and mount-paths checkpoints.

The launcher and bubblewrap are unchanged in this phase. This is the existing
locally staged installation described in `steam-client.md`: x86-64 ELF
interpreters were previously patched to the Nix glibc loader. It is not a
claim that pristine archive ELF interpreters execute without that preparation.

## Narrow null-device bridge

Brook provisions a retained reference to the exact initial `/dev` directory
before userspace starts. A data open of a missing `null` leaf may use the existing
null-device implementation only when its resolved parent matches that pinned
backing, object operations, and object identity. The parent identity and NODEV
flag are checked in one VFS transaction. A real leaf or unrelated overmount is
handled by ordinary VFS lookup. Final symlink handling respects O_NOFOLLOW and
exclusive creation. FD publication preserves access mode and CLOEXEC.

This is a compatibility bridge for the existing synthetic null device, not a
general devfs. O_PATH metadata for the synthetic leaf is still unsupported.
General synthetic-service eligibility is unchanged: binding `/dev` does not
expose the global shared-memory broker, random, GPU, or audio devices through
this exception. Existing open null descriptors remain usable after NODEV remount,
matching Linux. The boot anchor is intentionally held for the kernel lifetime.

## Verification

Evidence: `build/workloads/null-device-20261023/` and its original private review
`build/reviews/null-device-off-gpu-1ms-fixed-mlfq-a-20261023/`.
Kernel SHA256: `7fce28a2fa45ca61dae16c0ec3f52cd10ca41cac5b86f69a4a42fdf725ce7834`.
The ABI-46 kernel and all thirteen modules were staged together.

All 101 host tests pass, including retained identity, ordinary leaf shadows,
namespace-copy behavior, concurrent overmounts, atomic NODEV enforcement, and
symlink policy. The retained-path suite also passed thirty repetitions and
ASAN/UBSAN. Its mock backend reference counter is now atomic; the mountinfo
stress helper retries the API's bounded EAGAIN result during concurrent growth.
Final ELF entry/scheduler audits and mutation checks pass.

The long-path probe passes 113 fixture checks plus five parent checks on Linux
and Brook. The null-device probe passes 77+15 on Linux and 79+15 on Brook,
including runtime-root pivot, FD flags, NODEV, old-descriptor survival and
unrelated `/dev` shadows. Linux requires recursive binds of its inherited,
locked `/dev` submounts in an unprivileged user namespace; Brook uses nonrecursive
binds and additionally checks that global shared memory remains hidden. Those
extra isolation checks are Brook policy rather than a Linux equivalence claim.
Earlier Linux trials (nonrecursive bind rejected EINVAL, unavailable passwordless
sudo) are retained separately and are not counted as passing controls.

The O_PATH, socket/file-bind, tmpfs, mount restrictions, namespace, unchanged
bubblewrap, native application and Mousepad desktop regressions pass on the same
kernel. Full runtime stderr stays in guest files; only bounded tails cross the
bridge. Stopped-disk extraction verifies full logs against guest hashes.

The final review VM shut down cleanly, the original owner returned zero and all
four offline filesystem checks passed. Completion audit:
`build/workloads/null-device-20261023/completion-audit.json`.

A third diagnostic run also exits zero. Its getconf command returns zero without
version text and its in-container proc/self/maps read emits no rows. These remain
diagnostic gaps; no libc-version result is claimed. Offline comparison verifies
that the copied dash and getconf are nonempty, genuine x86-64 ELF binaries,
byte-identical to their installed sources, with the expected previously patched
Nix loader. This excludes empty runtime copies as the explanation. The 32-bit
libdl discovery helpers still fail, consistent with the current native64 scope.
