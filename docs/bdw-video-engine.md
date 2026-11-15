# Broadwell video engine (VCS0) and VA-API

Status: **phase 1 (1a, 1b) passes on the Dell (gen 393); phase 1c (VCS user jobs, `I915_EXEC_BSD`) is implemented and host/QEMU-tested (pair `pair-vaapi-p1c1`), not yet run on hardware.** Everything stays behind the build option `BROOK_BDW_VIDEO_ENGINE` and a runtime gate that is off at boot. Branch `work/vaapi`.

## Why

Brook's browsers now render on the GPU, but video decode is software (dav1d/ffmpeg on the CPU). Broadwell's fixed-function decoder (the MFX unit behind the video command streamer VCS0) handles H.264, VP8, MPEG-2 and VC-1. It has **no VP9 or AV1** decode, so YouTube must be steered to H.264 to benefit (phase 3).

## Phases

| Phase | Scope | Status |
|---|---|---|
| 1a | VCS0 constants, probe, domain reset, runtime gate, debug commands, explicit `HAS_BSD=0` | done (this branch) |
| 1b | VCS0 HWS/CSB, execlist init, logical context, one-shot submission, `gpu video selftest` | done (this branch), host-tested; untested on hardware |
| 1c | VCS jobs in the submission worker, `I915_EXEC_BSD`, `HAS_BSD=1` once the self-test passes | implemented (p1c1), untested on hardware |
| 2 | libva + intel-vaapi-driver (i965) running `vainfo` and `ffmpeg -hwaccel vaapi` H.264 | scoped below |
| 3 | Chrome VA-API (Ozone/Wayland, NV12 dma-buf into ANGLE), Firefox (ffmpeg VA-API), H.264 preference for YouTube | not started |
| 4 | Soak, hang recovery, A/V sync, multiple streams | not started |

## Hardware facts used (Gen8 / BDW GT2)

- **VCS0 MMIO base 0x12000** (BAR0 page `0xf6012000` on the Dell, already in `BdwOtherEnginePages[1]` and already sampled read-only by the forcewake observation as `other[1]`). There is no VCS1 on GT2 (`HAS_BSD2`=0).
- **Forcewake:** Gen8 (non-CHV) has a **single forcewake domain**, FORCEWAKE_MT (`0xa188` / ack `0x130044`). The render forcewake the driver already takes covers VCS. `FORCEWAKE_MEDIA_GEN9` does not exist on Gen8.
- **Reset:** GDRST `0x941c` bit 2 (`GEN6_GRDOM_MEDIA`, value 4), with the same Gen8 reset-ready handshake on `RING_RESET_CTL` (`0x120d0`) and the MI_MODE stop bit (`0x1209c`) as RCS (bit 1) and BCS (bit 3).
- **Execlists:** same engine-relative registers as BCS: HWSTAM `+0x98`, GFX_MODE `+0x29c`, MI_MODE `+0x9c`, HWS_PGA `+0x80`, ELSP `+0x230`, EXECLIST_STATUS `+0x234`, CSB pointer `+0x3a0`.
- **Logical context:** Gen8 xCS contexts share the BCS layout. `BdwBuildVcsContext64` is the BCS template with each engine-relative LRI register moved from 0x22xxx to 0x12xxx; the host test checks exactly those 22 dwords differ.

## Phase 1a: what is implemented

- **Build option** `BROOK_BDW_VIDEO_ENGINE` (OFF; requires `BROOK_SSD_AUTO_RECOVERY`), accepted by `build_dell_image.py --kernel-option` and the candidate verifier (`--allow-bdw-video-engine`).
- **Runtime gate** (`BdwVideoGate`, `intel_bdw_video.h`), boot default **off**:
  - `Off → Enabled` (`gpu video on`) `→ Probed` (a read-only probe succeeded) `→ ResetOk` (a domain reset was acknowledged).
  - A reset needs a probe this boot that saw VCS0 **quiescent**: we only ever take over an idle, unowned engine (`IdleTakeover`).
  - An uncertain reset → `Quarantined` for the boot (cannot be re-enabled), and the kernel panics like the existing RCS reset test, because GDRST is shared with the per-job RCS resets. The gate is off at boot, so the recovery reboot does not repeat it: **no boot loop**.
- **Device paths:**
  - `BdwVcsResetMmio` (`intel_bdw_vcs_reset_mmio.h`) permits only the reset-control, MI_MODE stop and GDRST media writes.
  - The shared reset/init/MMIO templates accept `0x12000` (`BdwResetVcsDomain`, `BdwInitializeVcsExeclists`, `BdwVcsMmio`).
  - `VmmDellVcsReset` runs inside the existing forcewake critical section (`VmmDellForcewakeCriticalImpl`, new `vcsReset` work item). That serialises it with every render, blit and present job through the shared `BdwForcewakeClaim`, and requires write-verified UC aliases for the VCS and reset pages.
- **Debug commands** (`gpu video on|off|status|probe|reset|selftest`): status reports built, stage, counters, last refusal, the last VCS sample (head, tail, MI_MODE, control, execlist status, GFX_MODE, idle, quiescent), and the reset outcome. `selftest` currently returns `not-implemented`.
- **Userspace ABI:** `GETPARAM HAS_BSD` now answers 0 explicitly (phases 1a–1c used id 5 by mistake, which is `HAS_GEM`; fixed in 2a to id 10), so libva/i965 see "no BSD" rather than an error. `execbuffer2` with any engine other than render still returns EINVAL (`engine>1`). Nothing user-controlled reaches VCS.
- **Tests:** `test_intel_bdw_video`:
  - the reset sequence on a fake VCS (writes `0x120d0=0x10001`, `0x941c=4`, `0x120d0=0x10000`; refusal before any write on catastrophic error or another domain in reset; uncertain on no ack or no reset-ready, with the request cancelled);
  - every gate transition including quarantine;
  - self-test batch encoding;
  - the VCS context image.

## Phase 1b: self-test submission (implemented)

Implemented by reusing the proven BCS one-shot flow rather than a parallel copy:
- **One-shot:** `BdwBcsOneShot::PrepareVideoSelftest` keeps the same private page layout, PPGTT, 8-page GGTT lease, takeover binding, PAT checks and CSB state machine. Execute additionally verifies the batch's store, and `RetireScanout`/`Recycle` accept the video image.
- **Backend:** `BdwEngineDeviceBackend<Owner,EngineBase>` is the old BCS backend parameterised on engine base. `BdwBcsDeviceBackend` is now an alias for base 0x22000, so every BCS caller is unchanged; `BdwVcsDeviceBackend` uses 0x12000.
- **Critical section:** the run is a `vcsSelftest` variant of the existing copy work item. Its `CopyOwner` gains an engine base (VCS quiescence, CSB acknowledgement at `0x123a0`, and VCS reset via `BdwVcsResetMmio`).
- **Prepare, run, retire, release:** `BdwPrepareVcsSelftest`, `BdwRunPreparedVcsSelftest`, `BdwRetirePreparedVcsSelftest` and `BdwReleasePreparedVcsSelftest` live in `intel_bdw_prepare.cpp`.

The steps:

