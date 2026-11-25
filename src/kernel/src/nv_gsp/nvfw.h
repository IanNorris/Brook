/*
 * Copyright 2022 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
/*
 * NVIDIA falcon firmware container layouts, ported from Linux nouveau
 * (SPDX MIT): include/nvfw/fw.h (bin_hdr, bl_desc), include/nvfw/hs.h
 * (HS header v2, load header v2), include/nvfw/flcn.h (flcn_bl_dmem_desc_v2),
 * nvkm/subdev/gsp/fwsec.c 29-87 and 134-170 (application interface,
 * DMEM mapper v3, FRTS command, falcon ucode descriptor v2/v3).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace brook::nvgsp::nvfw {

struct BinHdr {                       // fw.h nvfw_bin_hdr
    uint32_t bin_magic;               // 0x10de
    uint32_t bin_ver;
    uint32_t bin_size;
    uint32_t header_offset;
    uint32_t data_offset;
    uint32_t data_size;
};
static_assert(sizeof(BinHdr) == 24, "");
constexpr uint32_t kBinMagic = 0x10de;

struct BlDesc {                       // fw.h nvfw_bl_desc (acr/bl.bin)
    uint32_t start_tag;
    uint32_t dmem_load_off;
    uint32_t code_off;
    uint32_t code_size;
    uint32_t data_off;
    uint32_t data_size;
};
static_assert(sizeof(BlDesc) == 24, "");

struct HsHeaderV2 {                   // hs.h nvfw_hs_header_v2
    uint32_t sig_prod_offset;
    uint32_t sig_prod_size;
    uint32_t patch_loc;
    uint32_t patch_sig;
    uint32_t meta_data_offset;
    uint32_t meta_data_size;
    uint32_t num_sig;
    uint32_t header_offset;
    uint32_t header_size;
};
static_assert(sizeof(HsHeaderV2) == 36, "");

struct HsLoadHeaderV2App {
    uint32_t offset;
    uint32_t size;
    uint32_t data_offset;
    uint32_t data_size;
};
struct HsLoadHeaderV2 {               // hs.h nvfw_hs_load_header_v2
    uint32_t os_code_offset;
    uint32_t os_code_size;
    uint32_t os_data_offset;
    uint32_t os_data_size;
    uint32_t num_apps;
    // HsLoadHeaderV2App app[num_apps] follows
};
static_assert(sizeof(HsLoadHeaderV2) == 20, "");

#pragma pack(push, 1)
struct FlcnBlDmemDescV2 {             // flcn.h flcn_bl_dmem_desc_v2 (__packed)
    uint32_t reserved[4];
    uint32_t signature[4];
    uint32_t ctx_dma;
    uint64_t code_dma_base;
    uint32_t non_sec_code_off;
    uint32_t non_sec_code_size;
    uint32_t sec_code_off;
    uint32_t sec_code_size;
    uint32_t code_entry_point;
    uint64_t data_dma_base;
    uint32_t data_size;
    uint32_t argc;
    uint32_t argv;
};
#pragma pack(pop)
static_assert(sizeof(FlcnBlDmemDescV2) == 0x54, "");
constexpr uint32_t kFalconDmaIdxPhysSysNcoh = 4;   // FALCON_DMAIDX_PHYS_SYS_NCOH

struct FalconUcodeDescV2 {            // fwsec.c nvkm_falcon_ucode_desc_v2
    uint32_t Hdr;
    uint32_t StoredSize;
    uint32_t UncompressedSize;
    uint32_t VirtualEntry;
    uint32_t InterfaceOffset;
    uint32_t IMEMPhysBase;
    uint32_t IMEMLoadSize;
    uint32_t IMEMVirtBase;
    uint32_t IMEMSecBase;
    uint32_t IMEMSecSize;
    uint32_t DMEMOffset;
    uint32_t DMEMPhysBase;
    uint32_t DMEMLoadSize;
    uint32_t altIMEMLoadSize;
    uint32_t altDMEMLoadSize;
};
static_assert(sizeof(FalconUcodeDescV2) == 60, "");

struct FalconUcodeDescV3 {            // fwsec.c nvkm_falcon_ucode_desc_v3
    uint32_t Hdr;
    uint32_t StoredSize;
    uint32_t PKCDataOffset;
    uint32_t InterfaceOffset;
    uint32_t IMEMPhysBase;
    uint32_t IMEMLoadSize;
    uint32_t IMEMVirtBase;
    uint32_t DMEMPhysBase;
    uint32_t DMEMLoadSize;
    uint16_t EngineIdMask;
    uint8_t  UcodeId;
    uint8_t  SignatureCount;
    uint16_t SignatureVersions;
    uint16_t Reserved;
};
static_assert(sizeof(FalconUcodeDescV3) == 44, "");

struct AppIfHdrV1 { uint8_t ver, hdr, len, cnt; };       // fwsec.c 29-36
struct AppIfV1 { uint32_t id; uint32_t dmem_base; };     // fwsec.c 38-44
constexpr uint32_t kAppIfIdDmemMapper = 0x00000004;

struct DmemMapperV3 {                 // fwsec.c 46-68
    uint32_t signature;
    uint16_t version;
    uint16_t size;
    uint32_t cmd_in_buffer_offset;
    uint32_t cmd_in_buffer_size;
    uint32_t cmd_out_buffer_offset;
    uint32_t cmd_out_buffer_size;
    uint32_t nvf_img_data_buffer_offset;
    uint32_t nvf_img_data_buffer_size;
    uint32_t printf_buffer_hdr;
    uint32_t ucode_build_time_stamp;
    uint32_t ucode_signature;
    uint32_t init_cmd;
    uint32_t ucode_feature;
    uint32_t ucode_cmd_mask0;
    uint32_t ucode_cmd_mask1;
    uint32_t multi_tgt_tbl;
};
static_assert(offsetof(DmemMapperV3, init_cmd) == 0x2c, "");
constexpr uint32_t kDmemMapperCmdFrts = 0x15;
constexpr uint32_t kDmemMapperCmdSb   = 0x19;

struct FwsecFrtsCmd {                 // fwsec.c 70-87
    struct { uint32_t ver, hdr; uint64_t addr; uint32_t size, flags; } read_vbios;
    struct { uint32_t ver, hdr, addr, size, type; } frts_region;
};
static_assert(offsetof(FwsecFrtsCmd, frts_region) == 24, "");
constexpr uint32_t kFrtsRegionTypeFb = 2;

} // namespace brook::nvgsp::nvfw
