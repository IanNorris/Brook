# waylandd window composition: CPU cost and the multi-layer GPU path

_14 Nov 2026, branch `work/waylandd-compose`._

## What the gen 371 profile showed

- **Workload:** Chromium and Firefox YouTube playback (both render in software).
- **CPU:** the Wayland server took about one full core, 24% of all samples on 4 CPUs, entirely in user code.
- **Hot spot:** the per-pixel loop in `surface_render` (`surface-scene.h`): two 64-bit divisions for nearest sampling (`brook_geometry_pixel`) plus a `/255` per channel in `pixel_over`, then a straight-alpha pass with a divide per translucent pixel.
- **Frame rates:** about 170 ms per frame windowed and about 2 fps fullscreen (3200×1800).
- **Evidence:** `display-perf/build/{chrome-audio-gen371,firefox-fs-gen371}/`.

**Why the CPU path ran at all: a second server stole the display.**
- The desktop launcher runs `run --service --once waylandd` before each app (`src/kernel/src/window.cpp`). `--once` matches only the process basename, so it doesn't see the boot provider `INTLWL.ELF` (waylandd built with the Vulkan compositor and started with `--gpu-source`).
- `/nix/profile/bin/waylandd` therefore started, then unlinked and rebound `/tmp/wayland-0`. Every app launched afterwards connected to it, so their windows were CPU-composed with no GPU source channel.
- The profiled process was this second server (`/proc/309/exe` → `4p01…-waylandd-brook`).

## Done on this branch

1. **`8eb09d51`: waylandd no longer takes over a live display.** It probes `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY` and `/tmp/$WAYLAND_DISPLAY` and exits 0 when a server accepts connections. A stale socket file refuses the connection, so a dead provider is still replaced.
   - Host check: the second instance logs `already has a live server; exiting` and the first keeps serving.
   - Not yet checked on the Dell.
2. **`83dbd7e8`: bit-exact fast CPU composition (`surface-blit.h`).**
   - Sampling offsets are computed per column and per row, since `brook_geometry_pixel` is separable for all 8 transforms.
   - Blending uses an exact shift form of `(t+127)/255`, two channels per word.
   - Opaque XRGB SHM sources, and the first layer over the cleared composite, are copied rather than blended. Unscaled identity draws copy whole rows.
   - Straight-alpha conversion uses an exact reciprocal.
   - Correctness: `surface-blit-test.c` proves the arithmetic exhaustively and compares 4,000 random multi-layer scenes (all transforms, fractional source rectangles, clipping) against the original loop. It runs in the nix `checkPhase`.
   - Host benchmark (`surface-blit-test --bench`, per frame):

| Scene | Old | New | Speedup |
|---|---|---|---|
| 1600×900 ARGB page at 2× plus a 1280×720 video at 2560×1440, worst-case alpha | 249–291 ms | 36–40 ms | 7.0–8.2× |
| Same scene, typical page (≈2% translucent pixels) | 203–206 ms | 26–28 ms | 7.3–7.9× |
| Unscaled opaque 1600×900 | 30–34 ms | 3.7–4.6 ms | 7.3–8.6× |

On the Dell's Broadwell the absolute times are higher.

## Structural design: publish windows as layers, compose on the GPU (pending)

**Current state:**
- The GPU window channel (`src/shared/gpu_window_channel.h`, wire v1/v2) offers one DMA-BUF per window, which is the whole window.
- `surface_render` (`surface-scene.h`) offers it only when it is the sole, unchanged root layer.
- Everything else is flattened on the CPU into the window's vfb:
  - subsurfaces (the YouTube video plus the page);
  - viewports and scaling;
  - all SHM.
- The opt-in `surface_vulkan_compose` (`BROOK_WAYLAND_VULKAN_COMPOSE=1`) composes on the GPU but reads the result back into CPU memory and still runs the straight-alpha pass.

**Proposal: wire version 3, `OFFER_LAYERS`.**
- **Packet:**
  - one SEQPACKET per window commit;
  - the existing window identity;
  - `count ≤ 8` layer records, each with:
    - a buffer index;
    - a source rectangle in 24.8;
    - a destination rectangle in window pixels, after output scale;
    - the transform, plus `opaque` and `straight_alpha` flags;
  - `SCM_RIGHTS` carrying the layer buffers plus one fence (merged sync file).
- **Commits:** the renderer treats the set as one commit. PRESENTED and SAFE apply to the whole commit, so v2 retain semantics carry over unchanged, and every layer is released together.
- **Renderer (`tools/vulkan-compositor-pkg`):**
  - A layered window becomes N quads in the existing quad pass, using the M1 occlusion planner (`quad_plan.h`). Opaque layers occlude whatever lies beneath them.
  - Sampling stays nearest, to match the CPU reference pixels.
  - Import goes through the existing DMA-BUF import cache, one entry per buffer.
