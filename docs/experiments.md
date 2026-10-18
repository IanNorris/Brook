# Rebuilding the experiment environment

Recovery started from `reconcile/gl-stack-to-main` (`e60f80c`); the tested
changes now live on local `main`. Late
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
and `manifest.json`. Images are sparse; the 18 October browser-inclusive
546-path closure uses about 5.4 GiB of host storage before optional game/video
assets. An existing output is never
overwritten; choose another `--output` directory to rebuild.

`nix/experiment-packages.nix` uses the repository's package pin. The profile
contains Bash, coreutils, curl, Python, ffmpeg, Wayland, the Brook player,
file browser, App Gallery and calculator, LibreOffice, GIMP, Mousepad, Qalculate,
Chromium, Firefox with the clocked ALSA integration, fonts,
cursors, both yquake2 builds, and an OSS-enabled OpenAL library. Launchers
receive their actual dependency paths from Nix rather than hardcoded hashes.

For a new image without external media or game assets:

```bash
nix-shell --run 'python3 scripts/create_experiment_disks.py --output build/experiments/fresh'
```

The browser-inclusive image was built from source without reusing profiles.
Normal menu launches for Firefox and Chromium coexist in an eight-CPU, 4-GiB
guest. Each played a controlled 48-kHz Opus fixture and produced approximately
ten seconds of PCM without silent 10-ms blocks. A synthetic home file retained
contents, UID 1000 and mode 0600 across checked VM restart; Chromium also retained
its synthetic local-storage marker. Firefox initially rejected the write with
`QuotaExceededError` on the zero-filled `statfs`/`fstatfs` stub. With real ext2/FAT
capacity reporting and unchanged browser preferences, its write succeeds and
the marker survives a separate cold VM restart without being rewritten. Both
browsers use the same isolated disks and origin across these tests. Real-site
cookies and broader profile persistence remain unaccepted; see
[filesystem capacity acceptance](filesystem-space.md). Generic public package-index
entries remain stock packages,
and base-image components still lack complete Gallery ownership/registration.

Evidence is under ignored `build/workloads/fresh-browsers-20261018`, including
the generated closure manifest, separate boot/restart requests, audio reports
and filesystem checks. This does not rerun either full bootstrap path or prove
all applications on the image work.

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

The subsequent user session `347625569e0d4270bb8f522c45d1acb6` booted to a
black GPU scanout with host `BIND_OBJECT`/illegal-command-buffer errors and a
zero command header. The runtime DRM capability query cleared the shared GPU
request buffer without the command lock. Both that query and the normal 2D
framebuffer flush now take the same lock as compositor/app submissions; panic
presentation deliberately remains lock-free after other CPUs halt. The rebuilt
release passes all twelve host tests. A clean GL3 run in session
`c251499b3f5749e49f9eaf5befc3d31f` reached Outer Base at 1280×720 through Intel
virgl and produced a visible capture without host command-buffer errors. This
validates one startup after the fix, rather than proving every intermittent
graphics fault resolved. Existing running VMs retain their original driver.

## Guest package installation validation (14 October)

New seeds now compile `nix-fetch`, `nar-unpack`, `nix-install` and `nix-search`
from current source, include the package index and xz/zstd decompressors, and
create package-cache and application-index directories. Earlier experiment
seeds omitted the installer bootstrap. Recreate seeds in a new output directory;
existing images are not upgraded automatically.

`run /nix/bin/nix-install vlc` installed the indexed VLC 3.0.23-2 closure
(342 newly downloaded paths). `run /nix/bin/nix-install ladybird` installed
Ladybird 2026-04-04 with 36 additional paths, including the 173 MB Qt declarative
archive. Both installations used the guest downloader and NAR extractor.
Ladybird rendered Example Domain and navigated from example.com to example.org
through bridge keyboard input. `brook-ladybird` supplies a persistent `/data/home`
and writable XDG directories with CPU painting and the currently needed database
and HTTP disk-cache exclusions. This is a baseline browser smoke test, not a
claim that all browser workloads work.

