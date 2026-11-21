# Broadwell zero-copy scanout (M6)

**Status (November 2026):** in use on the Dell. Dell images enable `BROOK_BDW_ZERO_COPY_SCANOUT`, and the kernel arms it at boot. The design notes below date from the original implementation.

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

**One ring for all contexts.** The pipelined renderer has one render context per in-flight frame. The ring (`g_ring`, a `DesktopScanoutRing`) and its age history are process-wide: frames are committed in submission order whichever context prepared them. A frame keeps its image *in flight* from submit until it retires, so a second pipelined frame never writes the same image, even while the kernel still reports it free (not armed).

The first Dell run (gen 372) had a per-context ring:
- Only context 0 registered the images. Context 1's registration got EBUSY and it drew into its own output, which also zeroed the stats `images` field.
- Each context had its own history, so images missed the other context's frames.
- Copying showed nothing wrong (only damage is copied), but armed zero-copy scanned out stale taskbar regions: the flicker.

`desktop_scanout_ring_test.c` reproduces the bug (split histories: 194 of 198 presented frames wrong) and checks convergence for the shared ring. The scenarios are:
- cursor, clock overlay and tray blink;
- steady game;
- a mix with a window move and a fallback frame;
- 3 and 4 images, two alternating contexts, and a kernel front/armed model.

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

**Second Dell finding (integration image), which was the coherency risk.** The images' PTEs select PPAT slot 3, but the render-job PAT lease (`intel_bdw_critical.cpp`, `RenderOwner`) changed only slots 0 and 4. The inherited private PAT is `0x03030303/0x03030303` (every slot WB), so render writes to the images went write-back into the LLC, which the display does not snoop. That produced the taskbar flicker, which is in the region rewritten every frame.

hasvk's Gen8 MOCS for external buffers is 0x18 (target cache "L3, defer to PAT"; memory type from the PTE: `src/intel/isl/isl.c`), so the PTE does decide.

**Fix:** a render job that maps an image (`BdwExecSnapshotHasUncached`) sets `BdwRenderRunResult::uncachedScanout`. Its lease then also makes slot 3 UC (`0x00030307`), exactly as the BCS scanout copy's lease already does, and restores `0x03030303` afterwards.

`gpu zerocopy uc on|off` toggles this, for A/B only. `render_jobs_uc` and `render_jobs_wb` in `gpu zerocopy status` count jobs that wrote images under each policy.

**Objective check: `gpu zerocopy verify`** (zero-copy active, idle desktop).
1. Enable the pipe A CRC (primary-plane source, `PIPE_CRC_CTL_A` 0x60050, result 0x60064).
2. Take two CRCs, each after two vblanks.
3. CLFLUSH every line of the front image, which writes LLC-resident pixels back to memory.
4. Take two more CRCs.

| Result | Meaning |
|---|---|
| `coherent` | Stable and unchanged CRCs |
| `stale-memory` | The flush changed what the panel shows |
| `crc-unstable` / `front-changed` | Inconclusive (the desktop changed) |

A CPU read cannot do this alone: the CPU shares the LLC with the GT, so it always sees the newest pixels.

**Third Dell finding (gen 375): renderer park.** This was not a zero-copy or PAT fault.
- **What the log shows:** the renderer logged `DESKTOP_QUAD_UNCERTAIN` from the *submit* branch: `vkQueueSubmit` failed because the kernel refused the execbuffer or hasvk saw its context lost. The renderer then parked in `pause()`.
- **Mechanism in the kernel:** when a job's wait dependency failed, the worker failed the job without running it (`BdwExecSnapshotDependencies==2`). `BdwExecSnapshotMarkFailed` then poisoned the job's context and set a sticky failed bit on every buffer it referenced. One client's failed frame (yquake2 there; Chromium on gen 374) thereby poisoned the compositor job that sampled or waited on it, and every later job touching those buffers.
- **Fixes:**
  - A dependency-skipped job is now finished as *skipped*: its fences fail, so the error propagates, but its context and buffers are not poisoned.
  - The worker logs the first failures of each kind (`gpu-submit: job skipped|failed|unsafe pid=…`).
  - The renderer never samples a GPU offer whose producer fence reports an error (`SYNC_IOC_FILE_INFO`; `DESKTOP_GPU_SOURCE_FAILED`, CPU fallback for that window).
  - Every former `pause()` park logs `DESKTOP_RENDERER_EXIT_FOR_RESTART cause=… detail=…` (the quad VkResult included) and exits with status 76, so the supervisor restarts it. Process exit is the kernel's designed recovery.
