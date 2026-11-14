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