- **SHM:** waylandd copies each new SHM commit once into a linear GEM buffer it owns (`DRM_IOCTL_I915_GEM_CREATE` plus mmap; damage rectangles can bound the copy later) and offers that. Each commit's cost becomes one memcpy of the changed buffer, not a per-frame scale and blend of the whole window.
- **Fallback:** waylandd keeps the CPU path in these cases:
  - more than 8 layers;
  - transforms the shader doesn't take;
  - formats other than ARGB/XRGB8888;
  - a lost channel.
  Uncertainty handling is unchanged: no SAFE means the buffer is not released.
- **Kernel:** the window identity check against the OS desktop capture is per window and doesn't change. The kernel never maps the layers; only the renderer and waylandd touch them.

**Why it isn't implemented here yet:**
- It spans the shared channel ABI, waylandd, the renderer's window and quad planning, and the import cache, and needs a coordinated Dell test.
- The zero-copy scanout work is changing the renderer's present path at the same time (`work/zero-copy`), so the renderer half should land after that to avoid conflicts.
- Once apps reach the provider again, the CPU fix and socket fix above remove most of today's cost: single-buffer GPU apps (Quake, vkcube) already take the GPU path there.

## Damage

`wl_surface.damage` and `damage_buffer` are accepted but ignored (`waylandd.c`), so each repaint recomposes the whole window. Tracking damage through the surface tree, and recomposing and copying only the union, is the next CPU-path step after the layer offer.

## Deploying to the Dell (not executed)

There are two binaries:

| Binary | Where it comes from | What it gets from this branch |
|---|---|---|
| **`INTLWL.ELF`** (the boot provider, which owns `wayland-0`) | built from `tools/waylandd-pkg --arg enableVulkanCompositor true` and embedded in the boot pair | the fast CPU path and the socket guard |
| **`/nix/profile/bin/waylandd`** (started by the launcher) | the hardware profile package (currently `4p01fkxz…`) | **the socket guard, which needs this binary** |

Until the profile binary is updated, the launcher's second server keeps stealing the socket.

1. **Boot pair.** `build/dell/pair-waylandd-compose`, then `scripts/dell_deploy.py --session S --pair build/dell/pair-waylandd-compose --output build/dell/deploy-waylandd-compose`, with `BROOK_ROOT` set to the registry worktree. This alone fixes apps that connect before any launcher start.
2. **Profile waylandd** (a persistent `/nix` change, so it needs Ian's authorization).
   - New path: `nix-build tools/waylandd-pkg` (plain variant) gives `/nix/store/7gaxddpisyk93yxjs0wk3wp9jvv6mvh2-waylandd-brook-0.1-brook` (236 KiB: `bin/waylandd`, `bin/brook-xwm`). Rebuild and use the printed path if the branch changes.
   - Its runtime closure is identical to the installed `4p01fkxz…` apart from itself (`nix-store -qR` diff), so nothing else is needed.
   1. Read-only check (root job): `readlink /nix/profile/bin/waylandd /nix/profile/bin/brook-xwm` should point into `4p01…`, `/nix/profile/bin` should be a real directory (profiles are materialized; see `scripts/prepare_hardware_store.py`), and there should be no `/nix/store/7gax…`.
   2. Upload a tar of the store path with `BulkRun put`. Extract it to `/nix/store/.brook-waylandd-stage/`, check the file hashes against the host, `chmod -R a-w`, and `mv` it to `/nix/store/7gax…`, as `stage-openal.sh canonical` does.
   3. Atomically repoint both links:

      ```
      ln -s /nix/store/7gax…/bin/waylandd /nix/profile/bin/.waylandd.new && mv -T /nix/profile/bin/.waylandd.new /nix/profile/bin/waylandd
      ```

      Do the same for `brook-xwm`. `sync`.
   4. **Verify:**
      - Checked restart.
      - Launch Chromium from the launcher.
      - `procs` should show no `waylandd` besides `INTLWL.ELF`, and the launcher log should show `already has a live server; exiting`.
      - The Chromium window should be served by INTLWL: its process takes the CPU in a profile.
      - Repeat the gen 371 capture (`capture.py`) and compare the waylandd share and app stats ms.
   5. **Rollback:** repoint both links to `4p01…`, which is left in place.
3. **Pair-only alternative, if the store change is deferred.** Make the kernel launcher's `run --once waylandd` also count a running `INTLWL.ELF` (`src/kernel/src/window.cpp` around `ShellExecString("…run --service --once waylandd…")`). Not done on this branch.
