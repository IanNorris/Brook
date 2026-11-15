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

## Status, 15 Nov 2026 (branch `work/multilayer`)

**Client-size mismatch fixed** (gen 400, Chrome on YouTube: offers presented, windowed ~45 ms):
- waylandd and the renderer used to refuse a GPU source whenever the WM client size differed from the VFB.
- The renderer now draws an offer that *is* the VFB 1:1, cropped to the client rectangle, exactly as the CPU source path does.

**Wire version 3 layered offers are implemented:**
- **Wire** (`src/shared/gpu_window_channel.h`): `BrookGpuWindowLayersPacket` is the v2 header (describing layer 0) plus up to 8 `BrookGpuWindowLayer` records:
  - buffer identity, size, offset, stride, format;
  - an `OPAQUE` flag;
  - a source rectangle in buffer pixels;
  - a destination rectangle in window VFB pixels.

  `SCM_RIGHTS` carries buffer and fence per layer. SAFE and PRESENTED cover the whole commit. `brook_gpu_window_receive` refuses v3 offers, so an old renderer drops the channel rather than misreading one.
- **Renderer:**
  - `desktop_gpu_channel.h` stores every layer's fds and closes them on SAFE or disconnect.
  - SAFE is withheld while any selected offer shares a buffer with any layer.
  - `desktop_gpu_offer_scene_layer` places layer i at the client origin, clipped to the client rectangle (cropped to the VFB and to the screen).
  - `desktop_renderer.c` expands a layered offer into one scene layer and one external source per layer. An offer counts as used if any of its slots is drawn.
- **waylandd** (`gpu_window_offer_layers`):
  - **Eligible windows:** 2–8 layers with integer source rectangles and no transform.
  - **Buffers:** DMA-BUF layers come from the Intel queue (prime fd plus producer fence). CPU-pixel layers (SHM or earlier snapshots) are copied once into a GEM buffer owned by the image, using an already-signalled fence.
  - **Holding:** an image may be held by several groups and is released only after every holding group's SAFE.
  - **Commit numbers:** every published source uses a per-window `publish_commit` counter: single offers, layered offers and tagged CPU snapshots. A desynchronised subsurface update therefore always publishes a newer source than the tree commit alone would.
  - **Frame callbacks:** all surfaces in the window follow the layered commit's PRESENTED.
  - **Fallbacks to the CPU path:** a DMA-BUF layer's fence pending longer than 50 ms, unsupported geometry, or any failure.
  - **Switches:** `BROOK_WAYLAND_GPU_LAYERS=0` disables it. The status file reports a `layered` block and the `gpu_layers` path count.

**Tests:**
- `gpu_window_channel_test`: v3 wire validation, fd ownership, old-receive refusal.
- `desktop_gpu_channel_test`: selection, placement and clipping, and that SAFE closes every fd.
- `capture-release-test`:
  - DMA-BUF root plus DMA-BUF subsurface: layered offer with no readback; group holding across the older single offer's SAFE.
  - SHM root plus DMA-BUF subsurface: GEM upload, re-offer, release.
- **Renderer e2e on a real Intel GPU,** compared pixel-exact against the CPU reference (`HOST_E2E_DIRECT=1 HOST_E2E_LAYERS=1 brook-desktop-renderer-host-test`): each offer carries a second, 2×-scaled opaque layer, and it passes in full, partial, async-partial and pipelined modes.

**Not done:**
- Fractional source rectangles and buffer transforms stay on the CPU path.
- Damage is still ignored.
- Scaling a layer uses the quad shader's nearest sampling, which matches the CPU reference in the e2e test for integer ratios. Fractional ratios aren't proved bit-exact.

## Chromium's window frame and the opaque region (gens 403–406, 15 Nov 2026)

**Why Chrome is never opaque:**
- Chromium's default *custom frame* is translucent (shadows, rounded corners).
- It sends `set_opaque_region` with its initial window rectangle (0,0 1050×880, surface-local logical units) and then **clears it with NULL**.
- Its AR24 window is therefore never opaque, and the renderer blends the whole desktop beneath the browser every frame. Gen 404 status: `opaque_regions.last_kind=0`, `opaque_views=0`.

**What did not help:** answering Chromium's `CLIENT_SIDE` xdg-decoration request with `SERVER_SIDE` (`810b1997`, gen 405). Chromium keeps its custom frame inside Brook's title bar: a double frame with a transparent shadow margin, and the opaque region is still cleared. The override is opt-in only (`BROOK_WAYLAND_SSD_APPS`, unset by default; `49b15263`, gen 406). By default waylandd honours the client's request, so there is no double frame.

**What works:** Chromium's own setting *Settings → Appearance → "Use system title bar and borders"* (pref `browser.custom_chrome_frame=false`). A private profile on gen 405 then showed:
- `last_kind=1`;
- the window's `opaque_region` set to the whole 800×600 surface;
- view-opaque layers;
- 52 opaque one-layer v3 offers;
- a single clean frame (Brook title bar over Chrome's tab strip).

Captures: `display-perf/build/ssd-probe-frame/` (double frame) and `ssd-pref-frame/` (system title bar).

**Recommendation:** use the system title bar for Chromium on Brook. A proposed launcher default exists on `work/chromium-frame` (`14287f38`), **not installed**: `brook-chromium` would set the pref only when it is absent from the profile, keeping any later user choice. For now Ian can flip the setting himself; the wrapper default is his decision.

**How opacity is decided (`surface_opaque_covers`):** a single added rectangle, clamped to the surface, that covers the whole surface marks the view opaque. The comparison is in logical surface units, against the view's destination size before output scale. Multi-rectangle regions stay non-opaque (conservative).

**Diagnostics in the status file:**
- `opaque_regions`: counts of NULL / infinite / by-number-of-ops calls, the last call's kind and first op, and `opaque_views`;
- per window: the root's `opaque_region` `[set,x,y,w,h]`;
- per layer: `opaque` bit 0 = XRGB pixels, bit 1 = view marked opaque.