The downloader now retains long dependency lists, tracks closures beyond 256
paths, checks existing paths' transitive dependencies, supports zstd and valid
relative NAR symlinks, and terminates producers when extraction fails. Archive
transfers have a low-speed timeout instead of a five-minute wall-clock cap.
Metadata retries start with a fresh buffer; archives do not retry within a
partially emitted stream. `NIX_CACHE_URL` can select an experiment's local cache.

Runtime checks exposed shared descriptor-lock/duplication and shared `brk`
corruption, invalid *at syscall forwarding, COW output-buffer rejection, ext2
sector/free-count accounting, and lost directory entries at `getdents` buffer
boundaries. The probes in `tools/at-syscall-probe` retain these reproductions.
The 800-entry indexed-directory mutation test now passes and its synced stopped
image passes all five `e2fsck -fn` phases. The unmerged rename destination-inode
patch remains a separate investigation in REC-01.

Evidence is retained outside git in `build/experiments/results/`, including
`nightly-final-kernel-tests.log`, `nightly-bridge-final-tests.log`,
`installer-ext2-htree-clean-fsck.log`, and the installation/VM serial logs.
The [VLC compatibility build](../tools/vlc-brook-pkg/README.md) installed through
the guest installer and played the 720p movie with native OSS audio on one vCPU.
Two screenshots show advancing movie frames; a silent-host WAV capture contains
89,024 stereo frames at 44,100 Hz (2.019 s, RMS 0.0215). Keep `BROOK_SMP=1` for
this baseline. Four-vCPU startup and shutdown during concurrent installation
remain unstable and are retained in REC-03/REC-04. This establishes playback,
not general application or audio-stutter stability.

## Prepared browser/player checkpoint

The local checkpoint `build/experiments/results/apps-ready-20261014` contains
both guest-installed apps, the current boot shortcuts, and a fresh data disk
with the original private Quake assets and 720p video fixture. From Brook:

```sh
BROOK_SMP=1 nix-shell --run './scripts/run_installed_apps.sh build/experiments/results/apps-ready-20261014'
```

This is a historical one-vCPU browser/player baseline. It predates the current
native icons and launcher assets; do not use it to review today's desktop.
The helper now defaults to eight vCPUs and 4 GiB RAM. Select a prepared image
explicitly with its directory argument or `BROOK_APPS_RUN`; otherwise it uses
`build/experiments/current-apps`, a local pointer maintained when a new app
checkpoint is prepared. It opens a native SDL window with silent host audio and
the bridge enabled. Start Ladybird first, then the VLC media player shortcut, matching the verified
dual-app launch sequence. In the
guest terminal:

```sh
run /nix/profile/bin/brook-ladybird https://example.com
run --uid 1000 /nix/profile/bin/brook-vlc /data/videos/big-buck-bunny-720p.mov
```

`BROOK_EXPERIMENT_DISPLAY=headless` selects VNC instead. Set `BROOK_INSTANCE`
when another VM uses the default instance. The checkpoint is writable; stop
its VM before launching it again. `BROOK_APPS_RUN` selects another prepared
run. Original seeds and the installation-test images were preserved; a new
bootstrap seed is at `build/experiments/seeds-ready-20261014`. Runtime images,
cache exports, game data and capture artifacts remain ignored by git.

For a fresh checkout, build the kernel and create seeds using the instructions
above, install Ladybird with guest `nix-install`, and follow the VLC package
README for its local compatibility cache. The prepared checkpoint is a local
artifact, not a checked-in disk or a replacement for the reproducible build.

The longer playback check exposed a shared-memory fork defect: the old kernel
applied COW to memfd buffers and a fork from a worker inherited stale mapping
records. `sharedcheck.c` first reproduced a child write remaining invisible to
its parent, then passed ordinary and worker-thread fork cases after the fix.
Memfd backing now belongs to its object rather than the allocating PID, and
fork preserves its borrowed PTEs and copied VMA references. Subsequent movie
captures remained correctly rendered across repeated `xdg-screensaver` helper
forks. Earlier corrupted captures and shutdown faults remain saved as recovery
evidence rather than being treated as passing results.

