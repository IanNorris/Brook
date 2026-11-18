# App-driven present cadence and direct scanout

**Status (18 Nov 2026, branch `work/app-present`):** stage 1 (app-driven cadence) and the stage 2 decision logic are implemented in the renderer, behind a RAM-only kernel switch that is off at boot. Direct scanout of a client buffer is evaluated and counted but never performed: the kernel cannot scan out client buffers yet (section 4). Host tests and the QEMU image check pass. Nothing has run on the Dell.

**Decision (Ian, 18 Nov, DECISIONS.md "App-driven present cadence"):** a focused, visible window covering more than 60% of the output's pixel area takes over the present cadence. When it exactly covers the output with an opaque, scanout-compatible buffer, it is scanned out directly.

**Why:** Stardew Valley fullscreen at 3200×1800 through Xwayland (gen 438, `display-perf/build/dell/stardew-438-fs/`) shows 32.5 displayed fps with the GPU 95% busy. Each frame costs the game about 14.2 ms and the compositor about 14.2 ms. The compositor job redraws one opaque fullscreen window: 780 of 780 presents were full quad frames, 5.76 Mpx each.

## 1. Where the cadence was decided

The renderer (`tools/vulkan-compositor-pkg/desktop_renderer.c`, `run_buffered`/`render_frame`) has no timer of its own:

- It polls the desktop every 16 ms (`g_idlePollNs`) and wakes at once when a GPU offer arrives on the provider channel.
- Each poll captures the layers, then computes change-based damage against the displayed frame, or the latest in-flight frame when pipelined (`desktop_track.h`).
- Any damage is drawn and presented at once. The exceptions:
  - overlay-only changes are held to every 250 ms;
  - when nothing changes, a 1-pixel keepalive renews the lease every 200 ms.

So every pointer move, overlay change or popup commit costs its own composition. With the zero-copy ring that is expensive during a game: the image it draws into is older, so buffer-age repair redraws the game's last frames, roughly a full frame for a fullscreen game. The focused window and its geometry live in the kernel WM. The layer export carries geometry but not focus.

## 2. Stage 1: app-driven cadence

**Kernel.**

- `BROOK_DESKTOP_FOCUS_QUERY` (op 15 of `BROOK_SYS_DESKTOP_LAYERS`) returns `struct BrookDesktopFocus` (`src/shared/desktop_layers_abi.h`):
  - the focused window's id and incarnation;
  - its `BROOK_WINDOW_STATE_*` flags (`WmSnapshotFocus`);
  - the policy bits and the deferral.
- The policy is set by the debug command `desktop present ...` (`net.cpp`, `desktop_present_policy.h`). It lives in RAM only.

**Renderer** (`desktop_app_present.h`, pure; `focus_query` and the block after the damage computation in `render_frame`). Every frame:

1. **Dominance.** `desktop_app_dominant` decides whether a window drives the cadence. It must be:
   - the focused window;
   - visible, not minimized and present in the layer list with the same incarnation;
   - fully opaque;
   - covering **more than 60%** of the output with its client rectangle, clipped to the output. Exactly 60% does not count.
2. **App changed.** `desktop_app_window_changed` asks whether that window's content differs from the baseline frame: a new GPU commit, or changed CPU pixel bands.
3. **Cadence.** `desktop_app_cadence` decides only for frames that have damage:

   | Situation | Action |
   |---|---|
   | No dominant window | `free`: the old behaviour |
   | A different window became dominant | `transition`: present at once |
   | The dominant window committed | `present` (any waiting damage rides along) |
   | Only non-app damage | `defer` until the next app commit, at most **12 ms** (`DESKTOP_APP_DEFER_NS`, under one 60 Hz refresh; `desktop present defer <ms>` overrides it) |
   | Non-app damage waited the full deferral | `deadline`: present it |

   A deferred frame is an idle poll. `idle_until` ends the wait at the deadline instead of the next 16 ms poll, and a GPU offer still wakes the renderer at once. A frame without damage clears the wait.
4. **Frame callbacks.** When the frame that samples the dominant window's GPU offer is submitted, `desktop_gpu_channel_notify_early` sends PRESENTED to the provider at once, instead of after the OS present.
   - PRESENTED only releases frame callbacks.
   - The presented watermark, the current source and SAFE still wait for the real present, so buffer reuse is unchanged.
   - If that frame then fails to present, the client gets one early callback, never a buffer, and no second PRESENTED is sent.
   - Xwayland windows already get their callbacks at offer time (`waylandd` `XWAYLAND_FRAME_MIN_US`). This helps native Wayland clients.

**Defined behaviour:**