1. **GGTT reservation:** its own private reservation, like `BdwRetainedBcsReservation`, never shared with the BCS diagnostic scratch slot (65536 / `0x10000000`). It holds the HWS page, the 8 KiB context, a 4 KiB ring and a batch page.
2. **Reset VCS** (1a), then `BdwInitializeVcsExeclists(hws)`: HWSTAM, GFX_MODE execlist enable, MI_MODE, HWS_PGA, and the CSB cursor synchronisation already proven for BCS.
3. **Context and descriptor:** build the context with `BdwBuildVcsContext64`, PPGTT tables for the one batch page, and the descriptor (privilege and addressing bits as BCS, `0x13d`).
4. **Batch and ring:** the nonprivileged batch at PPGTT `0x10000` runs `MI_NOOP; MI_STORE_DATA_IMM (PPGTT) 0x11000 ← data; MI_BATCH_BUFFER_END`. Its data page is the Source page. The privileged ring is byte-identical to the BCS ring template (host-checked): `MI_FLUSH_DW`, `ARB off`, `BB_START`, then `MI_FLUSH_DW` post-sync GGTT nonce write to the Completion page and `ARB on`. `data = nonce ^ 0xa5a5a5a5`.
5. **Submit and retire:** submit through ELSP (`+0x230`, four dwords) and poll CSB for context-complete with a deadline (2 s), as `CopyOwner` does. Check the nonce. Reset VCS (`StopOwnedWork`) afterwards.
6. **Failure policy** (`BdwVideoSelftest`, `intel_bdw_scanout.cpp`):
   - **Refused before any engine write** (stage unchanged, no quarantine): gate or probe preconditions, page admission, PCI/D0, preparation (reservation window, allocation, DMA snapshot).
   - **Failed after an engine write**, with VCS reset, PAT restored and forcewake released: VCS is quarantined for the boot. This covers a bad breadcrumb, a readback mismatch, a CSB fault, and retire or release failure; in the last case the pages and lease are retained for the boot and never reused. RCS and BCS are unaffected.
   - **Genuinely uncertain:** a reset that reported Uncertain (GDRST state unknown), an engine that could not be stopped after submission (the shared private PAT then cannot be restored), PAT not restored, or forcewake not released. The kernel panics, as the scanout and RCS paths do. The gate is off at boot, so the recovery reboot does not repeat it.

**Dell check for 1a+1b, one boot:**
1. `gpu video status`: expect `built:true`, stage `off`.
2. `gpu video selftest` with the gate off: expect refusal `gate-off`.
3. `gpu video on`, then `gpu video probe`: expect stage `probed`, VCS idle and quiescent.
4. `gpu video reset`: expect `reset.complete:true`.
5. `gpu video selftest`: expect stage `selftest-ok`, and in the `selftest` block `reset_before`, `breadcrumb`, `context_complete`, `reset_after`, `retired` and `pat_restored` all true, `readback==expected`, `ggtt_writes` 16 (8 bind, 8 retire) and `elsp_writes` 4. Serial shows `gpu-video: VCS self-test ok …`.
6. Repeat `selftest` 3 times, since the image recycles.
7. Run the 2-buffer yquake demo straight after: expect 0 renderer restarts and unchanged frame times.
8. `gpu video off`.

## Phase 1c design: user submission

- **Queue:** a separate VCS queue in the submission worker, so a VCS job or hang never delays RCS presents. A VCS timeout resets VCS only.
- **Admission:** VCS jobs reuse execbuffer admission, the exec snapshot and the PPGTT builder. Every size or resource limit must fail the job with a logged `gpu-exec: … reason=` and must never call `PanicWorkerStage`; the gen 386 panic came from that pattern on RCS.
- **Advertising:** `HAS_BSD=1` and `I915_EXEC_BSD` (2) only after a self-test passed on this boot with the gate on.

## Phase 2 scoping: libva + intel-vaapi-driver (i965)

i965 talks to the kernel through **libdrm_intel** (`drm_intel_bufmgr_gem`), not Mesa's newer paths. Compared with what Brook implements today (`intel_bdw_ioctl.cpp`: GET_CAP, GETPARAM, GEM_CREATE, BUSY, SET_DOMAIN, MMAP_OFFSET, MADVISE, EXECBUFFER2 with NO_RELOC/softpin only, WAIT, CONTEXT_CREATE/DESTROY/GETPARAM/SETPARAM, RESET_STATS, syncobj family), the expected gaps are as follows. They need confirming by running `vainfo` under Brook's `run --strace` and on Linux with `strace -e ioctl`.

| Need (libdrm_intel / i965) | Brook today | Gap |
|---|---|---|
| **EXECBUFFER2 with relocations** (libdrm_intel relocates; i965 does not softpin) | refuses relocations (requires NO_RELOC) | **Large.** Either implement kernel relocation processing (patch batch dwords at presumed offsets), or patch libdrm_intel/i965 to softpin every BO and pass NO_RELOC. The second is smaller and keeps the kernel simple. |
| `I915_EXEC_BSD` engine selection | EINVAL | phase 1c |
| GETPARAM HAS_BSD, HAS_BLT, HAS_VEBOX, HAS_LLC, HAS_RELAXED_FENCING, HAS_RELAXED_DELTA, HAS_GEN7_SOL_RESET, HAS_ALIASING_PPGTT, NUM_FENCES_AVAIL, EU_TOTAL, SUBSLICE_TOTAL, HAS_BSD2 | HAS_BSD=0 only; others EINVAL | Small: answer each truthfully (BSD after self-test, BLT=0 to userspace, VEBOX=0, LLC=1, BSD2=0, EU/subslice from the GT topology already sampled) |
| GEM_SET_TILING / GET_TILING (Y-tiled NV12 surfaces) | absent | Medium: metadata only with PPGTT, plus swizzle report (none on BDW) |
| GEM_MMAP (legacy CPU/WC) and GEM_MMAP_GTT (16-byte struct) | MMAP_OFFSET (WB) only | Medium: WC mapping, and GTT mapping of tiled BOs needs fence detiling. Prefer avoiding GTT maps (vaGetImage via GPU copy) |
| GEM_GET_APERTURE, GEM_SW_FINISH, GEM_PWRITE/PREAD, GEM_SET/GET_CACHING | absent | Small/medium |
| GEM_FLINK/OPEN, PRIME handle↔fd (vaExportSurfaceHandle for zero-copy into the browser) | check dma-buf export path | Medium; needed for phase 3 |
| REG_READ (GPU timestamp) | absent | Small (allow-list the timestamp register only) |
| VEBOX (VECS0, base 0x1a000) for post-processing and colour conversion | not used | Optional; avoid by doing NV12→RGB in the browser and GL |

**Nix packaging:**
- `libva` (2.x), `intel-vaapi-driver` (nixpkgs; the i965 driver supports Gen8), `libva-utils` (`vainfo`), and nixpkgs `ffmpeg` (built with VA-API).
- Runtime environment: `LIBVA_DRIVER_NAME=i965` and `LIBVA_DRIVERS_PATH=<intel-vaapi-driver>/lib/dri`.
- The libdrm_intel softpin patch, if chosen, lives in a Brook overlay as the Mesa patches do. Stage like the Iris package: create-only, hash-checked, with a rollback record.

**Test clip plan:**
- On the host, generate H.264 clips with x264 from `testsrc2` plus a natural-content sample: baseline, main and high profiles; 720p30, 1080p30 and 1080p60; CAVLC and CABAC; with and without B-frames.
- Reference: `ffmpeg -i clip -f framemd5 ref.md5` (software). Test: `ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i clip -vf hwdownload,format=nv12 -f framemd5 hw.md5`.
- H.264 decoding is bit-exact by specification, so **every frame hash must match**. Also record decode fps, CPU and VCS busy time, and run a 10-minute soak.

## Risks

- **Phase 1b and 1c are the risky parts:** a mistaken VCS submission or reset path on real hardware. Mitigations: build option off, runtime gate off at boot, one engine owner via the shared forcewake claim, IdleTakeover only, quarantine on uncertainty, and the guard's panic-reset recovery.
- **Relocations** (phase 2) is the largest unknown in effort.
- **YouTube:** the payoff depends on the H.264 preference (phase 3).

## Driver choice: i965 versus iHD (intel-media-driver)

