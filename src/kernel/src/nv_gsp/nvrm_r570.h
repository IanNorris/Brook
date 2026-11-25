/* SPDX-License-Identifier: MIT */

/* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved. */

/*
 * Excerpt of RM headers from
 * https://github.com/NVIDIA/open-gpu-kernel-modules/tree/570.144, as already
 * excerpted (MIT) by Linux nouveau:
 *   drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/{gsp,rpcfn,msgfn}.h
 *   drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/gsp.h (the libos,
 *     message-queue, sequencer and RISC-V descriptor types; nouveau drives
 *     570.144 through the same r535 code for these, so their ABI is unchanged)
 * Ported to Brook (stdint types, namespaced, layout static_asserts) for the
 * pinned 570.144 GSP-RM firmware only. Do not mix with other RM versions.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace brook::nvgsp::rm570 {

typedef uint8_t  NvU8;
typedef uint16_t NvU16;
typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef NvU8     NvBool;
typedef NvU64    NvLength;
typedef NvU64    RmPhysAddr;

// ------------------------------------------------------------ WPR metadata
// r570/nvrm/gsp.h 343-491. Anonymous unions flattened to named members.
struct GspFwWprMeta {
    NvU64 magic;                    // GSP_FW_WPR_META_MAGIC
    NvU64 revision;                 // GSP_FW_WPR_META_REVISION
    NvU64 sysmemAddrOfRadix3Elf;
    NvU64 sizeOfRadix3Elf;
    NvU64 sysmemAddrOfBootloader;
    NvU64 sizeOfBootloader;
    NvU64 bootloaderCodeOffset;
    NvU64 bootloaderDataOffset;
    NvU64 bootloaderManifestOffset;
    NvU64 sysmemAddrOfSignature;    // union { sig addr/size | heap free-list (resume) }
    NvU64 sizeOfSignature;
    NvU64 gspFwRsvdStart;
    NvU64 nonWprHeapOffset;
    NvU64 nonWprHeapSize;
    NvU64 gspFwWprStart;
    NvU64 gspFwHeapOffset;
    NvU64 gspFwHeapSize;
    NvU64 gspFwOffset;
    NvU64 bootBinOffset;
    NvU64 frtsOffset;
    NvU64 frtsSize;
    NvU64 gspFwWprEnd;
    NvU64 fbSize;
    NvU64 vgaWorkspaceOffset;
    NvU64 vgaWorkspaceSize;
    NvU64 bootCount;
    NvU64 partitionRpcAddr;         // union { partitionRpc*/elf* | crash-report queue }
    NvU16 partitionRpcRequestOffset;
    NvU16 partitionRpcReplyOffset;
    NvU32 elfCodeOffset;
    NvU32 elfDataOffset;
    NvU32 elfCodeSize;
    NvU32 elfDataSize;
    NvU32 lsUcodeVersion;
    NvU8  gspFwHeapVfPartitionCount;
    NvU8  flags;
    NvU8  padding[2];
    NvU32 pmuReservedSize;
    NvU64 verified;                 // 0 unverified, 0xa0a0a0a0a0a0a0a0 verified
};
static_assert(sizeof(GspFwWprMeta) == 256, "GspFwWprMeta is exactly 256 bytes");
static_assert(offsetof(GspFwWprMeta, gspFwRsvdStart) == 0x58, "");
static_assert(offsetof(GspFwWprMeta, frtsOffset) == 0x98, "");
static_assert(offsetof(GspFwWprMeta, partitionRpcAddr) == 0xd0, "");
static_assert(offsetof(GspFwWprMeta, lsUcodeVersion) == 0xec, "");
static_assert(offsetof(GspFwWprMeta, gspFwHeapVfPartitionCount) == 0xf0, "");
static_assert(offsetof(GspFwWprMeta, verified) == 0xf8, "");

constexpr NvU64 GSP_FW_WPR_META_MAGIC    = 0xdc3aae21371a60b3ULL;
constexpr NvU64 GSP_FW_WPR_META_REVISION = 1;

// ------------------------------------------------------- RM boot arguments
// r570/nvrm/gsp.h 497-522 (r570 dropped the lockless queue offsets and added
// bDmemStack relative to r535).
struct MESSAGE_QUEUE_INIT_ARGUMENTS {
    NvU64    sharedMemPhysAddr;
    NvU32    pageTableEntryCount;
    NvU32    pad0_;                 // natural alignment of NvLength
    NvLength cmdQueueOffset;
    NvLength statQueueOffset;
};
static_assert(sizeof(MESSAGE_QUEUE_INIT_ARGUMENTS) == 32, "");