| Case | Behaviour |
|---|---|
| Popup or menu over the dominant window | It is a separate layer. Its commits are non-app damage: they ride the next app frame or present within 12 ms. If the popup takes focus it is small, so no window drives and the cadence is the desktop's own. |
| Pointer movement | Non-app damage. At 60 fps app commits it moves with every app frame. With a slow or stalled app it presents at the deadline, so it lags by at most about 12 ms plus one frame. |
| Taskbar, clock, notifications | Overlay damage, also non-app. The existing 250 ms overlay-only limit still applies. |
| Focus loss | Dominance ends at the next frame and waiting damage presents at once (`free`). A newly dominant window presents at once (`transition`). |
| Two large windows | Only the focused one can drive. A large unfocused window never does. A small focused window over a large one means no window drives. |
| Maximized window (stops at the taskbar) | Drives, if it covers more than 60%. It is never scanned out directly. |
| Translucent dominant window | Never drives: the desktop behind it shows. |
| Older kernel (query refused) | `focus_query=0` in the stats and the old cadence. |
| Untracked frame (no damage baseline) | Drawn in full as before; no decision. |

## 3. Stage 2: direct scanout decision

`desktop_direct_eligible` (pure) runs for every presented frame while the direct mode is `shadow` or `on`. It returns the **first** blocker, in this order:

| Reason | Meaning |
|---|---|
| `no_dominant` | no window drives |
| `not_covering` | client rectangle is not exactly (0,0,3200,1800), or the VFB is not 3200×1800 (windowed or maximized) |
| `chrome` | window chrome has pixels |
| `cpu_source` | drawn from the CPU snapshot, not a GPU offer sampled this frame |
| `layered` | more than one offer layer |
| `scaled` | buffer not the output size 1:1 (source crop, destination size or offset) |
| `format` | not XRGB8888, or ARGB8888 without the client's opaque flag (the gen8 primary plane has no alpha; opaque ARGB scans out as XRGB) |
| `tiling_y` | Y-tiled |
| `stride` | nonzero offset; pitch below width×4 or above 32 KiB; not 64-byte aligned (linear) or 512-byte aligned (X-tiled); buffer smaller than pitch×rows (X-tiled rows round up to 8) |
| `window_above` | any later layer with on-screen area (popup, menu, dialog) |
| `overlay` | a visible shell overlay band |
| `cursor` | pointer visible on the output. Brook has **no hardware cursor plane**, so the frame composites while the pointer shows. Games that hide it (Quake II with relative pointer; Stardew draws its own) are unaffected. |
| `no_ring` | zero-copy scanout not active |
| `kernel` | the kernel cannot scan out client buffers |

**Plane rules** (Linux i915 `i9xx_plane.c` for gen8: `ivb_primary_formats`, `i9xx_format_modifiers` {X_TILED, LINEAR}, `hsw_primary_max_stride` = min(8192×cpp, 32 KiB)):

- formats: XRGB8888 (also XBGR8888, RGB565, C8 and 10-bit; none with alpha);
- tiling: linear or X-tiled, never Y (Y is Skylake and later);
- linear stride 64-byte aligned;
- X-tiled stride a multiple of the 512-byte tile row.

The Xwayland tiled buffers (`dmabuf_tiled_for`) qualify only when X-tiled.

**Modes:**

- **`shadow`** (the boot default) answers "would this frame scan out given a capable kernel".
- **`on`** uses the real capability. No kernel reports it, so every frame composites and counts `kernel`.

**State machine** (`DesktopDirectMachine`, host-tested, not wired: it needs the kernel path):

- **Hold.** A client buffer presented directly is held: no SAFE to the provider.
- **Release on latch.** The hold is released only once the kernel reports the buffer neither front, armed nor queued. That is a latch away from it.
- **Mailbox.** A buffer replaced in the mailbox before it was armed was never fetched, so it is released at once.
- **Leaving.** Leaving direct scanout (popup, resize, focus loss, format change: any ineligible frame) moves to `LEAVING`:
  - the next composited frame is drawn in full, because the ring images missed every direct frame;
  - client buffers stay held until that image has latched;
  - `COMPOSITING` is reached only when both are true.
- **Uncertain.** While the kernel reports the scanout state uncertain, nothing is released.
- **Limit.** At most 6 buffers are held. A seventh is refused, and that frame composites.

## 4. What the kernel still needs (not implemented)

Presenting a client buffer through the zero-copy ring needs these kernel pieces, each with Dell risk. They touch the scanout/present path, not the submission path being changed in parallel. They are deliberately not started here.

1. **Retirable GGTT extents.** Today the ring binds its 4 images once per boot (`VmmDellFlipGgttBind`), and an extent can only be retired through the `Io` object that bound it. A client buffer must be:
   - bound when first presented (5,625 PTEs plus guard, 256 KiB aligned);
   - kept in a small cache, for example the 3 to 4 swapchain buffers;
   - unbound only when `Quiescent` for that slot and the buffer is destroyed or evicted.