Both need **relocation-free submission** from Brook's point of view: Brook's execbuffer accepts only softpinned, NO_RELOC submissions.
- **i965 (intel-vaapi-driver):** submits through libdrm_intel, which emits a relocation for every surface. libdrm_intel already has softpin support (`drm_intel_bo_set_softpin_offset`, softpin targets in the exec list without relocation entries). Patching it so every BO gets a fixed address from a per-process VA allocator and submits `NO_RELOC | HANDLE_LUT` is a userspace change, estimated at 1–2 days, with **no kernel relocation code**.
- **iHD (intel-media-driver):** submits through its own MOS buffer manager, also derived from libdrm_intel. As far as I know its softpin path is enabled only on newer platforms; Gen8 runs the relocation path. It is also a much larger C++ code base. Gen8 support is legacy there and may be absent from recent releases, which needs checking against the nixpkgs version (its `GEN8` build option). It also needs at least the same GETPARAMs, plus context engine parameters.

**Recommendation: i965 plus a small libdrm_intel softpin/NO_RELOC patch.** It needs the least kernel work: the kernel side is then `I915_EXEC_BSD`, truthful GETPARAMs, tiling metadata and the mmap modes, which both drivers need anyway. Kernel-side relocation processing stays a fallback if the patch proves fragile. Confirm both before committing to one: run `vainfo` and a one-frame H.264 decode under `strace -e ioctl` on a Linux Broadwell (or any Gen8 machine) with each driver, and list the ioctls, flags and GETPARAMs actually used.

## Dell results and the render-clock sag (gens 389–390)

- **Gen 389 (p1b):** 1a correct on hardware: probe idle and quiescent, `gpu video reset` complete. The self-test was refused before any engine write. The old status hid why; p1b2 added sub-reasons and bounded retries.
- **Gen 390 (p1b2):** **the self-test passes**, with readback equal to expected and every stage true. Two attempts were refused cleanly with `precondition-not-cpu0`: the debug channel's thread was not on CPU0.
  - **Fix:** probe, reset and self-test now run on a CPU0-pinned `gpu-video` kernel thread (`BdwVideoRunOnCpu0`, the RCS debug-job pattern), with a 10 s wait. A job that outlives the wait reports `pending:true` and is collected by the next call.
- **Regression found on gen 390:** after `gpu video reset` plus the self-tests, the render clock sagged for the rest of the boot. Worker mhz averaged 415–432 (min 400) against 500, the compositor job took 14.8 ms against 11.4, and the displayed mean was 24.1 ms against 20.5, with the package at ~66 °C. A restart without touching VCS was normal again. **Not yet understood.** Phase 1c waits on it.
  - **Instrumentation:** every VCS reset or self-test snapshots 22 registers inside the critical section, immediately before and after. They are the RPS controls and status (`0xa008`–`0xa06c`, `RPSTAT1` CAGF), `RC_CONTROL`/`RC_STATE`, `PMINTRMSK`, RC6 and RC6p residency, and the VCS engine registers (`MI_MODE`, `GFX_MODE`, `RING_CTL`, `EXECLIST_STATUS`, `PSMI_CTL`, `HWS_PGA`, `HWSTAM`). `gpu video status` reports both snapshots under `pm`, with `changed_mask`, `cagf_before_mhz` and `cagf_after_mhz`.
  - **Restore:** plain GT PM controls the operation changed (`RPNSWREQ`, `RC_VIDEO_FREQ`, `RP_DOWN_TIMEOUT`, `RP_INTERRUPT_LIMITS`, `RP_CONTROL`, `RP_UP/DOWN_THRESHOLD`, `RP_UP/DOWN_EI`, `RC_CONTROL`, `PMINTRMSK`) are written back to the exact pre-operation value and verified (`restored_mask`, `restore_failed_mask`). Status and counter registers, and the masked VCS engine registers, are reported only, never written.
  - **Hypotheses, in order:**
    1. **VCS left in execlist mode** (`GFX_MODE` bit 15) after the self-test, with `HWS_PGA` pointing at our released (scratch-backed) page and a context image no longer valid. An engine that is "idle" but in a state the PCU cannot power-gate keeps the media well and GT awake, so the package power budget lowers the GT frequency, consistent with 400–430 MHz at a cool package. Firmware had `GFX_MODE` 0 (legacy). Test: compare `GFX_MODE` before and after, and RC6 residency growth at idle with and without VCS use.
    2. **The GEN6_GRDOM_MEDIA reset altered an RPS or RC control register.** The restore would now correct it, and `changed_mask` will show it.
    3. **PSMI idle messaging changed on VCS.**
  - **Next Dell check:** on a fresh boot, run the 2-buffer yquake demo (baseline mhz), then `gpu video on`, `probe`, `reset` (read `pm`), `selftest` (read `pm`), then yquake again (mhz). Then `reset` only on another fresh boot, to separate the reset from the self-test.

### Gen 391 bisect and the experiment subcommands (p1b4)

- **Bisect:**
  - base: displayed 19.9 ms, mhz 500;
  - after `gpu video reset` alone: 24.7 ms, mhz avg 431 (min 400);
  - after the self-test as well: unchanged.

  **The GEN6_GRDOM_MEDIA reset alone causes it.** Across the reset only `RPSTAT1` changed; no RPS/RC control register and no VCS mode register (`GFX_MODE` stayed 0), so the restore cannot help.
- **Snapshot additions** (29 registers):
  - VCS `RING_MAX_IDLE` (`0x12054`, the power-context idle count i915 programs per engine for RC6), `RING_IMR` and `INSTPM`;
  - BCS and RCS `RING_MAX_IDLE` for comparison;
  - RC6pp residency and `GT_THREAD_STATUS`;
  - `pm.regs[].f`: the first snapshot of the boot (firmware state).

  Suspect: a domain reset returns VCS engine-block registers such as `RING_MAX_IDLE` to hardware defaults. Firmware may have programmed them, and RC6 entry depends on them.
- **`gpu video pm sample`:** two snapshots about 1 s apart, reporting RC6, RC6p and RC6pp residency deltas and percentages (1.28 µs units), RC state and control, CAGF, thread status, and the VCS/BCS/RCS MAX_IDLE and PSMI values. Run it before and after `gpu video reset`, at idle.
- **Experiments**, each a one-shot debug subcommand under the forcewake claim:

  | Subcommand | What it does | Guard |
  |---|---|---|
  | (a) `gpu video pm stop-start` | MI_MODE STOP_RING set, wait up to 100 ms for ring idle, clear | needs gate on and an idle probe |
  | (b) `gpu video pm idle-ring` | RING_CTL, RING_HEAD and RING_TAIL set to 0 | refuses an enabled ring |
  | (c) `gpu video reset` | a second domain reset | needs gate on and an idle probe |
  | (d) `gpu video pm rps-rewrite` | RP limits, timeout, EI, thresholds, control and PMINTRMSK rewritten with their current values, in i915's order | needs gate on and an idle probe |
  | (e) `gpu video pm engine-restore` | VCS MAX_IDLE, IMR and HWSTAM (plain), PSMI_CTL and INSTPM (masked) written back to their **first-boot** values where they differ | needs gate on and an idle probe |

  Every write is read back. The callback keeps the forcewake claim clean, because these registers hold no engine, GGTT or PAT ownership. The result reports `writes` and `failures`.
- **Suggested order on one boot:**
  1. yquake baseline;
  2. `gpu video on`, `probe`, `pm sample`;
  3. `reset`, `pm sample` (compare RC6 % and MAX_IDLE);
  4. yquake (sag);
  5. `pm engine-restore`, `pm sample`, yquake;
  6. if it still sags: (a), (b), (d), each followed by yquake.

  Try (e) first, since it follows directly from the snapshot data.

### Gen 392: clock sag root cause, and the renderer stall (p1b5)