struct GSP_SR_INIT_ARGUMENTS {
    NvU32  oldLevel;
    NvU32  flags;
    NvBool bInPMTransition;
    NvU8   pad_[3];
};
static_assert(sizeof(GSP_SR_INIT_ARGUMENTS) == 12, "");

struct GSP_ARGUMENTS_CACHED {
    MESSAGE_QUEUE_INIT_ARGUMENTS messageQueueInitArguments;
    GSP_SR_INIT_ARGUMENTS        srInitArguments;
    NvU32                        gpuInstance;
    NvBool                       bDmemStack;
    NvU8                         pad_[7];
    NvU64                        profilerArgsPa;
    NvU64                        profilerArgsSize;
};
static_assert(offsetof(GSP_ARGUMENTS_CACHED, srInitArguments) == 32, "");
static_assert(offsetof(GSP_ARGUMENTS_CACHED, gpuInstance) == 44, "");
static_assert(offsetof(GSP_ARGUMENTS_CACHED, bDmemStack) == 48, "");
static_assert(offsetof(GSP_ARGUMENTS_CACHED, profilerArgsPa) == 56, "");
static_assert(sizeof(GSP_ARGUMENTS_CACHED) == 72, "");

constexpr NvU32 NV2080_CTRL_GPU_SET_POWER_STATE_GPU_LEVEL_3 = 3;
constexpr NvU32 GPU_STATE_FLAGS_PRESERVING    = 1u << 0;
constexpr NvU32 GPU_STATE_FLAGS_PM_TRANSITION = 1u << 2;

// ------------------------------------------------------- WPR heap sizing
// r570/nvrm/gsp.h 568-574, r535/nvrm/gsp.h 781-783.
constexpr NvU64 GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS2       = 0ull << 20;
constexpr NvU64 GSP_FW_HEAP_PARAM_BASE_RM_SIZE_TU10X   = 8ull << 20;
constexpr NvU64 GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS2_MIN_MB = 64;
constexpr NvU64 GSP_FW_HEAP_PARAM_SIZE_PER_GB_FB       = 96ull << 10;
constexpr NvU64 GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE    = (48ull << 10) * 2048;

// ---------------------------------------------------------- libos regions
// r535/nvrm/gsp.h 608-631.
struct LibosMemoryRegionInitArgument {
    NvU64 id8;
    NvU64 pa;
    NvU64 size;
    NvU8  kind;
    NvU8  loc;
    NvU8  pad_[6];
};
static_assert(sizeof(LibosMemoryRegionInitArgument) == 32, "");
constexpr NvU8 LIBOS_MEMORY_REGION_CONTIGUOUS = 1;
constexpr NvU8 LIBOS_MEMORY_REGION_RADIX3     = 2;
constexpr NvU8 LIBOS_MEMORY_REGION_LOC_SYSMEM = 1;
constexpr NvU8 LIBOS_MEMORY_REGION_LOC_FB     = 2;

// ---------------------------------------------------------- message queues
// r535/nvrm/gsp.h 560-576.
struct msgqTxHeader {
    NvU32 version;   // queue version
    NvU32 size;      // bytes, page aligned
    NvU32 msgSize;   // entry size, bytes, power of 2, >= 16
    NvU32 msgCount;  // number of entries in queue
    NvU32 writePtr;  // message id of next slot
    NvU32 flags;     // if set it means "i want to swap RX"
    NvU32 rxHdrOff;  // offset of msgqRxHeader from start of backing store
    NvU32 entryOff;  // offset of entries from start of backing store
};
static_assert(sizeof(msgqTxHeader) == 32, "");
struct msgqRxHeader {
    NvU32 readPtr;   // message id of last message read
};

// Queue element header (nouveau r535/rpc.c struct r535_gsp_msg) and the RPC
// header that follows it (struct nvfw_gsp_rpc, rpc_message_header_v).
struct GSP_MSG_QUEUE_ELEMENT {
    NvU8  authTagBuffer[16];
    NvU8  aadBuffer[16];
    NvU32 checkSum;
    NvU32 seqNum;
    NvU32 elemCount;
    NvU32 pad;
};
static_assert(sizeof(GSP_MSG_QUEUE_ELEMENT) == 48, "");
struct rpc_message_header_v {
    NvU32 header_version;           // 0x03000000
    NvU32 signature;                // 'CPRV'
    NvU32 length;                   // header + payload bytes
    NvU32 function;
    NvU32 rpc_result;
    NvU32 rpc_result_private;
    NvU32 sequence;
    NvU32 spare;                    // cpuRmGfid
};
static_assert(sizeof(rpc_message_header_v) == 32, "");
constexpr NvU32 RPC_HEADER_VERSION = 0x03000000;
constexpr NvU32 RPC_SIGNATURE = ('C' << 24) | ('P' << 16) | ('R' << 8) | 'V';

