# Broadwell video engine (VCS0) and VA-API

Status: **phases 1a and 1b implemented**: VCS0 probe, domain reset and a submission self-test, behind a build option and a runtime gate; nothing is advertised to userspace. None of it has run on hardware yet. Phase 1c onwards is designed below but not implemented. Branch `work/vaapi`.

## Why

Brook's browsers now render on the GPU, but video decode is software (dav1d/ffmpeg on the CPU). Broadwell's fixed-function decoder (the MFX unit behind the video command streamer VCS0) handles H.264, VP8, MPEG-2 and VC-1. It has **no VP9 or AV1** decode, so YouTube must be steered to H.264 to benefit (phase 3).

## Phases

| Phase | Scope | Status |
|---|---|---|
| 1a | VCS0 constants, probe, domain reset, runtime gate, debug commands, explicit `HAS_BSD=0` | done (this branch) |
| 1b | VCS0 HWS/CSB, execlist init, logical context, one-shot submission, `gpu video selftest` | done (this branch), host-tested; untested on hardware |
| 1c | VCS jobs in the submission worker (separate queue), `I915_EXEC_BSD`, `HAS_BSD=1` once the self-test passes | designed |
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
- **Userspace ABI:** `GETPARAM HAS_BSD` (5) now answers 0 explicitly, so libva/i965 see "no BSD" rather than an error. `execbuffer2` with any engine other than render still returns EINVAL (`engine>1`). Nothing user-controlled reaches VCS.
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
