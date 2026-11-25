/* SPDX-License-Identifier: MIT */

/* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved. */

/*
 * RM object/control ABI subset for M2 (client, device, subdevice, VA space,
 * channel group, GPFIFO channel) and the GET_GSP_STATIC_INFO layout, from
 * https://github.com/NVIDIA/open-gpu-kernel-modules/tree/570.144 as excerpted
 * (MIT) by Linux nouveau:
 *   nvkm/subdev/gsp/rm/r570/nvrm/{gsp.h,client.h,fifo.h,engine.h,rpcfn.h}
 *   nvkm/subdev/gsp/rm/r535/nvrm/{alloc.h,ctrl.h,device.h,vmm.h,fifo.h}
 *     (nouveau drives 570.144 through these r535 definitions unchanged)
 * Offsets and sizes were computed by compiling nouveau's headers on the host
 * (25 Nov) and are pinned with static_asserts. Entries marked UNVERIFIED are
 * from open-gpu class headers not excerpted by nouveau.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "nvrm_r570.h"

namespace brook::nvgsp::rm570 {

typedef NvU32 NvHandle;

enum : NvU32 {
    NV_VGPU_MSG_FUNCTION_FREE           = 10,
    NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE = 70,
};

// ------------------------------------------------ GET_GSP_STATIC_INFO (r570)
// GspStaticConfigInfo, sizeof 0x678 (== kStaticInfoBytes).
namespace static_info {
constexpr uint32_t kGidInfo           = 0x018;   // NV2080_CTRL_GPU_GET_GID_INFO_PARAMS {index, flags, length, data[256]}
constexpr uint32_t kSkuInfo           = 0x124;
constexpr uint32_t kFbRegionInfo      = 0x158;   // NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO_PARAMS (0x308)
constexpr uint32_t kFbRegionCount     = 16;
constexpr uint32_t kFbRegionStride    = 0x30;    // base, limit, reserved (u64), performance, comp, iso, prot, blackList[17]
constexpr uint32_t kFbRegionFirst     = 0x008;   // within the params
constexpr uint32_t kFbLength          = 0x4c8;
constexpr uint32_t kFbBusWidth        = 0x4d8;
constexpr uint32_t kFbRamType         = 0x4dc;
constexpr uint32_t kL2CacheSize       = 0x4e8;
constexpr uint32_t kGpuName           = 0x4ec;   // 64 bytes
constexpr uint32_t kGpuShortName      = 0x52c;   // 64 bytes
constexpr uint32_t kBar1PdeBase       = 0x600;
constexpr uint32_t kBar2PdeBase       = 0x608;
constexpr uint32_t kVbiosValid        = 0x610;
constexpr uint32_t kSplitVas          = 0x61d;   // bSplitVasBetweenServerClientRm
constexpr uint32_t kInternalClient    = 0x640;
constexpr uint32_t kInternalDevice    = 0x644;
constexpr uint32_t kInternalSubdevice = 0x648;
constexpr uint32_t kIsGpuUefi         = 0x64e;
constexpr uint32_t kIsEfiInit         = 0x64f;
constexpr uint32_t kFwWprLayout       = 0x668;   // {nonWprHeapOffset, frtsOffset}
constexpr uint32_t kBytes             = 0x678;
}  // namespace static_info

// ---------------------------------------------------------- RM_ALLOC etc.
struct rpc_gsp_rm_alloc_v03_00 {
    NvHandle hClient, hParent, hObject;
    NvU32 hClass, status, paramsSize, flags;
    NvU8 reserved[4];
    // NvU8 params[] follows
};
static_assert(sizeof(rpc_gsp_rm_alloc_v03_00) == 0x20, "");
struct rpc_gsp_rm_control_v03_00 {
    NvHandle hClient, hObject;
    NvU32 cmd, status, paramsSize, flags;
    // NvU8 params[] follows
};
static_assert(sizeof(rpc_gsp_rm_control_v03_00) == 0x18, "");
struct rpc_free_v03_00 {
    NvHandle hRoot, hObjectParent, hObjectOld;
    NvU32 status;
};
static_assert(sizeof(rpc_free_v03_00) == 0x10, "");

// classes
constexpr NvU32 NV01_ROOT               = 0x0000;
constexpr NvU32 NV01_DEVICE_0           = 0x0080;
constexpr NvU32 NV20_SUBDEVICE_0        = 0x2080;
constexpr NvU32 FERMI_VASPACE_A         = 0x90f1;
constexpr NvU32 KEPLER_CHANNEL_GROUP_A  = 0xa06c;
constexpr NvU32 TURING_CHANNEL_GPFIFO_A = 0xc46f;
constexpr NvU32 TURING_USERMODE_A       = 0xc461;

struct NV0000_ALLOC_PARAMETERS {
    NvHandle hClient;
    NvU32 processID;
    char processName[100];
    NvU32 pad_;
    NvU64 pOsPidInfo;
};
static_assert(sizeof(NV0000_ALLOC_PARAMETERS) == 0x78 && offsetof(NV0000_ALLOC_PARAMETERS, pOsPidInfo) == 0x70, "");

struct NV0080_ALLOC_PARAMETERS {
    NvU32 deviceId;
    NvHandle hClientShare, hTargetClient, hTargetDevice;
    NvU32 flags, pad0_;
    NvU64 vaSpaceSize, vaStartInternal, vaLimitInternal;
    NvU32 vaMode, pad1_;
};
static_assert(sizeof(NV0080_ALLOC_PARAMETERS) == 0x38 && offsetof(NV0080_ALLOC_PARAMETERS, vaMode) == 0x30, "");

struct NV2080_ALLOC_PARAMETERS { NvU32 subDeviceId; };

struct NV_VASPACE_ALLOCATION_PARAMETERS {
    NvU32 index, flags;
    NvU64 vaSize, vaStartInternal, vaLimitInternal;
    NvU32 bigPageSize, pad_;
    NvU64 vaBase;
};
static_assert(sizeof(NV_VASPACE_ALLOCATION_PARAMETERS) == 0x30 &&
              offsetof(NV_VASPACE_ALLOCATION_PARAMETERS, bigPageSize) == 0x20, "");
constexpr NvU32 NV_VASPACE_ALLOCATION_INDEX_GPU_NEW = 0;
constexpr NvU32 NV_VASPACE_ALLOCATION_FLAGS_IS_EXTERNALLY_OWNED = 1u << 3;

constexpr NvU32 NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY   = 0x00801813;
constexpr NvU32 NV0080_CTRL_CMD_DMA_UNSET_PAGE_DIRECTORY = 0x00801814;
struct NV0080_CTRL_DMA_SET_PAGE_DIRECTORY_PARAMS {
    NvU64 physAddress;
    NvU32 numEntries, flags;
    NvHandle hVASpace;
    NvU32 chId, subDeviceId, pasid;
};
static_assert(sizeof(NV0080_CTRL_DMA_SET_PAGE_DIRECTORY_PARAMS) == 0x20, "");
constexpr NvU32 SET_PAGE_DIRECTORY_APERTURE_VIDMEM     = 0;
constexpr NvU32 SET_PAGE_DIRECTORY_APERTURE_SYSMEM_COH = 1;
struct NV0080_CTRL_DMA_UNSET_PAGE_DIRECTORY_PARAMS { NvHandle hVASpace; NvU32 subDeviceId; };

// ------------------------------------------------------------- FIFO
struct NV_MEMORY_DESC_PARAMS {
    NvU64 base, size;
    NvU32 addressSpace;   // 1 = ADDR_SYSMEM, 2 = ADDR_FBMEM (nouveau r535_chan_alloc)
    NvU32 cacheAttrib;    // NV_MEMORY_CACHED 0, UNCACHED 1, WRITECOMBINED 2 (nouveau: VRAM 1, mthdbuf 0)
};
static_assert(sizeof(NV_MEMORY_DESC_PARAMS) == 0x18, "");
constexpr NvU32 ADDR_SYSMEM = 1, ADDR_FBMEM = 2;

struct NV_CHANNEL_ALLOC_PARAMS {
    NvHandle hObjectError, hObjectBuffer;
    NvU64 gpFifoOffset;
    NvU32 gpFifoEntries, flags;
    NvHandle hContextShare, hVASpace;
    NvHandle hUserdMemory[8];
    NvU64 userdOffset[8];
    NvU32 engineType, cid, subDeviceId;
    NvHandle hObjectEccError;
    NV_MEMORY_DESC_PARAMS instanceMem, userdMem, ramfcMem, mthdbufMem;
    NvHandle hPhysChannelGroup;
    NvU32 internalFlags;
    NV_MEMORY_DESC_PARAMS errorNotifierMem, eccErrorNotifierMem;
    NvU32 ProcessID, SubProcessID;
    NvU32 encryptIv[3], decryptIv[3], hmacNonce[8];
    NvU32 tpcConfigID, pad_;
};
static_assert(sizeof(NV_CHANNEL_ALLOC_PARAMS) == 0x170, "");
static_assert(offsetof(NV_CHANNEL_ALLOC_PARAMS, engineType) == 0x80, "");
static_assert(offsetof(NV_CHANNEL_ALLOC_PARAMS, instanceMem) == 0x90, "");
static_assert(offsetof(NV_CHANNEL_ALLOC_PARAMS, mthdbufMem) == 0xd8, "");
static_assert(offsetof(NV_CHANNEL_ALLOC_PARAMS, internalFlags) == 0xf4, "");
static_assert(offsetof(NV_CHANNEL_ALLOC_PARAMS, ProcessID) == 0x128, "");
static_assert(offsetof(NV_CHANNEL_ALLOC_PARAMS, tpcConfigID) == 0x168, "");

// NVOS04 flags fields (r570 fifo.h)
constexpr NvU32 NVOS04_GROUP_CHANNEL_RUNQUEUE_SHIFT = 4;
constexpr NvU32 NVOS04_PRIVILEGED_CHANNEL_TRUE      = 1u << 5;
constexpr NvU32 NVOS04_USERD_INDEX_VALUE_SHIFT      = 8;    // 10:8
constexpr NvU32 NVOS04_USERD_INDEX_PAGE_VALUE_SHIFT = 12;   // 20:12
constexpr NvU32 NVOS04_USERD_INDEX_PAGE_FIXED_TRUE  = 1u << 21;
constexpr NvU32 KERNELCHANNEL_PRIVILEGE_ADMIN       = 1;    // internalFlags 1:0
constexpr NvU32 KERNELCHANNEL_ERROR_NOTIFIER_NONE   = 1;    // internalFlags 3:2 and 5:4 (ERROR_NOTIFIER_TYPE_NONE)

// open-gpu nvos.h 2510-2518 (not excerpted by nouveau). A channel whose parent
// is the TSG must pass hVASpace 0 and engineType 0 or the TSG's (kernel_channel.c 414-436, 696-744).
struct NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS {
    NvHandle hObjectError, hObjectEccError, hVASpace;
    NvU32 engineType;
    NvBool bIsCallingContextVgpuPlugin;
    NvU8 pad_[3];
};
static_assert(sizeof(NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS) == 20, "");

constexpr NvU32 NVA06F_CTRL_CMD_GPFIFO_SCHEDULE = 0xa06f0103;
constexpr NvU32 NVA06F_CTRL_CMD_BIND            = 0xa06f0104;
struct NVA06F_CTRL_BIND_PARAMS { NvU32 engineType; };
struct NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS { NvBool bEnable, bSkipSubmit; };
// open-gpu ctrlc36f.h 63-88. CPU-RM answers it locally in GSP-client mode, so
// GSP-RM may not; the Turing token is runlist << 16 | chid (kernel_fifo_tu102.c 72-127).
constexpr NvU32 NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN = 0xc36f0108;
struct NVC36F_CTRL_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS { NvU32 workSubmitToken; };

constexpr NvU32 NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE = 0x20801112;
constexpr NvU32 kDevInfoMaxEntries = 32, kDevInfoDataTypes = 16;
struct NV2080_CTRL_FIFO_DEVICE_ENTRY {
    NvU32 engineData[16];
    NvU32 pbdmaIds[2], pbdmaFaultIds[2];
    NvU32 numPbdmas;
    char engineName[16];
};
static_assert(sizeof(NV2080_CTRL_FIFO_DEVICE_ENTRY) == 100, "");
struct NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS {
    NvU32 baseIndex, numEntries;
    NvBool bMore;
    NvU8 pad_[3];
    NV2080_CTRL_FIFO_DEVICE_ENTRY entries[32];
};
static_assert(sizeof(NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS) == 12 + 32 * 100, "");
enum : NvU32 {
    ENGINE_INFO_TYPE_ENG_DESC = 0, ENGINE_INFO_TYPE_FIFO_TAG, ENGINE_INFO_TYPE_RM_ENGINE_TYPE, ENGINE_INFO_TYPE_RUNLIST,
    ENGINE_INFO_TYPE_MMU_FAULT_ID, ENGINE_INFO_TYPE_RC_MASK, ENGINE_INFO_TYPE_RESET, ENGINE_INFO_TYPE_INTR,
    ENGINE_INFO_TYPE_MC, ENGINE_INFO_TYPE_DEV_TYPE_ENUM, ENGINE_INFO_TYPE_INSTANCE_ID,
    ENGINE_INFO_TYPE_RUNLIST_PRI_BASE, ENGINE_INFO_TYPE_IS_HOST_DRIVEN_ENGINE, ENGINE_INFO_TYPE_RUNLIST_ENGINE_ID,
    ENGINE_INFO_TYPE_CHRAM_PRI_BASE,
};
constexpr NvU32 RM_ENGINE_TYPE_GR0 = 0x01, RM_ENGINE_TYPE_COPY0 = 0x09, RM_ENGINE_TYPE_COPY9 = 0x12;

constexpr NvU32 NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE = 0x20802a08;
struct NV2080_CTRL_CE_GET_FAULT_METHOD_BUFFER_SIZE_PARAMS { NvU32 size; };

// Host methods (TURING_CHANNEL_GPFIFO_A; nouveau nvhw/class/clc36f.h). NOP is
// the 0x0008 host method (open-gpu clc46f.h 68-224).
namespace host {
constexpr uint32_t NOP = 0x0008, SEM_ADDR_LO = 0x005c, SEM_ADDR_HI = 0x0060, SEM_PAYLOAD_LO = 0x0064,
                   SEM_PAYLOAD_HI = 0x0068, SEM_EXECUTE = 0x006c;
constexpr uint32_t SEM_EXECUTE_RELEASE = 1, SEM_EXECUTE_RELEASE_WFI = 1u << 20;
constexpr uint32_t USERD_GP_GET = 0x88, USERD_GP_PUT = 0x8c;
constexpr uint32_t kUsermodeBase = 0xbb0000, kDoorbell = 0x90;   // TU102 usermode (vfn 0xb80000 + 0x30000)
inline uint32_t IncHeader(uint32_t subc, uint32_t mthd, uint32_t count) {
    return (1u << 29) | (count << 16) | (subc << 13) | (mthd >> 2);
}
}  // namespace host

}  // namespace brook::nvgsp::rm570
