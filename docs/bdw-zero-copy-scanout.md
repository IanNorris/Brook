# Broadwell zero-copy scanout (M6)

**Status:** phase 1 is implemented behind `BROOK_BDW_ZERO_COPY_SCANOUT` (OFF by default; it requires `BROOK_BDW_SCANOUT_FLIP`) and a RAM-only runtime gate, `gpu zerocopy arm`. Host tests and a QEMU image check pass. It has not been run on the Dell. Branch `work/zero-copy`.

**Goal:** the display (pipe A, plane A, `DSPASURF`) scans out the Vulkan desktop renderer's own output image, so a present costs a register write instead of a BCS copy.

Why it matters:

| | gen 369 (BCS copy) | M3 gen 370 (copy-based flip) |
|---|---|---|
| Present per frame | about 10.9 ms (blit 7.9 ms) | 16.3 ms |
| Displayed interval (2 buffers) | 33.2 ms | 39.5 ms |

M3 proved the display side on the Dell: probe, GGTT takeover of inherited PTEs, vblank-latched `DSPASURF` writes, GOP restore. But it still copied. This design removes the copy.

## 1. Findings: how the renderer's output reaches the display today

**Allocation.** `tools/vulkan-compositor-pkg/desktop_renderer.c` `context_prepare` allocates one output with `BROOK_SYS_GPU_OUTPUT_ALLOC(width*height*4)`. The kernel side, `BdwGpuBufferCreateHandoff`, makes a protected buffer:
- `userMappingAllowed=false`; CPU-unmappable;
- zeroed, then CLFLUSHed;
- allocated in 256-page chunks, so it is not physically contiguous;
- tracked by a page DMA array.

The renderer imports it into Vulkan as an external buffer, not an image. It is linear, the stride is `width` words (3200 × 4 = **12800 bytes**, 64-byte aligned), and the format is BGRX8888 little-endian. That is byte-identical to the GOP surface, which the BCS copies raw. For the panel mode it is **exactly 5,625 pages**.

**Rendering.** The M1 quad pass draws only this frame's (16-pixel aligned) damage into that **single retained** output. Pipelined frames (two in flight) write the same buffer in order: tickets are dependent reservations, and all GPU work is serialised on the one `gpu-submit` worker.

**Present.** `BROOK_GPU_HANDOFF_PRESENT_REGIONS`/`QUEUE_REGIONS` names the ticket and its damage rectangles. On the worker, `BdwExecuteBcsScanout(source,…)` copies those regions from the output into the GOP (or, with M3, into a kernel flip buffer). The renderer may reuse the output as soon as the copy retires.

**Render cache policy.** `BdwBuildExecAddressSpace` maps every object write-back (PAT 0x80; `BROOK_BDW_COHERENT_RENDER` relies on this). The BCS reads the output through the LLC, so today coherency is free. The Broadwell display engine does **not** snoop the LLC. A scanned-out buffer must therefore be written uncached, as i915 does with `I915_CACHE_NONE` in `pin_to_display_plane`.

**Cursor and overlays.** The cursor is its own quad layer in the renderer since M1. The overlay bands (clock, taskbar indicators) are renderer layers too. Everything on screen is composited into the output. The only exceptions are CPU writers that target the GOP directly: the display remap and the clock overlay fallback. Those are invisible while an image is front, the same risk M3 noted.

## 2. Design

### Ring of renderer images (≥3, default 4)

The renderer allocates **4** panel-mode outputs (`BROOK_RENDER_SCANOUT_IMAGES=3|4|0`) and registers them once with `BROOK_GPU_HANDOFF_SCANOUT_REGISTER`.

**Why 4.** At any moment one image is front, one may be armed (written, latching at the next vblank) and one queued behind it. The renderer has up to two frames in flight. Four images let the renderer keep preparing while a flip is pending. With three, it waits up to one vblank for a latch (`scanout_pick` polls `SCANOUT_STATUS` for up to about 25 ms).

**Registration** (`BdwZeroCopyEligible`, pure, host-tested) refuses:
- a ring smaller than 3 or larger than 4;
- a mode other than 3200×1800 with stride 12800;
- a format other than XR24;
- a wrong size (exactly 5,625 pages);
- duplicate identities;
- **any user-mappable buffer.** A client could otherwise put arbitrary pixels on the panel outside the compositor.

The kernel also requires that the buffer has **never been used by the GPU** (`BdwGpuBufferMarkScanoutImage`). Its CPU zeroing was flushed at creation, so no write-back line can later land on top of uncached render writes.

**Lifetime (phase 1).** Registration is once per boot and the kernel retains the buffers for the boot. `BdwGgttExtentBind` can only retire an extent through the same `Io` object that bound it, and the M3 binder builds that `Io` on the stack. A retirable binder is phase 2. Memory cost: 4 × 22 MiB.

### Kernel state machine: `BdwZeroCopyRing` (`src/kernel/src/intel_bdw_zero_copy.h`)