2. **Coherency.** The client renders write-back into the LLC, and the display does not snoop it. The options are:
   - mark the buffer a scanout candidate, so the client's render jobs get the WT/UC PPAT lease as ring images do (needs Mesa Iris to use the "use PTE" MOCS for exported buffers; unverified);
   - CLFLUSH the whole 22 MiB per frame: measured at about 7.6 ms for 8.5 MB in `flush` mode, far too slow.

   `gpu zerocopy verify` (pipe CRC before and after CLFLUSH) is the check.
3. **Plane programming.** The plane needs:
   - DSPACNTR tiling bit 10 and DSPASTRIDE per buffer, with DSPATILEOFF or DSPALINOFF 0;
   - both latched with the DSPASURF write;
   - the GOP restore re-establishing linear mode and stride 12800.
4. **Policy.** The registration rule "never a user-mappable buffer" must allow a client buffer the compositor names in a present. The kernel should check that the buffer belongs to the window's provider (`WmGpuSourceProviderMatches`) and that the window is focused and fullscreen.
5. **ABI.** The ABI needs:
   - a present op naming the client dmabuf and its offer identity;
   - a `BROOK_GPU_PRESENT_CAP_CLIENT_SCANOUT` capability;
   - front/armed/queued buffer identities in `SCANOUT_STATUS`, feeding `desktop_direct_release`;
   - the renderer calling `desktop_direct_present`/`composite`/`release` and holding the offer from SAFE (`desktop_gpu_offer_in_use` would also check the hold).

A hardware cursor plane (CURACNTR/CURABASE/CURAPOS, 64×64 ARGB on gen8) would remove the `cursor` blocker for games that show the pointer. It is a separate, smaller project.

## 5. Switches and counters

| Command (debug channel) | Effect |
|---|---|
| `desktop present` / `desktop present status` | report |
| `desktop present app on` / `app off` | stage 1 cadence |
| `desktop present direct off` / `direct shadow` / `direct on` | eligibility counters off, shadow (boot default), or real (falls back with `kernel`) |
| `desktop present defer <ms>` | non-app deferral, 0 to 100 ms; 0 = 12 ms default |

The env vars `BROOK_RENDER_APP_PRESENT=0|1` and `BROOK_RENDER_DIRECT_SCANOUT=off|shadow|on` pin the renderer and override the kernel.

**Counters** in `/tmp/brook-desktop-stats.json`, shown by `scripts/desktop_stats_diff.py`:

- **`app_present`:**
  - `mode`, `focus_query`, `defer_us`;
  - `dominant`, the window driving now;
  - `early_notifies`;
  - `dominance`, a reason per frame with damage (deferred re-evaluations included);
  - `actions`: free, present, deadline, defer, transition.
- **`direct_scanout`:**
  - `mode`, `evaluated`, `eligible`;
  - `frames`, the frames actually scanned out (always 0 for now);
  - `enters`/`exits`, eligibility edges;
  - `last`, and `fallback` by reason.

## 6. Dell test plan

Deploy the pair. Zero-copy arms at boot (auto-arm). In each case take the desktop stats before and after a 20 to 30 s window, and diff them.

1. **Baseline**, nothing armed (`desktop present` reports `app_cadence:false`, `direct:"shadow"`).
   - **Stardew fullscreen.** Expect:
     - `dominance` mostly `driven`;
     - `actions` all `free` (cadence off);
     - direct `eligible` high if the pointer is hidden, otherwise `cursor`.

     Record the fps (around 32.5 at gen 438).
   - **Stardew maximized.** Expect `driven`, and direct `not_covering`.
   - **Quake II windowed** (2560×1440 at 2×). It covers 64%, so expect `driven` and `not_covering`.
   - **Quake II fullscreen.** Expect `driven`, and `eligible` unless the buffer is Y-tiled (`tiling_y`) or scaled (`scaled`).
2. **`desktop present app on`**, the same four cases.
   - **Expect:**
     - `actions.present` close to the game's displayed fps;
     - `defer` rising while the mouse moves;
     - `deadline` low while the game runs and rising when it is paused;
     - `presents.completed` at or just above the game fps (no extra pointer-only frames);
     - the per-window `displayed` fps equal or better;
     - `early_notifies` rising for native clients only.
   - **Interaction:** move the mouse over the paused game (the pointer must stay smooth, at most about 12 ms late), open a menu or popup, Alt-Tab away and back (`transition`), and open the launcher.
   - **Compare** compositor job time per displayed frame in `gpu worker stats` against step 1.
3. **Rollback:** `desktop present app off` (live), or reboot.

The decision criterion for keeping stage 1 on is the same or higher displayed fps for the four cases, no visible pointer stutter on an idle desktop, and no rise in `presents.failed`.