- **Root cause of the sag:** the GEN6_GRDOM_MEDIA reset returns VCS `RING_MAX_IDLE` (`0x12054`) from firmware's **10** to the hardware default **64**. The GT then never enters RC6: 99% before, 0% after. The clock rose to 400 MHz under load and power budgeting changed. `pm engine-restore` (one write) brought back RC6 99% and the normal clock. stop-start, idle-ring and rps-rewrite had no effect.
  - **Fix:** after **every** acknowledged VCS reset (the standalone reset, and both resets around the self-test), the firmware values of the VCS engine registers are written back within the same critical section, with readback. The values come from the boot's first snapshot: MAX_IDLE, IMR and HWSTAM (plain); PSMI_CTL and INSTPM (masked).
  - After a reset or self-test, the gpu-video thread samples RC6 residency over ~300 ms and reports `rc6_checked`, `rc6_pct` and `rc6_resumed` (RC6 entered at all), plus boot totals `engine_restores` and `engine_restore_failures`.
  - `pm engine-restore` remains as a debug command.
- **Renderer stall after `gpu video reset` (gen 392):** the renderer looped on `DESKTOP_ASYNC_RETRY errno=16 stage=quad_reserve` (`reserve: no free handoff slot, output still reserved, or zero-copy image busy`, count 4096). Compositor jobs dropped to 0 and zero-copy showed "software takeover".
  - **Mechanism, from the code:** render-job preparation (`DiagnosticLease lease; if(!lease.held) return {false,true};`) and every present (`BdwExecuteBcsScanout`, which takes the lease before the zero-copy hook) both require the shared diagnostic lease. p1b2–p1b4 held that lease on the gpu-video thread across bounded retries and sleeps: up to 0.5 s of retries for reset and self-test, and over 1 s for `pm sample`. Every present and render prepare in that window was refused.
    - With zero-copy armed, a present refused before the zero-copy hook leaves the ring image's accounting incomplete, and the renderer then cannot reserve a handoff output.
    - On gen 391 the reset held the lease only briefly. I have not reproduced the exact stranding sequence on the host; the code path is the evidence.
  - **Fix:** VCS diagnostics no longer take the diagnostic lease on the gpu-video thread at all. Every device step (reset, self-test prepare, run and release, PM ops and samples) is queued to the CPU0 application worker (`BdwVideoStepSlot`). The worker services it in `ServiceScanout`, between RCS jobs and presents, taking the lease only for the step itself. A step can therefore never overlap a render prepare or a present.
    - If the lease is busy, the step stays queued, untouched.
    - A step not started within 2 s is withdrawn; nothing ran.
    - Retries and sleeps happen on the gpu-video thread with no lease held.
    - Status reports `worker_steps` (serviced, lease_busy, timeouts).
  - **Regression test:** `TestStepSlot` checks that the caller side never holds the lease, that a busy lease requeues without running, that a started step cannot be withdrawn, and that a timed-out step never runs.
  - **Separate issue for the zero-copy owner:** any present refused at the diagnostic-lease check (`Unavailable`) while zero-copy is armed can strand a ring image. Other diagnostics (`gpu flip probe`, store probes and so on) can still trigger it. It should be fixed in the present path, by not refusing zero-copy presents on the lease, or by unwinding the image reservation.

## Status after gen 393, and phase 1c / 2a progress (p1c0)

**Phase 1 (1a, 1b) passes on the Dell (gen 393).**
- `gpu video reset`: RC6 resumed (95%), engine_restores 1.
- 3 self-tests pass, RC6 97%.
- yquake shows no regression: 19.8 ms base, 20.2–20.4 ms after; mhz 500; zero-copy stays active.
- No quad_reserve loops; worker_steps 20 serviced, 0 lease_busy, 0 timeouts.

**Done in this round:**
- **HAS_BSD and BSD admission:** both are now driven by `BdwVideoUserSubmissionReady()` (`intel_bdw_video.h`), which only a kernel with the VCS user-job executor will set, after a self-test on that boot with the gate on. This build has no executor, so `HAS_BSD`=0. An `I915_EXEC_BSD` execbuffer is refused with the recorded reason `execbuffer: I915_EXEC_BSD video engine submission not available`.
- **ioctl trace harness:** `gpu ioctl trace on|off|reset|status|dump` records, for every i915-compatible ioctl, the pid, nr (with name), size, detail and result, in a 256-entry ring. Detail is the GETPARAM id or the execbuffer2 flags. Running `vainfo` with tracing on shows exactly what i965 and libdrm_intel ask for and what Brook refuses, which is the phase 2 gap list measured instead of inferred.
- **Packaging:** `tools/vaapi-brook-pkg` is a buildEnv of `libva`, `intel-vaapi-driver` (i965, `i965_drv_video.so`), `libva-utils` (`vainfo`) and `ffmpeg` with the VAAPI hwaccel, from the pinned nixpkgs. It builds on the host. i965 links `libdrm_intel` from `libdrm-2.4.131`.

**Not done: the phase 1c VCS user-job executor.** The plan, from the code:
- **Page tables:** reuse the engine-independent page-table build (`BdwExecTables` / `gemProbeTables.Prepare` plus the snapshot DMA inspection and publication in `BdwPrepareRetainedSubmissionChecked`). Use a **second** `BdwExecTables` instance and a VCS-private GGTT reservation, never the RCS ones (65544/29).
- **One-shot:** a `BdwVcsUserOneShot`, modelled on `BdwRenderOneShot::AttachExternalSnapshot`/`ArmExternalSnapshot` without render state. It needs:
  - a fresh, restore-inhibited xCS context per job from `BdwBuildVcsContext64`, with PML4 from the snapshot's tables;
  - a ring of `MI_BATCH_BUFFER_START` (PPGTT) to the user batch, then the breadcrumb;
  - its own HWS/CSB.

  Media state is fully re-emitted per batch by i965, so no context persistence is needed.
- **Run:** the existing `vcsSelftest` copy-work-item flow: reset with firmware engine restore, PAT lease, takeover bind, execlist init, ELSP, CSB, reset, unbind. Device step via `BdwVideoStepSlot`.
- **Worker routing:** an execbuffer with engine 2 becomes a snapshot tagged VCS. The application worker routes VCS snapshots to the VCS executor, between RCS jobs, so RCS presents are never blocked longer than one VCS job. Fences use the existing snapshot retirement.
- **Failure policy:**
  - size or resource limits refuse the job (fence error);
  - a breadcrumb or CSB deadline (hang) means VCS reset, then fail the job's fence, then quarantine VCS for the boot;
  - panic only on uncertain GDRST, PAT or forcewake, as in 1b.

**Phase 2a, still to do:**
- the libdrm_intel softpin patch: per-process VA allocator at BO creation, `drm_intel_bo_set_softpin_offset`, execbuffer with `NO_RELOC | HANDLE_LUT`, as an override of `libdrm` for the i965 driver only;
- truthful GETPARAMs;
- `SET_TILING`/`GET_TILING` metadata and `GEM_MMAP` WC;
- confirming the list with `gpu ioctl trace` while running `vainfo`.

## Phase 1c executor (p1c1, commit 29818357)