- **Caveat:** a restarted renderer cannot re-register the zero-copy ring (one registration per boot), so it uses single outputs and the copy path until reboot.
- **Tests:** `test_intel_bdw_buffer` (skipped versus executed failure) and `desktop_gpu_channel_test` (failed-fence offer excluded, newer good offer selected).
- **Not established:** what made the first client job fail. The new log line names it next time.

**Fourth Dell finding (gen 376).** The recovery worked. The cause was not the dependency poison: `vkQueueSubmit` returned `VK_ERROR_DEVICE_LOST` (-4) with no job failure logged, during the first matched Quake run, with zero-copy unarmed.
- **Why the reason was invisible:** Mesa turns *any* execbuffer error into DEVICE_LOST, and the kernel logged none of its refusal reasons.
- **Diagnostics added:**
  - Every refusal site (handoff reserve/submit, queue full or frozen, arm, decode, prepare) records a reason (`intel_bdw_refusal.h`).
  - The syscall layer logs `gpu-exec: execbuffer refused pid=… errno=… reason=…` and `gpu-handoff: reserve refused …` (first 16, then powers of two), plus `gpu-exec: pid=… reset stats: context reports lost batches` when Mesa is told a context is lost.
  - The renderer's `DESKTOP_ASYNC_RETRY` now names the failing stage. Ring starvation logs `DESKTOP_SCANOUT_STARVED kernel_free=… in_flight=…` and sets EAGAIN, where it previously left a stale errno: the gen 376 `errno=11` was such a failure.
- **Ring re-adoption:** `BROOK_GPU_HANDOFF_SCANOUT_ADOPT` (22) hands the retained images to a restarted renderer. It is allowed only for the owner attached to the handoff, only when every image is idle (`BdwZeroCopyAdoptDecision`, host-tested), and only after any active zero-copy returns to the GOP. The renderer adopts first and registers new images only when none exist (`DESKTOP_SCANOUT_RING images=4 adopted=1`).

**Fifth Dell finding (gen 377): the recurring submit failure, probable cause.**
- **Correction:** `SerialPrintf` lines (including the `gpu-submit: job …` failure log) never reach `hw_logs`, so "no job failed" was not established. All `gpu-exec`, `gpu-handoff`, `gpu-submit` and `gpu-zerocopy` diagnostics now use `KPrintf`, which the bridge log captures.
- **Candidate found in code:** `BdwExecSnapshotReserve` → `BdwGpuBufferBeginGpuUse` refuses a job outright when a referenced client buffer is in its CPU-transition gate (bit 62). `TRY_CPU_READ`, used by waylandd's GPU provider, holds that gate while flushing the *whole* buffer, which takes milliseconds for a 2560×1440 Quake frame. A renderer quad job sampling that buffer in the window gets -16, and Mesa reports `VK_ERROR_DEVICE_LOST`. This matches periodic hitches, happening unarmed (gen 376) and recurring within minutes.
- **Fix:** the execbuffer ioctl waits out a transition before admission (bounded, about 100 × 1 ms).
- **Refusal reasons:** they name the exact buffer state (`BdwExecSnapshotUseRefusal`): sticky-failed, CPU transition, another ticket's output, or count overflow.
- **Sticky-failed client buffers:** the renderer also skips any client buffer the kernel reports sticky-failed (`BROOK_GPU_HANDOFF_BUFFER_FAILED`, 23). This covers a retained offer whose buffer a later job failed on, while the offer's own fence still reports success.
- **Supervisor:** it restarts a renderer that exited with status 76 after 1 s, without backoff.
- **Tests:** `test_intel_bdw_buffer` reproduces the refusal: a job in CPU transition is refused, then admitted once the transition ends, and a sticky-failed buffer is refused and named. `desktop_gpu_channel_test` covers the failed-buffer offer.

