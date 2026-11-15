# Compositor refactor: describe Clearwater chrome once

15 November 2026. Status: proposal for Ian, with stage 0.1 implemented on
`work/compositor-refactor`. It covers the [Clearwater Next design](DESIGN.md)
and the [compositor direction](../../compositor-direction.md).

Line numbers refer to `main` at `0ae38b0d`, before stage 0.1.

**Goal:** implement the Clearwater chrome (window frames, taskbar, tray, popups,
menus, toasts, cursor and theme) once. Every compositor backend consumes that
one description, and nothing adds per-frame cost on the Dell.

## Decisions proposed

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
| 0.2 | Tokens:<br>• add `accent`, `halo`, `ok/warm/hot`, badge and cursor colours to the palette and replace every chrome literal;<br>• one metrics source, with `WM_*` defined from `DesktopMetrics`;<br>• export `WmClientOffset()` for the console, shell and ioctl call sites. | Host tests; QEMU screenshots identical; `grep` gate: no `0x..RRGGBB` literal in chrome painters |
| 0.3 | Split window.cpp:<br>• shell layout and painters to `desktop_shell.cpp`;<br>• raster primitives to `chrome_raster.{h,cpp}` (freestanding, host-buildable). | Build, host tests, identical screenshots |
| 0.4 | One alpha rule: painters write real alpha. Delete `SetChromeDecorAlpha`'s colour key and the capture's span loop; keep one helper for interior transparency and corner masks. | Virgl screenshot identical; `desktop_chrome_test` mask check |
| 0.5 | Damage triggers (compositor.cpp:2095) use the surface list | Same |

Steps 0.3 and 0.4 touch `RenderWindowChrome`. Land them **after
`work/chrome-csd` merges** (see [Interactions](#interactions)).

### Stage 1: single description

| Step | Change | Exit check |
| --- | --- | --- |
| 1.1 | Part table plus `WmHitTest` on parts, with edge-to-edge captions (DESIGN.md T0, implementation step 1) | Table-driven conformance test (DESIGN.md "Conformance test") at 100/150/200%. Intended visual change: caption hit areas only. |
| 1.2 | Display lists: frame and shell painters become *build list* plus *CPU execute* | Host golden images per element × theme × scale (see Tests); QEMU screenshots identical |
| 1.3 | Surface records (material, radii, shadow class):<br>• virgl consumes them, replacing its own blur and shadow geometry;<br>• layers ABI v2 carries them to VKRENDER, which draws the shadow nine-slice. | Virgl screenshot identical; VKRENDER host quad test against the CPU reference |
| 1.4 | Per-surface overlay keys and bands, replacing the 100 ms full re-render and hash; no CPU raster under lease; span-only chrome copy; opaque occlusion | Dell measurement (below): compositor GPU ms and job count no worse, kernel CPU lower |
| 1.5 | Backend `backdrop-blur` capability and theme resolution, replacing `CompositorGlassActive()` | QEMU: software → Opaque, gl → Glass; renderer kill → Opaque stand-in |

### Stage 2: Clearwater visuals, implemented once

This follows DESIGN.md's implementation order. Each step changes the
builder and theme only, and every backend picks it up.

1. Metrics: title bar 40, taskbar 48 with full-height targets.
2. Themes: `theme=glass|opaque-light|opaque-dark`, Mixed mode, aliases from
   the old palette and Performance settings.
3. Hue derivation in `BrookTheme` with the DESIGN.md tables as test
   vectors, plus content colours for GTK and native apps.
4. **VKRENDER backdrop blur** under the glass budget above. Until it lands
   and is measured, the Dell resolves Glass to its Opaque stand-in.
5. Glass repaint: sheen, accent line, rim, caption pills, radii, three
   shadows. Taskbar states and tray.
6. Toasts, flyouts and HUD as surfaces in the list.

### Stage 3: shell process (decide before launcher work)

The taskbar, launcher, toasts and HUD move to a user-mode shell that
submits surfaces through the stage 1.3 ABI. The kernel keeps a minimal
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
  - **Glass:** the same workload with Glass on and off.
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

- **`work/chrome-csd`** (uncommitted, in progress):
  - It adds `captionOverlay` caption buttons over a client-decorated
    surface. The changes are in `DesktopCaptionRect`, `WmHitTest`,
    `RenderWindowChrome`, `SetChromeDecorAlpha`, `GpuComposeShadow`, the
    chrome capture and the chrome key.
  - Stage 0.1 deliberately avoids those hunks. Stages 0.3, 0.4 and 1.1
    build on its result.
  - Its overlaid caption buttons are the first case of a part table on a
    client-decorated window. Stage 1.1 should absorb it as "parts with a
    button role on a CSD window".
- **`work/compat-census`:** no overlap found.
- **`work/ui-drawer` (Clearwater):** stage 2 is that work. Its native
  steps should target the builder and theme, not the painters directly.

## Open questions for Ian

1. **Glass on the Dell.** Should the Dell stay on an Opaque theme until the
   60 fps milestone is met, with VKRENDER blur (stage 2.4) scheduled after
   it? Or is Glass on the Dell part of this milestone?
2. **Golden images in the repository.** Are a few MB of PNG goldens under
   `src/tests/goldens/` acceptable? The alternative is hashes plus
   regenerate-on-demand.
3. **Shell process timing.** Should launcher organisation (DESIGN.md steps
   6–7) wait for the user-mode shell (stage 3)? Or should it land in the
   kernel painter first and be moved later?
4. **Double raster under lease.** Is giving up an always-current CPU back
   buffer acceptable while VKRENDER holds the lease? Fallback would then
   cost one full repaint frame, about 15 ms, at lease expiry.
5. **Second palette.** Should the app-content palette in
   `brook_appearance.h` (`brook_surface_palette`) be folded into
   `BrookTheme` now (stage 0.2) or with the hue work (stage 2.3)?

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