### Real-site Ladybird check (2026-10-14)

Test session `eba0405cf1db4078828d7721fac86825` used a separate reflink copy
of the prepared checkpoint, one vCPU, headless display and CPU painting.
Google's HTTPS homepage rendered; rejecting optional cookies, typing a search,
submitting it and following the Ladybird result all worked. Amazon UK rendered
its homepage, search autocomplete, results for `cable`, and a product detail
page with images. No CAPTCHA appeared during these public browsing tests.
Sign-in, checkout and YouTube were not tested.

This is a functional browsing baseline, not a smooth-browser certification.
Loads took tens of seconds, input was delayed or partially lost during loading,
and text/layout rendering had defects. Wheel events, Page Down and a scrollbar
click did not visibly move the tested pages. The guest remained accessible and
navigation recovered after a long apparent stall. Serial logs contain
`HTMLParserEndState` timeouts; their cause and the scrolling/input path need
investigation before attributing these limits to Ladybird or Brook.

Evidence is in the ignored session artifact directory: Google results
`e5e9325277074d3fabe1073f146afc19.png`, followed result
`735cdcc47b9a41978edeae34e53d3745.png`, Amazon results
`ac6e544b38a0492f9327b943f9730e55.png`, product detail
`009d5fb6a6384e83944a22173af2b7d6.png`, and `serial.log`.

### Ladybird performance investigation (2026-10-14)

All measurements used isolated copies of the prepared disks, one vCPU with
KVM confirmed by QMP, silent host audio, software display and Ladybird CPU
painting. No user VM or host desktop was controlled. Evidence lives under
ignored `build/experiments/results/ladybird-perf-20261014/` and the session
artifact directories below.

The deterministic workload is now saved as
`tools/profile/browser_benchmark_server.py`. Run it on the host, then launch
`run /nix/profile/bin/brook-ladybird http://10.0.2.2:8899/` in a QEMU
user-network guest. It serves a small HTML page and 32 uncached 128 KiB
resources from host loopback. `BENCH_DONE_<ms>` records the JavaScript
transfer duration for 4 MiB; it excludes browser process startup. This is a
network/IPC workload, not a page-rendering score or Internet speed test.

| Measurement | Original kernel | Opt-in tracing kernel |
| --- | ---: | ---: |
| Initial local transfer, one browser | 5,824 ms | 1,720 ms |
| Diagnostic bytes across the recorded launch window | 346,234 | 18,526 |
| Unconditional RequestServer trace lines in that window | 5,178 | 0 |
| Recent compositor interval p95 after that launch | 413 ms | 306 ms |
| Recent compositor interval maximum after that launch | 2,183 ms | 1,849 ms |

The first pair demonstrates an approximately 3.4x transfer improvement from
removing unconditional RequestServer syscall/poll logging. It is one paired
run, not a universal browsing speedup. Both launches used a five-second guest
profile during startup; export added overhead outside the transfer interval.
The compositor values are rolling 256-loop intervals at the query point,
not full-run percentiles or measured display scanout intervals.

Once Google was also open in the comparison VM, three unprofiled, untraced
reloads took 4,694 / 3,488 / 3,589 ms (median 3,589 ms). QEMU consumed
5.11 / 3.87 / 4.34 CPU seconds over 5.06 / 3.81 / 4.32 host seconds to
observe the completion markers. These are aggregate QEMU process CPU times,
including device threads. The additional browser and accumulated allocations
make these different conditions from the initial pair. They show substantial
remaining CPU work. Explicit `strace RequestServer net` still worked; one
reload with it enabled took 4,059 ms. Tracing was disabled again afterward.

The primary remaining bottleneck is the kernel heap, not exhausted RAM:

- Host QMP snapshots during a new Google launch: 201/250 in `kmalloc` or
  `WriteBlock`; 215/250 had the interrupt-enable flag clear. Of the allocator
  snapshots, 123 landed in the first-fit block scan and 76 in the forensic
  free-poison fill loop, confirmed against the run's ELF disassembly.
- A separate warm local reload, without guest profiling: 52/71 snapshots in
  allocator/poison work, 52/71 with interrupts disabled; 13/71 in block-write
  or request-submission paths. Host snapshots avoid guest timer masking,
  but can favor VM-exit points and introduce synchronization overhead; these
  are corroborating observations, not precise CPU utilization fractions.
- Heap inspection: 76,712 KiB region, 11,645 blocks (11,637 allocated),
  72,209,408 used bytes, 4,853,120 free bytes, largest free 4,814,848 bytes,
  poison enabled. `kmalloc` scans all blocks from the start; split and
  coalescing paths repeatedly poison large already-free regions while holding
  an IRQ-disabling lock. No allocator behavior or poisoning was changed here.
- Cold launch also samples synchronous virtio block reads. Storage writes
  remain visible in the warm reload. Prioritize allocator indexing and
  redundant-poison elimination before tuning the browser renderer; preserve
  free-poison/forensic checks and validate heap integrity and concurrency.

The guest timer profiler has two limitations for this workload: samples can
land just after interrupt re-enable, and its own periodic FAT writes consume
allocator/storage time during recording. Use host sampling and unprofiled
transfer runs to corroborate it. ELF exports previously lost all symbols when
`llvm-cxxfilt` was missing even though `nm` worked; the converter now retains
ELF names in that case, with a regression test.

A settled local-page click was observed in 764 ms from host injection to the
serial title marker, including bridge/QMP and 10 ms polling overhead. This
single observation is not pure input latency or a percentile. QMP keyboard
injection remained unreliable during the test, whereas kernel debug
`inject combo ctrl+r` reliably reloaded the page; keep input routing separate
from startup starvation. A later stats query reported `max_ms=4294967295`, an
invalid extreme requiring telemetry investigation rather than interpretation
as a real stall. Google eventually rendered its consent page in the comparison
VM; this performance run does not certify whole-site compatibility or SMP.

Baseline benchmark session: `06d9393ccf98449484204845a7396c4e`, kernel SHA256
`2e5e3c5eafcb06273fc5642d38ff689192ef56d045e6f2b4450de686dd8d4b9f`.
Comparison session: `15b83c6d82674d158d1ef7c434179452`, kernel SHA256
`d113eb8dd3b4aa43d007904df5f2285db1ecf585c40ec8c66c8aff084afbd073`.
The later source cleanup only removes empty process-name diagnostic branches;
the final kernel was rebuilt. Build, all 12 kernel host checks, all 15 bridge
checks and the symbol-export regression passed. Temporary VMs and the test
HTTP server were stopped after evidence capture.

### Runtime heap poison and Quake GL profiles (2026-10-14)

Heap alloc/free poisoning now defaults **off**. Enable it for diagnostics with
`heap poison on`, restore the normal setting with `heap poison off`, and
inspect it with `heap`/`mem heap`. Both the guest shell and framed kernel TCP
bridge accept the toggle; the bridge also accepts `heap check`. Enabling holds
the heap lock while reseeding existing free regions, so unpoisoned old data
is not misreported as UAF; live allocations are preserved. Forensic poison
remains available when enabled. The standalone allocator regression boot
explicitly reported `[PASS] heap`, including off-to-on reseeding and live-data
preservation, followed by ordinary allocation/reallocation/integrity checks.

Matched browser tests used the same kernel, prepared seed disks, one KVM vCPU,
software display, and the same 4 MiB local workload. The poison-on run enabled
it through the bridge before launching Ladybird; normal early boot was off in
both runs. Each mode opened the local benchmark and then a second Ladybird
instance on Google. Reloads explicitly focused the benchmark window and had
no active guest profiler. An idle, separately prepared GPU VM was present in
both verified runs; no Quake workload ran during browser measurements.

