# Broadwell page flip and persistent scanout map (M3)

Status: implemented behind two kernel options that are OFF by default. Each is also gated off at runtime on every boot. Not yet run on the Dell. Branch `work/m3-flip`.

## 1. Findings: how Brook sees the display today

**Discovery.** Brook does no modesetting. `BdwPlanGopPtes` (`intel_bdw_gtt_probe.h`, called from `BdwGopObserveConfigure`) checks the UEFI GOP framebuffer from the boot protocol against the BAR layout. The boot framebuffer is 3200×1800 with a stride of 3200 px, at physical `0xe0000000` (the GMADR aperture). Display MMIO is read only through `BdwDisplayCriticalRead` (`intel_bdw_display_probe.h`). That function double-reads 11 registers per pipe, for pipes A, B and C at `BAR0+0x70000 + pipe*0x1000`:

| index | offset | register |
|---|---|---|
| 0 | 0x70180 | DSPCNTR |
| 1 | 0x70188 | DSPSTRIDE |
| 2 | 0x7019c | DSPSURF |
| 3 | 0x701ac | DSPSURFLIVE |
| 4–6 | 0x70280, 0x7029c, 0x702ac | sprite control, surface and live surface |
| 7–9 | 0x70080, 0x70084, 0x700ac | cursor control, base and live base |
| 10 | 0x701a4 | DSPTILEOFF |

The only consumers were `bridge hardware gpu-display-state` and the per-job invariants (`ExecuteRetainedWorker`, `BdwObserveGopBacking`, and the one-shot diagnostics). The invariants require the exact GOP state. Nothing in Brook ever wrote a display register. Nothing read `PIPE_FRMCOUNT` (0x70040), `PIPEASTAT` (0x70024), PSR, PIPESRC or the transcoder registers.

**Dell evidence.** This comes from the read-only snapshots of generation 34 (5 November) and generation 185:

- Pipe A `DSPACNTR=0x98000000`: enabled, format 0110 (XRGB8888), linear (bit 10 clear), no gamma, no rotation.
- Stride 12800 bytes.
- `DSPASURF` and `DSPASURFLIVE` are both 0, so the GOP surface is at GGTT offset 0.
- Tile offset 0.
- All sprite and cursor registers, and everything on pipes B and C, read 0.
- `BdwGopBackingSnapshot` requires GGTT PTEs 0..5624 to map one contiguous run of stolen memory (inside `0xdd000000–0xdf800000`). The present path re-reads all of them twice on every present.

**Uncertainties (what the new probe answers).**

1. **Pipe and transcoder.** Pipe A's plane is the enabled one. The pipe B/C zeros do not prove those pipes are disabled: power well 2 may be off, and then its registers read 0. On Haswell and Broadwell the eDP panel is driven by the dedicated EDP transcoder (`TRANS_DDI_FUNC_CTL_EDP` 0x6F400, with `PIPECONF_EDP` at 0x7F008). Its input select (bits 14:12) names the pipe. Brook has never read it, so pipe A as the panel source is very likely but unverified. Transcoder A's `PIPECONF` (0x70008) may legitimately read disabled.
2. **PSR.** It is unknown whether the GOP enabled panel self-refresh (`EDP_PSR_CTL` 0x6F800 on Broadwell). PSR can stop the frame counter and changes how updates reach the panel.
3. **Frame counter.** Whether `PIPE_FRMCOUNT_G4X(A)` advances at the panel rate has never been measured. DRRS could change the rate.
4. **Scanout from high GGTT.** Brook's GGTT space starts at index 65536 (offset 256 MiB), above the 256 MiB mappable aperture. I believe Broadwell display can scan out from any GGTT offset; i915 needs the mappable range only for fenced CPU access. This has not been verified on this machine.

Register offsets follow Linux i915: `i9xx_plane_regs.h`, `intel_display_reg_defs.h` and `intel_psr_regs.h`.

**GGTT ownership.** `BdwGgttSpace` hands out indices from 65536 upward only:

- BCS retains 8 entries at 65536;
- RCS retains 29 entries at 65544 (and the rebased context at 65573);
- the present path reserves its own 8.

