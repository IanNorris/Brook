#pragma once
#include <stdint.h>
#include "completion_wait.h"

namespace brook {
// Shared wire declarations let actual-body hosted tests use the same layout.
struct __attribute__((packed)) VirtioGpuCtrlHdr {
    uint32_t type,flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx,padding[3];
};
struct __attribute__((packed)) VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags,next;
};
struct __attribute__((packed)) VirtqUsedElem { uint32_t id,len; };
static_assert(sizeof(VirtioGpuCtrlHdr)==24 && sizeof(VirtqDesc)==16 && sizeof(VirtqUsedElem)==8);

enum class GpuCommandStorageState:uint8_t { Writable,Preparing,Published,Completed,Quarantined };
enum class GpuCommandPurpose:uint8_t { General,CompositorVertexTransfer };
enum class GpuControlFault:uint8_t { None,Timeout,UnexpectedUsed,InvalidCompletion,RecoveryFailed };
// Allocated outside the frame loop. Buffer pages and this metadata outlive every
// token and remain retained after quarantine. Caller serializes normal access.
struct GpuCommandStorage {
    uint8_t* bytes=nullptr;
    uint64_t physical=0,generation=0,expectedFence=0;
    uint32_t responseCapacity=0,expectedContext=0;
    uint16_t head=0,usedPosition=0;
    uint8_t expectedRing=0;
    bool timeline=false;
    GpuCompletionWaitOrigin origin{};
    GpuCommandStorageState state=GpuCommandStorageState::Writable;
    GpuCommandPurpose purpose=GpuCommandPurpose::General;
    GpuCommandStorage()=default;
    GpuCommandStorage(const GpuCommandStorage&)=delete;
    GpuCommandStorage& operator=(const GpuCommandStorage&)=delete;
};
struct GpuCommandToken {
    GpuCommandStorage* storage=nullptr;
    uint64_t generation=0;
    explicit operator bool() const { return storage && generation; }
};
} // namespace brook