**Path of an `I915_EXEC_BSD` execbuffer:**
1. `CopyExecSnapshot` admits engine 2 only while `BdwVideoUserSubmissionReady()`. Otherwise it is refused (EINVAL, reason `execbuffer: I915_EXEC_BSD video engine submission not available`). Engines above 2 stay refused. The snapshot is tagged `BdwExecEngineVideo`.
2. It is queued on the normal application queue. On the CPU0 worker, `ExecuteRetainedWorker` hands tagged snapshots to `ExecuteVcsUserJob` (`intel_bdw_scanout.cpp`) before any render-path state is touched. VCS jobs therefore run one at a time, between RCS jobs. There is no separate VCS queue yet: a VCS job delays the next RCS job by its own duration.
3. `BdwPrepareVcsUserJob` (`intel_bdw_prepare.cpp`) uses objects that the RCS path never touches:
   - **Tables:** its own `BdwExecTables` instance with a table-only `BuildTables`. There is no render ring and no table cache.
   - **One-shot:** its own `BdwVcsUserOneShot` (`intel_bdw_vcs_user.h`). Each job gets a fresh, restore-inhibited VCS context with PML4 set to the job's table root, and a ring of `MI_BATCH_BUFFER_START` (PPGTT, 48-bit, 8-byte aligned) to the user batch followed by the GGTT breadcrumb.
   - **GGTT:** its own 8-entry reservation in the one-shot window, plus a scratch page.

   Snapshots containing scanout (uncached) or protected buffers are refused. After a clean build, DMA inspection, publication and cache sync, the snapshot is armed.
4. `VmmDellVcsUserRun` runs the 1b sequence in the forcewake critical section:
   1. VCS reset, with the firmware engine values (RING_MAX_IDLE etc.) restored;
   2. PAT lease;
   3. a check and flush of the table pages under the lease;
   4. takeover bind, execlist init, ELSP;
   5. breadcrumb and CSB completion within the 2 s budget;
   6. VCS reset, unbind.
5. Fences complete or fail through the queue's normal snapshot retirement.

**Failure policy:**

| Outcome (`BdwClassifyVcsUserRun`) | Job | VCS |
|---|---|---|
| Refused before any VCS write: gate, preconditions, size or allocation limits, claim busy (retried 20× at 1 ms) | fence fails (skipped), buffers not poisoned | unchanged |
| Touched and failed (hang: no breadcrumb/CSB in 2 s; CSB fault), reset acknowledged | fence fails, buffers marked failed (GEM_WAIT returns EIO) | quarantined for the boot, `HAS_BSD`→0 |
| Stopped but the lease could not be unbound | fence fails, pages retained for the boot | quarantined |
| Uncertain GDRST, PAT or forcewake | — | kernel panic (guard recovery), as in 1b |

**Readiness:** `HAS_BSD` and BSD admission are published only by a video-build kernel when all of these hold:
- the gate is on;
- a self-test has passed this boot;
- VCS is not quarantined.

Gate off withdraws them, and gate on again restores them without a new self-test. The worker reports a quarantine through an atomic flag, because a running self-test holds the video lease while it waits on that same worker. Holders of the lease fold the flag into the gate. `gpu video status` now reports `user_ready` and `user_jobs {jobs, completed, refused, failed, retries, last_us, max_us, quarantined, quarantine_detail}`.

**Probe tool:** `tools/vcs-store-probe/vcs_store_probe.c` is static and has no libdrm dependency. Build it with:

```
$MUSL_CC -static -O2 -Wall -Wextra -Werror tools/vcs-store-probe/vcs_store_probe.c -o build/vcs-store-probe/vcs_store_probe
```

It finds the i915 render node and reads `HAS_BSD`. It then submits a softpinned `MI_STORE_DATA_IMM` batch with `I915_EXEC_BSD | NO_RELOC | FENCE_ARRAY`, using a signalling syncobj. It waits on the syncobj and `GEM_WAIT`, then checks the value. Modes:
- `--count N`: run N jobs.
- `--high`: place the objects above 4 GiB.
- `--expect-refused`: requires `HAS_BSD=0` and an EINVAL refusal.
- `--hang`: destructive for the boot. A self-looping batch; it expects EIO on the BO and `HAS_BSD=0` afterwards.

**Known costs, left for phase 4:**
- Every job does two media resets and full execlist init. That is fine for correctness, but per-job latency will matter for decode throughput; persistent VCS execlist state is the phase 4 fix.
- A hang stalls the CPU0 worker, and so the desktop, for up to the 2 s budget once before the quarantine.

**Tests:**
- Host tests:
  - user ring encoding and its refusals;
  - every classifier outcome;
  - advertisement and quarantine;
  - `BdwVcsUserOneShot` complete, hang and refusal flows on the fake backend;
  - decoder tagging and refusal of engines 2, 3 and 4.
- The full host suite passes (352 tests).
- `pair-vaapi-p1c1` passes the guarded QEMU check on the first attempt.

## Phase 1c on the Dell (gen 395) and phase 2a (p2a0)

**1c passed on the Dell** (gen 395, session `979e6fe7a4e44dcaa9789fcbe62fd4cc`, evidence in `display-perf/build/vcs-user-p1c1/`):
- `--expect-refused` passed. After the self-test, `advertised` and `user_ready` were true.
- Single jobs, low and `--high` placement, passed.
- 500 jobs: mean 2180 µs, worst 6009 µs, kernel `max_us` 680.
- 5000 jobs during yquake: mean 1973 µs. yquake ran at 19.82 ms under VCS load against 20.40 ms alone, and zero-copy stayed active.
- Gate off/on: SKIP, then refused, then PASS again with no new self-test.
- `--hang` was detected at 2002 ms and VCS was quarantined (`breadcrumb-timeout`) with no panic. yquake afterwards: 20.65 ms.
- In total 5504 jobs ran: 1 failed (the hang), 0 refused.

**Correction:** the 1a–1c kernels and probe used GETPARAM id **5** for `HAS_BSD`. That id is `HAS_GEM`; `HAS_BSD` is **10**. The Dell results stand, because the probe and kernel agreed, but libdrm and i965 query 10. 2a moves `HAS_BSD` to 10, answers `HAS_GEM`=1, and rebuilds the probe.

**2a kernel (`intel_bdw_ioctl.cpp`, `intel_bdw_buffer.*`, `syscall.cpp`):**
- **GETPARAM:**

  | Param | Answer |
  |---|---|
  | HAS_GEM | 1 |
  | NUM_FENCES_AVAIL | 0 (no fences) |
  | HAS_BSD (10) | executor-ready |
  | HAS_BLT | 0 (BCS is kernel-owned) |
  | RELAXED_FENCING | 1 |
  | RELAXED_DELTA | 1 |
  | GEN7_SOL_RESET | 0 |
  | HAS_LLC | 1 |
  | ALIASING_PPGTT | 3 |
  | SEMAPHORES | 0 |
  | HAS_VEBOX | 0 |
  | SECURE_BATCHES | 0 |
  | HAS_WT | 0 |
  | MMAP_VERSION | 1 |
  | HAS_BSD2 | 0 |
  | SUBSLICE_TOTAL | 3 |
  | EU_TOTAL | 24 |
  | POOLED_EU | 0 |
  | MIN_EU_IN_POOL | 0 |

  `HAS_HUC` stays EINVAL.
- **`GEM_SET_TILING`/`GET_TILING`:** metadata only, shared by every handle of a buffer. Mode is X or Y. Strides must be a multiple of 512 (X) or 128 (Y), at most 256 KiB. Swizzle is always NONE. No fences and no detiling: every mapping stays linear.
- **`GEM_MMAP` (legacy, 40 bytes):** maps the buffer's mmap token through the caller's own `mmap` on the same fd (new `BdwI915UserAccess::mapBuffer`). The WC flag is accepted and served write-back, which is coherent on Broadwell's LLC.
- **`GEM_MMAP_GTT` (16 bytes):** returns the WB token for linear buffers. Tiled buffers are refused with ENODEV, because there is no aperture to detile.
- **`GEM_GET_APERTURE`:** 256 MiB. That is the worker's real per-execbuffer limit (65536 pages), so libdrm flushes before it.
- **`SW_FINISH`:** handle check only.
- **`SET_CACHING`/`GET_CACHING`:** SET accepts NONE, CACHED and DISPLAY. GET reports CACHED, the effective policy.
- **`PREAD`/`PWRITE`:** copy while holding the buffer's CPU transition, so no GPU use can start mid-copy. They wait for current GPU use like SET_DOMAIN and return EIO for a failed one.
- **`SET_DOMAIN`:** now also accepts the GTT domain (0x40), which libdrm's `map_gtt` uses.
- **`REG_READ`:** not implemented. i965 does not use it.