Firmware owns 0..5624. `BdwGgttExtentBind`/`BdwGgttExtentMmio` (large extents) and `BdwScanoutGgttMapping` (VT-d guard, 256 KiB alignment) existed as unwired groundwork. The flip path now wires the extent binder.

## 2. Design

### Why blit-once instead of rendering directly

Rendering directly into a kernel scanout buffer (zero copies) would need the renderer to do three things:

- track buffer age and repair damage itself;
- import kernel-owned scanout memory as GPU buffers whose lifetime is tied to flip retirement;
- render the full composite into them.

The renderer is being rewritten in parallel, and the M2 channel ABI hands the kernel a *damage-packed* output buffer, not a full frame. The smaller safe step keeps that ABI unchanged and does the following:

- The kernel owns three scanout buffers (3 × 5625 pages plus 128 guard PTEs each). They are mapped persistently in the GGTT for the display and in persistent BCS leaf tables for the blitter.
- Each present runs **one** BCS batch into the back buffer: first the repair rectangles (copied from the newest complete frame), then the renderer's damage. The blitter executes the batch in order, so newer damage wins.
- Per present, only the batch and the renderer's source pages are mapped into the private PPGTT. The ~5,600 destination PTEs per present and the full GOP PTE re-read are gone.
- Then `DSPASURF` is written. It latches at the next vblank: no tearing, and no CPU writes into a live surface.

The remaining per-present device cost (two BCS resets, the PAT lease and the busy-wait) is M5 territory (persistent contexts).

**Zero-copy contract, for later; nothing implemented.**

1. The kernel exports N scanout buffers as protected GPU buffers (3200×1800 XRGB8888, linear, pitch 12800) with identities.
2. Each present names one buffer and its damage. The kernel returns that buffer's age (the frames since it was last presented) and replies PRESENTED only after the flip *after* it has latched. That is the moment the buffer is free again.
3. The renderer must repair `age` frames of damage itself, never write a buffer the kernel has not released, and treat a `RepairRequired` reply as "the next present must be full".

### State machine (`intel_bdw_display_flip.h`, pure, host-tested)

- **Slots.** `BdwFlipChain` has slot 0 for the GOP and slots 1..3 for kernel buffers.
- **Armed and queued frames.** At most one frame is *armed* (written to `DSPASURF` and not yet latched), and at most one complete frame is *queued* behind it. The queue is a mailbox: a newer frame rewrites the queued buffer, which the display has never seen. Presents therefore never wait for vblank.
- **Latch proof.** The frame counter, sampled before the write, must have advanced, and `DSPASURFLIVE` must equal the armed surface. If three frames pass without that, the chain is marked Uncertain.
- **Reuse.** A buffer is reused only when it is not front, armed, queued or otherwise retained.
- **Staleness.** It is tracked relative to the newest frame, as up to 32 rectangles that collapse conservatively to a bounding box, or as "full".
- **Polling.** The present worker polls the latch from `ScanoutPending` between jobs, about every 1 ms. As soon as the armed frame latches, it arms the queued one.
- **Leaving flip mode.** `gpu flip off` makes the GOP itself the next target: repair, then damage, then arm slot 0.
- **Software takeover.** When the present lease expires, the CPU compositor sees `BdwScanoutFlipAwayFromGop()` and forces a full repaint of the GOP. Only then does `BdwScanoutFlipReturnToGop()` arm the GOP, so the hidden GOP is never shown half-written. If the CPU fallback (`Unavailable`) is reached while a kernel buffer is live, it becomes `RepairRequired`: the present fails and the frame is repainted, because a CPU copy into the GOP would be invisible.

### Runtime verification and fallback

The flip path falls back to the existing BCS copy, and logs `gpu-flip: disabled for this boot (<reason>)`, in any of these cases:

