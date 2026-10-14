# Rebuilding the experiment environment

The current recovered base is `reconcile/gl-stack-to-main` (`e60f80c`). Late
July GL3 fixes were not found in any surviving branch. The GPU-object limit,
draw-aware submission splitting, and app-context present barrier have now
been reconstructed from saved observations, rather than recovered as their
original commits. The profile includes both software and GL yquake2 builds.

## Build and supply assets

Run from the repository root. The development shell supplies compilers,
image tools, and Pillow. The first command builds the kernel, native apps,
boot/data images, and host tests without the optional FUSE-only Nix refresh:

```bash
nix-shell --run 'BROOK_SKIP_NIX_TOOLS=1 ./scripts/build_all.sh Release'
```

Quake 2 game data and CD audio stay outside version control. `build/` is
ignored, as are packs, baseq2 directories, installers, and Track*.ogg files.
No game packs or OST tracks are required in the repository.

The GOG Quad Damage installer can be read without executing it:

```bash
nix-shell -p innoextract --run 'innoextract --extract --include app/baseq2 --output-dir build/experiments/assets/gog /home/ian/Downloads/setup_quake2_quad_damage_2.0.0.3.exe'
```

Supply another installation's `baseq2` folder with `--q2-dir` if preferred.
The disk creator validates the `pak0.pak` header and normalizes OST filenames
to lowercase `trackNN.ogg`. It copies the original music without transcoding.