| Browser measurement | Poison on | Default off |
| --- | ---: | ---: |
| Initial transfer duration | 2,050 ms | 1,537 ms |
| Three reloads with only benchmark open | 1,257 / 1,202 / 1,125 ms | 1,235 / 1,163 / 1,238 ms |
| Three reloads with Google also open | 4,000 / 3,953 / 3,797 ms | 2,708 / 1,873 / 1,509 ms |
| Median with Google also open | 3,953 ms | 1,873 ms |
| Google launch snapshots in free-poison fill | 88/250 | 0/250 |
| Google launch snapshots in allocator | 121/250 | 158/250 |

The loaded-browser median improved about 2.1x; the lighter reloads showed no
clear improvement. Off-run reloads trended down, so do not treat the median as
a universal or steady-state speedup. Free-fill work disappeared, but first-fit
scanning still dominates: the warm off reload had 31/38 host snapshots in
`kmalloc`, versus 32/82 allocator and 23/82 poison-fill snapshots when on.
Both heaps passed integrity checks with approximately 11,600 live blocks.
Captures retain the IRQ-masking/VM-exit sampling caveats from the earlier
investigation. An initial off trial reloaded the wrong focused browser after
opening Google; its incomplete results were discarded and retained under
`poison-off-initial` as harness evidence. Verified results are `poison-off`
(session `bbf54474e4254b31a0dc3fd0287ea66b`) and `poison-on`
(`4f020eca13224af1b5ab3323dade3217`) under the ignored performance directory.
Both use kernel SHA256
`fa1c34f0559eaf53a73ab5fed1b7561d5f7e4bb1a5844e19eab8bf0d3cf31713`.

Quake used one KVM vCPU, GL3, a confirmed **1280x720 drawable**, GPU composition,
Intel `/dev/dri/renderD128`, and host audio `none`. Its renderer reported
`virgl (Mesa Intel(R) UHD Graphics 630 (CML GT2))`, rather than software GL.
Separate cold-start and settled stationary `base1` gameplay captures recorded
five-second guest CPU/context-switch profiles plus randomized host register
snapshots. Gameplay comparisons toggled heap poison in the same running game.
The workload is an active rendered game scene, not a timedemo FPS benchmark.

A second avoidable logging cost appeared: Quake probes up to 1,000 numbered
pak paths in multiple search roots. Roughly 2,000 normal missing-file probes
triggered unconditional `sys_open FAIL` serial diagnostics. Those detailed pak
messages now require explicitly enabled file-category strace; open results and
discovery remain unchanged. Comparing the same initial 300-snapshot launch
window, serial printing fell from 95/300 snapshots (31.7%) to 3/300 (1.0%).
Allocator snapshots increased from 103/300 to 155/300 as other work became
visible. These are sampled phase observations, not a measured 32x whole-game
loading speedup. Full time-to-playable was not benchmarked.

| Quake gameplay host snapshots (250 each) | Poison off, baseline | Poison on | Off, logging fix |
| --- | ---: | ---: | ---: |
| GPU SubmitCommand + queue notification | 142 | 140 | 138 |
| Allocator | 31 | 38 | 24 |
| Poison fill | 0 | 6 | 0 |
| HDA register reads | 14 | 18 | 24 |

The GPU driver's synchronous control-queue submission/poll loop is the main
runtime target (~55–57% of these snapshots), with allocator scanning next.
Investigate submission counts, batching and completion waits while preserving
fence/resource lifetime correctness. Poisoning has a much smaller visible
runtime contribution than in the loaded browser. Silent host output still
includes guest audio processing; HDA MMIO reads are a secondary observation,
subject to host VM-exit sampling bias. Do not infer GPU hardware utilization
from these guest instruction snapshots. Recent compositor interval p50/p95
were 34/45 ms (baseline off), 37/45 ms (on), and 35/44 ms (final off), queried
after captures. They are not application frame percentiles; invalid UINT32_MAX
maxima occurred again in some windows and remain PERF-04.