- **The probe refuses.** Probe refusal reasons: `edp-transcoder-disabled`, `edp-input-not-pipe-a`, `transcoder-disabled`, `psr-enabled`, `plane-disabled`, `tiled`, `plane-control`, `stride`, `surface-not-gop`, `surface-not-latched`, `plane-offset`, `other-pipe-plane-enabled`, `pipe-source-size`, `frame-counter-stalled`, `frame-rate`. The frame-rate check accepts 45–125 Hz.
- **The plane changes before any arm** (`plane-changed`).
- **A GGTT bind fails.** The GGTT lives in stolen memory and survives warm resets. On the Dell every entry above the GOP holds leftovers from an earlier OS (gen 186 survey; no zero entries from 66048). The first Dell arm (gen 364) failed with `ggtt-bind` for exactly this reason: the first binder accepted only all-zero ranges. The bind now twice-reads a snapshot of the extent and takes over inherited values against that exact snapshot, as i915 does at load. The extent is above the 256 MiB CPU aperture and outside every Brook reservation, and nothing references it. A failed bind restores touched entries to an owned scratch page. `gpu flip status` reports `activation_step` and a `ggtt` object: stage (`reserve`, `flush-page-audit`, `pte-page-audit`, `snapshot-unstable`, `bind-refused`, `bind-uncertain`, `bound`), buffer, first, count, total entries, the failing PTE page, zero/valid/other counts, and the first nonzero index and value.
- **A copy fails to retire.**
- **The latch is uncertain.**

After a disable, the GOP is re-armed and one full repaint is owed.

## 3. Panic, reset and recovery

- `DisplayPowerPanicRestore()` is called by `KernelPanic` and by every exception and panic path in `idt.cpp`, always before drawing. It now first calls `BdwScanoutPanicRestore()`, which is MMIO-only and lock-free. That function reads a published UC alias of `BAR0+0x70000` and checks that `DSPACNTR` is still the GOP mode. It writes `DSPASURF=0`, then spins up to 50 ms (TSC-bounded, about three frames) for `DSPASURFLIVE==0`. It never writes anything if the plane mode changed, and it is a no-op unless a flip was activated. Panic text and the QR code are then drawn into the GOP framebuffer as before.
- `PlatformPowerIssue()` (reset and power-off: shutdown, guard reboot, the panic recovery reset) calls the same restore first.
- **Recovery kernel.** That kernel, and any later Brook boot, starts from firmware. The GOP driver programs the plane at POST. A boot that finds `DSPASURF≠0` fails the existing strict display checks, so no GPU path runs and the CPU path draws into the GOP. If firmware ever kept a stale surface, that boot would show a stale image. This is only possible after an un-hooked reset such as a triple fault. The risk is noted, not mitigated: a boot-time write was judged worse than the risk.
- **Per-job invariants.** `ExecuteRetainedWorker` and `BdwObserveGopBacking` use `BdwObserveDisplayAccepted`. It requires the exact GOP mode, with surface and live surface either 0 or a *published* kernel buffer. It retries up to 4 times in flip builds, because a latch can land between the two stable reads.

## 4. Options and gates

| | Build option (default OFF) | Runtime gate (RAM only, off at boot) |
|---|---|---|
| Persistent GOP map | `BROOK_BDW_PERSISTENT_SCANOUT_MAP` | `gpu scanout map on` / `off` / `status` |
| Page flip | `BROOK_BDW_SCANOUT_FLIP` | `gpu flip probe` (read-only), `gpu flip arm` (needs a passing probe this boot), `gpu flip off`, `gpu flip status` |

- Both options require `BROOK_SSD_GUARDED_DESKTOP_RESET`.
- To build an image with them: `build_dell_image.py --kernel-option BROOK_BDW_SCANOUT_FLIP=ON`, which passes `--allow-bdw-scanout-flip` to the verifier. Without that, the verifier requires both options OFF.
- The flip uses the persistent GOP leaves internally (as the repair source and the exit target) without enabling the `map` gate.

**Stats.** `gpu worker stats` now has a `scanout` object:

- `targets`: presents through the GOP per-present map, the GOP persistent map, and flip buffers;
- `flip_armed`, `flip_queued`, `flip_latched` and `flip_uncertain`;
- `latch_us` (average and maximum, from the write to the first observed live surface) and `latch_frames` (1/2/3/>3);
- `missed_vblanks`;
- `map_hits`, `map_full` and `map_failed`.

