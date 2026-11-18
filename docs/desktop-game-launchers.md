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

## Rootless Steam on the Dell

The Dell's Steam shortcut runs `/data/home/.local/share/brook/steam/brook-steam`
(source `scripts/workloads/brook_steam_launcher.py`, with its helper
`steam-client-check.py` from `scripts/workloads/steam_client_check.py`). Until
November 2026 it started a nested `Xephyr :14` with `LIBGL_ALWAYS_SOFTWARE=1`.
With Stardew Valley on gen 419 that path kept the GPU 73% busy with about 286
tiny jobs/s: the desktop Xwayland's glamor uploading Xephyr's llvmpipe frames
as images. The compositor was starved to about 18 composites/s while the CPU
was 59% idle. The software path was chosen when the Dell's desktop Xwayland had
no GLX ([checkpoint](steam-dell-software-glx-checkpoint.md)).

### What the desktop Xwayland offers

`waylandd --xwayland … --xdisplay :0` starts Xwayland with `-rootless -wm FD
-displayfd FD -nolisten tcp -noreset -ac -glamor gl`
(`tools/waylandd-pkg/xwayland-xwm.h`); `BROOK_XWAYLAND_SOFTWARE_GLX=1` is the
only software variant. It inherits the kernel's Dell Mesa environment (stock
Mesa 26.0.6, `9kypdl7x…`, `iris`). Xwayland 24.1 has two GLX providers.
`xwl_glamor_init()` pushes glamor's EGL-backed provider
(`GlxPushProvider(&glamor_provider)`). Underneath it sits DRISWRast, which only
works if a swrast driver can be found through `LIBGL_DRIVERS_PATH`. The earlier
software-rendered Dell session had neither: no glamor, and the kernel then gave
non-virtio machines the QEMU Mesa path, which the Dell lacks. That is why it
reported no GLX. The kernel now selects the Dell Mesa paths, and glamor runs on
the Dell (its GPU jobs are visible), so the server should advertise GLX 1.4
backed by glamor's EGL context. GLX alone does not prove hardware rendering,
because QEMU's software Xwayland also reports it (below). DRI3 and
Present follow from the GBM backend, which uses waylandd's linux-dmabuf v4
feedback (one render node, LINEAR XR24/AR24). Clients therefore render directly
with Iris in their own process and present through Present. This is derived
from source and the Dell's glamor activity; `brook-steam --probe` is the check
on the Dell. It reports the server's extensions, then creates a GLX context and
reports `glXIsDirect`, `GL_RENDERER`, a red-pixel readback and the `libgallium`
that was loaded. Expected: `GLX`, `DRI3` and `Present` true, renderer
`Mesa Intel(R) HD Graphics 5500 (BDW GT2)`, `direct: true`. A software renderer
here means DRI3 failed and Mesa fell back to DRISW.

### Launcher design

- **Display.** The default mode targets `$DISPLAY`, or `:0`. Before starting
  anything it completes an X11 handshake with a small pure-Python client and
  queries GLX, DRI3, Present and MIT-SHM. It does not use libX11, which exits the
  process when the connection drops, and the compositor can restart its
  Xwayland generation. It refuses to start without GLX unless given
  `--allow-no-glx`; `--xephyr` is then the fallback.
- **Graphics.** It removes `LIBGL_ALWAYS_SOFTWARE` and keeps the desktop's Mesa
  environment, which pressure-vessel imports through the provider
  `ld.so.cache` and the helper's client library path.
  - `--gl software` restores llvmpipe without nesting.
  - `--iris` (experimental) reads the patched build's library directory from
    `brook-iris-run` and passes it as the helper's new `--library-dir`, which
    puts it ahead of the stock `libgallium` for the client. The game containers
    (Soldier) import graphics through capsule capture from the provider cache,
    so they keep the stock Iris until a provider cache variant exists.
  - `--gl-debug` sets `LIBGL_DEBUG=verbose`.
  - CEF stays on `-cef-disable-gpu`; its software frames reach Xwayland over
    MIT-SHM.
- **Audio.** It starts a private `pulseaudio -n --daemonize=no
  --exit-idle-time=-1 --use-pid-file=no` with a native socket in
  `$XDG_RUNTIME_DIR` (or `/tmp`) at `brook-steam-pulse-<pid>/native`
  (`auth-anonymous=1`), `module-null-sink` and `module-always-sink`. It passes
  this to the helper as `--pulse-server`, which sets `PULSE_SERVER` for the
  container and the companion game launcher.
  - Readiness is a socket connect plus `pactl info`.
  - The server runs under a small keeper process. Brook accepts but does not
    implement `PR_SET_PDEATHSIG`, so the keeper polls its parent and stops
    pulseaudio if the launcher disappears, even after SIGKILL.
  - `--pulse-server` reuses an existing server and `--no-pulse` starts none.
  - The output is still a null sink: Steam and game audio are silent.