**Sixth Dell finding (gen 378): the actual cause of the submit failures.** With `KPrintf` the log named it: `gpu-exec: execbuffer refused … reason=submit: job references the outputs of two handoff tickets`.
- **Mesa side:** hasvk (softpin, Gen8) adds the BO of *every* live `VkDeviceMemory` of the device to each execbuffer (`anv_batch_chain.c`, "Add the BOs for all user allocated memory objects"), not only those the batch uses.
- **Renderer side:** the quad pass cached its target imports, so once a context had rendered into several ring images, every later job referenced all of them. That includes the image the other pipelined context had reserved.
- **Why it only appeared now:** before the ring each context had one output.
- **Fix:** ring targets are imported for the frame only (`output.cache=0`). Any cached render-target import is dropped before such a job.
- **Diagnostics:** `brook_vk_quad_pass_target_imports` reports what is alive, and a failed submit logs `DESKTOP_QUAD_TARGET_IMPORTS`.
- **Host test:** the GPU test (`quad_pass_host_test`, `ring_targets`) checks that no target import survives a retired frame, that cached targets accumulate, and that switching to uncached evicts them.
- **Related, not yet fixed:** cached *source* imports (client buffers) are in every execbuffer for the same reason. The CPU-transition wait and the failed-buffer skip cover the transient case, but an unused cached import of a sticky-failed client buffer would still be refused until the cache evicts it after about 120 submissions.

**Cache modes (runtime, `gpu zerocopy cache flush|uc|wt`, default `flush`).** Gen 377/378: UC render writes cost about +8 ms per compositor job (13.6 → 21.9 ms), so latches slipped to a second vblank (p50 41.5 ms). That cancelled the removed copy.

| Mode | Render writes | Coherency for the non-snooping display |
|---|---|---|
| `flush` | write-back (PPAT slot 3 inherited WB) | before an image is armed, the kernel CLFLUSHes every line in the present regions; an image's first present after activation or a mode change is flushed in full |
| `uc` | PPAT slot 3 leased UC for jobs writing images (previous behaviour) | memory is always current |
| `wt` | slot 3 leased write-through (`0x02`) | memory is always current, reads stay cached |

- **Why CLFLUSH suffices:** it writes a line back from every cache level, including the shared LLC. The GT's writes reach the LLC through the end-of-batch render-target flush that precedes the fence, and the present runs after the fence.
- **Renderer:** while zero-copy is active it presents the regions it actually drew (own damage plus buffer-age repair), so the flushed set covers every written line.
- **Stats:** `gpu zerocopy status` reports `cache_mode`, `flushes`, `full_flushes`, `flush_bytes` and `flush_us` [avg, max].
- **Correctness check:** `gpu zerocopy verify` (pipe CRC before and after a full CLFLUSH) proves coherency in each mode.

**Gen 379 results and follow-ups.**
- **Restarts:** the restart fix held (0 restarts in 4 runs).
- **Cache modes, matched 2-buffer yquake:**

  | Mode | Displayed mean | Compositor job | Notes |
  |---|---|---|---|
  | wt | 31.4 ms | 16.0 ms | latch_frames [1726, 19, 0, 0] |
  | uc | 31.3 ms | 16.0 ms | |
  | flush | 38.6 ms | 16.0 ms | 7.6 ms of CLFLUSH (8.5 MB average) on the gpu-submit worker |
  | off | 33.4 ms | 12.6 ms | |

- **Default is now `wt`; `flush` is opt-in** and runs its CLFLUSH on the `gpu-present` thread (`BdwZeroCopyPrepareScanout`, called from `PresentOnBcs`), not on the CPU0 worker. Images are flushed from the arm request onwards, each in full the first time.
- **About 33 ms displayed with about 98% one-vblank latches:** the live status showed `replaced` 0, so it isn't mailbox pacing. The single gpu-submit worker is saturated: per displayed frame it runs a 16 ms compositor job plus about 1.5 game jobs of about 6.7 ms each, roughly 26 ms. On a 60 Hz panel that alternates 1- and 2-vblank intervals (p50 33 ms). Getting to 60 Hz needs the compositor job back near 12.6 ms (WB with off-worker flush is the candidate) and less worker time per game frame (M4a/M5).
- **`gpu zerocopy verify`** now holds the panel still for the measurement. Presents are accepted and mailboxed but not armed (`BdwZeroCopyRing::PendingArm`, host-tested) until it ends, so the cadence graph no longer makes it inconclusive.
- **Idle:** the overlay's cadence graph changes with every present, so a frame whose only change is the overlay caused the next one forever. Such frames are now presented at most every 250 ms (`overlay_only_skips` in the desktop stats); scene, client and cursor changes are immediate.

