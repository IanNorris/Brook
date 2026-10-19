# Standalone Proton checkpoint

This is a bounded Helltaker smoke test using Proton GE, without the Steam
client or a Steam Linux Runtime container. It does not establish Steam,
anti-cheat or 32-bit games. Separate [D3D12 checkpoints](d3d12.md) cover
VKD3D shader/readback, Windows SDL3 Snake and Veloren's 3D singleplayer path.

## Runtime

The test uses the official [GE-Proton11-10 x86-64 release](https://github.com/GloriousEggroll/proton-ge-custom/releases/tag/GE-Proton11-10).
The 566,144,788-byte archive has SHA-256
`3c26ee96735ce9f42de018db4f82ab33e275f19150d6cea25ea7937b7f2530dd`,
checked against the release asset digest. Its bundled Wine and DXVK are used;
the game logs DXVK `v3.1.1-134-g2cb40186c43d6170`, D3D11 feature level 11.1 and
hardware Venus on Intel UHD 630. Helltaker's own bundled Mono/Unity 2019.2.9f1
runtime is unchanged. The game copy omits the standalone DXVK DLLs from the
[earlier Wine test](wine-unity.md).

The VM has eight vCPUs, 8 GiB, GPU composition and a silent host audio backend.
Its kernel is the preceding signal-context checkpoint, SHA-256
`f932600c7ef0ac9606057f8e976e47b3f397695aae69383dc965a6d342e2f1b3`.
No kernel changes are required for this test. Private images, runtime payloads,
logs and audio remain under `build/workloads/proton-20261019/`.

## Required integration

- Brook now advertises `wl_output` v4 and sends stable `BROOK-1` name and
  description events before `done`. GE's Wayland driver rejected the old v3
  output and repeatedly failed to initialize its display configuration. The
  real-protocol regression checks output event versions and ordering for
  clients binding v1, v2, v3 and v4; the compositor package's existing checks
  pass too.
- Set `PROTON_ENABLE_WAYLAND=1`, `PROTON_WAYLAND_MONITOR=BROOK-1`,
  `WAYLAND_DISPLAY=wayland-0` and `XDG_RUNTIME_DIR=/tmp`. Supply the installed
  xkeyboard-config package's `share/X11/xkb` as `XKB_CONFIG_ROOT`; Proton's
  default `/usr/share/X11/xkb` is absent in the Nix guest.
- The copied runtime's x86-64 ELF interpreters use the guest's existing Nix
  glibc loader. A private library directory links the tested Wine closure's
  libraries; Proton still prepends its own bundled libraries. Optional
  preloaders are disabled, with `WINELOADERNOEXEC=1`: Brook's low placement of
  interpreter-less PIE executables otherwise collides with Wine's low-address
  reservations. This remains a packaging workaround, not a loader fix.
- `PROTON_NO_ESYNC=1`, `PROTON_NO_FSYNC=1` and `PROTON_NO_NTSYNC=1` select Wine's
  server-side synchronization. These accelerators remain unaccepted.
- A separate `STEAM_COMPAT_DATA_PATH` holds the prefix. `UMU_ID` selects GE's
  non-Steam launch path; a Windows `Z:\...\Helltaker.exe` argument avoids the
  Unix-path helper. This uses GE's Proton script directly, not the umu runner.
  Game-specific ProtonFixes are disabled for this baseline.
- Use the per-prefix ALSA selection and clocked OSS plugin described in
  [the Unity audio configuration](wine-unity.md#audio-configuration).
  Proton must create its prefix through `run` before `runinprefix reg add`.
  Wine stdout/stderr and Unity player logs are file-backed.

The reproducible host staging helper is:

```sh
python3 scripts/workloads/proton_stage.py \
  --source build/path/to/extracted/GE-Proton11-10-x86_64 \
  --wine build/wine64-brook-result \
  --patchelf build/proton-patchelf-result/bin/patchelf \
  --output build/workloads/proton-stage \
  --disable-preloaders
```

It refuses an existing output or an output outside `build/`, preserves the
source, and records hashes for each changed interpreter. It produces a runtime
archive and library map for an isolated guest installation. The helper
reproduces all thirteen interpreter patches and the runtime library map used
by the test. It neither installs games nor attaches a user's source folders.

Updating a package profile alone does not replace Brook's flat boot tools:
`data/INIT.RC` explicitly starts `/nix/bin/waylandd`. The private test updated
that copy too. Fresh images get it from `create_nix_disk.sh` or
`update_nix_disk.sh`; verify the running process's `/proc/PID/exe`, not just
`/nix/profile/bin/waylandd`.

## Gameplay scope

The corrected run in `candidate11-v4` reaches the Unity configuration dialog,
selects 1280x720 windowed mode, renders the intro and enters the first puzzle.
Arrow keys move the player and kick a skeleton; the counter falls from 23 to
20. `R` resets the board and counter to 23. Minimize and restore pass. Normal
window close returns zero and all tracked game tasks exit.

A four-second intro audio capture contains 175,215 stereo frames at 44.1 kHz,
with RMS 0.1590. After close, the stream is inactive and QEMU emits no frames.
This verifies PCM delivery and stream cleanup, not subjective audio quality.
Native Vulkan shader/readback and persistent EGL RGB readback pass afterward.
GPU backing mappings and byte counts balance, with no quarantined resources.

A clean VM restart preserves the entire saved `user.reg` byte-for-byte. The
cold reopen displays 1280x720 with Windowed still selected and exits normally
with no tracked descendants. This verifies configuration persistence; later
chapter progress and game save/load remain untested. Final heap integrity and
all four offline filesystem checks pass after orderly shutdown.

![Helltaker through standalone Proton GE on Brook](images/brook_proton_helltaker_1080.png)

Later chapters, completion, long-session stability and gamepad support remain
untested. Optional service/device diagnostics remain in the retained logs.

## Earlier diagnostic runs

The installed Valve Proton Experimental build lacks a Wayland driver. The
pinned GE-Proton10-34 includes one, but its optional preloader collided with
Brook's low PIE placement. With that disabled, it faults in Wine's
`server_get_name_info` handling of an empty imported standard-handle name; the
newer Wine has a guard for the zero-length case. GE 11 avoids that startup
fault and supplies the driver needed for this test. The earlier diagnostics
are retained separately and are not gameplay acceptance evidence.
