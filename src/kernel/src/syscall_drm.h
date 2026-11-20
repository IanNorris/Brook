#pragma once

// syscall_drm.h -- internal declarations shared from syscall_drm.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include "sync_file.h"
#include "gpu_reservation.h"
#include "process.h"
#include "intel_bdw_file.h"
#include "gpu_app.h"

namespace brook {

void SyncFileRef(SyncFileData* file);
void SyncFileUnref(SyncFileData* file);
extern const char g_drmDriDirTag[];

// Linux dev_t for char 226:128 (gnu_dev: minor low 8 | major<<8).
static constexpr uint64_t DRM_RENDER_RDEV = 0xE280;

const char* DrmSysAttrContent(const char* path);
bool DrmSysIsDir(const char* path);
const char* IntelDrmSysAttrContent(const char* path);
bool IntelDrmSysIsDir(const char* path);

// Per-fd DRM render-node state (GL shim M1). Each open("/dev/dri/renderD128")
// gets one. Holds the app's GpuApp (virgl) context id plus a per-fd GEM-handle
// table that translates the Linux DRM model (per-fd GEM handles, small ints)
// to Brook's host-global virgl resource ids. This is the isolation layer the
// architecture review called for: an app can only name resources via GEM
// handles in ITS OWN table, never a raw host-global id, and GEM_CLOSE/teardown
// are scoped per fd. RESOURCE_CREATE returns res_handle=gres (host id, used
// inside virgl command streams) and bo_handle=GEM index (used in ioctls) —
// matching the upstream virtio-gpu uABI.
// Mesa's GL3 streaming buffers exceed 256 live objects during first present.
// Reconstructed BRO-227 fix (06a8d07); keep headroom for console/UI transitions.
// Warzone's small terrain buffers exceed 2,048 live handles well before RAM
// exhaustion. Keep this bounded until the table can grow without moving entries.
static constexpr uint32_t DRM_MAX_GEM = 8192;

// File contexts own handle tables; resources own only a small GPU-context
// lease. Keeping these separate prevents GEM/PRIME cross-import ownership cycles.
struct DrmGpuOwner { uint32_t references; int32_t ctxId; GpuContextConfig config; };

void DrmOwnerUnref(DrmGpuOwner* owner);

struct DrmResource {
    MappingBacking* lifetime;
    DrmGpuOwner* owner;
    uint32_t gres,size,stride,width,height,target,format;
    uint64_t backingKva;
    uint32_t backingBytes;
    KMutex backingLock;
    bool backingAttached;
    bool hostBlob;
    uint32_t blobFlags;
    GpuHostMapping hostMapping;
    bool everExported;
    KMutex reservationLock;
    GpuReservation reservation;
    DrmResource* retiredNext;
};

void DrmCaptureComplete(void* context,bool safe,bool captured);

struct DrmGemObj {
    bool used;
    DrmResource* resource;
    uint64_t mapToken; // issued once for this file's GEM incarnation
};

// The exported capability owns the resource independently of its source GEM
// and file. FdRef/SCM_RIGHTS/fork references retain this capability in turn.
struct DrmPrimeObj { DrmResource* resource; uint32_t refCount; };

static inline void DrmPrimeRef(DrmPrimeObj* p) {
    if (p) __atomic_fetch_add(&p->refCount,1,__ATOMIC_RELAXED);
}

static inline void DrmPrimeUnref(DrmPrimeObj* p) {
    if (p && __atomic_fetch_sub(&p->refCount,1,__ATOMIC_ACQ_REL)==1) {
        MappingBackingUnref(p->resource->lifetime);kfree(p);
    }
}

struct DrmCtx {
    volatile uint32_t refCount;
    DrmGpuOwner* owner;
    DrmGemObj gem[DRM_MAX_GEM];
    KMutex mappingLock;
};

struct DrmMappingGuard {
    DrmCtx* context;
    explicit DrmMappingGuard(DrmCtx* value) : context(value) { KMutexLock(&context->mappingLock); }
    ~DrmMappingGuard() { KMutexUnlock(&context->mappingLock); }
};

void DrmGemDrop(DrmGemObj* gem);
DrmGemObj* DrmGemGet(DrmCtx* c, uint32_t bo_handle);

constexpr uint8_t  DRM_IOCTL_TYPE        = 'd';   // 0x64
int CreateCompletedSyncFile(Process* process);
int CreateIntelSyncFile(Process* process,BdwFence* fence);

// Adopts the pin installed atomically by CreateCompletedSyncFile; no second
// lookup/pin is allowed because a sibling may already have logically closed it.
struct SyncFilePublicationPin {
    Process* process;
    int fd;
    SyncFilePublicationPin(Process* p,int f):process(p),fd(f) {}
    ~SyncFilePublicationPin() { FdPut(process,fd); }
    SyncFilePublicationPin(const SyncFilePublicationPin&)=delete;
    SyncFilePublicationPin& operator=(const SyncFilePublicationPin&)=delete;
};

int64_t DrmRenderIoctl(DrmCtx* dctx, uint8_t nr, uint64_t arg);
int64_t IntelRenderIoctl(brook::BdwGpuFile* file,uint64_t cmd,uint64_t arg,uint64_t renderFd);

} // namespace brook