The kernel-only symbol exporter initially mislabeled loaded-driver addresses
as huge offsets beyond `LZ4_slideInputBuffer`. Analysis reconstructed each
module's SHF_ALLOC section layout using the loader's logged base, copied the
unchanged module ELFs into each run, and bounded function addresses by symbol
size. `module-host-summary.json`, SHA manifests, `module-combined.symmap` and
regenerated `*.module.speedscope.json` preserve the corrected GPU/HDA labels.
The first startup profile's marker also scrolled beyond the bridge's 256 KiB
recent-log window; its complete FAT profile was recovered from the preserved
stopped boot disk, rather than counted as missing samples.

Quake baseline: `quake-baseline`, session
`f769ce95fbd24f7489b67c6da8384db8`, kernel `fa1c34f...`; it reached its configured
test timeout after captures. Logging-fix comparison: `quake`, session
`69dca28ce1d546ab906aa8526bbb3a71`, kernel SHA256
`52394eb267c36d083570eb057072dd3de2aab9aa9e452e754786f24629450914`.
Both rendered gameplay successfully; heap checks passed and the logging-fix
run retained poison off. Tests passed: 12 kernel host tests, 15 bridge tests,
one profiler-symbol regression and one bare-metal allocator regression.
Test VMs and the temporary HTTP server were stopped. Licensed game data,
OST files, VM images, logs and captures remain outside git.


## 14 October: bounded GPU investigation and recorded Quake demos

The user extended this investigation to at most one hour and asked for recorded
moving scenes. Runs use one KVM vCPU, Intel `/dev/dri/renderD128`, hardware
virtio/virgl GL3, GPU composition, a 1280x720 game drawable and silent host audio
(`BROOK_AUDIODEV=none`). Writable experiment clones and licensed recordings stay
under ignored `build/experiments/results/gpu-short-20261014/`.

The first unprofiled `+map base1` launch reached the post-refresh relative-mouse
warning in **32.2 seconds**; a subsequent capture confirmed rendered gameplay.
This is a map-ready marker, not instrumented first-pixel latency. Earlier startup
profiles were intrusive and are not a trustworthy before/after loading timer.

Fresh profiles lost the normal startup demo because Brook adds `+exec
brook-controls.cfg`, and Yamagi starts its automatic `d1` loop only without late
commands. The launcher now appends `+d1` only in that first-profile case when the
caller supplied no late command. Explicit map/demo/connect/exec commands and
saved preferences keep their existing behaviour. This was confirmed against the
actual Yamagi 8.60 source and tested through the real wrapper with a fake game.

For deterministic repeats, `brook-benchmark.cfg` alternates the licensed
`demo1.dm2` and `demo2.dm2`, explicitly disconnecting at each boundary so Yamagi
prints separate timedemo results. It contains only original commands and is now
included in fresh experiment seeds. Use `+set timedemo 0 +exec
brook-benchmark.cfg` for ordinary moving-scene profiling. For uncapped timing on
a fresh guest with no Quake process already running:

```sh
python3 tools/profile/q2_demo_benchmark.py SESSION --runs 6 \
  --output build/experiments/results/q2-demo-results.json
```

The runner launches GL3 at 720p, filters zero-frame startup reports, saves the
kernel identity and records results without sampling or screenshots during
timing. FPS uses the guest clock; host elapsed includes asset loading. The game
continues running afterward. Compare equal configurations and per-demo frame
counts, and keep timer calibration and cold/warm runs in mind.

The rejected GPU experiment attached a requested render fence to the final
nonempty command chunk instead of issuing an additional empty fenced submission.
It retained synchronous render completion, ordering and failure propagation.
Its callback signature temporarily required ABI 2. After comparison, both the
original GPU API and ABI 1 were restored and every module rebuilt together.

