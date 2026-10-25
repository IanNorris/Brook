# Desktop launcher acceptance corrections

The first live review exposed differences from structured bridge launches.
Passing a workload through the bridge alone does not establish that its app
launcher entry works, nor does non-silent captured PCM establish smooth audible
playback.

## GUI standard input

The kernel-console stdin inherited by desktop launches makes ioquake3 enter its
tty-console path. Wine's attempt to transfer this descriptor to wineserver fails
with `sendmsg: Bad file descriptor`; Alpaca Stacka's launcher then exits 1.
Structured jobs already receive a null stdin and do not reproduce these errors.

Shell-created processes tagged to a desktop launch with no kernel VFB now get
`DevNull` on fd 0. stdout/stderr remain available for diagnostics. Terminal and
ordinary kernel-shell launches retain their existing input contract. This does
not change Linux fork/exec descriptor inheritance.

The compiled candidate is kernel SHA-256
`27b0bfa8f3b620541ce9fa579c1392ac13bd5ae1de9dc5b0ce6968bd7f6301be`,
with all 13 driver modules checked at ABI 52. An actual app-launcher probe runs
as UID 1000 and observes `isatty(0) == false` and immediate EOF. The per-game
stdin redirects and Q3 tty-console override were removed for candidate testing.
The earlier per-app Alpaca stdin redirect independently restores its title
window on the old kernel; candidate game acceptance is recorded separately.
On the candidate, the same Alpaca shortcut creates a native Wine window without
either per-app workaround; semantic close removes its window and confirms the
client group has exited. This establishes launch/close, not smooth gameplay.
An additional candidate retry reaches the rendered title screen in about 31
guest seconds and exits 0 through its own Quit button.

## Steam cold-start dependencies

The first Steam shortcut stops before launching the client because only about
0.7 GiB remains on the private data disk. The existing 1 GiB free-space guard is
correct; it must not be bypassed. A fresh private copy was expanded from 16 to
32 GiB with `resize2fs`, then passed all five `e2fsck -fn` phases. Steam's next
attempt has over 15 GiB free.

A second failure occurs because the new live-review launcher omitted
`BROOK_PROVIDER_PAYLOAD`. A warm diagnostic session had inherited this variable
from its controller. Without the validated provider loader/cache payload in the
initrd, SteamRT's capsule library capture fails opening its loader cache.
The corrected review launcher exports the payload explicitly before entering
the development shell; the existing `make_initrd.sh` validation and packaging
remain in use. A prepared review therefore needs both its installed closure
and this validated boot payload. Do not describe it as a portable fresh Steam
installation or fabricate an empty loader cache.

With that payload restored, a launch reaches CEF but does not produce the main
window. A subsequent launch with the previous private PulseAudio setup restored
renders the authenticated Steam store. `pactl info`, using the same endpoint and
cookie as Steam, exits 0 and reports `pulseaudio` with default sink
`brook_workload_test`. This is stronger than accepting a Unix socket connection.
The diagnostic server uses `module-null-sink`; audible Steam/game audio remains
unverified. Its process, fresh cookie and socket belong to the launcher and are
cleaned up when the client wrapper exits. Account data and raw screenshots stay
in private review artifacts.
Exiting the rendered client through its menu removes the main window but leaves
CEF helpers and the runtime supervisor alive. Automatic descendant cleanup is
still open; window disappearance alone is not a successful full shutdown.

SteamRT also logs repeated hardlink-to-copy fallbacks. Both `link` and `linkat`
currently return `EPERM` in Brook. Runtime copying is therefore a concrete
startup-time and disk-space cost to investigate separately; changing file
ownership cannot provide missing hardlink semantics.

### Bounded client descendant cleanup

The client launcher now wraps the real Steam executable with the bundled
`pressure-vessel/bin/steam-runtime-supervisor`, using
`--terminate-with-main --terminate-idle-timeout=2 --terminate-timeout=5`.
The outer container and companion game-launch service keep their existing
ownership. No descriptor-closing option or parent-death signal is added.
Timeout flags on the outer `pv-adverb` alone do not enable main-exit cleanup
and failed the initial control; they are not retained.

A native owned-child control has its main command exit 7 while a descendant
ignores SIGTERM. The supervisor reaps that descendant after about 7.42 seconds
and returns 7. The real UI-launched Steam then renders the authenticated store
and exits through its own menu: client and desktop wrapper return 0, and no
launch-tagged client, CEF, container, companion or private Pulse tasks remain.
No manual signals are needed for that accepted retry. Its transient sign-in
window resolves automatically without account input. This validates bounded
exit cleanup, not cooperative CEF shutdown or a game running across Steam exit.

## Q3 performance and remaining input work

The user's audible live run reports about 15 FPS and heavy stuttering. A later
isolated audible Q3 run shows 56 FPS in a different scene. Its 200-ms capture
contains 160 CPU samples and 840 context switches, with no reported drops;
substantial sampled CPU time is idle. It does not identify the cause of the
earlier slowdown or prove consistent framerate. Audio pacing remains open.

The kernel and native SDL/Wayland capture controls now pass; SDL/Xwayland also
passes with the Brook SDL metadata backport. See
[pointer capture](pointer-capture.md) for exact evidence and remaining limits.
The full path requires raw relative
motion before screen clamping, focused window/incarnation ownership, button and
wheel routing during capture, synchronous revocation on focus loss/destruction,
and an emergency release. Wayland needs relative-pointer and pointer-constraints
protocols with correct lifetime and commit semantics. Relative coalescing must
sum motion without crossing button/focus boundaries; a global accumulator
drained later can reorder input. The native QEMU frontend also needs a true
relative pointing path: the current absolute tablet cannot provide unlimited
motion at screen edges. Test physical mouse turns and release behavior, including
SDL/Xwayland, before accepting game capture. Cursor hiding or warping alone does
not satisfy this requirement.