**libdrm_intel softpin patch (`tools/vaapi-brook-pkg/libdrm-intel-brook-softpin.patch`, i965 only):**
- **VA allocator:** per bufmgr, first-fit and address-sorted with coalescing, so BOs pack densely and need few page tables. It covers [1 MiB, 4 GiB) by default, because i965 writes `bo->offset + delta` into 32-bit relocation fields itself. `BROOK_LIBDRM_SOFTPIN_48B=1` places everything at or above 4 GiB, with `EXEC_OBJECT_SUPPORTS_48B_ADDRESS`, for testing only.
- **Pinning:** every BO is softpinned at allocation, cache reuse (the pin is re-established after `unreference_final` clears `kflags`) and flink/prime import.
- **No relocations:** relocation emission becomes softpin targets. Write domains become `EXEC_OBJECT_WRITE` per execbuffer, because Brook maps unwritten objects read-only. Submission carries `I915_EXEC_NO_RELOC`.
- **CPU maps:** try `GEM_MMAP`, then fall back to `MMAP_OFFSET` WB.
- **GTT maps of tiled BOs:** a linear CPU shadow, detiled (X and Y) on map and retiled on the last unmap. Imported BOs have no known stride and are refused.
- **Opt-out:** `BROOK_LIBDRM_SOFTPIN=0` restores stock behaviour.

The patch uses only standard UAPI, so it also runs on Linux i915.

**Package:**
- `tools/vaapi-brook-pkg` is `brook-vaapi-0.2`: libva, i965 built against `libdrm-brook-softpin`, libva-utils and **ffmpeg-headless**. ffmpeg-headless still has the VAAPI hwaccel, through libva-minimal. The closure is 126 paths, 314 MiB.
- The stock libdrm stays for everything else.

**Host harness (`tools/vaapi-brook-pkg/host-shim/`):**
- An LD_PRELOAD shim built from Brook's own `intel_bdw_ioctl.cpp`, `intel_bdw_file.cpp`, `intel_bdw_buffer.cpp` and `intel_bdw_exec_snapshot.cpp`.
- It opens the host render node only for a genuine DRM fd. Every ioctl and mmap on that fd, and on its dups, is served by Brook's code; buffer pages come from a memfd arena. Nothing reaches the host GPU.
- Admitted execbuffers are retired as completed without execution, so decoded pixels are meaningless and only the ioctl contract is tested.
- Build it with `nix-shell --run tools/vaapi-brook-pkg/host-shim/build.sh`.

**Host results:**
- **`vainfo`:** under the shim, i965 initialises as "Intel i965 driver for Intel(R) Broadwell" and lists, among others, `H264ConstrainedBaseline`/`Main`/`High` (VLD and EncSlice), MPEG2, VC1, JPEG, VP8 and VideoProc. Its only failed ioctl is GETPARAM HAS_HUC.
- **First decode run:** `ffmpeg -hwaccel vaapi` decoding a 720p H.264 High clip (60 frames) had 119 of 120 execbuffers refused. Cause: BOs reused from libdrm's cache lost `EXEC_OBJECT_PINNED` and fell back to relocations. Fixed in the patch.
- **After the fix:** 120 of 120 execbuffers were admitted by Brook's decoder: 60 BSD decode and 60 RCS (i965's hardware `vaGetImage`). At most 16 objects, 2705 pages and 18 page tables per execbuffer, against Brook's 65536 pages and 512 tables. The 48-bit mode is also admitted.
- `framemd5` produces 60 frames. On the host they are not meaningful; the Dell comparison is against the software reference `build/brook-i915-shim/sw.framemd5`.

**Staging bundle (`tools/vaapi-brook-pkg/stage/`):**
- `make_bundle.py` writes `store.tar`, `manifest.json` (per-file SHA-256, exec bits, symlinks, exact entry sets), `closure.txt` and `install.py` into `bundle.tar`.
- `install.py` (root) refuses if any path already exists, extracts into a private `/nix/store/.brook-vaapi-stage-TX`, verifies, renames into place, re-verifies, and checks that the whole closure is present.
- It never overwrites, removes or touches `/nix/profile`.
- A gen 396 inventory (job in `build/hardware/vaapi-stage-gen396/`) found 120 of 126 closure paths already on the Dell. The bundle carries the 6 missing ones (15 MB): libdrm-brook-softpin, i965, libva-utils, ffmpeg-headless bin and man, and the env.

## Phase 3: Chromium VA-API (work/browser-vaapi, in progress)

**Probe:** `scripts/workloads/browser_vaapi_probe.py`. It runs on the Dell as uid 1000 with a private profile and closes the browser afterwards.
- It serves a clip in an autoplaying `<video>` and launches Chromium (or Firefox) through `brook-run` and `brook-iris-run`.
- For Chromium it reads the DevTools Media domain (`kVideoDecoderName`, the player messages), `SystemInfo.getInfo`, the playback quality, and aggregate CPU from `/proc/stat`.
- VA-API flags:
  - `--enable-features=VaapiVideoDecoder,VaapiIgnoreDriverChecks,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL`;
  - `--disable-features=Vulkan,UseChromeOSDirectVideoDecoder`;
  - `--render-node-override=/dev/dri/renderD129`.

  The environment sets `LIBVA_DRIVER_NAME=i965` and `LIBVA_DRIVERS_PATH`.

**Two bugs found and fixed in the package (`brook-vaapi-0.3`, not yet staged):**
1. **Frame-pool export.** Chromium's VaapiVideoDecoder creates surfaces without a pixel-format attribute and exports them (`vaExportSurfaceHandle`) before decoding. i965 allocates a surface's BO lazily, so the export failed with "invalid VASurfaceID" and Chromium fell back to FFmpeg.
   - Fix: `i965-brook-export-alloc.patch` allocates NV12 or P010, tiled, on export.
   - The export carries modifier `I915_FORMAT_MOD_Y_TILED` (0x0100000000000002), NV12 as R8 plus GR88 layers, pitch 1280, UV offset 942080 for 1280×720.
   - Host check: `tools/vaapi-brook-pkg/host-shim/va_export_check.c` under the shim.
2. **Stock libdrm_intel shadowed the softpin one: the green frames.** Chromium's GPU process loads Mesa's libgallium, which links stock `libdrm_intel.so.1`. When libva later loaded i965, the loader reused that object for i965's `libdrm_intel.so.1`.
   - Every BSD execbuffer then went out stock (no NO_RELOC, unpinned objects with relocations), and Brook refused hundreds of them. The surfaces were never decoded into, so the video showed solid green. `gpu video status` user_jobs did not move.
   - `exec_logger.so` (`tools/vaapi-brook-pkg/exec-logger/`) captured the refused requests on the Dell.
   - The host shim reproduces it: LD_PRELOAD stock `libdrm_intel` makes 31 of 31 ffmpeg execbuffers refused.
   - Fix: i965 links the softpin `libdrm_intel` **statically**, with its symbols made local (`--exclude-libs`). The build fails if i965 still needs or exports `libdrm_intel`.
   - With stock `libdrm_intel` preloaded, the host shim then admits all 31 of 31.

**Dell results (gen 398, `dell-integration7`), private profiles, 20 s each.** The patched i965 was loaded from a RAM copy (`LIBVA_DRIVERS_PATH`), with no store write.

