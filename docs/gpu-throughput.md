# Full-screen GPU throughput (maximised Chrome), 15 Nov 2026

Gen 401 (`dell-integration8b-20261115`), Chrome maximised at 3200×1712 playing YouTube with software decode. Ian saw about 140 ms per frame.

## What the frame costs

- **GPU worker over about 11 s, zero-copy not armed:**

  | Job class | Jobs | Time | Per job |
  |---|---|---|---|
  | app | 112 | 3.34 s | ~30 ms |
  | compositor | 159 | 4.74 s | ~30 ms |
  | present | 129 | 1.91 s | ~15 ms (BCS copy) |

  The worker was idle for only 1.1 s, so it was saturated.
- **Renderer `last_frame`:**
  - `damage_pixels`: 5,760,000, i.e. the whole screen on every frame (`partial: 0`);
  - `pixels_shaded`: 12,799,488, about 2.2× the screen;
  - `layers_drawn`: 6;
  - `cpu_submit_to_fence`: 33.5 ms.
- **Live arm of zero-copy (wt) with Chrome still playing:** compositor jobs measured **70.7 ms**. The likely reason is that Chrome's layer is drawn as translucent, so every pixel uses `READ_DESTINATION`, an input-attachment read. With zero-copy the destination is a write-through scanout image, so those reads are much slower. The same arm also preceded a compositor job failure (site=2, no CSB events), which the worker agent is investigating.

## Cause 1: Chrome's buffer is treated as translucent

waylandd ignores `wl_surface.set_opaque_region`, so an AR24 window is blended over the wallpaper and the window chrome. The multilayer agent (`work/multilayer`) is adding opaque-region tracking and the opaque flag on offers. `quad_plan` already culls everything beneath an opaque layer and draws it as `QUAD_KIND_REPLACE`, with no blending and no destination read. That should take the frame to about 5.8 Mpix shaded with no destination reads, and should matter even more when zero-copy is armed. This branch deliberately doesn't touch it.

## Cause 2: full-frame damage

waylandd discards client damage (`surface_damage` and `surface_damage_buffer` are no-ops), and the renderer damages a GPU-source window's whole client rectangle on every new commit (`desktop_track.h`). Chrome reports the video rectangle (plus the progress bar) as damage; the rest of the page doesn't change.

**Plan:** carry the client's buffer damage on the GPU-window wire, bounded to a few rectangles plus a bounding fallback. The renderer then damages `client rect ∩ (damage translated to the destination)` instead of the whole client rectangle. With zero-copy, buffer age already unions the last N frames' damage per image, so this composes.

This belongs in the v3 `OFFER_LAYERS` wire the multilayer agent is defining, so it's specified here but not implemented, to avoid two conflicting ABI changes. Expected gain: the share of composite cost that scales with pixels, roughly the window area divided by the video area (about 1.5–2× in maximised YouTube).

## Direct scanout of the client buffer

**When it applies:** a single opaque window covers the whole plane, for example YouTube fullscreen at 3200×1800. A maximised window doesn't qualify: the 88 px taskbar is still composited, so it needs an overlay plane instead.

**Format:** Chrome's buffers are linear AR24/XR24 with a 12800-byte stride, which plane A can scan out (stride a multiple of 64; the same layout as the zero-copy images). Gen8's primary plane supports only linear and X-tiled, not Y-tiled.

**Mechanism:** reuse zero-copy's flip path (the M3 DSPASURF write and latch tracking, plus GGTT extent binding with takeover). The present names the client's buffer instead of a ring image.

**What's missing:**
1. **GGTT-binding a user-mappable buffer.** The zero-copy rule today is "never scan out a user-mappable buffer". The real requirement is lifetime, not secrecy: hold a buffer reference and keep the client from reusing the buffer (by withholding its release) until a newer flip latches. waylandd already holds offered buffers until SAFE, and SAFE would have to wait for the latch.
2. **Switching between direct and composited frames.** The renderer must recomposite into a ring image on the first frame that isn't eligible (a cursor over it, a popup, a toast), and the window manager has to report full-screen opaque coverage.
3. **The cursor.** Gen8 has a hardware cursor plane, which Brook doesn't use yet. A cursor over a direct-scanout window either disables direct scanout or needs the cursor plane.

