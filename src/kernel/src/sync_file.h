#pragma once
#include <stdint.h>
namespace brook {
// Linux UAPI: completed fences only. No counter, read/write or signalling API.
// Creation is permitted only after the selected host timeline has completed.
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
struct SyncFileData { uint32_t references;uint64_t timestampNs,identity; };
static_assert(sizeof(SyncFileInfo)==56);
static_assert(sizeof(SyncFenceInfo)==80);
static constexpr uint32_t SYNC_FILE_INFO_IOCTL=0xc0383e04;
static inline bool SyncFileInfoRequestValid(const SyncFileInfo& info) { return !info.flags && !info.pad; }
static inline uint32_t SyncFileReady(uint32_t events) { return events&1u; }
static inline void SyncFileDescribe(const SyncFileData& fence,SyncFileInfo& info,SyncFenceInfo& detail) {
    info={};detail={};
    const char name[]="brook-virtgpu";
    for(uint32_t i=0;i<sizeof(name);++i) {info.name[i]=name[i];detail.driverName[i]=name[i];}
    const char object[]="completed-timeline";
    for(uint32_t i=0;i<sizeof(object);++i)detail.objectName[i]=object[i];
    info.status=detail.status=1;info.numFences=1;detail.timestampNs=fence.timestampNs;
}
}
