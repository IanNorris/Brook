#pragma once
#include <stdint.h>
#include "intel_bdw_fence.h"
namespace brook {
// A null Intel fence denotes the existing already-completed virtgpu snapshot.
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
struct SyncFileData { uint32_t references;uint64_t timestampNs,identity;BdwFence* intelFence=nullptr; };
static_assert(sizeof(SyncFileInfo)==56);
static_assert(sizeof(SyncFenceInfo)==80);
static constexpr uint32_t SYNC_FILE_INFO_IOCTL=0xc0383e04;
static inline bool SyncFileInfoRequestValid(const SyncFileInfo& info) { return !info.flags && !info.pad; }
static inline uint32_t SyncFileState(const SyncFileData& file) {
    return file.intelFence?BdwFenceState(file.intelFence):1;
}
static inline uint32_t SyncFileReady(const SyncFileData& file,uint32_t events) {
    return SyncFileState(file)?events&1u:0;
}
static inline void SyncFileDescribe(const SyncFileData& fence,SyncFileInfo& info,SyncFenceInfo& detail) {
    info={};detail={};
    const char* name=fence.intelFence?"brook-intel":"brook-virtgpu";
    for(uint32_t i=0;name[i] && i<31;++i) {info.name[i]=name[i];detail.driverName[i]=name[i];}
    const char* object=fence.intelFence?"intel-fence":"completed-timeline";
    for(uint32_t i=0;object[i] && i<31;++i)detail.objectName[i]=object[i];
    const uint32_t state=SyncFileState(fence);
    info.status=detail.status=state==2?-5:static_cast<int32_t>(state);
    info.numFences=1;detail.timestampNs=fence.intelFence?0:fence.timestampNs;
}
}
