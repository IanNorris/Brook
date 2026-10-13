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

Once graphics is stable, expose VM-scoped lifecycle controls, keyboard/mouse
input, GPU readback images, serial/guest logs, and process state through MCP.
Use selected run directories and the monitor socket so writable experiments
remain isolated. Include bounded PCM-buffer capture with sample format/rate,
timestamps, queue-depth and underrun telemetry. Waveforms and sample-arrival
timelines can then distinguish missing guest audio from host playback delays;
capture should also work while host playback is muted. This MCP is planned,
not yet implemented.
