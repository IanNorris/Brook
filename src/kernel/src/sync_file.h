#pragma once
#include <stdint.h>
#include "gpu_sync.h"
namespace brook {
// A null fence denotes the existing already-completed virtgpu snapshot. `driver` names the
// backend that owns a non-null fence (SYNC_FILE_INFO names only; Intel is the default).
// Non-null fences are retained immutable identities and may still be pending.
struct SyncFileInfo {
    char name[32];
    int32_t status;
    uint32_t flags,numFences,pad;
    uint64_t fences;
};
struct SyncFenceInfo {
    char objectName[32],driverName[32];
    int32_t status;
    uint32_t flags;
    uint64_t timestampNs;
};
enum class SyncFileDriver : uint8_t { Intel = 0, Nouveau = 1 };
struct SyncFileData { uint32_t references;uint64_t timestampNs,identity;GpuFence* fence=nullptr;SyncFileDriver driver=SyncFileDriver::Intel; };
static_assert(sizeof(SyncFileInfo)==56);
static_assert(sizeof(SyncFenceInfo)==80);
static constexpr uint32_t SYNC_FILE_INFO_IOCTL=0xc0383e04;
static inline bool SyncFileInfoRequestValid(const SyncFileInfo& info) { return !info.flags && !info.pad; }
static inline uint32_t SyncFileState(const SyncFileData& file) {
    return file.fence?BdwFenceState(file.fence):1;
}
static inline uint32_t SyncFileReady(const SyncFileData& file,uint32_t events) {
    if (!(events & 1u)) return 0;
    return BdwFenceObservedState(file.fence) || !file.fence ? 1u : 0u;
}
static inline void SyncFileDescribe(const SyncFileData& fence,SyncFileInfo& info,SyncFenceInfo& detail) {
    info={};detail={};
    const bool nv=fence.driver==SyncFileDriver::Nouveau;
    const char* name=fence.fence?(nv?"brook-nouveau":"brook-intel"):"brook-virtgpu";
    for(uint32_t i=0;name[i] && i<31;++i) {info.name[i]=name[i];detail.driverName[i]=name[i];}
    const char* object=fence.fence?(nv?"nouveau-fence":"intel-fence"):"completed-timeline";
    for(uint32_t i=0;object[i] && i<31;++i)detail.objectName[i]=object[i];
    uint64_t timestamp=0;
    const uint32_t state=fence.fence?BdwFenceState(fence.fence,&timestamp):1;
    info.status=detail.status=state==2?-5:static_cast<int32_t>(state);
    info.numFences=1;detail.timestampNs=fence.fence?timestamp:fence.timestampNs;
}
}