| Run | Decoder | VCS jobs | Frames presented / total / dropped | Frame p50 / p90 | CPU busy |
|---|---|---|---|---|---|
| 720p H.264, VA-API | VaapiVideoDecoder | 515 completed, 0 refused or failed, ~4.9 ms each (max 7.5) | 270 / 505 / 243 | 55 / 104 ms | 25.3% |
| 720p H.264, software | FFmpegVideoDecoder | — | 382 / 519 / 141 | 40 / 69 ms | 22.1% |
| 1080p H.264, software | FFmpegVideoDecoder | — | 325 / 512 / 175 | 44 / 77 ms | 23.0% |
| 1080p H.264, VA-API | — | — | browser crashed at start twice (SIGTRAP after "Corruption detected in shared-memory segment"), before any decode; not yet understood | | |

**VA-API decode works end to end in Chromium, but it is slower than software today.**
- Each VCS job holds the CPU0 submission worker for about 5 ms: two media resets, execlist init, and polling for the breadcrumb. RCS composition and Chromium's own GL work queue behind it.
- That is the phase 4 item: keep VCS state between jobs and do not block the worker for the decode time.
- Software decode at 720p and 1080p also drops frames here (the package was near 90 °C with the clock capped during these runs), so these are relative numbers.

**Not done yet:**
- the Firefox VA-API path and its VP9 0.16× issue;
- the YouTube H.264 preference;
- the launcher proposal;
- the 1080p VA-API crash.

## Phase 4 (work/vcs-lane): measurement first, cheap savings, and the async-lane design

**Per job today:** up to **three** VCS GDRSTs, each followed by the firmware engine restore:
1. the copy work item's reset-before;
2. the reset inside the execlist takeover (`initializeExeclists` always resets so the CSB cursor is synchronized);
3. the reset-after.

Every ordinary job buffer is also clflushed before the job and again at retirement. A 720p decode job references 659–2705 pages, which is several MB flushed twice per frame.

**This round (all switchable at runtime, both on by default):**
- **`gpu video tune resets min|full`:** the reset-before is skipped when the previous user job ended in a verified clean stop and VCS is observed quiescent. The reset-after is replaced by a CSB context-complete on a quiescent engine (the context switched out and the engine went idle). A GDRST still happens on any failure or hang, and inside the execlist init, so that is 1 reset per job instead of 3. Any other VCS operation (self-test, reset, PM ops) clears the clean-stop state.
- **`gpu video tune coherent on|off`:** job buffers use the leased WB LLC PAT slot without CPU cache flushes, as RCS coherent-render does. The VCS ring already invalidates TLBs before the batch and flushes before the breadcrumb (MI_FLUSH_DW). Check with the ffmpeg framemd5 bit-exact test before trusting it.
- **Timing in `gpu video status` user_jobs:**
  - `timed` (count) and `span_ms` (VCS jobs/s = timed / span);
  - `prep_us`: CPU-side table build, DMA inspection and cache sync;
  - `run_us`: the critical-section run, i.e. **time the CPU0 worker is blocked on VCS**;
  - `release_us`;
  - parts of the run: `reset_before_us`, `pat_us`, `init_us` (execlist takeover including its reset), `decode_us` (ELSP to breadcrumb: the hardware decode), `csb_us`, `tail_us` (stop, unbind, PAT restore, PM finish);
  - `max_run_us`, `skipped_resets_before`, `skipped_resets_after`.

  All are sums over timed jobs.

**Why the asynchronous VCS lane is not in this pair.** Returning the worker to RCS/BCS while VCS decodes means leaving VCS running between forcewake critical sections. Two invariants of the current safety model forbid that, and changing them touches every GPU path:
- **Quiescence:** `BdwObserveBcsWithWork` runs a work callback only if **every** other engine (RCS, VCS, VECS) is submission-quiescent. A busy VCS would refuse every RCS present and composite.
  - Needed: an "owned busy" exemption. VCS is busy only with a job whose lane state Brook tracks, with its context, HWS and lease recorded, and that exemption is accepted by the RCS/BCS paths.
- **PAT ownership:** each run establishes and then restores the global private PAT (lo/hi) through `BdwPatLease`, from the inherited firmware values (0x03030303) to 0x…07/0x…0b. The zero-copy scanout variant also changes slot 3. A VCS job in flight relies on the leased values, while RCS would restore or retarget them underneath it.
  - Needed: one persistent GPU-wide PAT policy, established once and verified on each run instead of leased per run. It must be agreed with the scanout/zero-copy owner.

With both in place, the lane is:
1. **Submit:** critical section A does bind, init if needed, and ELSP, then returns with the job recorded as in flight.
2. **Wait outside the claim:** the worker interleaves RCS/BCS work. It checks the breadcrumb in RAM (the completion page is CPU-readable), so no MMIO is needed until it changes.
3. **Retire:** critical section B processes the CSB and unbinds.
4. **Timeout:** the existing 2 s deadline then triggers reset and quarantine.

A **persistent execlist context** comes with it: a lane-owned HWS page and GGTT lease, a CSB cursor kept across jobs, and no init reset. That removes the last per-job reset. The phase 4 timing above shows how much each of these is worth (`init_us` and `decode_us` against `run_us`) before that larger change.

**1080p VA-API startup SIGTRAP (open):** twice on gen 398, Chromium's browser process logged "Corruption detected in shared-memory segment" (`persistent_memory_allocator.cc:890`) and died with SIGTRAP about 0.5 s after launch, before DevTools and before any decode. Software runs before and after were fine, and an earlier 720p VA-API run was fine.
- **Hypotheses to test:**
  1. GPU-process VA initialization (which maps BOs through the new GEM_MMAP path) corrupting an unrelated shared mapping;
  2. a memfd or shm reuse problem;
  3. an unrelated flake.
- **Next step:** repeat 720p and 1080p VA-API launches on the phase 4 kernel, and if it recurs, run Chromium with `--enable-logging --v=1` plus the exec logger, capturing which segment (field-trial or histogram shm) is corrupt.

## Phase 4 measurements (gen 403–406) and the asynchronous lane (work/vcs-async)

**ffmpeg bit-exactness** (gen 403, i965 from the RAM copy of brook-vaapi-0.3): `-hwaccel vaapi -pix_fmt yuv420p` framemd5 matches software for **300/300 frames at 720p and 1080p, with `coherent on` and with `coherent off`**.

**Per-job VCS cost** (ffmpeg, 300 jobs each, µs per job):

| tuning | clip | prep | run (worker blocked) | decode (ELSP→breadcrumb) | reset-before | init | CSB | tail | wall |
|---|---|---|---|---|---|---|---|---|---|
| min + coherent | 720p | 451 | 1941 | 1493 | 5 | 57 | 44 | 67 | 2.80 s |
| min + coherent | 1080p | 537 | 3756 | 3287 | 5 | 65 | 44 | 67 | 4.14 s |
| full + no coherent | 720p | 3139 | 1944 | 1490 | 14 | 46 | 44 | 63 | 4.50 s |
| full + no coherent | 1080p | 5860 | 3761 | 3283 | 14 | 60 | 47 | 64 | 7.27 s |

- **Resets are cheap.** Each GDRST is about 10 µs. `resets min` hardly matters, and the post-job reset was never skipped (`skipped_resets_after` stayed 0).
- **`coherent` (no CPU cache flushes) is the big CPU win:** it saves 2.7 ms per 720p job and 5.3 ms per 1080p job of preparation.
- **The hardware decode is 77–87% of the worker's blocked time:** 1.5 ms at 720p and 3.3 ms at 1080p. Only overlapping it with other work, the asynchronous lane, can recover that.

**Chromium, 20 s, private profile, VA-API via `LIBVA_DRIVERS_PATH` to the RAM copy** (no store writes):