**Gen 380: full-draw feedback loop (my bug, from the cache-mode change).** Presenting the *drawn* regions while zero-copy was active also changed the regions the pipeline carries as the next frame's damage (`base.carry = &pending[older].regions`).
- **The loop:** one full draw (a new image of unknown age) made the next frame's damage the full screen, and so on.
- **Armed-run evidence:** `full_draws` +501 against `partial_draws` +40; `presents.full` 250 = `waits_behind_full` 250; `full_forced` only 2.
- **Effects:**
  - the compositor job rose to 16.6 ms (WB and WT alike);
  - flush mode wrote back about 18 MB per present;
  - the pipeline waited behind a full frame on every other frame, so p50 was 33 ms;
  - fewer frames were composited (539 against 706 presents).
- **Fix:** the carried regions stay the frame's own damage; the drawn regions travel separately (`kernelRegions`) to the kernel only.
- **Timelines added:**
  - Desktop stats `timeline`: the last 32 frames as `[start, submit, fence, present_call, presented (µs), image, partial, draw_pixels, commit]`.
  - `gpu zerocopy timeline`: the last 64 presents as `[sequence, image, queued, present_us, arm_us, latch_us, vblanks]`, on the same TSC-based microsecond clock.
  - `gpu zerocopy client-timeline`: the last 64 admitted direct-client
    presents in builds with GPU submission timing. Each row identifies the
    renderer incarnation, its internal frame ID, the DMA-BUF inode and the
    publication worker chosen at admission. Monotonic microsecond timestamps
    mark admission, the cache flush, the display register arm, the first
    confirmed live-frame observation, and release of the kernel's buffer
    hold. The older row's `retired_by` names the new row in a normal
    replacement. Restore and pre-arm failure have no successor. Wayland
    buffer release is later because the renderer still has to poll and
    retire any outstanding GPU use before sending SAFE.


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

## 5. Dell test plan (operator present; staged)

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

## Pipe A cursor plane

The Dell build with zero-copy scanout can opt in with `gpu cursor on`.
`gpu cursor off` restores the compositor cursor. The feature is off at boot.
It needs an active pipe A scanout ring, a current presentation lease, and a
renderer that negotiates cursor-plane version 2 on the overlay cursor query.
The query offers an epoch and suppresses the software layer. The renderer
acknowledges its successful zero-copy present with that epoch. Brook waits for
that cursor-free primary image or a later image in the same epoch to latch
before arming the cursor. Older query formats use software composition.

The BSP submission worker binds two 64×64 ARGB8888 images into persistent
GGTT extents. Both allocations are aligned to 256 KiB for Brook’s checked
extent binder, which also satisfies the stricter VT-d alignment case, and
have safe trailing mappings. Before arming a changed image, Brook
premultiplies alpha and flushes its CPU writes. The other image is reused only
after a subsequent vblank and observation of the new live address. Shape
changes and fully transparent cursors retain ownership of the plane. Cursor
control, base, position and live registers are at `0x70080`, `0x70084`,
`0x70088` and `0x700ac`. Motion changes position and rewrites the same base
to arm the update. The write ordering follows the
[Linux i915 cursor code](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/display/intel_cursor.c).

GOP takeover, lease loss and runtime off disable the cursor plane. The
renderer and compositor resume drawing their pointers after disabled
register readback and two vblanks. Failed hardware transitions count as
fallbacks. The cursor mappings remain pinned for the boot.

On the Dell, use the zero-copy desktop path, enable the cursor, and check
`gpu cursor status`. Expect a bound plane followed by an active plane. Sweep
across a fullscreen client that hides its cursor and over desktop or launcher
chrome. Expect the cursor to hide and reappear without double images or
fallbacks. Move the pointer through the screen edges and check that positions
increase while the cursor image remains stable. Turn the feature off and
restart the renderer to check the handoff back to software. The fake MMIO
test checks rapid alternating images and both handoff directions. QEMU checks
the image boot path. Actual cursor scanout requires a Dell test.

## Client primary-plane scanout

