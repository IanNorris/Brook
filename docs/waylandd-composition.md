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

**Superseded for the look (Ian, 15 Nov):** the system title bar puts Brook's blue bar above Chromium's light tab strip, which looks wrong. Chromium's own frame (the default) is kept; see the next section.

**How opacity is decided (`surface_opaque_covers`):** a single added rectangle, clamped to the surface, that covers the whole surface marks the view opaque. The comparison is in logical surface units, against the view's destination size before output scale. Multi-rectangle regions stay non-opaque (conservative).

**Diagnostics in the status file:**
- `opaque_regions`: counts of NULL / infinite / by-number-of-ops calls, the last call's kind and first op, and `opaque_views`;
- per window: the root's `opaque_region` `[set,x,y,w,h]`;
- per layer: `opaque` bit 0 = XRGB pixels, bit 1 = view marked opaque.
- `surfaces` (one entry per toplevel, Xwayland and subsurface, at most 32): `client_pid`; `frames` pending (requested, not committed), waiting (committed, tree not yet applied), committed (eligible), `oldest_ms` and `state` — why committed callbacks are not sent: `due`, `image_pending` (GPU readback/fence), `presented` (offer awaiting PRESENTED), `group`, `layers`, `no_window`, `unapplied_commit`; `configure` newest serial sent vs the client's newest ack, their ages, and the pending size transaction; `pending_buffer` size; commit ages; tree sequence; and the current and latest images (size, view, pending_cpu, offer_state, gpu_presented, client buffer size, `buffer_uses` > 0 = not yet released, `buffer_alive`).

## A game in front of an idle window (`work/occlusion`, 16 Nov 2026)

Dell gen 419: yquake2 GL3 (a 640×480 drawable shown 2× in a 1280×960 window) in front of an idle Qalculate! window. The compositor job rose from 9.1 to 15.9 ms and partial presents averaged 2.57 Mpx, about twice the game's area. Two defects, both found from code and host tests, not yet measured on the Dell:

- **The scaled game was never a direct GPU source.** A single-buffer offer required the buffer to be the VFB pixel for pixel (`buffer = logical size × output scale`). A client that is not scale-aware (SDL's default, a 640×480 buffer under a 640×480 logical surface at 200%) failed that check (`offer_skipped.geometry`). waylandd then composed the buffer into the VFB on every frame, and the renderer received a CPU window layer. The renderer never marks CPU layers opaque, so it blended the wallpaper and every window beneath the game. Now any whole-pixel source rectangle without a transform is offered as a one-layer version 3 commit carrying source and destination rectangles. The opaque flag is set for XRGB or a covering opaque region. Status: `layered.scaled_single`. Opt out with `BROOK_WAYLAND_GPU_SCALED=0`.
- **Carried damage never shrank.** With two frames in flight, the kernel requires each frame to present the regions of the frame it overlaps. Those regions already contain what that frame carried. While the game keeps the pipeline full, one large change stays in every later frame. For example, the restack when the game window opened damages both windows, and bounding-box merging turns that into the union of both. The renderer now waits once behind a frame that presents more than `BROOK_RENDER_CARRY_SLACK` beyond its own damage (default 1/64 of the output). The next frame then starts from the screen. Stats: `pipeline.waits_carry`. `BROOK_RENDER_CARRY_SLACK=off` restores the old behaviour.

`xdg_toplevel.set_fullscreen` is still a no-op, so the game is composited as an ordinary window with chrome.

## Chromium's tab strip in the title area, with Brook's caption buttons (`work/chrome-csd`, 15 Nov 2026)

**Goal (Ian):** Chromium draws its tabs at the top of the window, as on Linux desktops, with Brook's own minimise/maximise/close at the right instead of a Brook title bar above the tab strip.

**What Chromium does:**
- **No `zxdg_decoration_manager_v1`, or the compositor answers `CLIENT_SIDE`:** Chromium draws its custom frame: tab strip at the top, its own caption buttons, rounded corners and a translucent shadow margin. It reports the visible frame with `xdg_surface.set_window_geometry` (on Brook: 16 px right, 10 px top inset in QEMU while restored, none while maximised). This was already Brook's default.
- **`SERVER_SIDE`:** Chromium keeps drawing its custom frame inside Brook's frame (gen 405 double frame). Only the *Use system title bar and borders* preference drops it.
- **Its caption buttons** follow GTK's `gtk-decoration-layout`. The pinned Chromium 148.0.7778.96 reads this property through `SettingsProviderGtk`. GTK on Wayland reads GNOME's `org.gnome.desktop.wm.preferences button-layout` from the Settings portal when GTK uses it and otherwise uses GSettings. [Chromium settings provider](https://raw.githubusercontent.com/chromium/chromium/148.0.7778.96/ui/gtk/settings_provider_gtk.cc) [GTK 3 Wayland settings](https://raw.githubusercontent.com/GNOME/gtk/3.24.52/gdk/wayland/gdkscreen-wayland.c) [GTK 4 Wayland settings](https://raw.githubusercontent.com/GNOME/gtk/4.20.3/gdk/wayland/gdksettings-wayland.c)
- **Chromium's own buttons** call `xdg_toplevel.set_minimized`/`set_maximized`/`unset_maximized` and close itself; waylandd already forwards these to the WM. Empty tab-strip space calls `xdg_toplevel.move` (and a double-click toggles maximise).

**What Brook does now:**
- **Kernel WM:** a client-decorated window can carry a *caption overlay* (`Window::captionOverlay` plus right/top insets in VFB pixels). The WM draws only its caption-button block, in the title material, at the top-right of the client's window geometry (`DesktopCaptionOverlayRect`, `desktop_metrics.h`), and hit-tests the three buttons there; the rest of the surface stays client area. The block uses the normal caption code (hover, close red, restore glyph when maximised). The CSD "drag the top 36 px" heuristic is skipped for these windows so tab drags reach Chromium.
  - Paths: CPU compositor (drawn after the client), the GPU chrome layer (`SetChromeDecorAlpha`/`GpuComposeChrome`), and the desktop renderer's chrome capture, which returns a surface covering just the block. The renderer draws chrome after the client; `desktop_track_chrome_hole` now treats a chrome surface the client fully covers as an overlay with no hole.
  - The chrome key includes the overlay and its insets.
- **Syscall 512** `WM_SET_DECORATION_MODE(wmId, mode, insets, height)`: mode 2 is CSD with the overlay; `insets` packs right | top << 16; `height` is the client's title-strip height in VFB pixels (0 uses Brook's title height). It returns 2 when the overlay is in effect. Older kernels return 0 and treat mode 2 as plain CSD, and waylandd then stops asking.
- **waylandd:** when an xdg-decoration client asks for `CLIENT_SIDE` and its app_id starts with an entry in `BROOK_WAYLAND_CAPTION_APPS` (default `chromium:40,google-chrome:40`; empty disables it), it uses mode 2. The optional `:height` is the client's title-strip height in surface units; waylandd multiplies it by the output scale. Chromium's strip is 40 restored and maximised: 40 px in QEMU, and 80 px on the Dell at scale 2. The block therefore spans the whole strip. It also re-sends the insets when the window geometry or size changes (shadow added or dropped on restore and maximise). Popups, GTK/KDE-decorated clients and other apps are unchanged.
- **Desktop defaults** configure `icon:minimize,maximize,close` in the profile's GTK 3 and GTK 4 settings. The GNOME schema uses `menu:minimize,maximize,close` because GDK translates `menu` to `icon`. Normal GTK and GSettings overrides keep their precedence. Chromium retains access to existing GLib keyfile settings and creates no layout preference in the user file. The experimental `BROOK_CHROMIUM_BUTTON_LAYOUT` launcher variable has been replaced by normal settings, where `:` still hides the caption buttons. [GDK button translation](https://raw.githubusercontent.com/GNOME/gtk/3.24.52/gdk/wayland/wm-button-layout-translation.c)

**Results (QEMU, software Chromium 148, private profile under `/tmp`):**
- **Layout `:`** (Chromium draws no buttons): tab strip at the top, Brook's block at the top-right, insets 16,10 restored and 0 maximised. With 15 tabs, though, the tabs and the new-tab button run under the block, because Chromium reserves no space.
- **Layout `:minimize,maximize,close`** (the legacy launcher test): the same look, but Chromium keeps its tabs and new-tab button left of the block. Its own buttons (about 100 px wide) sit entirely under Brook's 138 px block; a transient frame during maximise, before Chromium redrew, showed them in the old buffer.
- **Behaviour:**
  - Dragging empty tab-strip space moves the window (Chromium's `xdg_toplevel.move`).
  - A double-click there maximises (Chromium's `set_maximized`, honoured).
  - Brook's maximise/restore, minimise (with restore from the taskbar) and close all work, and hover shows the normal caption colours (close red).
  - Popups and tooltips get no block, and a native terminal keeps its normal title bar.
- **Speed:** QEMU on a heavily loaded host was very slow (Chromium's first window took about 30 minutes; WM clicks were sometimes delayed by seconds), which says nothing about Dell speed.
- **Evidence:** `chrome-csd/build/csd-qemu/evidence/` (screenshots 01–15, serial logs).

**Not done / limits:**
- Superseded on `work/chrome-csd-style`: the block now takes Chromium's strip colour (next section).
- The strip height is a per-app constant from waylandd's list. If a future Chromium changes its tab-strip height, the block will again be shorter or taller than the strip; edit `BROOK_WAYLAND_CAPTION_APPS`.
- Pre-existing: restoring a client-decorated window configures the VFB size, which includes the shadow margin, so Chromium grows by its shadow (800×600 to 832×642 geometry) on each maximise/restore.
- The Dell desktop renderer path (chrome capture plus no-hole rule) is covered by host/package tests, not yet exercised on hardware.

### Staging the launcher on the Dell (approved by Ian, 16 Nov; not yet run)

The pattern is the same as the GPU browser launchers (`p0mwsladz…`) and `brook-vaapi-0.2`:
- a create-only, hash-verified store install;
- an atomic `/nix/profile` symlink swap;
- a rollback record.

Tools: `tools/launcher-stage/make_bundle.py` (host) and `install.py` (guest, root).
- `install.py --check` only reports. A real run refuses if the new path exists or any closure path is missing.
- It swaps only `/nix/profile/bin/brook-chromium`.
- The new store path also carries main's other launcher changes (`brook-firefox`, `brook-run`), but those links are not swapped.

```sh
# Host: build the launcher path and the bundle (from this branch).
out=$(nix-build nix/hardware-network-packages.nix -A launchers --no-out-link)
python3 tools/launcher-stage/make_bundle.py build/chrome-launcher-stage \
  --path "$out" --link /nix/profile/bin/brook-chromium="$out/bin/brook-chromium"
```

Guest steps go through the bridge, as root:
1. `hw_transfer` (action `put`) of `bundle.tar`. Note the returned `/data/.brook-transfer/<id>.bin`.
2. `hw_guest_run` with `uid` 0:
   `/nix/profile/bin/bash -c 'set -e; d=/data/.brook-launcher-stage-TX; mkdir -m 700 $d; cd $d; tar -xf /data/.brook-transfer/<id>.bin; /nix/profile/bin/python3 install.py store.tar manifest.json closure.txt links.json TX --check'`
3. If it prints `CHECK_OK`, rerun without `--check` and expect `LAUNCHER_STAGE_OK`.
4. **Rollback:** point `/nix/profile/bin/brook-chromium` back to the `previous` target in `/nix/.brook-profile-rollback-TX.json`.

### Caption style matched to the client's strip (`work/chrome-csd-style`, 16 Nov 2026)

**Ian on gen 417:** "grey on the left then our buttons rendered on top as a blue block … we should try to match the style."

**How other platforms do it:**
- **Windows:** Chromium draws Windows-style caption buttons itself on its custom frame. With Window Controls Overlay (installed web apps only), the OS draws its own buttons over the app's title area, in colours the app supplies (`theme_color`, or a CSS-reported title-bar colour).
- **macOS:** the OS draws the traffic lights over a transparent, full-size-content title bar on whatever the app paints there, and the app leaves room for them.
- **GNOME/KDE:** client-side apps draw their own buttons in the toolkit theme. Chromium draws GTK-styled buttons in the layout GTK reports.

**Brook's choice is the macOS/WCO model,** with the colour taken from the client's pixels instead of a protocol:
- waylandd samples the strip just inside its right edge (3 px in, mid-height) from the committed buffer, plus the alpha along the strip's top row, which gives the frame's rounded-corner radius.
  - **SHM** buffers are read directly.
  - **Intel GPU** buffers are read only after `TRY_CPU_READ` reports the producer idle with caches synchronised. It never sleeps: a busy newest frame is retried from a 20 ms timer (up to 25 times), reading the previous frame meanwhile.
  - **When:** on map, resize, maximise and focus change, otherwise at most every 500 ms.
  - **Packing:** syscall 512's 5th argument carries the result: RGB, a valid bit, and the radius in bits 25–31.
- **Kernel drawing:**
  - It fills the block with the strip colour, which hides Chromium's own buttons underneath. Those buttons stay requested (`:minimize,maximize,close`), so Chromium keeps its tabs clear of the corner.
  - Glyphs are dark (`#1f1f1f`) on light strips and light (`#e8eaed`) on dark ones. They are softened when the window is unfocused.
  - Hover is a 10% ink tint; close hover stays red.
  - Restored windows clip the block to the frame's rounded top-right corner; the corner pixels are transparent in the renderer's chrome surface too.
  - The chrome key includes the style, so a colour change (theme, incognito, focus) recaptures.
- **Until a sample exists,** or for apps with no `:height`, the block keeps Brook's title colours.

**Fallback (Chromium draws everything):** take the app out of `BROOK_WAYLAND_CAPTION_APPS`, or set it empty for all apps. waylandd then answers plain client-side, and Chromium shows its own three buttons (with the launcher's layout).

## GTK4 window actions and xdg-shell v5 (20 November 2026)

GTK 4.20.3 constructs titlebar controls from the layout, modality, transience
and resizability. Libadwaita 1.8.5.1 uses those GtkWindowControls in
`src/adw-header-bar.c` around lines 293 and 331. Once a Wayland surface exists,
`update_window_actions` requires the GDK capabilities for Minimise and
Maximise. The action gate has no tiled-state condition. A visible Maximise
button already satisfies the resizable check. GDK populates the two flags
from `xdg_toplevel.wm_capabilities`, which Brook's v3 global could never
send. [GTK controls](https://raw.githubusercontent.com/GNOME/gtk/4.20.3/gtk/gtkwindowcontrols.c)
[GTK actions](https://raw.githubusercontent.com/GNOME/gtk/4.20.3/gtk/gtkwindow.c)
[GDK Wayland capabilities](https://raw.githubusercontent.com/GNOME/gtk/4.20.3/gdk/wayland/gdktoplevel-wayland.c)

Brook advertises v5. Each toplevel gets the implemented Maximise, Minimise
and Fullscreen capabilities before its first configure. The window menu
request remains a stub. v4 and later get advisory bounds in logical output
coordinates when known and updates when the reported dimensions change.
The provider stays at v5 until the WM can report repaint suspension for v6.

The wire fixture checks v3, v4 and v5, exact capability values and their
order before the configure. Pinned GTK4 enabled both controls against the
new provider on a private Wayland socket. GTK3 and Chromium both received
and acknowledged the initial configure. They hit the same retained-scene
failure after buffer attach in the v3 and v5 host probes because the host
lacks Brook's window syscalls. Their pinned listeners accept both new
events. Full rendering and button actions on the Dell remain unverified.
[GTK3 Wayland listener](https://raw.githubusercontent.com/GNOME/gtk/3.24.52/gdk/wayland/gdkwindow-wayland.c)
[Chromium Wayland listener](https://raw.githubusercontent.com/chromium/chromium/148.0.7778.96/ui/ozone/platform/wayland/host/xdg_toplevel.cc)
