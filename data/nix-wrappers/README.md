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

### Run (needs GPU mode)

Boot with `BROOK_GPU=gl BROOK_COMPOSITE=gpu`, then:

    --script yquake2_gl      # GL1 renderer  (data/scripts/yquake2_gl.rc)
    --script yquake2_gl3     # GL3 renderer  (data/scripts/yquake2_gl3.rc)

The native software Quake 2 stays available via `--script quake2`
(`data/scripts/quake2.rc`) for a side-by-side comparison.

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
