# Brook nix-app launch wrappers

Launch wrappers for nix-installed GL apps running on Brook's virgl GL stack.
Each `*-play.sh` injects the Brook virgl environment (force the Mesa `virtio_gpu`
DRI driver + glvnd EGL vendor dir), an SDL Wayland video driver, a dummy SDL
audio backend, and a writable `HOME`, then execs the app. They are staged on the
**nix disk** at `/nix/<name>-play.sh` (the same place `stk-play.sh` /
`gltron-play.sh` live), not on the boot FAT disk.

## yquake2 (Yamagi Quake II) — GL renderer for Quake 2

Quake 2 already runs on Brook via the original **id software renderer**
(`ref_soft`, built by `scripts/build_quake2.sh`). `yquake2` adds the **OpenGL**
renderers (`gl1`, `gl3`, `gles3`) plus its own software renderer, selectable at
runtime via the `vid_renderer` cvar — so software and GL can be compared on the
**same** game data.

`yquake2-play.sh` points `basedir` at `/data/games/quake2`, which is the shared
`baseq2/pak0.pak` the native software port already uses, so both renderers run
identical assets. The first arg selects the renderer (`gl1` default, or
`gl3` / `gles3` / `soft`).

### Install — Option A: offline pre-stage (recommended while BRO-200 is open)

`nix-install yquake2` currently fails at DNS resolution (BRO-200: a recent guest
DNS regression — curl can't resolve `cache.nixos.org` for any *uncached*
package). Until that's fixed, stage the closure directly from the host:

    nix-shell --run ./scripts/prestage_yquake2.sh

This realises `nixpkgs#yquake2` on the host (working DNS), fuse2fs-mounts the nix
disk, copies only the missing closure store paths (~12 paths / ~60 MB; the rest
is shared with the STK/gltron installs), and creates `/nix/profile/bin/yquake2`.
No guest networking needed afterwards.

### Install — Option B: in-guest nix-install (needs working guest DNS)

    set wm
    run /nix/bin/nix-install yquake2          # or: --script yquake2_install

`yquake2` 8.60 is in Brook's package index; the closure (SDL2 + glvnd + the
renderer/game `.so`s) is fetched over TCP. Mesa (`mesa-26.0.6`) is already on the
nix disk from the STK/gltron installs and is reused for the `virtio_gpu` DRI
driver. Blocked by BRO-200 until guest DNS is fixed.

### Run — from the desktop launcher (recommended, matches the normal flow)

Boot your usual desktop (`wm` / `desktop`) with the GPU compositor:

    BROOK_GPU=gl BROOK_GPU_DISPLAY=sdl BROOK_COMPOSITE=gpu ./scripts/run-qemu.sh --release --script desktop

then click **Quake II GL** (gl1) or **Quake II GL3** in the Apps launcher. These
are normal launcher shortcuts (`data/shortcuts/yquake2.rc` / `yquake2_gl3.rc`)
that `source` the Wayland launch scripts (`data/scripts/wayland_yquake2_gl.rc` /
`wayland_yquake2_gl3.rc`) — same two-file convention as VLC/GIMP, including
`set vfb none`. The native software **Quake II** launcher entry stays available
for the side-by-side comparison.

### Run — from a boot script (headless / scripted)

    BROOK_GPU=gl BROOK_GPU_DISPLAY=sdl BROOK_COMPOSITE=gpu ./scripts/run-qemu.sh --release --script wayland_yquake2_gl
    # or wayland_yquake2_gl3 for the shader path

### Notes / status

* The wrapper hardcodes the guest `mesa-26.0.6` store path, matching
  `stk-play.sh` / `gltron-play.sh`. If the guest Mesa is updated, update all
  three wrappers together.
* `yquake2` dlopen()s its renderer `.so` from its own `lib/yquake2/` dir; no extra
  `LD_LIBRARY_PATH` is needed for the renderer itself, only the virgl DRI env.
* Verified so far: `nix-install yquake2` correctly resolves `yquake2 8.60` and
  begins fetching its closure, but the fetch fails at guest DNS resolution
  (BRO-200, a recent regression — any uncached package is affected). The offline
  `scripts/prestage_yquake2.sh` path works around this and has been verified to
  stage the binary + all four renderers (`ref_gl1/gl3/gles3/soft.so`) onto the
  nix disk. A live GL run needs a host with a GPU render node (e.g. Khione).