Slot 0 is the GOP (GGTT 0); slots 1..n are the images. It has the same latch semantics as M3's `BdwFlipChain`: the frame counter is sampled before the `DSPASURF` write, the latch is proven by the counter advancing plus `DSPASURFLIVE`, and three frames without that latch freeze the ring (Uncertain). There is no staleness tracking; that is the renderer's job.

- **`BeginWrite(slot)`** is called at handoff **reservation** through `BdwZeroCopyOnReserve`, from `BdwHandoffReserve`. It refuses an image that is front, armed, queued or already being written, so the GPU never writes an image the display may fetch. The reservation then fails and the renderer's frame is a safe retry.
- **`Present(slot)`** happens at present, on the worker (`BdwZeroCopyPresentSource`, called from `BdwExecuteBcsScanout`). It returns:
  - **ArmNow**, after which the worker writes `DSPASURF`;
  - **Queued**, behind the armed flip. This is a mailbox: a newer present replaces the queued image, which the display never fetched, and frees it.
- **Release on latch, not on submit.** An image becomes free only when a later latch makes another slot front. `FreeMask()` is what `SCANOUT_STATUS` returns.
- **`AbandonWrite`** runs at ticket release without a present (`BdwZeroCopyOnRelease`, from `BdwHandoffReleaseFree`). The image becomes free and its kernel age becomes unknown.
- **Polling.** The worker polls latches between jobs (`BdwScanoutFlipPoll`, about every 1 ms) and arms the queued image after each latch.
- **Leaving.** `RequestGop`, `Quiescent` and `GopObserved` handle leaving zero-copy and recovering from Uncertain.

### Buffer age in the renderer (`desktop_buffer_age.h`, host-tested)

Each image must be complete when presented. When the renderer draws frame n into image i, which last held frame k, it draws the union of:
- frame n's own damage;
- the own damage of frames k+1..n−1, from an 8-frame history.

If the age is unknown or older than the history, it draws in full. Frames are committed in submission order, at submit. A refused present (ESTALE, pipelined) does not invalidate an image: its content is still a valid desktop state. A frame that did not write the image (the batch/compute fallback output) resets all ages, so the next use of every image draws in full. `desktop_buffer_age_pick` chooses the youngest free image.

**Presents still name only the frame's own damage.** That is exact for the kernel's BCS fallback, whose GOP holds the previous frame, and the zero-copy path ignores the rectangles.

**Cost.** The kernel no longer copies the frame. The renderer pays only the repair, the parts of earlier frames' damage outside this frame's own damage:

| Case | Extra drawing |
|---|---|
| Steady game | The window region (2560×1440 at 2×) changes every frame, so the union equals this frame's damage. Extra: **0 pixels**, against today's ~14.7 MB copy per frame. |
| Cursor or clock moved 1–2 frames ago | Extra: that small rectangle (about 48×48 for the cursor), drawn again into the older image. |
| Worst case | A different large region changes every frame. At ring age 3, the draw is the union of three frames' damage, bounded by the full screen. That is the same as one full frame, so still no copy. |

The renderer reports `scanout.full_draws`, `partial_draws` and `repair_pixels` in `/tmp/brook-desktop-stats.json`.

### Coherency

Registered images are mapped **uncached** in every render PPGTT (`BdwExecMemoryObject::uncached`, which selects `BdwPpgttCache::Uncached`, PTE 0x18). This is the same leased-PAT index M3's BCS writes use.

Mesa (hasvk) programs *external* buffers with the "use PTE" MOCS on Gen8, so the PTE decides cacheability. The render cache flush at the end of the batch (`PIPE_CONTROL`, before the breadcrumb) then lands the pixels in memory before the present can run.

This is the part most likely to need Dell evidence: stale or garbled 64-byte lines on the panel mean the MOCS is not "use PTE". A second risk: uncached render-target writes, and any destination reads during blending, could slow the quad pass.

### Present path and fallbacks

`BdwZeroCopyPresentSource(source)` runs after the existing BCS admission. Its outcomes:

| Situation | Outcome |
|---|---|
| No registration, or source not an image while inactive | Copy (unchanged) |
| Image, gate off or the M3 flip armed | **Copy from the image**: exact, because the image is complete |
| Image, gate on, first time | Bind the GGTT extents (M3 takeover binder `VmmDellFlipGgttBind`, 5,625 + 128 guard PTEs, 256 KiB aligned), configure a ring with front = GOP, publish the surfaces for the per-job display invariant and panic restore, then present |
| Image, active | **Presented**: arm or queue, no copy, no BCS device time |
| Non-image present while active (renderer fallback path), or gate turned off | Leave: restore the GOP with a bounded wait for the latch (≤50 ms), return RepairRequired (the compositor repaints in full). The next image present re-activates if armed. |
| Latch uncertain, plane changed, an image presented that was not reservable, a bind failure | Disabled for the boot: GOP re-armed, RepairRequired once, then the BCS copy |

**Software takeover** (lease expiry or renderer exit): `BdwScanoutFlipAwayFromGop` reports the image front, the compositor repaints the GOP in full, and `BdwScanoutFlipReturnToGop` re-arms the GOP and deactivates the ring. The M3 flip and zero-copy exclude each other: each arm refuses while the other is requested or active.

