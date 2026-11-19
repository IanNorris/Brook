# Compositor refactor: describe Clearwater chrome once

15 November 2026. Status: accepted by Ian, with his answers recorded in
[Decisions (Ian)](#decisions-ian-15-november). Stages 0.1–0.5 are
implemented on `work/compositor-refactor`; stages 1.1 and 1.2 on
`work/clearwater-s1` (see [Stage 1.1/1.2 evidence](#stage-11-and-12-evidence));
stages 1.3 and 1.4 on `work/clearwater-s1b` (see [Stage 1.3/1.4
evidence](#stage-13-and-14-evidence)). It covers the [Clearwater Next design](DESIGN.md)
and the [compositor direction](../../compositor-direction.md).

Line numbers refer to `main` at `0ae38b0d`, before stage 0.1.

**Goal:** implement the Clearwater chrome (window frames, taskbar, tray, popups,
menus, toasts, cursor and theme) once. Every compositor backend consumes that
one description, and nothing adds per-frame cost on the Dell.

## Architecture decisions

1. **One description, three kinds of consumer.** The chrome is described as
   - theme tokens;
   - a layout with a part table, which is also the hit table;
   - a per-surface display list.

   Painting, hit-testing and every backend read the same data.
2. **Rasterise 2D content once, on the CPU, into cached surface textures.**
   Only composition effects are executed per backend: material/opacity,
   backdrop blur, shadow nine-slices and rounded clips.
   - Today's model is already the right one for cost: VKRENDER composites
     kernel-rasterised textures, and the kernel skips unchanged ones by
     content key.
   - Drawing glyphs on the GPU (DESIGN.md milestone T1) is deferred until a
     measurement shows the raster or copy matters.
3. **Material is resolved before any backend sees it.** Glass needs a
   `backdrop-blur` backend capability.
   - Without it, the theme resolves to the opaque stand-in (Opaque Light or
     Opaque Dark).
   - So no backend emulates glass, and the CPU fallback never needs blur.
   - "Glass locked when not accelerated" is one boolean that the active
     backend reports.
4. **Location.**
   - The layout, part/hit table and window-frame display list stay with the
     kernel WM for now. Hit-testing must be synchronous and authoritative
     (DESIGN.md, "Interface between window manager and theme").
   - The builder and rasteriser are written as freestanding code (no kernel
     dependencies). The same sources build into the kernel, host tests and,
     later, a user-mode shell.
   - Shell surfaces (taskbar, launcher, toasts, HUD) move to a restartable
     user-mode shell process at DESIGN.md T3. Nothing in stages 0–2 blocks
     that move, and nothing forces it early.
5. **No new per-frame work on the Dell, and less than today:**
   - per-surface content keys and damage;
   - no CPU raster of windows and chrome while VKRENDER holds the lease;
   - opaque shell surfaces occlude what is beneath them;
   - blur computed only when the pixels beneath a glass surface change.

   See [Performance](#performance-on-the-dell).

## How chrome is produced today

### Three compositor paths

| Path | Where it runs | Composition | Chrome input |
| --- | --- | --- | --- |
| **CPU** | QEMU default (`gpu=software`). Dell fallback whenever the renderer has no lease (500 ms expiry). | Kernel thread writes the back buffer and scans it out. | Painters draw straight into the back buffer. |
| **virgl** | QEMU `gpu=gl` only. `virtio_gpu_mod.cpp` is the only module that registers `GpuCompositorOps`. | Kernel records textured quads, blur barriers and shadow quads. | Full-screen chrome layer (`g_chromeBuffer`) and overlay layer (`g_overlayBuffer`), plus alpha post-passes. |
| **VKRENDER** | Dell (Broadwell, hasvk). User process with a present lease. | Vulkan quad pass with exact shader blend. It draws no primitives of its own. | Syscalls `0xB19` and `0xB1A`. Per-window chrome is captured as ARGB, a 3-band overlay snapshot is captured, and the cursor is a 64×64 image. |

Glass is active only on virgl. `CompositorGlassActive()` (compositor.cpp:896)
requires the kernel `GpuCompositorOps` with `BlurSupported`. That has two
consequences:

- **The Dell never shows glass today.**
- **If glass were active, VKRENDER would yield.** Chrome capture returns
  `-95` (window.cpp:3781), overlay capture is suppressed (compositor.cpp:1953,
  :2008), and any window with `blurRadius` makes the layer export fail.

### Per element

Window-frame and shell code lives in `window.cpp` unless a file is named.

| Element | Layout decided by | Rasterised by | Duplicated in | Theme input |
| --- | --- | --- | --- | --- |
| Title bar, border, title text, icon, accent stroke | `RenderWindowChrome` (window.cpp:1711), inline arithmetic on `WM_*` constants | Same function, CPU, `WmFillRect`/`WmRenderGlyph` | The alpha post-pass is written twice: `SetChromeDecorAlpha` (compositor.cpp:1720, virgl; it recovers glyph alpha by colour-keying `rgb==fg‖0x68acff`) and the capture loop (window.cpp:3810, VKRENDER). Rounded corners come from the virgl post-pass only. | Palette `titleActive/Inactive`, `text`, `mutedText`, `borderActive`. Accent `0x0068ACFF` is a literal. Material rows/rims come from `ChromeMaterialCache` (chrome_material.h). |
| Caption buttons | `DesktopCaptionRect` (desktop_metrics.h), shared by painter and `WmHitTest` | `RenderWindowChrome`, hand-drawn glyphs | Settings preview draws its own "− □ ×" (settings.c:49) | `closeHover`, `captionHover` |
| Title/resize hit zones | `WmHitTest` (window.cpp:1014), separate from painting except caption cells | n/a | Title height comes from `WM_TITLE_BAR_HEIGHT` and again from `DesktopMetrics::title()`; the same value appears in two headers | n/a |
| Shadows, title/panel blur | Virgl compose loop: `GpuComposeShadow`, `GpuComposeTitleBlur`, `GpuComposeRectBlur` (compositor.cpp:3440–3530, 3790) | Shadow: cached 64 px nine-slice (chrome_material.h). Blur: virtio driver. | Virgl only. The CPU path and VKRENDER have no equivalent. | `CompositorMaterialBlur()`, opacity |
| Taskbar, task buttons, tray, clock, metrics | `GetTaskbarLayout` (window.cpp:1850), shared by render, hit-test and tooltip | `WmRenderTaskbar` plus `desktop_telemetry_runtime.inc` | Surface enumerated in 5 places (see below). Accent `0x68acff` literals at :2313, :2318, :2353 and telemetry :73, :194. | Palette `task*`, `separator`, `minimized` |
| Launcher | `GetLauncherLayout` (window.cpp:3071), shared with hit-test | `WmLauncherRender` | Enumerated in 5 places | `launcher`, `launcherBorder`, `itemHover`. Text halo `0x071526` is a literal. |
| Volume popup, tray tooltip | `taskbar_volume.h` (pure, host-tested), `WmTrayTooltip` | `WmVolumePopupRender`, `WmTrayTooltipRender` | Enumerated in 5 places | Palette plus literal accent |
| Boot summary, build badge | desktop_telemetry_runtime.inc | `WmRenderBootSummary`. The badge is drawn twice: `WmDrawBuildId` and `WmDrawBuildIdSpan` (span version for backdrop capture). | Separate handling in each path | Literals `0xa4b2c7` / `0x172335` |
| Menus | None. There are no WM menus. Wayland popups are client surfaces with `noChrome`. | n/a | n/a | n/a |
| Cursor | `cursor_composition.h` (pure, host-tested) | CPU sprite plus busy ring | CPU blit with save-under; virgl texture; VKRENDER op 8. A fallback CPU blend inside the overlay capture (compositor.cpp:2026). No hardware cursor anywhere. | Busy ring uses `borderActive`; `0xff53677b` is a literal |
| Kernel Console, terminal, TTY | debug_overlay.cpp, terminal.cpp, tty.cpp: client content inside normal WM chrome | Their own painters | Re-derive the frame offset from `WM_BORDER_WIDTH`/`WM_TITLE_BAR_HEIGHT` (debug_overlay.cpp:278, shell.cpp:629, :1115, syscall_ioctl.cpp:379) | Own literals. Background is `0x001A1A2E` vs `0x00001A2A`, which is inconsistent. |
| Server-side decorations | Kernel WM. waylandd draws no chrome; it sets `noChrome` (syscall 512, `BROOK_WAYLAND_SSD_APPS` override). Xwayland follows `_MOTIF_WM_HINTS`. | Kernel | None | n/a |

**Theme flow:**

- `settings.ini [Desktop]` (`theme`, `scale`, `dark`, `performance`, `opacity`,
  `blur`, `taskbar_metrics`) and fw_cfg `opt/glass` feed `WmRestoreTheme`
  (window.cpp:149).
- From there they reach the atomics `g_themeId`, `g_appearance` and
  `g_desktopMetrics`.
- Settings changes them through syscalls `0xB02`, `0xB04`, `0xB0B` and
  `0xB0C`. Settings reads the same `brook_theme.h` table for its preview.
- App surfaces use a second palette (`brook_surface_palette` in
  `brook_appearance.h`). It is pushed to GTK as CSS and also copied as
  literals in brook-files.c and gallery.c.
- Painters call `WmCurrentPalette()` per draw. The chrome cache key folds in
  `theme | appearance | scale | settingsEpoch`.

### Where the duplication is

1. **Shell surface enumeration (fixed by stage 0.1).** The taskbar, launcher,
   volume popup, tooltip and boot summary were listed separately in five
   places in compositor.cpp:
   - VKRENDER capture;
   - CPU draw;
   - virgl overlay draw;
   - virgl blur rects;
   - virgl compose.

   A sixth list remains: the open/close damage triggers at :2095.
2. **Alpha and material post-processing per backend** (two chrome
   versions, two overlay versions). This is where glass actually forks.
3. **Effects geometry exists only in the virgl path.** Shadows and title or
   panel blur are computed from window geometry in compositor.cpp. They are
   not part of any description VKRENDER could consume.
4. **Metrics in two headers.** `WM_*` in window.h and `DesktopMetrics` hold
   the same values, and four call sites outside the WM re-derive the frame
   offset.
5. **Colour literals outside the palette:** about 15 in kernel chrome
   (accent ×7, halo, telemetry, badge, cursor, agent icon). Settings, Files
   and the gallery hold more.
6. **Rendering twice.** While VKRENDER holds the lease, the CPU compositor
   still rasterises every window and its chrome into the back buffer
   (compositor.cpp:2122 skips only the overlay). The overlay is also rendered
   by both `CheckDesktopOverlay` and `CaptureDesktopOverlay`.

The rasteriser itself is not duplicated: `RenderWindowChrome` and the
shell painters are the only painters. That makes a single description cheap
to reach: the painters become its CPU executor.

## Target architecture

```
 settings.ini / Settings ──► Theme  (tokens: hue-derived set, metrics, material,
                                     resolved against backend capability)
                                │
 WM state (windows, focus,      ▼
 hover, launcher, tray) ──► Chrome model  ── part table ──► WmHitTest, taskbar/launcher hit
                             (layout)     ── surfaces[] ──┐
                                                          ▼
                          per surface: { rect, z, material, radii, shadow, key,
                                         display list (rect/gradient/glyphs/icon/nine-slice) }
                                                          │
        ┌─────────────────────────┬───────────────────────┴───────────────┐
        ▼                         ▼                                       ▼
  CPU executor             virgl backend                          VKRENDER backend
  (raster list into        (raster via CPU executor into          (texture via capture ABI +
   back buffer; materials   textures; material → BlurBarrier,      surface record: opacity,
   are opaque by rule)      shadow quad, opacity)                  shadow, blur when supported)
```

- **Theme.** One `BrookTheme` token struct (shared header, C-compatible) per
  (theme, hue, mode, scale, capability).
  - It holds every colour the painters use (no literals), the metrics
    (replacing both `WM_*` and `DesktopMetrics`), material parameters and
    motion.
  - It is derived once per settings change. The OKLCH derivation of
    DESIGN.md "Hue and accent colours" lives here, with its tables as host
    test vectors.
  - Settings and native apps include the same header. App content colours
    are emitted from it.
- **Chrome model and part table.**
  - For a window in a given state, `ChromeLayout(window state, theme)` returns
    the frame insets and the parts: role, rect anchored to window edges, and
    bleed. The edge-to-edge caption spec and conformance test (DESIGN.md
    T0) land here.
  - `WmHitTest` walks the parts. The painter iterates the same parts.
  - Shell layouts (`GetTaskbarLayout` and friends) already follow this
    pattern. They return parts the same way.
- **Surfaces and display lists.**
  - **Surface kinds:** every window frame, plus the shell surfaces
    (taskbar, the popup slot, menus, toasts, HUD, boot summary) and the
    cursor. A surface carries:
    - rect and z;
    - material: `opaque`, or `glass(level)`;
    - corner radii and shadow class;
    - content key;
    - a display list of a few primitives (DESIGN.md "Primitives":
      rect/gradient, nine-slice, glyphs, icon, clip).
  - **Effects are surface attributes, not list items.** Backdrop blur,
    opacity and shadow live on the surface, so a backend that cannot do them
    is never handed them.
  - **Executors.** One CPU executor rasterises a list into ARGB with
    straight alpha. It writes real alpha, so no backend recovers alpha by
    colour key. Today's `WmFillRect`/`WmRenderGlyph`/`WmDrawIcon` are its
    starting point.
- **Backends consume the surface records.**
  - **CPU** draws every surface opaque. Glass cannot be selected here, by
    rule.
  - **virgl** maps material to `BlurBarrier` plus a blur quad, and shadow
    class to the cached nine-slice. It replaces `GpuComposeTitleBlur`,
    `GpuComposeShadow` and `SetChromeDecorAlpha`.
  - **VKRENDER** receives the same records through a versioned extension of
    the layers ABI: opacity, radii, shadow class and blur level per chrome
    and overlay surface. It draws the shadow nine-slice itself. Blur comes in
    stage 2.4.
- **Capability and Glass lock.** The active backend reports
  `backdrop-blur`:
  - virgl: `BlurSupported`;
  - VKRENDER: once blur exists;
  - CPU: never.

  The theme resolver picks the Opaque stand-in when the capability is absent.
  It re-resolves when the backend changes at runtime (renderer exit, lease
  loss, GPU fault). Settings reads the same flag to grey the Glass card. This
  replaces `CompositorGlassActive()` and removes the VKRENDER yield.

### Where it lives, and why not a shell process first

- **The WM is in the kernel** and owns geometry, focus and input. The part
  table must be evaluated synchronously at input time. Moving the builder
  out of the kernel now would put an IPC hop on every chrome change and
  hover, with no restartability gain, because the WM itself is not
  restartable.
- **The compositor direction** (DECISIONS.md, "Compositor ownership") is
  that the OS keeps display and scanout and the renderer is a restartable
  user-mode backend. This plan fits it:
  - the description is device-neutral data;
  - backends are interchangeable;
  - the CPU executor remains the always-available fallback (DESIGN.md
    "If the renderer itself dies").
- **The shell surfaces are where a user-mode process pays off.** That means
  launcher organisation, search, drag and drop, toasts and the HUD
  (DESIGN.md steps 6–8). Stage 3 moves them. They then submit display lists
  for privileged shell surfaces through the same surface-record ABI that
  VKRENDER consumes. Because the builder is freestanding code from stage 1,
  that move is a relocation, not a rewrite.
- **Theme-as-data inside VKRENDER** (DESIGN.md T2) stays possible. The
  part table and display list are already data. Decide it after stage 2,
  once there is a second theme worth loading.

### Performance on the Dell

The compositor already costs about 40% of GPU time, plus 24% for present
(HANDOVER.md, Chrome video run). The rules below keep the chrome off the
per-frame GPU and CPU path.

- **Nothing is regenerated per frame.**
  - Surfaces are re-rasterised only when their key changes: hover part,
    focus, title, theme epoch or size. That is today's chrome key, applied
    per surface.
  - The one 100 ms full-overlay re-render and FNV hash becomes per-surface
    keys:
    - the clock invalidates the taskbar once a minute;
    - telemetry graphs update at 2–4 Hz (DESIGN.md, HUD);
    - hover invalidates one surface.
- **Stop the double raster.** While a renderer holds the lease, skip CPU
  composition of windows and chrome. The lease-expiry path already forces a
  full repaint (compositor.cpp:2119). This saves kernel CPU on every Dell
  frame. Validate the fallback with a renderer-kill test.
- **Copy only frame spans.** Chrome capture currently `kmalloc`s, clears and
  copies the full outer w×h, including the transparent client interior.
  Copy the four frame strips instead. The renderer already has a spans mode
  (`BROOK_RENDER_CHROME_SPANS`).
- **Occlusion.** Mark opaque shell surfaces and opaque frames as
  `OPAQUE_SOURCE` in the quad plan. Today chrome and overlay never occlude,
  so the wallpaper under the taskbar is shaded and then overdrawn.
- **Glass budget.** Blur is recomputed only when pixels beneath a glass
  surface changed.
  - A maximised game leaves only wallpaper beneath the taskbar and its own
    title bar, so the blurred wallpaper is computed once and reused.
  - One blur target at 480×270 per frame at most, shared by all glass
    surfaces in the frame (one barrier, as the virgl path already does for
    shell surfaces).
  - The per-frame chrome budget is DESIGN.md's 1.0 ms at 3200×1800,
    measured with GPU timestamps by class. `desktop_stats.h` already splits
    per class.
- **Fullscreen** has no chrome and no zones (DESIGN.md), so it costs nothing.

## Staged migration

Each step ships on its own, leaves the desktop working and has an exit
check. Stages 0 and 1 change no pixels unless a step says otherwise.

### Stage 0: consolidate without changing pixels

| Step | Change | Exit check |
| --- | --- | --- |
| **0.1 (done)** | One shell-surface list: `WmShellSurfaces()` and `WmRenderShellSurface()`. The VKRENDER capture, CPU draw, virgl overlay draw, virgl blur and virgl compose all iterate it. | Host test (`test_window_surfaces`), QEMU CPU screenshots pixel-identical (see Phase 1 evidence) |
| **0.2 (done)** | Tokens:<br>• `brook_theme.h` gains palette fields `accent`, `halo`, `ok/warm/hot` and `badgeText/badgeFill`, plus named constants for theme-independent colours (material shade, wallpaper fallback, cursor busy dots, agent icon). Every theme keeps the old literal values.<br>• Painters, telemetry, `chrome_material.h`, `cursor_composition.h` and `desktop_backdrop.h` read tokens.<br>• One metrics source: `WM_TITLE_BAR_HEIGHT`/`BORDER`/`BUTTON`/`TASKBAR` come from `DesktopMetrics::*Base`.<br>• `DesktopMetrics::clientOffsetX/Y()` replaces the frame-offset arithmetic in the shell, ioctl and Kernel Console code.<br>**Held for 0.4**, because chrome-csd edits those functions: the accent stroke literal in `RenderWindowChrome` and the colour key in `SetChromeDecorAlpha`. | `test_window_surfaces` pins the metric equivalence and the token values; QEMU screenshots identical (see Stage 0.2/0.5 evidence) |
| **0.3 (done)** | Split window.cpp (4234 → 3155 lines), pure move:<br>• raster primitives to `chrome_raster.{h,cpp}`, one translation unit with a header;<br>• taskbar/tray/popup/tooltip code to `wm_shell_taskbar.inc` and launcher UI to `wm_shell_launcher.inc`, still included in window.cpp's translation unit because they share the WM's window list and launcher state. They become their own unit when the shell builds surfaces (stage 1.2/3).<br>Not done: `chrome_raster` is not yet freestanding, since it still calls `WmDesktopFont`, `WmMetric` and the compositor's glass state; that is the executor work in stage 1.2. | Expanded includes reproduce the old file exactly; host tests; identical screenshots (see Stage 0.3/0.4 evidence) |
| **0.4 (done)** | One alpha rule: `chrome_alpha.h`, where frame = outer minus client is opaque, the interior is transparent, and an overlaid caption block is opaque. Used by the virgl chrome layer's opaque branch and by the VKRENDER capture. The glass branch (virgl only: title material alpha, opaque text/accent colour key, corners) stays and takes the accent token; it goes when surfaces carry their material (stage 1.3). Painters writing real alpha is moved to the stage 2 repaint, because the colour key encodes behaviour (hover glyphs keep material alpha) that a pure step must not change. | `test_chrome_alpha`: exact against copies of both old loops; virgl and CPU screenshots identical |
| **0.5 (done)** | Damage triggers use the surface list: one comparison of the shell surfaces (kind and rect, boot summary excluded) replaces the separate launcher, volume-popup and tooltip triggers. A surface that resizes while it is open now also repaints the scene under it. | Same |

Steps 0.3 and 0.4 landed after `work/chrome-csd` merged into main.

### Stage 1: single description

| Step | Change | Exit check |
| --- | --- | --- |
| **1.1 (done)** | Part table plus `WmHitTest` on parts, with edge-to-edge captions (DESIGN.md T0, implementation step 1):<br>• `chrome_layout.h`: freestanding part table per window state (role, visual rect, hit rect), in the spec's test order;<br>• the full DESIGN.md zone table at today's metrics (title 36, buttons 46): 6 px outer margin on every side with 16 px diagonal corners, buttons hit from y = 0 and Close to the outer right edge, 2 px inner band with 8 px corner squares, no zones when maximised, none on an edge touching the screen;<br>• chrome-csd's `captionOverlay` is three button parts on a CSD window, hit where drawn;<br>• hover follows the hit rect. | `test_chrome_layout` (DESIGN.md "Conformance test") at 100/150/200%. Intended change: hit areas only (see Stage 1.1/1.2 evidence). |
| **1.2 (done)** | Display lists: frame and shell painters become *build list* plus *CPU execute*:<br>• `chrome_display_list.h` (7 primitives: fill, title rows, corner-clipped fill, material rect, text, icon, square runs), `chrome_style.h` (palette, metrics, font, resolved material);<br>• `chrome_raster` is the freestanding executor; `chrome_frame` and `chrome_shell` are freestanding builders with the taskbar and launcher layouts;<br>• the WM snapshots state into models (`wm_shell_*.inc`) and keeps hit tests and the build-badge spans. | `test_chrome_goldens`: 144 goldens (16 elements × 3 themes × 3 scales); QEMU CPU screenshots identical |
| **1.3 (done)** | Surface records (material, radii, shadow class, content key):<br>• `chrome_surface.h` builds one record per frame and shell surface, resolved against the backend (shadow and glass radii only under glass);<br>• virgl takes its shadow quads, title-blur radius and shell blur from them; the shadow nine-slice is shared (`brook_chrome_shadow.h`);<br>• layers ABI v2: op 16 (frame records), overlay op 10 (shell records); VKRENDER draws the shadow nine-slice. | Virgl screenshot identical; VKRENDER host quad test against the CPU reference (see Stage 1.3/1.4 evidence) |
| **1.4 (done)** | • Per-surface overlay keys (display-list hash): the 100 ms check builds lists only, a capture rasterises only changed surfaces, band keys come from surface keys;<br>• no CPU raster of wallpaper, windows or chrome under a lease (one full repaint after);<br>• span-only chrome (op 17) drawn as in-place spans;<br>• opaque frames (spans, caption blocks) occlude in the quad plan, with `work/shell-opaque`'s bands. | Host tests and QEMU screenshots; Dell measurement plan below (not yet run) |
| 1.5 | Backend `backdrop-blur` capability and theme resolution, replacing `CompositorGlassActive()` | QEMU: software → Opaque, gl → Glass; renderer kill → Opaque stand-in |

### Stage 2: Clearwater visuals, implemented once

This follows DESIGN.md's implementation order. Each step changes the
builder and theme only, and every backend picks it up.

1. Metrics: title bar 40, taskbar 48 with full-height targets.
2. Themes: `theme=glass|opaque-light|opaque-dark`, Mixed mode, aliases from
   the old palette and Performance settings.
3. Hue derivation in `BrookTheme` with the DESIGN.md tables as test
   vectors, plus content colours for GTK and native apps.
4. **VKRENDER backdrop blur** under the glass budget above. Until it lands,
   the Dell resolves Glass to its Opaque stand-in. Once it exists, run the
   **Glass-on-Dell experiment** (Ian, decision 1):
   - matched yquake2 runs with Glass on and with Opaque;
   - a maximised game and an idle desktop with glass panels open;
   - measure displayed fps and compositor GPU ms per class.

   Whether Glass is the Dell default follows from that measurement, not
   from an assumption.
5. Glass repaint: sheen, accent line, rim, caption pills, radii, three
   shadows. Taskbar states and tray.
6. Toasts, flyouts and HUD as surfaces in the list.

### Stage 3: shell process (before launcher organisation)

The taskbar, launcher, toasts and HUD move to a user-mode shell that
submits surfaces through the stage 1.3 ABI. Launcher organisation
(DESIGN.md steps 6–7) waits for this process (Ian, decision 3); it is not
built in the kernel painter first. The kernel keeps a minimal
taskbar and frame for when the shell is down (DESIGN.md T3).

## Tests

- **Host unit tests (ctest):**
  - shell-surface list, part table and hit conformance;
  - theme derivation against the DESIGN.md tables and contrast targets;
  - key invalidation (`test_wm_chrome_key`).
- **Host golden images.** The freestanding CPU executor renders each element
  (frame active/inactive/hover/maximised, taskbar states, launcher, popup,
  tooltip) per theme × scale into PNG goldens under
  `src/tests/goldens/chrome/`. Test: byte-identical, or an explicit
  `--update` with review.
  - This is the oracle that QEMU and the Dell lack today: nothing compares
    chrome pixels against a reference (`desktop_chrome_present_test` samples
    only 4 pixels).
  - Golden data uses the baked fonts (`scripts/bake_font.py`), so it is
    deterministic.
- **VKRENDER:** extend `quad_pass_host_test` (CPU oracle `quad_reference.h`)
  with surface records: shadow, opacity and later blur. Blur gets a
  tolerance, not exactness.
- **QEMU screenshots:**
  - Run the scene script used for stage 0.1: idle, tray tooltip, volume
    popup, launcher, terminal window. Run it on `gpu=software` and, where a
    virgl-capable QEMU exists, `gpu=gl`.
  - Compare the before and after builds with the clock, the CPU/MEM graphs
    and the build label masked.
  - Every stage 0/1 step must be identical. Stage 2 steps are reviewed by
    eye against `index.html`.
- **Dell measurement points:**
  - **When:** after 1.4, after 2.4 and after 2.5. Not after pure moves: QEMU
    already proves identical pixels, and the CPU path is unchanged.
  - **Workload:** the matched yquake2 GL3 demo, measured with
    `dell_quake_measure.py`.
  - **Metrics:** displayed fps (DECISIONS.md metric), compositor GPU ms
    from `desktop_stats` per class, chrome keeps and misses, overlay
    captures/s, and worker idle %.
  - **Idle desktop:** compositor jobs/s while hovering the taskbar.
  - **Glass:** the Glass-on-Dell experiment (stage 2.4), the same workload
    with Glass on and off.
  - **Accept if:** compositor GPU time per displayed frame is no worse than
    baseline (Opaque), and Glass costs ≤ 1.0 ms per frame with a maximised
    game.

## Risks

- **Changing the hit spec changes muscle memory.** Stage 1.1 is
  intentionally visual-free. Land it with the conformance test only.
- **Dell-only paths can't run in QEMU.** These include the capture ABI and
  the shadow and blur work in VKRENDER.
  - Keep ABI changes versioned, with old-kernel and old-renderer fallbacks,
    as the KEYED/KEEP ops already do.
  - Host-test them against the CPU oracle.
  - Gate Dell deployment on Ian being present (HANDOVER.md incident
    lesson).
- **Text parity.** If glyphs ever move to a GPU atlas, they would differ
  from the CPU rasteriser. The plan avoids this by keeping the 2D raster on
  the CPU.
- **WM and renderer state in step during resize.** Chrome must be keyed by
  geometry and paired with the client's identity, which
  `desktop_chrome_binding.h` already enforces. Surface records inherit that
  pairing.
- **Glass cost.** Blur with changing content beneath it costs GPU. The
  budget and auto-fallback (DESIGN.md "Budgets and safety", escalation
  step 2) must be in place before Glass is the Dell default.
- **Merge friction.** window.cpp and compositor.cpp are hot files. Pure
  moves (0.3) go in small commits at quiet moments, after chrome-csd.

## Interactions

- **`work/chrome-csd`** is merged into main. Its overlaid caption buttons
  (`captionOverlay`) are the first case of a part table on a
  client-decorated window. Stage 1.1 should absorb it as "parts with a
  button role on a CSD window". Stages 0.3 and 0.4 were built on top of it.
- **`work/shell-opaque`** (16 November) makes opaque shell bands occlude in
  VKRENDER and adds per-band keys (overlay op 9). A trial merge with this
  branch is clean. It is the first piece of "surfaces carry their
  material" (stage 1.3), so stage 1.3 should extend its band opacity and
  key to window-frame surfaces rather than add a parallel mechanism.
- **`work/compat-census`:** no overlap found.
- **`work/ui-drawer` (Clearwater):** stage 2 is that work. Its native
  steps should target the builder and theme, not the painters directly.

## Decisions (Ian, 15 November)

1. **Glass on the Dell: measure it.** Once VKRENDER blur exists (stage
   2.4), run the Glass-on-Dell experiment and decide from the numbers. The
   Dell does not stay Opaque by assumption until 60 fps.
2. **Golden images in the repository are fine.** A few MB of reference PNGs
   go under `src/tests/goldens/chrome/`.
3. **Launcher organisation waits for the user-mode shell process** (stage
   3).
4. **Stop drawing everything twice.** While VKRENDER holds the lease, skip
   the CPU raster of windows and chrome (stage 1.4). The one-off full
   repaint of about 15 ms at fallback is acceptable.
5. **App-content colours** (`brook_surface_palette` in
   `brook_appearance.h`) fold into the theme whenever it makes sense as the
   work progresses. There is no fixed stage. Stage 0.2 does not move them.

## Phase 1 evidence (stage 0.1)

**Change.** `src/kernel/src/desktop_shell_surfaces.h` holds
`WmShellSurfaces()` and `WmRenderShellSurface()`. compositor.cpp's five
enumerations now iterate it:

- VKRENDER overlay capture;
- CPU back buffer;
- virgl overlay layer;
- virgl blur;
- virgl compose.

window.cpp and window.h are unchanged, which keeps clear of
`work/chrome-csd`.

**Host tests:**

- `test_window_surfaces` gained shell-list cases: taskbar first, a single
  popup slot, the boot summary last and opaque.
- `test_gpu_capture_queue` runs the real list against its stubs through
  `RenderDesktopOverlay`.
- Full suite: 359/359. Four tests timed out under parallel load and passed
  when rerun alone.

**QEMU, CPU path (`gpu=software`).** Seven scenes were captured on the
baseline (`0ae38b0d`) and on the candidate: idle, tray tooltip, volume
popup, popup closed, launcher, launcher closed, terminal window. They are
**pixel-identical** outside these masks:

- the clock;
- the CPU/MEM graphs;
- the build label, which gains a dirty-tree suffix;
- the blinking caret.

Scene 7 on the candidate came from a QEMU `screendump`, because the debug
bridge stalled (below). The baseline frame for that scene is a bridge frame;
an untouched wallpaper patch matches exactly, so the two capture methods
agree.

**Not checked by screenshot.** The virgl `gpu=gl` path and the Dell
VKRENDER path were reviewed only, plus covered by the host test above.

**Evidence** is in the worktree under `build/refactor-evidence/`
(`qemu-cpu-baseline/`, `qemu-cpu-candidate/`, `compare-cpu.txt`,
`scenes.py`, `compare.py`). It is not committed.

**Incidental QEMU stall.** Both candidate boots ran on a heavily loaded host
(load around 35, with another agent's test using 12 cores). In both, the
debug bridge stopped accepting connections:

- one boot froze the desktop;
- the other kept rendering.

`info registers` showed most vCPUs in `SchedLockWaitForTicket` and one in
`virtio_net`'s `SpinLockAcquire`. The change under test adds no locks and
no blocking, and in the second boot the compositor kept drawing. This looks
like a pre-existing network/scheduler-lock issue under host contention, not
this change. It was not investigated further. Evidence:
`build/refactor-evidence/qemu-hang-virtio-net/`.

## Stage 0.2 and 0.5 evidence

The branch was rebased onto `main` at `02e48c76`. A trial merge with
`work/chrome-csd` (`git merge-tree`) is clean.

**Host tests:** 361/361 pass. `test_audio_delivery` timed out under
parallel load and passed when rerun alone. `test_window_surfaces` now pins:
- the `WM_*` constants against the `DesktopMetrics` bases;
- `clientOffsetX/Y()` against the WM arithmetic at 100/150/200%;
- the new token values for all three themes.

**QEMU CPU path (`gpu=software`)** compares the same seven scenes on the
`main` baseline (`02e48c76`) and the stage 0.2+0.5 build (`d54bf370`). All
seven are **pixel-identical** outside the masks for the clock, telemetry,
build label and caret (`compare-0205.txt`). Every scene was checked to be in
its intended state.

How each side was booted and captured:
- **Kernel source:** the kernel boots from the build directory's ESP, not
  from the disk image. The baseline was therefore booted with
  `BROOK_BUILD_DIR` pointing at a separate copy of the `main` build.
- **Capture method:** scenes are bridge frames, except candidate scene 5,
  which is a QMP screendump. A later bridge capture of scene 5 is also
  identical.
- **Input:** driving input straight over QMP `input-send-event` was not
  reliable for clicks after the first one, so input went through the bridge
  (`vm_input`).

**QEMU stalls, again under heavy host load (load around 40):**
- **The 0.2/0.5 candidate froze once,** at scene 5: the clock stopped and
  all eight vCPUs were in `SchedLockWaitForTicket`. A heuristic stack scan
  shows LAPIC-timer `SchedulerTimerTick` spinning for the scheduler lock
  over `SchedulerBlockImpl` and the network poll thread
  (`build/refactor-evidence/qemu-hang-sched/`).
- **The record so far:**
  - one freeze among the two stage 0.1 candidate boots;
  - one freeze among the two stage 0.2/0.5 candidate boots;
  - no freezes in the 0ae38b0d baseline boot or the three main-baseline
    boots.
- **The changes don't touch the scheduler.** The new per-frame work takes
  only the launcher and launch spinlocks, which these paths already took.
- **The pattern is still worth a soak on an idle host** before integration.

**Follow-up pass.** A second full seven-scene pass on the re-booted
candidate completed without a stall once host load fell (load about 8).
That is consistent with stalls triggered by contention. It does not
replace the idle-host soak.

## Stage 0.3 and 0.4 evidence, and the idle-host soak

The branch was rebased onto `main` `73409c2a` (chrome-csd included).

**Soak.** One QEMU boot per row, alternating `main` and the rebased stage
0.1+0.2+0.5 build. Each boot ran the seven-scene bridge sequence twice, then
checked for a freeze: is the taskbar clock unchanged across 65 s of QMP
screendumps? vCPU RIPs were also sampled. Results are in
`build/refactor-evidence/soak/results.jsonl`.

| Boot | Build | Load average | Scene passes | Frozen |
|---|---|---|---|---|
| 0 | main | 34 | 7 + 7 | no |
| 1 | candidate | 34 → 39 | 4 (bridge stalled) | no |
| 2 | main | 38 → 13 | 7 + 7 | no |
| 3 | candidate | 13 | 7 + 7 | no |
| 4 | main | 11 | 7 + 7 | no |
| 5 | candidate | 11 | 7 + 7 | no |

- **No freezes in six boots.** Three more candidate boots for the
  screenshots below were also clean.
- **One bridge stall,** on a candidate boot under load 34–39. The guest
  stayed alive.
- **The `SchedLockWaitForTicket` spin shows up in RIP samples of both
  builds** under load.
- **The earlier freezes** (two of four candidate boots) happened at load
  40–49.
- **Conclusion:** with this sample, there is no evidence that the refactor
  changes the freeze rate. The stalls track host contention. If it comes
  back, look at the scheduler-lock path under vCPU preemption, not at the
  compositor.

**Stage 0.3/0.4 checks:**
- **Host tests:** 364 run, `test_fd_table` excluded because it hangs on main
  too.
  - `test_audio_delivery` hit the ctest timeout under load and passes when
    run alone.
  - `test_intel_bdw_buffer` segfaulted once; it flakes on main as well (3
    of 20 runs on main, 0 of 20 on this branch).
  - New: `test_chrome_alpha`.
- **QEMU CPU path:** all seven scenes match `main` exactly outside the usual
  masks (`compare-034-cpu.txt`).
- **QEMU virgl path (`gpu=gl`, glass active):**
  - This path exercises the glass alpha, shadows, blur and overlay
    composition.
  - All seven scenes are identical outside the usual masks plus two blur
    halos (`compare-034-gl-blurmask.txt`).
  - The halos come from things outside this change. The build label differs
    in length (dirty-tree suffix), and the glass taskbar, popup and tooltip
    blur it. The blinking terminal caret is in the desktop backdrop, and the
    glass titlebar blurs it.
  - The differences are small: at most 13 levels near the caret, and only
    within the blur radius of those two sources.

## Stage 1.1 and 1.2 evidence

Branch `work/clearwater-s1`, based on `work/integration32` (`7268f1b6`).
Commits: `ddd9b9ce` (part table and conformance test), `306b2e53`
(`WmHitTest` on parts), `0acc8712` (builders and executor), `311bea32`
(goldens).

**Caption hit areas changed in stage 1.1, not deferred.** Ian left the
timing to the implementer (DECISIONS.md, 18 November). Rewriting
`WmHitTest` onto the part table is where every zone is defined, so the
spec's zones landed there with the conformance test. Stage 1.2 does not
touch input. What changed for the user:

- Caption buttons are hit from the outer top edge (y = 0) down to the
  titlebar bottom, and Close to the outer right edge. A maximised window's
  Close owns the screen's top-right pixel.
- Hover follows the hit rect: the pill (still drawn in today's cell)
  lights whenever a click would press the button, including from the top
  border.
- Resize: a 6 px outer margin on every side (new on the left and top;
  diagonal within 16 px of a corner). Inside the frame, a 2 px band with
  8 px corner squares replaces the old 6 px right/bottom band and 16 px
  bottom-right square. Left and top resizes use the existing
  `CompositorBeginWindowResize` edges.
- Maximised windows have no resize zones. An edge that touches the screen
  edge loses its zones, so a window pushed to the right edge has Close in
  the screen corner.
- Client-decorated windows are unchanged: overlaid caption buttons are
  three button parts, hit exactly where they are drawn, and the client's
  shadow margin stays the client's.
- Metrics are unchanged (title 36, buttons 46). Title 40 and buttons 48
  are stage 2.1.

**Host tests:** 376 run, all pass. `test_audio_delivery` timed out under
parallel load and passed alone (known flake).
- `test_chrome_layout`: the DESIGN.md conformance set at 100/150/200%
  (outer top-right pixel in normal, maximised and right-tiled states; one
  pixel either side of each button boundary; a y = 0 sweep across the
  titlebar; the margin on every side; corner extents; inner bands; CSD
  overlay with and without a shadow inset). Expected zones are written
  from the spec's logical sizes, not the implementation's arithmetic.
- `test_chrome_goldens`: 16 elements × 3 themes × 3 scales = 144 PNGs
  (2.2 MB) under `src/tests/goldens/chrome/`, byte-identical, plus a hash
  of each ARGB buffer (alpha included) in `MANIFEST`. The binary links
  only `chrome_raster.cpp`, `chrome_frame.cpp`, `chrome_shell.cpp` and the
  desktop fonts baked as the kernel bakes them, which also proves those
  sources are freestanding. `--update` rewrites them.

**QEMU CPU path (`gpu=software`), headless with QMP input and
screendump** (no debug bridge, `BROOK_NET_DEVICE=none`). The same seven
scenes as stage 0, plus close-button hover at its centre, a hover at the
outer top-right pixel (the cursor landed one row above the window, in the
resize margin), a taskbar-button hover and a final idle. Baseline
`7268f1b6` against candidate `311bea32`: all 11 scenes are
**pixel-identical** outside the usual masks (clock, telemetry, build
label, caret) (`compare-cpu.txt`). A candidate-only scene with the
pointer on the top border above Close shows the hover pill: the intended
hit-area change. The boot summary disappears before the scenes start, so
it is covered by the goldens only.

**Dell pair:** `build/dell/pair-cw1` (payload `dell-cw1-20261118`, scanout
flip, persistent scanout map, zero-copy scanout and video engine on;
clean tree at `311bea32`; kernel `290d1958…`). `check_dell_image_qemu.py
--attempts 4`: PASS.

**Not checked:** virgl (`gpu=gl`) and VKRENDER screenshots. Both consume
the same CPU raster (chrome layer, chrome capture, overlay capture) and the
material inputs are unchanged, so they were reviewed only.

**Check by hand on the Dell (200%):** Close from the screen's top-right
corner with a maximised window; hover and click on the top border above
the buttons; resize from the new left and top margins (12 px outside the
frame at 200%) and the smaller inside band (4 px; 16 px corner squares);
a maximised window must not start a resize from any edge; Chromium's
overlaid captions behave as before.

**Evidence** (not committed): `build/cw1-evidence/` in the worktree:
`base/`, `cand/`, `cand-edge/`, `compare-cpu.txt`, `scenes_qmp.py`,
`scenes_extra.py`, `scenes_edge.py`, `compare.py`, serial logs, and the
pair build and QEMU check logs.

## Stage 1.3 and 1.4 evidence

Branch `work/clearwater-s1b`, based on `work/integration36` (`c1b926f2`).
Commits: `31447592` (per-surface overlay keys), `9b0fa2cb` (no CPU raster
under a lease), `aad73960` (surface records, ABI v2, virgl), `8b8e4158`
(VKRENDER).

**What changed.**

- **Overlay keys (1.4).** Each shell surface's display list is hashed
  (`ChromeListKey`, seeded with kind, rectangle, screen, font and material;
  `WmShellSurfaceKey`). The 100 ms / input check (`CheckDesktopOverlay`)
  builds the lists and compares keys; it no longer rasterises or hashes
  pixels, and it no longer touches the capture buffer, so it also runs
  while a capture lease is held. A capture rasterises only surfaces whose
  key or rectangle changed, plus anything that shares pixels with them
  (`desktop_overlay_surfaces.h`, planned to a fixed point; a pointer
  composited into a debug capture is cleared and the surfaces beneath it
  redrawn next time). Op 9 band keys are folded from the surface keys, so
  an unchanged launcher keeps its band key while the taskbar graph or a
  hover changes. Counters in `compositor idle`: `overlay_surface_draws`,
  `overlay_surface_keeps`, `overlay_key_checks`.
- **No CPU raster under a lease (1.4).** While a renderer holds the
  present lease, the WM loop skips the wallpaper, window contents, carets
  and chrome into the back buffer (`cpu_scene_skips`). The first loop
  without the lease (expiry, release, GOP writer) repaints everything once.
- **Surface records (1.3).** `chrome_surface.h` builds a record per window
  frame (or overlaid caption block) and per shell surface: rectangle,
  client interior, material and blur level, opacity, radii, shadow class,
  alpha, spread and corner, an opaque flag and the content key. Today's
  rules are kept: a shadow and rounded glass corners exist only under
  glass, never maximised or client-decorated; opaque frames and shell
  surfaces are opaque in every pixel. The nine-slice source and slicing
  are shared C/C++ (`brook_chrome_shadow.h`).
- **virgl** takes its shadow quads, title-blur radius and shell blur
  decision from the records (pixel-identical by construction).
- **Layers ABI v2** (`desktop_layers_abi.h`): op 16
  `BROOK_DESKTOP_SURFACES_QUERY` (version 2) returns the frame records,
  matched to layers by window and incarnation; overlay op 10 returns the
  shell records of a capture lease; op 17
  `BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_SPANS` is a keyed capture that
  clears, renders and copies only the frame strips, never the client
  interior. **Fallbacks**, as KEYED/KEEP: an older kernel returns EINVAL
  for 16, 10 and 17; the renderer then turns records off for good (no
  shadows, nothing marked opaque) and uses the keyed full capture. Older
  renderers never call the new ops. `BROOK_RENDER_SURFACES=0` and
  `BROOK_RENDER_CHROME_SPANS=0` turn the renderer features off.
- **VKRENDER** draws each record's shadow (eight straight-alpha quads
  beneath the window, top four windows as virgl; each piece has its own
  64x64 source, because the compute and graphics fallbacks reject layers
  sharing a buffer), with the shadow bounds in change tracking. Chrome is
  drawn as in-place spans of the chrome surface (no per-frame span copy);
  in-place spans share a buffer, so they are used on quad-pass frames only
  and a quad rejection retries with whole layers for a second. The chrome
  buffer's interior is zeroed once per allocation and geometry, so a whole
  layer never shows stale pixels. Spans and caption blocks of a record
  marked opaque, at opacity 255, are `BROOK_VK_OPAQUE_SOURCE`: together
  with `work/shell-opaque`'s bands, nothing beneath an opaque frame or
  shell surface is shaded. Shell records are captured and adopted with the
  bands but draw nothing yet (no shell shadows until stage 2.6).
- **On the Dell nothing looks different.** Glass is never active under
  VKRENDER (it yields), so every record has shadow none and opaque
  material: the visible effects are fewer layers shaded, no span copies,
  less kernel CPU and cheaper overlay captures. The shadow path is
  exercised by host tests and on virgl until stage 1.5.

**Host tests** (kernel, ctest): 380 run. `test_audio_delivery` times out,
on the baseline build as well, under host load 30–56; four others timed out
under parallel load and passed alone. New or extended:
- `test_desktop_overlay_surfaces`: 200 000 random steps (overlapping,
  moving, appearing and leaving surfaces, unknown keys, composited
  pointers, wiped buffers); incremental redraw equals a full redraw every
  step; band keys change exactly with their surfaces.
- `test_gpu_capture_queue`: an unchanged surface is kept (no raster), a
  changed taskbar is redrawn while the launcher is kept, and only the
  taskbar band's key changes; op 10 records; key-only checks never touch
  the buffer.
- `test_chrome_goldens`: equal list keys mean equal pixels over the 144
  goldens (goldens unchanged).
- `test_chrome_surface`: record rules at 100/200%; the shared nine-slice
  equals the old `GpuComposeShadow` quads and the old source pixels.

**Compositor package** (`nix-build tools/vulkan-compositor-pkg`): all
built-in tests pass, plus `desktop_surfaces_test` (shadow quads against a
direct nine-slice raster through `quad_reference.h`, 155 M pixel checks;
opaque spans change no pixel and occlude in the plan; shadow damage).

**VKRENDER on the host GPU (i915, headless):**
- `brook-quad-pass-host-test`: `QUAD_HOST_PASS`, including a new surface
  scene (two windows with record shadows sharing one source, in-place
  spans, one opaque and one translucent frame) exact against the CPU
  oracle for full, partial and title-only damage, with nothing shaded
  under the opaque title strip.
- `brook-desktop-renderer-host-test` (fake kernel serving ops 16/17,
  interior left untouched, CPU reference with shadows): `HOST_E2E_PASS` in
  serial, pipelined async-partial, direct GPU sources (`HOST_E2E_DIRECT=1
  HOST_E2E_LAYERS=1`), game and no-spans runs, and with
  `HOST_E2E_OLD_KERNEL_NO_SURFACES=1` (old-kernel fallback).

**QEMU, headless, QMP input, no NIC or bridge.** Baseline `c1b926f2`
against the candidate, the stage 1.1/1.2 scene set (11 scenes; the
taskbar-hover probe times out on both builds, the frame is still compared):
- `gpu=software`: all 11 scenes **pixel-identical** outside the usual masks
  (clock, telemetry, build label, caret) (`compare-cpu.txt`).
- `gpu=gl` (virgl, glass active, record-driven shadows visible beside the
  terminal): all 11 **identical** outside the usual masks plus the two blur
  halos of stage 0.3/0.4 (build label, caret) (`compare-gl-blurmask.txt`).
  QMP `screendump` cannot read the egl-headless scanout ("no surface"), so
  these frames were read from a VNC socket on the same QEMU
  (`vncgrab.py`, raw RFB).

Evidence (not committed): `build/cw1b-evidence/` in the worktree.

### Dell measurement plan (stage 1.4)

Deploy the pair with Ian present. Matched against the gen 441 runs
(`display-perf/build/dell/q2-441-launcher{,-off}/`), the same yquake2 GL3
demo, 20–30 s windows, desktop stats `s0`/`s1` diffed with
`scripts/desktop_stats_diff.py`, plus `compositor idle` and `desktop chrome
status` before and after:

1. **Quake II fullscreen, launcher open, pointer moving** (the gen 441
   case). Expect the renderer's overlay capture wait (`overlay` wait in the
   stats, `DESKTOP_OVERLAY_TIMING wait_ms` with `BROOK_RENDER_TIMING`) near
   zero, against 12–14 ms; `overlay_surface_keeps` rising with the launcher
   kept and `overlay_surface_draws` about one per capture (the taskbar);
   prepare ms/frame back near the launcher-off value (2.7 ms), displayed
   fps no worse than launcher-off.
2. **Quake II fullscreen, launcher closed.** Compositor GPU ms per
   displayed frame and compositor job count no worse than gen 441;
   `cpu_scene_skips` equal to the leased loops.
3. **Kernel CPU:** compositor-thread time (profiler or worker idle %) lower
   than gen 441 in cases 1 and 2.
4. **Idle desktop, terminal plus a windowed app, hover the taskbar and the
   caption buttons:** compositor jobs/s no worse; renderer `chrome`
   `span_captures` rising and `bytes_copied` per capture about the frame
   strips; `opaque_spans` rising; `surfaces.records` 1 and `shadows` 0
   (opaque material).
5. **Lease loss:** kill the renderer; the CPU desktop must come back in one
   full repaint (about 15 ms) with every window and its chrome.

Accept if 1–4 hold and 5 shows a complete desktop. Rollback without a
rebuild: `BROOK_RENDER_SURFACES=0` and `BROOK_RENDER_CHROME_SPANS=0` in the
renderer environment; the kernel changes have no switch (the previous pair
is the rollback).