## 5. Tests and what QEMU can cover

**Host tests.** `test_intel_bdw_display_flip` covers:

- every probe refusal, and counter wrap;
- damage collapse;
- rotation and mailbox reuse;
- latch edges, including the counter-before-latch edge;
- uncertainty;
- GOP takeover while a flip is armed;
- leaving through the GOP;
- `BdwFlipRestoreGop` ordering: idempotent, bounded when there is no vblank, and no write after a modeset;
- the persistent-map cadence;
- persistent leaf construction and linking.

`test_intel_bdw_worker_stats` covers the new counters.

**QEMU.** QEMU has no Intel display or GT, so none of the Intel paths run there. The guarded QEMU check (`--qemu-check`) proves the default image boots, repairs and reaches the desktop with the changed code compiled in. It does not prove probe values, the latch, the GGTT bind, display coherency or panic restore on hardware.

## 6. Dell test plan (staged; Ian present for every flip-build boot)

1. **Image with `BROOK_BDW_SCANOUT_FLIP=ON` and `BROOK_BDW_PERSISTENT_SCANOUT_MAP=ON`, no commands issued.** This must behave exactly like the baseline: every gate is off at boot, and no display register is written. Check that the boot is verified and that `gpu flip status` reports `active:false` with `probe.sampled:false`. Rollback: the previous slot, via `hardware_promote_slot.py` from the USB maintenance boot if needed.
2. **Read-only: `gpu flip probe`, about three times, idle and under the Quake demo.** Expect `ddi_edp` bit 31 set, input select 0 or 4, `pipeconf_edp` with bits 31:30 set, and `psr_ctl` bit 31 clear. Expect `dspacntr` 0x98000000, stride 12800, surf and live both 0, and `pipesrc_a` 0x0c7f0707. Expect `frame1-frame0` ≈ 6 over about 100 ms. Also run `bridge hardware gpu-display-state` and compare. Any refusal ends M3 on this design until it is understood.
3. **Persistent map alone: `gpu scanout map on`.** Run the matched demo, then `gpu worker stats`. Check `map_hits` against `map_full`, `map_failed=0`, and the drop in present prepare time. Toggle it off and repeat on the same boot for an A/B comparison.
4. **Flip, idle desktop first: `gpu flip arm`.** Watch serial for `gpu-flip: active`. Check `flip_latched` rising, `latch_frames[0]` dominant, `missed_vblanks` near 0 and `flip_uncertain=0`. Check visually for tearing and stale regions: the bridge frame capture may read the GOP, not the live buffer, so use the panel or a photo. Then let the lease expire (500 ms idle) to exercise the software takeover, and run `gpu flip off`.
5. **Flip under the Quake demo.** Compare displayed fps and present time against the BCS baseline on the same boot (with `gpu flip off` and then `arm` again).
6. **Panic restore.** On a flip-active boot, trigger the existing GPU capture-probe panic, if built in, or another controlled panic. The panic screen and QR code must be visible, and the guarded reset must reach a normal boot.

**Rollback at any stage:** `gpu flip off` (live), reboot (every gate resets), or promote the previous slot.

## 7. Risks

- **Unverified display facts.** Pipe and transcoder routing, PSR, frame-rate stability, and scanout from GGTT ≥256 MiB are all assumptions until stage 2 has run.
- **The 15 ms full repair.** It happens once after activation and once after each software takeover, when buffers start fully stale (it is skipped when the damage itself is full-screen).
- **CPU writers that target the GOP directly** (clock overlay, display remap) are invisible while a kernel buffer is live. The remap path copies the GOP back, which can be stale then.
- **Rare activation failures** (allocation or GGTT bind) or copy failures disable flipping for the boot and force a repaint.
- **Activation runs once on the worker.** It allocates and flushes about 66 MiB, and the GGTT bind (about 17k UC MMIO operations) runs with IRQs off. Expect a stall of about 0.1–0.3 s; `gpu flip status` reports `setup_us`.
- **Device time.** The BCS still pays two resets and a PAT lease per present. A flip removes the copy into a live surface and the destination PPGTT build, not the device overhead.
