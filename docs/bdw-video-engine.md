# Broadwell video engine (VCS0) and VA-API

Status: **phase 1a implemented** (VCS0 probe and domain reset behind a build option and a runtime gate; nothing advertised to userspace). Phase 1b (VCS execlist init, context, self-test submission) and later phases are designed below but not implemented. Branch `work/vaapi`.

## Why

Brook's browsers now render on the GPU, but video decode is software (dav1d/ffmpeg on the CPU). Broadwell's fixed-function decoder (the MFX unit behind the video command streamer VCS0) handles H.264, VP8, MPEG-2 and VC-1. It has **no VP9 or AV1** decode, so YouTube must be steered to H.264 to benefit (phase 3).

## Phases

| Phase | Scope | Status |
|---|---|---|
| 1a | VCS0 constants, probe, domain reset, runtime gate, debug commands, explicit `HAS_BSD=0` | done (this branch) |
| 1b | VCS0 HWS/CSB, execlist init, logical context, one-shot submission, `gpu video selftest` (MI_STORE_DATA_IMM breadcrumb) | designed; batch and context builders host-tested |
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

## Phase 1b design: self-test submission

Mirror the BCS one-shot (`BdwBcsOneShot`, `BdwBcsDeviceBackend`, `BdwRunRetainedBcs`), parameterised on engine base:

1. **GGTT reservation:** its own private reservation, like `BdwRetainedBcsReservation`, never shared with the BCS diagnostic scratch slot (65536 / `0x10000000`). It holds the HWS page, the 8 KiB context, a 4 KiB ring and a batch page.
2. **Reset VCS** (1a), then `BdwInitializeVcsExeclists(hws)`: HWSTAM, GFX_MODE execlist enable, MI_MODE, HWS_PGA, and the CSB cursor synchronisation already proven for BCS.
3. **Context and descriptor:** build the context with `BdwBuildVcsContext64`, PPGTT tables for the one batch page, and the descriptor (privilege and addressing bits as BCS, `0x13d`).
4. **Batch:** `BdwBuildVcsSelftestBatch` writes `MI_NOOP; MI_STORE_DATA_IMM (GGTT) hws+0x100 ← nonce; MI_BATCH_BUFFER_END`. The ring holds `MI_BATCH_BUFFER_START` plus the breadcrumb, as in the BCS ring template.
5. **Submit and retire:** submit through ELSP (`+0x230`, four dwords) and poll CSB for context-complete with a deadline (2 s), as `CopyOwner` does. Check the nonce. Reset VCS (`StopOwnedWork`) afterwards.
6. **Uncertainty:** any uncertain step quarantines VCS for the boot and retains every page. It panics only if the GT reset domain itself is in an unknown state.

**First Dell check for 1b:** `gpu video on`, then `probe`, `reset`, `selftest`. Expect `selftest ok`, the nonce read back, CSB context-complete, and RCS presents unaffected throughout.

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
