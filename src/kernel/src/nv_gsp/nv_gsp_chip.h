#pragma once
// Per-generation GSP-RM bring-up parameters (the "HAL"). The RPC, queue,
// radix3 and sequencer layers are chip-neutral; everything that differs
// between Turing, Ampere and Ada lives in one ChipOps table:
//   - identity (PMC_BOOT_0) and the falcon bases (GSP, its RISC-V core, and
//     the falcon that runs booter: SEC2 on TU10x)
//   - the readiness, VGA-workspace, FB-size, WPR2 and FWSEC error registers
//   - the FWSEC ucode type / FRTS size and whether FWSEC boots through the
//     ACR bootloader (desc v2)
//   - the WPR2/FB layout function
//   - the firmware file set (names relative to lib/firmware, sizes, sha256)
// Only TU104 (RTX 2080, 570.144) exists. GA102/AD102 would add a table here
// (nouveau gsp/ga102.c, ad102.c) without touching the generic code.
#include <stdint.h>
#include <stddef.h>
#include "nv_gsp_wpr.h"
#include "nv_gmmu.h"

namespace brook::nvgsp {

enum FwRole : uint8_t { kFwGsp = 0, kFwBooterLoad, kFwBooterUnload, kFwBootloader, kFwAcrBl, kFwCount };
inline const char* FwRoleName(uint32_t r) {
    static const char* const n[kFwCount] = {"gsp", "booter_load", "booter_unload", "bootloader", "acr_bl"};
    return r < kFwCount ? n[r] : "?";
}

struct FirmwareFile {
    const char* path;     // relative to the firmware root (".../lib/firmware")
    uint32_t bytes;
    uint8_t sha256[32];
};

struct ChipOps {
    const char* name;
    const char* fw_version;
    uint32_t boot0;                       // exact PMC_BOOT_0
    // falcons
    uint32_t gsp_falcon, gsp_riscv, booter_falcon;
    // registers (BAR0)
    uint32_t reg_boot0, reg_devinit, reg_devinit_scratch, reg_vga_ws, reg_fb_size;
    uint32_t reg_wpr2_lo, reg_wpr2_hi;
    uint32_t reg_frts_err;                // FWSEC-FRTS status word, error in bits 31:16
    uint32_t reg_sb_err;                  // FWSEC-SB status word, error in bits 15:0
    uint32_t reg_booter_boot_status;      // sequencer core-resume poll (bit 26 on TU10x)
    uint32_t booter_debug_fuse_bit;       // booter falcon 0x408: debug-signed part
    // FWSEC
    uint8_t fwsec_ucode_type;
    uint64_t frts_size;
    // layout (WPR2 carve-out and GspFwWprMeta inputs)
    bool (*compute_layout)(const FbLayoutInput&, FbLayout*);
    FirmwareFile fw[kFwCount];
    // M2 / BAR1 handover (after INIT_DONE)
    uint32_t reg_bar1_block, reg_bar2_block;   // BAR1/BAR2 block: bit 31 virtual, 29:28 target, 27:0 inst >> 12
    uint32_t reg_pramin_base, pramin;          // BAR0 window: base register (addr >> 16) and the 1 MiB data window
    uint32_t reg_fb_flush;                     // write 1, poll bit 1 clear
    uint32_t reg_mmu_inval_pdb_lo, reg_mmu_inval_pdb_hi, reg_mmu_inval_trigger;
    uint32_t usermode;                         // TURING_USERMODE_A BAR0 base (doorbell at +0x90)
    const GmmuFormat* gmmu;
};

// The 570.144 TU10x set from linux-firmware (tu104/gsp and tu104/acr are
// WHENCE links to tu102). nix/nv-gsp-firmware.nix checks the same hashes.
inline constexpr ChipOps kChipTu104 = {
    "tu104", "570.144", 0x164000a1,
    0x110000, 0x111000, 0x840000,
    0x000000, 0x118128, 0x118234, 0x625f04, 0x100ce0,
    kRegWpr2Lo, kRegWpr2Hi,
    0x001400 + 0x0e * 4, 0x001400 + 0x15 * 4, 0x1180f8, 0x00100000,
    0x85, 0x100000,
    &ComputeFbLayout,
    {
        {"nvidia/tu104/gsp/gsp-570.144.bin", 28542040,
         {0x30,0x52,0xae,0xe2,0x87,0x21,0x82,0xa1,0x4d,0x8d,0x7c,0x06,0x9e,0x3a,0x14,0xfe,
          0x46,0x42,0x40,0x58,0x94,0xb2,0x46,0x92,0xc4,0xac,0xa4,0x10,0x1d,0xfb,0x18,0x09}},
        {"nvidia/tu104/gsp/booter_load-570.144.bin", 59272,
         {0x7b,0xb1,0x81,0xf5,0x44,0xa9,0x42,0x29,0x9b,0xbf,0x46,0x42,0xda,0x6d,0x9b,0xb5,
          0x8b,0xfe,0xd8,0x45,0x97,0x25,0x09,0x4e,0x2a,0xce,0x56,0x64,0x7c,0xd3,0xec,0x8c}},
        {"nvidia/tu104/gsp/booter_unload-570.144.bin", 39304,
         {0x5c,0x7f,0x1a,0xa3,0x7d,0x04,0x5c,0xc2,0x0b,0xb8,0x15,0x84,0xa8,0x30,0xe8,0xdf,
          0xa4,0x48,0x3f,0x1c,0xd6,0x39,0x43,0xde,0x83,0xa5,0x8c,0x4b,0xe1,0x9a,0x17,0x82}},
        {"nvidia/tu104/gsp/bootloader-570.144.bin", 4196,
         {0x12,0xe9,0x87,0xb6,0x36,0xc2,0xf0,0x0f,0xa4,0x0f,0x42,0xfd,0x95,0x09,0x75,0x15,
          0xc0,0x81,0x7b,0x15,0x81,0x19,0xc5,0x84,0x04,0x9a,0x37,0xfa,0xf3,0x8f,0x8f,0x96}},
        {"nvidia/tu104/acr/bl.bin", 1280,
         {0x01,0x32,0x6c,0xc9,0x97,0x71,0x6b,0xeb,0xb0,0xea,0xb9,0xfe,0x1f,0x46,0x3f,0xff,
          0xa0,0xee,0x58,0x60,0x79,0xe4,0xca,0x0e,0x5b,0xb0,0x96,0xab,0x6f,0xcf,0xed,0xab}},
    },
    // nouveau: vfn/tu102.c (0xb80000 VF base, usermode +0x30000), mmu/vmmtu102.c
    // tu102_vmm_flush (0xb830a0/a4/b0), bar/g84.c flush (0x070000); P4 read
    // 0xb80f40/48 change to virtual mode at INIT_DONE.
    0xb80f40, 0xb80f48,
    0x001700, 0x700000,
    0x070000,
    0xb830a0, 0xb830a4, 0xb830b0,
    0xbb0000,
    &kGmmuV2,
};

inline const ChipOps* ChipForBoot0(uint32_t boot0) {
    return boot0 == kChipTu104.boot0 ? &kChipTu104 : nullptr;
}

} // namespace brook::nvgsp