- **Interpreter.** The Dell's `/nix/profile/bin/python3` is python3-minimal,
  which has no `_ctypes`. The launcher imports ctypes optionally. Without it,
  it re-executes itself under a full Python found at runtime:
  `BROOK_STEAM_FULL_PYTHON`, or an installed `/nix/store/*-python3-3.*`
  (not `-minimal`) whose `lib-dynload` has `_ctypes` and which imports it.
  If no such Python exists it still runs. It then cannot become a subreaper,
  so it remembers each descendant's PID and command line and signals the
  orphans that still match. `--probe` reports that it needs ctypes.
- **Container `/etc`.** The outer srt-bwrap container starts from
  `--tmpfs /`, so only files the launcher binds exist there. pressure-vessel
  treats that container as the host, and imports `machine-id`, `hosts` and
  `resolv.conf` from it into the client and game containers.
  - Torchlight II (app 200710) exited about 2 s after start because libdbus
    aborts when neither `/etc/machine-id` nor `/var/lib/dbus/machine-id`
    exists.
  - The launcher now binds one ID file to both paths. It uses the host's
    `/etc/machine-id` (Brook's FAT root spells it `/ETC/MACHINE-ID`) only if
    the file is 32 lowercase hex and is not the constant
    `0123456789abcdef…` that `make_initrd.sh` writes into every image. That
    constant is valid but not an identity, and the Dell's separately
    assembled initrd may not carry it.
  - Otherwise it creates `steam-private-etc/machine-id` once (16 random bytes
    as hex plus a newline) and never regenerates it; an invalid file there
    stops the launch.
  - It also binds private `hosts` (localhost), `nsswitch.conf` (files, then
    dns), `os-release` (ID=brook) and `host.conf`, alongside the existing
    passwd, group, resolv.conf, ld.so.cache and loader.
  - No `/etc/localtime` is bound, so containers use UTC, as Brook's clock
    does.
  - pressure-vessel's "Cannot find locale/localedef" warnings mean it cannot
    generate extra locales. With `LANG=C.UTF-8` the runtime's built-in
    C.UTF-8 suffices. Torchlight II's "Failed to set locale to UTF-8 locale …
    trying to force it" is non-fatal.
- **Ownership.**
  - The launcher is a child subreaper (`PR_SET_CHILD_SUBREAPER` is
    implemented), so descendants that daemonize stay in its tree.
  - Single instance: a second launch while one is running logs "already
    running" and exits 0. It does not yet bring a hidden Steam window back.
  - `steam-launcher.log` records each step as JSON lines.
    `steam-launcher-state.json` records the mode, display report, Pulse
    endpoint, PIDs, stop reason and leftovers.
- **Ending Steam.** Brook has no tray, so a Steam without a window is
  unreachable, and the graceful restart's Closing phase waits for every
  non-service process. The launcher therefore lists viewable top-level windows
  every 2 s.
  - Once a window with WM_CLASS containing `steam` has appeared, it ends Steam
    20 s (`--windowless-grace`) after the last such window closed, unless a
    game is running (a `reaper … SteamLaunch` descendant).
  - Without any window in 900 s (`--startup-timeout`) it also gives up.
  - SIGTERM, SIGINT and SIGHUP stop it immediately.
  - Stopping sends SIGTERM to the `steamrt64/steam` main process; Steam treats
    this as a normal shutdown, and its runtime supervisor ends the client
    subtree. It then waits up to 15 s, sends SIGTERM to the remaining
    descendants for 5 s, then SIGKILL. Finally it stops pulseaudio and removes
    its directory. The worst case is about 25 s after the trigger.
  - During a restart, Closing closes Steam's windows, the launcher ends Steam
    20 s later, and Closing can proceed. When the planned kernel change sends
    SIGTERM to windowless processes, that becomes the fast path.
- **Fallback.** `--xephyr` keeps the previous nested software path, now with the
  same Pulse ownership and cleanup. If Xephyr's window is closed the server
  exits, and the launcher then stops Steam.

### Host and QEMU checks

`scripts/test_brook_steam_launcher.py` uses a private Xvfb display, private
PulseAudio and a fake SteamRT tree. Its client maps a `steam` window, checks
the server with `pactl info`, and starts a descendant in its own session that
ignores SIGTERM. The test covers `--check`; the windowless shutdown (with the
SIGKILL of that descendant); SIGTERM; single-instance behavior; and the absence
of leftovers, Pulse directory or PID file afterwards. It also runs the launcher
with ctypes blocked: the `--probe` error, re-execution under a full Python, and
cleanup without a subreaper. Finally it installs and rolls back a fresh bundle.
Run it with
`nix-shell -p xvfb pulseaudio --run 'python3 scripts/test_brook_steam_launcher.py'`.
The GLX probe passed against Xvfb (llvmpipe, direct, red pixel).

QEMU (16 November) used a private reflink clone of
`build/reviews/steam-rootless-stardew-offline-20261104`, with its 4 November
kernel, Nix disk and installed Steam. It ran with 4 vCPUs, 6 GiB,
`BROOK_NET_RESTRICT=1` (Steam offline), headless and without audio playback.
Its desktop Xwayland 24.1.10 runs `-glamor gl` without a GPU, so glamor fails;
it still reports GLX 1.4 with DRI3 absent. The provider is DRISWRast, which
finds swrast through the kernel's `LIBGL_DRIVERS_PATH`. GLX alone therefore
does not show hardware rendering; DRI3 and the probe's renderer do.

- **`--check` and `--probe`.** `--check` passed. `--probe` created a direct
  llvmpipe context and read back the red pixel, as expected without DRI3.
- **Startup.** As uid 1000, the launcher started its Pulse server in 2 s
  (`pactl info`: pulseaudio 17.0, default sink `brook_null`). It found `:0`
  and started the real client through srt-bwrap, the helper and SteamRT,
  reaching about 140 processes. A rootless "Unexpected Transport Error" window
  appeared after 165 s (offline) as a native window from waylandd/Xwayland.
- **Audio.** `pactl list clients` on the launcher's socket showed two `steam`
  connections, so the null sink was in use and no autospawn loop occurred.
- **Window close.** A semantic close completed, but Steam kept this dialog
  open. A WM close alone does not end it.
- **SIGTERM.** After SIGTERM to the launcher, Steam had not exited within
  15 s. The launcher then sent SIGTERM to 18 leftovers and, 5 s later, SIGKILL
  to 3 of them. The bridge job then reported zero live tasks and exit 0. The
  Pulse directory was gone and the state file showed `leftovers: []`.
- **Teardown time.** The final "stopped" line came 278 s after SIGTERM. The
  launcher's own steps took about 20 s. For the rest, all four vCPUs were
  sampled in `SchedLockWaitForTicket`, and file access in `/data/home` stalled.
  The serial log shows Steam's hundreds of offline TCP sockets being torn
  down, each logged to serial. This is old-kernel teardown cost, not launcher
  logic. Measure it on the Dell's current kernel before relying on the
  restart bound.

The windowless-grace path was not reached in QEMU, because the dialog ignored
close; it is covered by the host test.

A second QEMU boot checked the python3-minimal fixes, with ctypes blocked in
the guest's interpreter:
- **Install.** The bundle's `install.py` backed up, installed and
  import-tested the launcher on the guest's ext2.
- **Probe.** `--probe` re-executed under a full Python found by the store glob
  (`0r6k8xa2…-python3-3.13.12`, the same path as the Dell's) and passed. With
  `BROOK_STEAM_FULL_PYTHON=none` it reported that it needs ctypes and exited 1.
- **Launch.** A launch under the re-executed launcher reported subreaper true
  and started Pulse and the client.
- **SIGTERM.** Sent during SteamRT setup (host load about 40, so no window
  yet), it cleaned up in 8 s: four leftovers terminated, the Pulse directory
  removed, and the job drained with zero live tasks.

A third boot (rootless-3 bundle) checked the container `/etc`. The guest's
`/ETC/MACHINE-ID` is the placeholder, so the launcher created a private ID
(`machine_id: private`). A stand-in helper (`BROOK_STEAM_HELPER`) printed the
files in the outer container and then inside a real `srt-client-container`
(pressure-vessel, 16 s).
- **Machine ID.** Both containers had the same 32-hex ID at `/etc/machine-id`
  and `/var/lib/dbus/machine-id`. A second launcher run reused that ID.
- **Imported from the outer container.** pressure-vessel took `/etc/hosts`
  and `resolv.conf` from it.
- **Runtime's own copies.** Inside the client container, `/etc/nsswitch.conf`
  and `/etc/os-release` (steamrt3c) came from the runtime, as expected.

Evidence: `etc-probe-run4.txt` in the evidence directory. Evidence:
`build/qemu-steam-rootless/evidence/` in the `work/steam-rootless` worktree.

### Install and Dell test

`python3 scripts/package_steam_launcher.py` writes
`build/steam-launcher-bundle{,.tar}` with both scripts, `install.py`,
`SHA256SUMS` and `SOURCE.json`. On the Dell, as uid 1000,
`/nix/profile/bin/python3 install.py` verifies the checksums. It backs up the
installed copies as `*.before-<stamp>` with a `rollback-<stamp>.json`, installs
the files atomically and syncs. It copies with `copyfile`, `chmod` and `utime`,
not `copy2`: Brook's ext2 returns ENOSYS for the xattr calls, and `copystat`
does not tolerate that. It then import-tests the launcher under the shebang
interpreter and runs `brook-steam --check`.
`install.py --rollback` restores the newest backup set. Nothing goes into
`/nix/store`. The application entry is unchanged.

## Quake II and VSCodium Apps entries (Dell)

`nix/dell-app-launchers.nix` builds `brook-dell-app-launchers`:
- `bin/brook-quake2` runs Ian's gen 419 recipe: `brook-run brook-iris-run quake2 -datadir /data/home/quake2 +set s_openal 1 +set al_driver libopenal.so.1`, with the OSS OpenAL `lib/` first on `LD_LIBRARY_PATH` and `ALSOFT_CONF` set to the packaged `share/brook-quake2/alsoft-oss.conf`. It refuses root (yquake2 does too).
  - The game, OpenAL and Iris paths are plain strings, not closure references. On the Dell, the complete yquake2 (`sb7y1c75…`) and OSS OpenAL (`59h20vp1…`) are in the private stage directories `.brook-gpu-stage-c6f05db4` and `.brook-audio-stage`, and the top-level OpenAL path has no `lib/`. The wrapper takes the first complete candidate: the top-level path of the same repo derivation, then the stage copy. For Iris: `998k7n88…` (the measured Quake path), then the repo's `xiwkhbn3…`, then `/nix/profile/bin/brook-iris-run`.
  - `brook-quake2 --check` prints each choice and `BROOK_QUAKE2_CHECK_OK`, or `BROOK_QUAKE2_CHECK_MISSING`.
- `bin/brook-codium` is `data/nix-wrappers/brook-codium` with a store bash: `/nix/profile/bin/codium --no-sandbox --disable-chromium-sandbox` under `brook-run`, as uid 1000 with its own `VSCodium-brook` profile. Electron's SUID sandbox is not set up, so the bare `codium` entry aborts.
- `share/applications/brook-quake2.desktop`, a 24×24 icon from yquake2's `Quake2.png`, and `share/brook-stage/apps.json`, the menu edits for the staging bundle.

The Apps menu reads `/nix/share/applications/applications.idx` (Name, Exec, Icon, Categories, app id). `tools/launcher-stage/install.py --apps apps.json` edits it atomically: it adds `Quake II` (`--uid 1000 /nix/profile/bin/brook-quake2`), rewrites every entry whose executable is `codium`/`vscodium` to `--uid 1000 /nix/profile/bin/brook-codium` (keeping its arguments), installs the icon if absent, then notifies the desktop (syscall 0xB03). The previous manifest is saved, and `install.py --rollback TX` restores it with the links.

`nix-install`'s own manifest rebuild already redirects codium to `brook-codium` when the wrapper is in the profile, and now also launches `brook-quake2` as uid 1000. An older guest `nix-install` (the Dell's current one) drops that uid on its next rebuild; Quake II then refuses root with a message, and rerunning the bundle's manifest step (`--rollback` then install) restores it.

```sh
out=$(nix-build nix/dell-app-launchers.nix --no-out-link)
python3 tools/launcher-stage/make_bundle.py build/apps-launcher-stage --path "$out" \
  --link /nix/profile/bin/brook-quake2="$out/bin/brook-quake2" \
  --link /nix/profile/bin/brook-codium="$out/bin/brook-codium" \
  --link /nix/profile/share/applications/brook-quake2.desktop="$out/share/applications/brook-quake2.desktop" \
  --apps "$out/share/brook-stage/apps.json"
```
Guest steps are as for the Chromium launcher (`docs/waylandd-composition.md`), adding `--apps apps.json` to both `install.py` runs. Afterwards `run --uid 1000 /nix/profile/bin/brook-quake2 --check`.

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