// ----------------------------------------------------------- CPU sequencer
// r535/nvrm/gsp.h 350-415, 684-694.
struct rpc_run_cpu_sequencer_v17_00 {
    NvU32 bufferSizeDWord;
    NvU32 cmdIndex;
    NvU32 regSaveArea[8];
    // NvU32 commandBuffer[] follows
};
static_assert(sizeof(rpc_run_cpu_sequencer_v17_00) == 40, "");
enum GSP_SEQ_BUF_OPCODE : NvU32 {
    GSP_SEQ_BUF_OPCODE_REG_WRITE = 0,
    GSP_SEQ_BUF_OPCODE_REG_MODIFY,
    GSP_SEQ_BUF_OPCODE_REG_POLL,
    GSP_SEQ_BUF_OPCODE_DELAY_US,
    GSP_SEQ_BUF_OPCODE_REG_STORE,
    GSP_SEQ_BUF_OPCODE_CORE_RESET,
    GSP_SEQ_BUF_OPCODE_CORE_START,
    GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT,
    GSP_SEQ_BUF_OPCODE_CORE_RESUME,
};
// GSP_SEQUENCER_PAYLOAD_SIZE_DWORDS: write {addr,val}=2, modify
// {addr,mask,val}=3, poll {addr,mask,val,timeout,error}=5, delay {val}=1,
// store {addr,index}=2, core ops 0.
constexpr NvU32 GspSeqPayloadDwords(NvU32 op) {
    return op == GSP_SEQ_BUF_OPCODE_REG_WRITE  ? 2 :
           op == GSP_SEQ_BUF_OPCODE_REG_MODIFY ? 3 :
           op == GSP_SEQ_BUF_OPCODE_REG_POLL   ? 5 :
           op == GSP_SEQ_BUF_OPCODE_DELAY_US   ? 1 :
           op == GSP_SEQ_BUF_OPCODE_REG_STORE  ? 2 : 0;
}

// ------------------------------------------------- RISC-V bootloader desc
// r535/nvrm/gsp.h 697-747.
struct RM_RISCV_UCODE_DESC {
    NvU32 version;
    NvU32 bootloaderOffset;
    NvU32 bootloaderSize;
    NvU32 bootloaderParamOffset;
    NvU32 bootloaderParamSize;
    NvU32 riscvElfOffset;
    NvU32 riscvElfSize;
    NvU32 appVersion;
    NvU32 manifestOffset;
    NvU32 manifestSize;
    NvU32 monitorDataOffset;
    NvU32 monitorDataSize;
    NvU32 monitorCodeOffset;
    NvU32 monitorCodeSize;
    NvU32 bIsMonitorEnabled;
    NvU32 swbromCodeOffset;
    NvU32 swbromCodeSize;
    NvU32 swbromDataOffset;
    NvU32 swbromDataSize;
    NvU32 fbReservedSize;
    NvU32 bSignedAsCode;
};
static_assert(sizeof(RM_RISCV_UCODE_DESC) == 84, "");

// ------------------------------------------------ RPC function / events
// r570/nvrm/rpcfn.h (subset) and msgfn.h.
enum : NvU32 {
    NV_VGPU_MSG_FUNCTION_UNLOADING_GUEST_DRIVER = 47,
    NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO    = 65,
    NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD    = 71,
    NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO    = 72,
    NV_VGPU_MSG_FUNCTION_SET_REGISTRY           = 73,
    NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL         = 76,
    NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC           = 103,
    NV_VGPU_MSG_FUNCTION_NUM_FUNCTIONS          = 227,
};
enum : NvU32 {
    NV_VGPU_MSG_EVENT_GSP_INIT_DONE             = 0x1001,
    NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER     = 0x1002,
    NV_VGPU_MSG_EVENT_POST_EVENT                = 0x1003,
    NV_VGPU_MSG_EVENT_RC_TRIGGERED              = 0x1004,
    NV_VGPU_MSG_EVENT_MMU_FAULT_QUEUED          = 0x1005,
    NV_VGPU_MSG_EVENT_OS_ERROR_LOG              = 0x1006,
    NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT         = 0x100c,
    NV_VGPU_MSG_EVENT_PERF_BRIDGELESS_INFO_UPDATE = 0x100f,
    NV_VGPU_MSG_EVENT_GSP_LOCKDOWN_NOTICE       = 0x101c,
    NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD     = 0x1020,
    NV_VGPU_MSG_EVENT_NUM_EVENTS                = 0x1023,
};

} // namespace brook::nvgsp::rm570