The boot policy remains `desktop present direct shadow`. Only
`desktop present direct on` admits a client scanout request, and it requires
an active zero-copy ring and the current compositor presentation lease.
The first version accepts exactly the Dell panel size, XRGB8888, a zero
source offset, and a focused, visible window whose client area covers the
panel without chrome. The provider socket must match the window creation
identity. A visible pointer needs an active hardware cursor. Linear pitch
must be 64-byte aligned, X-tiled pitch 512-byte aligned, and the kernel
checks the buffer size and Intel tiling metadata. Broadwell uses the
combined DSPOFFSET register and supports linear and X-tiled primary
surfaces. See [Linux i915 primary plane programming](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/display/i9xx_plane.c).

The kernel holds an exclusive GPU buffer transition from admission through
latch-away. The RCS ring flushes render and data caches before the completion
breadcrumb. `gpu zerocopy publish ap` moves the cache flush to a separate
CPU worker so the BSP submission worker can run unrelated jobs. `gpu zerocopy
publish bsp` restores the default. The worker choice applies to new requests.
Both modes flush every client page mapped for scanout, including pitch and
tiling padding, because the display does not snoop the coherent CPU/GT LLC.
The BSP revalidates ownership and binds and arms only after publication
finishes. Cancellation retains the buffer until publication exits the flush.

Three cached GGTT bindings retain references and have
guard PTEs. Eviction checks the old mapping, clears the PTEs, and
invalidates the extent after the buffer has left the display. Uncertain
bindings stay pinned. The per-job display check admits only the published
client address and layout or a known kernel surface.

For a same-boot comparison, hold the GPU clock fixed and sample
`gpu worker stats` in each publication mode after the mode has settled.
`client_scanout.flush` measures the publication stage on its selected CPU.
`bind` and `finish` measure the submission worker, and `total` sums the
three elapsed stages. The totals exclude time between workers. Compare
client scanout counts and frame cadence with the same fullscreen content.
Check alternating static patterns and leave/restore with and without the
hardware cursor. Repeat for linear and X-tiled buffers. The existing
`gpu zerocopy verify` only takes CRCs for the renderer ring. A CRC sampled
from a latched client front is a separate hardware verification step.

The capability `BROOK_GPU_PRESENT_CAP_CLIENT_SCANOUT` advertises ABI
version 1 of the client operations in `src/shared/gpu_output_abi.h`.
Each operation uses `BROOK_SYS_GPU_HANDOFF_CTL` with the unused syscall
arguments set to zero.

| Operation | Renderer contract |
| --- | --- |
| `CLIENT_SCANOUT_PRESENT` | Pass the version, authenticated provider socket, window creation identity, PRIME fd, exact layout, and a nonzero offer cookie. Wait for the producer fence first. Return 0 queues the request. A negative result leaves the offer with the renderer and permits the normal composited path. |
| `CLIENT_SCANOUT_STATUS` | Query a separate versioned status structure. Front, armed, and queued are kernel buffer identities. The last asynchronous error includes the input offer cookie. Retain an accepted offer while its identity appears in any of the three fields. An uncertain state keeps all held offers. |
| `CLIENT_SCANOUT_LEAVE` | Cancel an unarmed request and return to the last known kernel desktop image. Wait for status to clear before releasing held offers or sampling the prior front, then repaint the composited scene in full. |

The kernel tracks only one armed flip. An additional proposal can be
retried after the current latch. A normal fallback restores the last
kernel image. Software takeover and panic restore the linear GOP layout.
A pending client flip remains held until a later vblank confirms the
restore target, including when the target was already in the live register.

The kernel-only pair boots with the existing renderer. Once the renderer
wiring lands, run the following checks on the Dell.

1. Boot with the shadow default. Run an opaque fullscreen client and confirm
   the existing compositor still presents while client present and latch
   counters stay at zero.
2. Enable direct scanout on a zero-copy desktop. Exercise separately
   confirmed linear and X-tiled offers, with a hidden or hardware cursor.
   Expect the ABI's `presents` and `latched` counters to advance and
   the front identity to match the actual provider buffer. Check visual
   geometry, row alignment, and changing content at each layout.
3. Trigger a popup, resize, mode-off, or lease expiry while a client flip
   is pending. Expect `restores` to advance on the known surface's
   latch, with holds persisting until the front and armed identities
   clear. Verify the desktop gets a full repaint and the provider receives
   SAFE only after that latch.
4. Compare compositor work, frame pacing, and cache-flush cost against
   shadow mode over the same fullscreen scene. Verify the panic return to
   GOP from an X-tiled front on a separately authorized test.