**Payoff:** the composite drops to zero for fullscreen video and games. This is the single biggest win, but it's a multi-day item (kernel plus renderer plus waylandd), so do it after the opaque and damage fixes.

**Overlay-plane variant for maximised windows:** sprite plane A could scan Chrome's buffer under or over a primary plane carrying the composited taskbar and chrome. Gen8 sprites support RGB and scaling. That's more work again, because it needs plane z-order and blending, which the primary plane can't do over a sprite, plus watermarks.

## Tiled modifiers

Everything is linear today:
- waylandd advertises only `LINEAR` AR24/XR24 in dmabuf feedback;
- the renderer imports and renders with `VK_IMAGE_TILING_LINEAR` (`quad_pass.c` `image_create`);
- the zero-copy ring images are linear.

On Gen8, sampling a linear texture and rendering to a linear target are both markedly slower than X-/Y-tiled, because of cache-line utilisation and sampler efficiency. Chrome/ANGLE (Iris) would pick Y-tiled or X-tiled if offered.

**What tiled needs:**
1. waylandd advertises `I915_FORMAT_MOD_X_TILED` and `Y_TILED` alongside `LINEAR`, accepts them in `params_make_buffer`/`BrookDmabufValidate`, and offers them, which needs a modifier field on the wire (v3 again).
2. The renderer imports with `VK_EXT_image_drm_format_modifier` (hasvk supports X/Y tiled) instead of `TILING_LINEAR`.
3. The kernel keeps tiling as metadata only. GPU sampling of a tiled buffer through PPGTT needs no fences. The `GEM_MMAP_GTT` refusal of tiled buffers stays.
4. **Direct scanout** could only use X-tiled buffers, never Y.
5. The renderer's own outputs (ring images) could become X-tiled, which the display supports: faster render-target writes and still scanout-able. That needs the plane's tiling bit and stride in the flip, plus the BCS copy fallback learning X-tiling. It's a moderate kernel change.

Expected gain: unmeasured on this hardware. Gen8 guidance suggests linear sampling and render targets cost noticeably more bandwidth. It's the right default for client buffers once the wire supports modifiers.

## Zero-copy at boot

This branch adds `BdwZeroCopySetAutoArm`. When it's enabled, the kernel requests arming as soon as the renderer registers its ring. It was gated on the boot marker `/etc/dell-zero-copy-auto` (off by default) until gen 422; since 16 Nov it is **on by default**, with `/etc/dell-zero-copy-no-auto` to opt out, after a gen 422 live arm ran 12k+ flips with no GOP fallback. The arm still needs a passing flip probe, `gpu zerocopy off` cancels auto-arm for the rest of the boot, and `gpu zerocopy status` reports `auto_arm`. Making it the default means inverting one test in `BdwGopObserveConfigure`.

## GPU clock headroom

Matched 2-buffer yquake with zero-copy (wt) armed, `gpu clock` 500, 650 and 700, run one after another on gen 401:

| Request | Swaps/s | Compositor | Achieved clock (avg / max) | Package max | Guard |
|---|---|---|---|---|---|
| 500 | 47.0 | 23.2 ms | 499 / 500 | 90 °C | already tripped |
| 650 | 49.3 | 20.7 ms | 559 / 650 | 91 °C | tripped again (trips 3→4) |
| 700 | 47.9 | 21.2 ms | 537 / 700 | 92 °C (cores 95 °C) | tripped again (trips 4→5) |

The package reaches the 90 °C guard under this load **even at 500 MHz**. Requests of 650–700 are cut back to 500 within each run, so the average achieved clock is only about 540–560 MHz, worth about 10% on the compositor. **Recommendation: keep 500 MHz as the default.** Higher clocks won't hold until package power drops: the CPU sits at about 2.95 GHz turbo during the game, and package C-states stop at PC2. The idle governor helps at idle, not under load. Revisit after a P-state/turbo policy, or with a temperature-scaled clock request (step down at 85 °C rather than cutting off at 90 °C).

Side note: compositor jobs in this run, 21–23 ms with zero-copy armed and Chrome's maximised window open, are well above gen 381–386's 11–13 ms. That's consistent with the translucent-layer destination reads above.