| boot | run | decoder | presented / total / dropped | p50 / p90 ms | CPU busy | VCS run µs/job |
|---|---|---|---|---|---|---|
| 404 | VA-API 720p, min + coherent | Vaapi | 430 / 527 / 108 | 36.7 / 57.2 | 16.5% | 1926 |
| 404 | VA-API 1080p, min + coherent | Vaapi | 397 / 515 / 127 | 38.9 / 62.9 | 17.1% | 3756 |
| 404 | VA-API 720p, full + no coherent | Vaapi | 354 / 528 / 182 | 44.4 / 77.1 | 17.9% | 1982 |
| 404 | VA-API 1080p, full + no coherent | Vaapi | 240 / 522 / 292 | 66.6 / 115.4 | 18.8% | 3821 |
| 404 | software 720p | FFmpeg | 461 / 526 / 68 | 35.9 / 52.7 | 18.0% | — |
| 404 | software 1080p | FFmpeg | 449 / 507 / 59 | 35.9 / 51.8 | 20.5% | — |
| 406 | VA-API 1080p (×4) | Vaapi | 440–450 / ~517 / 69–73 | 33.3–33.7 / 58–61 | 15.9–16.4% | 3757 |
| 406 | VA-API 720p | Vaapi | 478 / 520 / 40 | 33.1 / 50.0 | 15.2% | 1929 |
| 406 | software 1080p | FFmpeg | 492 / 511 / 16 | 32 / 47 | 19.6% | — |
| 406 | software 720p | FFmpeg | 515 / 526 / 9 | 31.9 / 47 | 17.3% | — |

- **With the default tuning, VA-API now reaches the 1080p30 p50 target (≤ 33 ms on gen 406).** It uses 3–4 points less CPU, but still drops more frames than software. That is consistent with the worker being blocked about 3.8 ms per decoded frame.
- **1080p startup crash: not reproduced.** Six 1080p VA-API launches across gen 404 and 406 all started and decoded.

**The asynchronous lane** (`gpu video tune async on|off`, **default off**, commit 8884eb3e, pair `pair-vcs-async`, QEMU check passed):
- **Submit:** a critical section does the usual preparation, PAT lease and takeover, then writes ELSP and returns with VCS running the job. The CPU0 worker **parks** the request.
- **Meanwhile:** the exec queue lets the worker take only later work that is provably independent of the parked job (no shared buffers, distinct contexts), and never a second video job.
- **Complete:** the worker polls the breadcrumb in RAM, which needs no device access, about once a millisecond. When it lands, or after 2 s, a second critical section verifies the breadcrumb and CSB, stops VCS, unbinds and restores the PAT. A hang is a reset plus quarantine, with no 2 s worker stall.
- **Shared-model changes, inert unless a lane job is in flight:**
  - the forcewake observation accepts a VCS that the lane owns as non-quiescent;
  - the private PAT keeps the lane base (entry 0 = 0x07, entry 4 = 0x0b) between the two critical sections. RCS and BCS leases accept that base and restore to it, the zero-copy slot-3 change included. The complete phase adopts the held lease and restores the firmware values.
- **Pausing VCS diagnostics:** while the lane is busy, VCS diagnostics (`gpu video reset`, `selftest`, `pm …`) wait.
- **New status fields:** `lane_jobs`, `lane_latency_us`, `lane_complete_retries`, `parked`, `overlapped` and `lane_busy`.
- **Tests:** host tests cover the parked queue rules and the worker park/poll/finish flow. It has not run on hardware yet.

**Dell test plan for the lane:**
1. Gate on, probe, self-test.
2. The ffmpeg framemd5 test with `async on`: it must be bit-exact.
3. Chromium VA-API at 720p and 1080p with `async on`, compared with `off`: frame p50, drops, `overlapped` and `lane_latency_us`.
4. Desktop and yquake regression, with zero-copy active, while VA-API plays.
5. The `--hang` probe with `async on`: quarantine, and the desktop stays responsive.

## Lane results on the Dell (integration10-lane, gen 407–408)

**Lane fix:** the lane backend's ELSP port accessor is single-use. Reusing it failed every lane job after the first (`execlist-init-or-elsp`); VCS was reset cleanly and quarantined, with no panic. 42c84994 rebuilds the backend in place for each submit.

**ffmpeg, async on:**
- framemd5 bit-exact: **300/300** at 720p and 300/300 at 1080p.
- 600 jobs, all through the lane, 0 failures.
- Worker blocked **555 µs per job**, against 1941 µs (720p) and 3756 µs (1080p) synchronous.

**Chromium, 20 s runs:**

| run | dropped / total | p50 / p90 ms | CPU | worker blocked µs/job | lane submit→retired µs |
|---|---|---|---|---|---|
| async 720p | 38 / 520 | 32.8 / 47.7 | 17.2% | 497 | 6564 |
| sync 720p | 48 / 519 | 33.3 / 52.5 | 17.2% | 1927 | — |
| software 720p | 7 / 518 | 31.3 / 47.3 | 19.7% | — | — |
| async 1080p (×3) | 40–47 / ~510 | 32.9–33.1 / 50–52 | 16.5–17.5% | 500–506 | 8963–9677 |
| sync 1080p (×2) | 76–77 / ~514 | 33.4–34.0 / 56–58 | 17.7–17.8% | 3759–3761 | — |
| software 1080p (×2) | 7 / ~505 | 31.9 / 47 | 21.1–22.2% | — | — |

- **Async against sync:** it halves the drops and cuts worker blocking 7×.
- **Against software:** it uses 4–5 points less CPU, but still drops about 40 frames per 20 s where software drops 7.
- **Where the time goes:** the instrumented build shows `lane_crumb_us` of about 8.7 ms from submit to the first breadcrumb sighting, against about 3.3 ms of actual decode, with only about 3 polls per job. The worker polls only between jobs, so a long RCS job delays the sighting.

**Quake II 600-record demo, with 1080p video playing in Chromium** (single runs, noisy):

| condition | demo interval |
|---|---|
| no video | 13.64 s |
| software video | 17.45 s |
| sync VA-API video | 15.07 s |
| async VA-API video | 19.09 s, then 14.35 s |

In the overlap runs, CPU was 22.8% with async VA-API against 28.1% with software video. Zero-copy stayed registered.

**`--hang` with async on:**
- Detected at 2002 ms; VCS quarantined (`breadcrumb-timeout`); `HAS_BSD` back to 0.
- **The desktop never stalled:** compositor frame interval max 18 ms throughout, where the synchronous hang froze the worker for 2 s.

**Panic (gen 408, sequence 41):** during the `spin on` 1080p run (yield idle while a lane job is parked), on `pair-int10-lane2`, the worker panicked.
- **Panic message:** "Broadwell worker uncertain … pid=542 objects=20 pages=7314 gt=1 pat=1 reset=1 restore=0 … head=0 tail=e0 acthd=0".
- **What happened:** an **RCS** Chromium job was submitted (port=4) but produced no CSB event and no breadcrumb (`render run failed site=2 csb=0/0/0/0 event=0 breadcrumb=0`). The PAT restore after it did not complete. The guard recovered.
- **Unknown:** whether a VCS lane job was in flight. On gen 407, async without spin ran 6754 lane jobs, 3371 of them overlapped with RCS work, without this.
- **Decoded packet:** `build/hardware/vcs-lane-int10c/recovery-panic` (decoded against int10-lane BROOK.elf).

**Recommendation:**
- **Keep `async` default off.** VA-API itself stays off by default, and `spin` must stay off, until it is shown whether the RCS failure is caused by the lane (RCS running while VCS is busy, or the lane PAT base) or by spin.
- **Next:**
  1. Long async-only and sync-only Chromium soaks, to see whether the RCS failure recurs with the lane alone.
  2. Record lane state in the RCS failure panic message.
  3. Poll the lane breadcrumb from inside the RCS completion wait, to cut the 8.7 ms sighting delay.