### Panic, reset and recovery

These are unchanged from M3. `BdwScanoutPanicRestore` writes `DSPASURF=0` and waits up to 50 ms. It is published at activation, as are the image surfaces. A recovery boot starts from firmware.

## 3. ABI (`src/shared/gpu_output_abi.h`, mirrored in the package)

- **`BROOK_GPU_PRESENT_CAP_SCANOUT` (8):** reported only by kernels built with the option.
- **`BROOK_GPU_HANDOFF_SCANOUT_REGISTER` (20):** argument `struct BrookGpuScanoutImages {count,width,height,stride,fourcc,reserved,fds[4]}`. Returns 0, -EINVAL (refusal; reason in `gpu zerocopy status`), -EBUSY (already registered, or a buffer was used) or -EBADF.
- **`BROOK_GPU_HANDOFF_SCANOUT_STATUS` (21):** argument `struct BrookGpuScanoutStatus`:
  - `flags` = REGISTERED|REQUESTED|ACTIVE|DISABLED;
  - `freeMask` (bit i = image i). While inactive, every image is free;
  - `front`/`armed`/`queued`: slots 1..n, 0 = GOP, ~0 = none;
  - `sequence`, `presents`, `latched`, `missedVblanks`.

## 4. What is implemented, and what is pending

**Implemented (phase 1):**
- **Kernel policy and wiring:**
  - the pure policy and its host test (`test_intel_bdw_zero_copy`);
  - the `scanoutImage` buffer flag and the never-used check, plus uncached render mapping;
  - registration and status syscalls;
  - the reserve/release hooks;
  - the worker present hook;
  - GGTT bind, arm, poll and mailbox;
  - the leave/disable/takeover paths;
  - M3 exclusion;
  - `gpu zerocopy arm|off|status` with stats (presents, armed_now, queued, replaced, latched, missed_vblanks, latch_us, latch_frames, refused_reserve, fallback by reason, GGTT diagnostics);
  - build option and verifier flag.
- **Renderer:**
  - cap detection;
  - 4-image ring registration (only at 3200×1800);
  - image pick and wait;
  - buffer-age draw damage in the quad path;
  - commit and reset;
  - ring stats.
- **Default kernels** (option OFF) are unchanged; the renderer then keeps its single output.

**Pending (phase 2+):**
- Retirable GGTT extents (re-registration after a renderer restart; today zero-copy is lost for the boot after a context reopen).
- Waking the renderer on a latch instead of polling `SCANOUT_STATUS`.
- Presenting without a worker round trip.
- Tiled (X) images for faster rendering.
- Buffer age in the compute/graphics fallback paths (they reset the ring today).
- A display CRC or readback check of coherency.

## 5. Dell test plan (Ian present; staged)

Build: `nix-shell --run 'python3 scripts/build_dell_image.py --payload-id dell-zero-copy-<date> --output build/dell/pair-zero-copy --kernel-option BROOK_BDW_SCANOUT_FLIP=ON --kernel-option BROOK_BDW_PERSISTENT_SCANOUT_MAP=ON --kernel-option BROOK_BDW_ZERO_COPY_SCANOUT=ON --qemu-check'`. Deploy with `scripts/dell_deploy.py` (BROOK_ROOT = the registry holding the session).

1. **Boot, nothing armed.** The desktop must look and perform as on gen 371.
   - `gpu zerocopy status`: `built:true`, `registered:true`, `images:4`, `active:false`, `registration_refusal:"none"`.
   - Renderer stderr shows `DESKTOP_SCANOUT_RING images=4`.
   - Desktop stats `scanout.picks` rising and `full_draws` small.
   - The matched Quake demo (`scripts/dell_quake_measure.py … A,C`) should match gen 371 within noise. This is the uncached-render cost check: compare compositor job time in `gpu worker stats`.
2. **Probe:** `gpu flip probe` must pass.
3. **Idle desktop: `gpu zerocopy arm`.** Move the mouse.
   - Serial shows `gpu-zerocopy: active; 4 images …`.
   - Status: `active:true`, `stats.presents` and `latched` rising, `latch_frames[0]` dominant, `missed_vblanks` low, `uncertain:0`, `fallback` mostly zero.
   - **Visually** (panel or photo; bridge capture reads the GOP): no stale tiles, no tearing, cursor smooth.
4. **Quake demo with zero-copy active.** Compare against the same boot after `gpu zerocopy off`:
   - displayed interval (desktop stats);
   - `gpu worker stats` present class time (it should drop by about 10 ms per frame), idle, compositor job time;
   - renderer `scanout.waits` / `starved`.
5. **Transitions.**
   - `gpu zerocopy off`: one full repaint, then the BCS path; `gop_returns` increments. Then `arm` again.
   - Lease expiry: stop the renderer job briefly if a safe tool exists, or close all windows and idle more than 500 ms. The desktop stays correct.
6. **Panic restore:** as in M3 stage 6, on a zero-copy-active boot.

**Rollback:** `gpu zerocopy off` (live), a reboot (gates reset), or promoting the previous slot.