That rebuild exposed missing custom-command header dependencies: only the GPU
module rebuilt initially, so old NIC/input/audio modules were rejected. Module
compiles now emit/consume compiler depfiles and explicitly depend on the ABI
header. An isolated build test verified ABI edits rebuild both modules, a
transitive private-header edit rebuilds only the affected module, and unchanged
builds remain clean. Every production module was rebuilt and import checks
passed before continuing the VM comparison.


The standalone timedemo rendered 689 frames: reference 24.9 FPS, candidate
25.1 FPS. That difference was too small to establish an improvement. Repeated
benchmarks used the original alternating config (688/617 frames at its explicit
disconnect boundaries), the same rebuilt kernel, identical HUD/display/audio
settings, and no profiler or screenshots during timing:

| Recorded demo | Separate empty fence, FPS (three runs) | Fused fence, FPS (three runs) | Median comparison |
| --- | --- | --- | --- |
| demo1 | 25.5, 24.6, 24.4 | 24.4, 23.1, 23.2 | 24.6 → 23.2 |
| demo2 | 23.7, 24.0, 24.1 | 23.1, 23.4, 22.9 | 24.0 → 23.1 |

Total host elapsed for all six runs including loading was 209.1 seconds for
reference versus 213.1 seconds for fusion. APIC rate adjustments occurred during
the runs, so guest-clock FPS is not a precise wall-clock speedup measure. The
reference reproduced the original two queue submissions within one driver call
and held its guard continuously; the original shipping path releases/reacquires
the guard between callbacks. This isolates fence placement but is not an exact
copy of the old scheduling. These results do not justify retaining fusion: its
patch is archived at `build/experiments/results/gpu-short-20261014/fused-fence-experiment.patch`;
the original rendering path remains in the source and normal build.

Moving-scene profiles were captured separately: 300 QMP CPU snapshots per run
and 500 guest timer samples. GPU SubmitCommand/notification accounted for
156/300 reference snapshots (52%) and 150/300 candidate snapshots (50%). These
captures cover different points in moving playback, are biased by IRQ masking
and VM exits, and include the profiler's own disk writes. They identify the
synchronous queue as continuing work, rather than proving a throughput gain.
Driver ELF copies/load addresses and bounded symbol maps were preserved.
Candidate and reference heap integrity checks passed. No virgl command errors
were reported in the completed comparison runs. The candidate's final capture
fell in a black map transition; its standalone demo capture shows rendered
scenery, and repeated timings completed both recordings.

Validation passed: 12 kernel host tests, 20 bridge/launcher tests, the profiler
symbol regression, shell syntax, module import checks and the isolated module
dependency rebuild experiment. Socket tests require execution outside the
socket-restricted sandbox. The prepared checkpoint's GL/software wrappers were
refreshed as local regular files in `/nix/bin` and `/nix/profile/bin`; future
seeds get generated wrappers from the updated Nix template. Its authored
benchmark config was also staged locally. Backups and licensed assets remain
in ignored build directories.


Final production-path check: kernel SHA256
`024d3cb478cf85e1dbc73341b5aec5ea2fa99d1b5fa69969c88271968d47412c`,
ABI 1, original two-callback GPU submission. Session
`cd738d89cebe471db35eba1132662bfd` launched a fresh profile with only renderer
and early display settings, no explicit map/demo command. Automatic intro and
`Installation` demo playback reached its gameplay marker in 37.0 seconds,
including the intro. One capture confirmed rendered gameplay; a later capture
showed loading at the next demo boundary. Heap
integrity passed and QEMU reported no virgl command errors. The updated normal
checkpoint's boot-driver copies were synchronized with the restored build.

A fresh `nix-build nix/experiment-packages.nix -A profile --no-out-link` succeeded;
its generated GL and software wrappers match the locally staged copies byte
for byte. Final host tests passed after rollback. Experiment VMs were stopped;
normal host launches retain silent audio unless explicitly enabled.
