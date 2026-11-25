# NVIDIA GSP-RM groundwork (M0): RTX 2080 / TU104, firmware 570.144

M0 is offline groundwork for running GSP-RM on the desktop's RTX 2080. It is
host-tested only. Nothing here touches hardware or runs at boot. The kernel
unit compiles only with `-DBROOK_NV_GSP=ON` (default OFF), and nothing calls it.

## Code

| File (src/kernel/src/nv_gsp/) | What it does |
|---|---|
| `nvrm_r570.h` | The r570 RM ABI subset: GspFwWprMeta, GSP_ARGUMENTS_CACHED, libos regions, msgq headers, queue element, RPC header, sequencer, RM_RISCV_UCODE_DESC and the function/event numbers. Ported from nouveau's MIT excerpt of open-gpu-kernel-modules 570.144, with layout `static_assert`s. |
| `nvfw.h` | Falcon container layouts from nouveau's MIT `include/nvfw`: bin_hdr, HS v2, flcn_bl_dmem_desc_v2, FWSEC desc v2/v3, DMEM mapper, FRTS command. |
| `nv_gsp_fw.h` | Bounds-checked parsers for the GSP ELF (`.fwimage`, `.fwsignature_tu10x`), the booter (HS v2, signature patch), the RISC-V bootloader, ACR `bl.bin`, and the VBIOS (image walk, 0xE0 remap, BIT 'p', falcon ucode table, FWSEC v2/v3). Also the FRTS/SB DMEM-mapper patch. |
| `nv_gsp_wpr.h` | The FB/WPR2 layout and GspFwWprMeta fill, plus decoding of the WPR2 registers. |
| `nv_gsp_radix3.h` | Builds the radix3 table for the firmware image (non-contiguous pages) and a software walk to check it. |
| `nv_gsp_queue.h` | The CPU side of the cmdq/msgq rings: wrap-around, CONTINUATION_RECORD split and reassembly, checksums, and events dispatched while waiting for a reply. |
| `nv_gsp_seq.h` | The RUN_CPU_SEQUENCER interpreter. It validates the whole buffer before executing any command, and fails on a poll timeout. |
| `nv_gsp_boot.h` | The planned register sequence: preflight, FWSEC-FRTS, GSP RISC-V reset, booter_load, the handshake, and the sequencer core ops. It uses an abstract bus, and is the code the trace harness records. |
| `nv_gsp_m0.cpp` | The `BROOK_NV_GSP` compile check, built with the freestanding kernel flags. |

## Tests

These are host tests, run with `ctest -LE stress`:

| Test | Covers |
|---|---|
| `test_nv_gsp_fw` | Parsers on synthetic containers, with truncation and corruption fuzzing. |
| `test_nv_gsp_core` | WPR2 goldens, radix3, and the sequencer. |
| `test_nv_gsp_queue` | The rings against a fake GSP peer thread. |
| `test_nv_gsp_boot` | The boot plan against the recording fake register file `src/tests/nv_gsp_fake/fake_gpu.h`. |
| `test_nv_gsp_mmiotrace`, `test_nv_gsp_trace_diff` | The Python tools. |

The NVIDIA blobs and the VBIOS dump never enter the repo. To run the real-file
checks, configure the host tests with:

    -DBROOK_NV_GSP_FW_DIR=<linux-firmware>/lib/firmware/nvidia/tu104/gsp
    -DBROOK_NV_GSP_ACR_BL=<linux-firmware>/lib/firmware/nvidia/tu104/acr/bl.bin
    -DBROOK_NV_GSP_VBIOS=<capture>/vbios-nouveau.rom   (333824 B, sha256 0f94b4a7...)
    -DBROOK_NV_GSP_PCIROM=<capture>/vbios-pcirom.bin   (optional, 55808 B)

### Trace replay

`scripts/nv_gsp_mmiotrace.py` decodes the Linux mmiotrace of nouveau booting
GSP into a golden reference JSONL. To diff Brook's plan against it:

    python3 -I scripts/nv_gsp_trace_diff.py extract-seq --dmesg <capture>/dmesg-final.txt --out seq.bin
    BROOK_NV_GSP_FW_DIR=... BROOK_NV_GSP_ACR_BL=... BROOK_NV_GSP_VBIOS=... \
        build/release/host_tests/test_nv_gsp_boot --record brook-raw.jsonl --seq seq.bin
    python3 -I scripts/nv_gsp_trace_diff.py diff --ref gsp-reference-sequence.jsonl --brook brook-raw.jsonl --md diff.md