The video fixture is Blender Foundation's original 1280×720, 24 fps H.264
Big Buck Bunny movie from the [official download directory](https://download.blender.org/peach/bigbuckbunny_movies/).
Download `big_buck_bunny_720p_h264.mov.zip` there, then extract the MOV into
`build/experiments/assets/big-buck-bunny-720p.mov`. Attribution is copied to
the data disk alongside the video. Assets are checksummed in the manifest.

```bash
nix-shell --run 'python3 scripts/create_experiment_disks.py --q2-dir build/experiments/assets/gog/app/baseq2 --q2-music "/data/Music/Ripped/Quake 2 OST HQ" --video build/experiments/assets/big-buck-bunny-720p.mov'
```

This creates `build/experiments/seeds/data.img` (16 GiB), `nix.img` (32 GiB),
and `manifest.json`. Images are sparse; the initial assets and 477-path
closure use about 5.2 GiB of host storage. An existing output is never
overwritten; choose another `--output` directory to rebuild.

`nix/experiment-packages.nix` uses the repository's package pin. The profile
contains Bash, coreutils, curl, Python, ffmpeg, Wayland, the Brook player,
file browser and calculator, LibreOffice, GIMP, Mousepad, Qalculate, fonts,
cursors, both yquake2 builds, and an OSS-enabled OpenAL library. Launchers
receive their actual dependency paths from Nix rather than hardcoded hashes.

## DOOM fixture

The DOOM shortcut expects `/boot/DOOM1.WAD`. A fresh build can contain the
engine without that data and will then exit immediately. To restore the
original 1.9 shareware fixture from the
[idgames archive](https://www.gamers.org/pub/idgames/idstuff/doom/):

```bash
python3 scripts/fetch_doom_shareware.py
nix-shell --run './scripts/update_disk.sh'
```

The helper verifies pinned archive/WAD SHA-256 checksums and reads the split
DOS installer ZIP without executing it. It writes only beneath ignored
`build/`. `update_disk.sh` picks up that cached WAD automatically; alternatively
set `BROOK_DOOM_WAD` to your own file. Launch a new experiment afterwards;
already-running clones keep their existing contents. No WAD enters git.

## Run and inspect

```bash
nix-shell --run './scripts/run_experiment.sh desktop'
nix-shell --run './scripts/run_experiment.sh video'
nix-shell --run './scripts/run_experiment.sh quake-soft'
nix-shell --run './scripts/run_experiment.sh quake-gl3'
nix-shell --run './scripts/run_experiment.sh office'
```

Run one scenario at a time with the default instance. Each gets writable
clones of the boot, data, and Nix disks under `build/experiments/results/`.
The seed images remain unchanged. The script prints the run directory and
serial log, exposes VNC on `127.0.0.1:5998`, and limits runtime to 600 seconds.
To reuse a particular run's state, set `BROOK_EXPERIMENT_RUN` to its directory.
To select other seeds or a monitor/VNC instance, set `BROOK_EXPERIMENT_DISKS`
or `BROOK_INSTANCE`. Stop its VM before reusing a run directory.

The default keyboard is PS/2. `BROOK_USB_KBD=1` explicitly adds a USB keyboard
for USB testing. The experiment runner does not enable USB input automatically. Set
`BROOK_USB_KBD=0` and `BROOK_USB_MOUSE=0` to disable inherited USB toggles.

`quake-gl3` selects hardware virgl and GPU composition by default, and requests
an actual 1280×720 drawable using `r_mode=-1`, `r_customwidth`, and
`r_customheight`. The older software scenario's `vid_width`/`vid_height` do
not establish its drawable size. `BROOK_RENDERNODE` selects the host GPU:
on this machine `renderD128` is Intel UHD 630 and `renderD129` is NVIDIA.
Only Intel has been tested in this reconstruction. Override
`BROOK_GPU_QEMU`, `BROOK_GPU_MESA`, and `BROOK_GPU_GLVND` to reuse known store
paths rather than resolving a different host graphics stack.

With EGL headless, monitor `screendump` returned `Error: no surface` even
while guest GL was rendering. `BROOK_GPU_THUMB=1` provides small periodic
GPU readbacks over serial instead. `BROOK_GPU_FULL=1` is much more intrusive
and can interleave a large frame dump with application logs.

The headless default has a silent host backend while providing a guest HDA
device. To listen through the host's PipeWire server:

```bash
nix-shell --run 'BROOK_AUDIODEV=pipewire ./scripts/run_experiment.sh quake-soft'
```

```bash
python3 scripts/qemu_monitor.py --instance 98 'info status'
python3 scripts/qemu_monitor.py --instance 98 'screendump /tmp/brook.ppm'
python3 scripts/qemu_monitor.py --instance 98 'quit'
```

## Verified and outstanding

- Release kernel/native apps build; all 12 host tests pass, including a
  regression test for splitting between a vertex-buffer bind and its draw.
- Desktop, Bash terminal, calculator, and generated wallpaper render under KVM.
- Big Buck Bunny decodes at about 24 fps; the player opens `/dev/dsp` for audio.
  The current window is 800×600 even though the source is 720p. Native sizing
  and aspect preservation remain runtime work.
- yquake2's software renderer loads `base1` from the GOG packs. The public
  Linux OSS `GETOSPACE` request is `0x8010500c`; correcting Brook's old value
  restores OpenAL context creation. `osscheck` probes the live ABI using the
  toolchain's independent Linux header definition.
  Ian confirmed music playing through PipeWire, with limited periodic
  stuttering consistent with the previous session's performance baseline.
- Guest writes corrupted ext2 directory/inode metadata on the first reused
  game-test disk. A read-only `e2fsck` confirmed this. Fresh clones preserve
  reproducibility; they do not fix that persistence defect. Inspect or repair
  a stopped run's copy, never a live image or the verified seeds.
- The compositor lacks relative-pointer and output-scaling protocols used
  by SDL. The late GL3 fixes were reconstructed: GEM capacity 256→2048;
  whole-command splitting rewound to a completed draw; and a fenced empty
  submit on the app context before returning a signaled out-fence.
  The barrier now runs for every requested out-fence, rather than behind the
  old experiment's opt-in flag. General `VIRTGPU_WAIT` remains a separate gap.
- Mipmap resource creation fix `35ab108` is included in this checkout, but
  is absent from the surviving `origin/main`. It forwards the requested mip
  levels to virtio-gpu; it does not replace legacy mipmap generation.
  Saved BRO-228 notes associate `GL_GENERATE_MIPMAP` with yquake2's GL1
  renderer. The later note (`44b8eba3d972ec6c`) reports black non-power-of-two
  textures even with the older host graphics stack: incomplete mip chains
  explained the white symptom, while black level-zero texture data remained
  unresolved. No corresponding autogeneration replacement patch was found
  in the surviving branches or restored yquake2 patches.
- LibreOffice and the other desktop packages are staged; their presence in
  the closure does not establish that every application works on Brook.
  LibreOffice's real Nix wrapper was tested. Its DBus bootstrap failed to
  create `/tmp/libreoffice-1000/libreoffice-dbus` with `ENOENT`, so Writer
  did not start. The log is retained in its isolated run directory.

These runtime findings are separate from the repaired fresh-checkout
problems: incomplete submodule registration, missing Bash libraries, wrong
musl-loader search depth, FUSE-denied mounts, stale launcher hashes, and
silently ignored host-test failures.

## GL3 reproduction, 14 October 2026

The baseline at 1280×720 used QEMU 10.2.2, virglrenderer 1.3.0, and Mesa
26.0.6 on Intel UHD 630. The guest reported `GL_RENDERER: virgl` and an
actual 1280×720 drawable. It reproduced host `IOV data size exceeds resource
capacity`, illegal surface/handle errors, and failed guest `AppSubmit3D`.
The existing 1D staging-buffer creation fix `f564d39` is already included.

After reconstruction, the same hardware stack presented 25 recorded 720p
dmabuf buffers with no logged submission errors, command timeouts, or guest
faults, including menu and console transitions. GPU thumbnails still show
suspect UI/text rendering; lack of logged errors does not establish visual
correctness or resolve the old high-resolution menu issue. Captures and logs
are under `build/experiments/results/experiment_quake_gl3-O9Q9Yu/`; baseline
host logs are `gl3-720p-qemu.log` and `gl3-720p-gpu-compose-qemu.log` in the
results directory. The host test report is `gl3-host-tests.log`.

## VM control MCP follow-up

See [vm-control-mcp.md](vm-control-mcp.md) for the Linux plugin research and
VM-only interface proposal, and [backlog.md](backlog.md) for acceptance criteria.

Once graphics is stable, expose VM-scoped lifecycle controls, keyboard/mouse
input, GPU readback images, serial/guest logs, and process state through MCP.
Use selected run directories and the monitor socket so writable experiments
remain isolated. Include bounded PCM-buffer capture with sample format/rate,
timestamps, queue-depth and underrun telemetry. Waveforms and sample-arrival
timelines can then distinguish missing guest audio from host playback delays;
capture should also work while host playback is muted. This MCP is planned,
not yet implemented.

## Terminal input regression, 14 October 2026

The old USB-default fix `7b0e50a` is already included in main; this was not a
missing branch merge. The recovered experiment runner forced USB keyboard
attachment back on. A native terminal doubled characters while Mousepad did
not. Disposable-VM comparison showed `abc123` with PS/2 alone and
`aabbcc112233` with USB enabled.

The USB driver supplied ASCII on release events, and the terminal forwarded
nonzero ASCII regardless of event type. The driver now translates characters
only on press; the terminal also independently accepts character input only
on press. Releases still reach raw/Wayland consumers. The runner again leaves
USB input off by default; only an explicit value `1` enables the optional USB
keyboard/mouse (`0` now really disables them).

Rebuilt the release kernel and xHCI module, updated the boot image and passed
all twelve host tests. With USB explicitly enabled, the `abc123` terminal
capture matched the PS/2 baseline byte-for-byte; one Backspace then Shift+Z
produced `abc12Z`. Raw evidence stays in ignored experiment/bridge directories.
Existing running VMs retain their old devices and kernel; a new launch is
required to use these changes.

## Wayland GPU borders and missing DOOM data, 14 October 2026

The accelerated compositor created every window-content texture as XRGB and
always drew it without alpha blending. GTK client-side shadow/corner pixels
therefore became a solid black rectangle. SHM Wayland textures now preserve
ARGB and blend content alpha; texture format is re-created if the source type
changes. Legacy framebuffers and external GL targets keep their prior opaque
handling. Verified Mousepad's shadow area composites over underlying windows
and wallpaper in an Intel virgl VM. This does not claim to resolve external
GL swapchain alpha/resize issues (REC-03).

DOOM's immediate exit was an explicit missing-IWAD error, not a kernel crash.
The verified shareware fixture is staged in the ignored boot image and its
shortcut now reaches the title/menu and a playable first level. Capture/log evidence is retained under
`build/vm-bridge/0df06b9e8906444da02654802c8d312d/`. The release build and all
twelve host tests pass. Video-player logs show AAC decode and `/dev/dsp` output;
`BROOK_AUDIODEV=none` intentionally discards host playback. Use `pipewire` on a
new user launch to listen, while keeping MCP test launches silent.

## Dragging, Quake defaults, argv and audio writes, 14 October 2026

Native-window testing exposed additional issues:

- The compositor painted chrome before applying drag/input changes, then drew
  GPU content using updated geometry. Input now runs before scene painting.
  GPU compose submissions also wait for a render fence before publishing the
  result to the host display context. Repeated VM-only drag captures retain
  coherent decorations. Physical SDL presentation still needs human retesting.
- Yamagi 8.60's bundled `stuff/yq2.cfg` uses ESDF, explicitly `D=+back`.
  The recovered SDL probe reports W/A/S/D correctly (26/4/22/7 and the expected
  key symbols). Fresh Brook profiles execute the installed WASD defaults;
  existing `config.cfg` or `autoexec.cfg` remains authoritative. Two wrapper
  tests check renderer/map arguments and preservation of saved preferences.
- The GL3 scenario has 25 shell arguments, exceeding the old limit of 16.
  The shell now accepts 32. Its wrapped exec reaches 37 arguments (38 during diagnostic setup), exposing a
  separate initial-stack bug: a 32-pointer array was followed by an argc-sized
  read. Stack setup now supports the exec syscall's 64-entry bound and actually
  clamps before indexing. `/boot/SCRIPTS/ARGVCHECK.RC` forwards 41 ordered
  arguments through Bash and verifies the terminating null pointer.
- `/dev/dsp` previously acknowledged all bytes after the mixer silently
  clamped an oversized write. It now returns accepted-byte counts (or EAGAIN
  when no samples fit) and bounds blocking-space requests to ring capacity.
  `run /boot/BIN/OSSCHECK --stream` demonstrates the old failure and the fix:
  176,400 bytes formerly acknowledged in one call; the corrected first return
  is 65,536 and retries deliver the full input in three calls.

Guest-generated config files disappeared during first-run game-directory
creation. The disk creator now installs OpenAL configuration at
`/nix/etc/alsoft-oss.conf`, installs `brook-controls.cfg` alongside game data,
and precreates the user directories. This avoids relying on runtime mkdir/
heredoc writes for those supplied defaults; the filesystem defect itself is
still unresolved. The OpenAL fixture pins OSS to 44,100 Hz, stereo, S16_LE.

The existing KVM pvclock fix (`f56619e`) is present and enabled. A sampled
LAPIC/compositor tick comparison measured about 1,025 ms per host second,
not a two-times clock in that run. Silent SDL captures after the OSS fix
produced about ten seconds of PCM in ten seconds of host monotonic time.
This does not prove the reported intermittent fast-tempo music is fully fixed:
SDL tests used its dummy host audio driver, and an OST-envelope comparison was
inconclusive. Preserve the real SDL/PipeWire comparison and guest queue/sample
telemetry investigation under RUN-02.

The release build, twelve host checks and fourteen bridge/wrapper checks pass.
The recovered probes live in `tools/sdl2-input-probe`; their old investigation
notes are distinguished from current verification. All PCM, images, test
runs and licensed game assets remain in ignored build directories.

The selected default seed set is now `build/experiments/seeds`; previous seeds
are preserved at `build/experiments/seeds-before-20261014-input-audio`.
The final installed-config retry initialized GL3 at 1280×720 through Intel
virgl, executed `brook-controls.cfg`, and read the installed 44.1 kHz/stereo/
int16 OpenAL configuration. Evidence is under
`build/vm-bridge/ef8fc1e9a08c455885ca2cae39ce534b/`. Its final SDL-dummy capture
contains 10.136 seconds of PCM over a 10.090-second host interval. Validation
VMs were stopped; use a new launch to pick up the boot image and seed changes.