The diff passes Brook's log through the decoder's own normalisation before
comparing: DMA values are masked, polls collapsed, and IMEM/DMEM PIO hashed.

## Layout on this card

Under Linux, 0x625f04 reads 0x00020e09. Bit 3 is set and the base it encodes
is below fb-1 MiB, so nouveau shrinks the VGA workspace to the top 128 KiB.
The resulting layout for 8 GiB:

| Region | Offset |
|---|---|
| non-WPR heap | 0x1f7800000 |
| WPR2 | 0x1f7900000 |
| GSP-RM heap (105 MiB) | 0x1f7a00000 |
| ELF | 0x1fe3a0000 |
| bootloader | 0x1ffedf000 |
| FRTS | 0x1ffee0000 |
| VGA workspace | 0x1fffe0000 |

That reserves **136 MiB**. FWSEC reported WPR2 0x01ffee00-0x01fffc00, and RM
reported FB region 4 from 0x1f7800000 with 0x8800000 reserved. The design
doc's table (FRTS at 0x1ffe00000, 137 MiB) is what results when bit 3 is
clear. Brook computes the layout from the register it reads at boot.

## Firmware packaging plan (570.144, not wired yet)

**Files.** All come from `nvidia/tu102/gsp`; `tu104/gsp` is a WHENCE link to it.

| File | Bytes | sha256 |
|---|---|---|
| gsp-570.144.bin | 28542040 | 3052aee2872182a14d8d7c069e3a14fe4642405894b24692c4aca4101dfb1809 |
| booter_load-570.144.bin | 59272 | 7bb181f544a942299bbf4642da6d9bb58bfed8459725094e2ace56647cd3ec8c |
| booter_unload-570.144.bin | 39304 | 5c7f1aa37d045cc20bb81584a830e8dfa4483f1cd63943de83a58c4be19a1782 |
| bootloader-570.144.bin | 4196 | 12e987b636c2f00fa40f42fd95097515c0817b158119c584049a37faf38f8f96 |
| `nvidia/tu104/acr/bl.bin` | 1280 | 01326cc997716bebb0eab9fe1f463fffa0ee586079e4ca0e5bb096ab6fcfedab |

`bl.bin` is needed because this VBIOS carries FWSEC desc v2. That ucode is
DMA-loaded through the ACR bootloader.

**Fetch.**
- A derivation `nix/nv-gsp-firmware.nix` takes these five files from the
  pinned nixpkgs (`nix/nixpkgs.nix`) `linux-firmware` (20260410 at the current
  pin), following symlinks.
- It runs `sha256sum -c` against the table above and fails the build on any
  mismatch. A nixpkgs bump that changes a blob must fail, not silently change
  the ABI.
- It writes `$out/lib/firmware/nvidia/tu104/{gsp,acr}/...` (uncompressed) and
  a `SHA256SUMS` file.

**Licence.**
- The nixpkgs firmware output does not carry the licence texts.
- The derivation copies `LICENSES/LICENCE.nvidia` (and the relevant WHENCE
  stanza) from `linux-firmware.src` into
  `$out/share/licenses/nv-gsp-firmware/`, and sets `meta.license` to
  `lib.licenses.unfreeRedistributableFirmware`.
- The licence must ship next to the blobs in every image that carries them.
- §2.1.2 allows unmodified redistribution for OSI-licensed OSes, so both
  image variants qualify (public and limited). This matches design doc §1.6.

**Where.** The desktop live store (`/nix` on the SSD store slot p2/p3),
referenced from the desktop profile next to the Mesa runtime. Not on p1,
because:
- About 29 MB would ride along on every signed p1 kernel update set.
- p1 holds the kernel, loader and INITRD, and is size-checked per push.
- GSP boot is deferred until the desktop and store are up (design §4.1), so
  early availability buys nothing.

The kernel learns the store path from an `ETC/nv-gsp-firmware` record, written
the same way as `ETC/live-store`. It re-verifies each file's sha256 with the
existing BrookSha256 before copying it to DMA memory, and refuses on any
mismatch.

## Not done in M0

- The live MMIO bus and DMA allocation (radix3/queue memory).
- The libos/rmargs objects and the SET_SYSTEM_INFO/SET_REGISTRY payloads.
- The GSP_INIT_DONE wait, postinit (GET_GSP_STATIC_INFO, the interrupt table),
  and fini (UNLOADING_GUEST_DRIVER, FWSEC-SB, booter_unload).
- The bridge verbs.
- Signature selection for a debug-fused SEC2: Brook refuses, and this card is
  production-fused.
- FWSEC v3 fuse-version selection: index 0 is used, which matches nouveau on
  TU10x.
