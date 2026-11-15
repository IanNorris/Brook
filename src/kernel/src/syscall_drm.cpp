// syscall_drm.cpp -- DRM render-node emulation: sysfs shim, GEM/PRIME objects, virtio-gpu and i915 ioctls, sync files.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "kprintf.h"
#include "drm_resource_layout.h"

#include "syscall.h"
#include "drm_map_tokens.h"
#include "drm_backing_diagnostics.h"
#include "intel_bdw_buffer.h"
#ifdef BROOK_BDW_CACHE_PROBE
#include "../../shared/bdw_cache_probe_abi.h"
#endif
#include "intel_bdw_ioctl.h"
#include "intel_bdw_scanout.h"
#include "intel_bdw_refusal.h"
#include "debug_hardware.h"
#include "sync/ksemaphore.h"
#include "string.h"
#include "memory/virtual_memory.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_eventfd.h"
#include "syscall_drm.h"
#include "syscall_memory.h"
#include "syscall_time.h"
#include "syscall_handlers.h"

namespace brook {

void SyncFileRef(SyncFileData* file) { __atomic_fetch_add(&file->references,1,__ATOMIC_RELAXED); }
void SyncFileUnref(SyncFileData* file) { if(__atomic_fetch_sub(&file->references,1,__ATOMIC_ACQ_REL)==1){BdwFenceUnref(file->intelFence);kfree(file);} }

namespace {

// ---------------------------------------------------------------------------
// DRM render-node sysfs/devfs shim (GL shim, M0 step 2)
//
// Minimal synthetic /dev/dri + /sys topology so the unmodified Linux libdrm
// (drmGetDevices2) can enumerate /dev/dri/renderD128 as a PCI virtio-gpu device
// and open it. Backed by the real per-process GPU path (GpuAppOps); no card0 /
// KMS node. Surface determined empirically from libdrm 2.4.131 source +
// host strace (see artifacts/gl-drm-enumeration-spike.md):
//   - get_pci_path() falls back to the literal path if realpath() fails, so the
//     PCI attributes are served directly at /sys/dev/char/226:128/device/* with
//     NO symlinks — except 'subsystem', which get_subsystem_type() reads via
//     readlink() to classify the bus (we return ".../pci" -> DRM_BUS_PCI).
// All responses are gated on a 3D driver being present (GpuAppGet()).
// ---------------------------------------------------------------------------
struct DrmSysAttr { const char* path; const char* content; };
static const DrmSysAttr kDrmSysAttrs[] = {
    { "/sys/dev/char/226:128/device/uevent",
      "DRIVER=virtio_gpu\nPCI_SLOT_NAME=0000:00:02.0\n" },
    { "/sys/dev/char/226:128/device/vendor",           "0x1af4\n" },
    { "/sys/dev/char/226:128/device/device",           "0x1050\n" },
    { "/sys/dev/char/226:128/device/subsystem_vendor", "0x1af4\n" },
    { "/sys/dev/char/226:128/device/subsystem_device", "0x1100\n" },
    { "/sys/dev/char/226:128/device/revision",         "0x00\n" },
    { nullptr, nullptr }
};
static const DrmSysAttr kIntelDrmSysAttrs[] = {
    { "/sys/dev/char/226:129/device/uevent", "DRIVER=i915\nPCI_SLOT_NAME=0000:00:02.0\n" },
    { "/sys/dev/char/226:129/device/vendor", "0x8086\n" },
    { "/sys/dev/char/226:129/device/device", "0x1616\n" },
    { "/sys/dev/char/226:129/device/subsystem_vendor", "0x1028\n" },
    { "/sys/dev/char/226:129/device/subsystem_device", "0x0665\n" },
    { "/sys/dev/char/226:129/device/revision", "0x09\n" },
    { nullptr,nullptr }
};
// Directories that must stat as S_IFDIR (realpath / openat / drmNodeIsDRM).
static const char* kDrmSysDirs[] = {
    "/sys/dev/char/226:128/device",
    "/sys/dev/char/226:128/device/drm",
    nullptr
};

} // namespace
// Marker handle so getdents64 / fstat recognise the /dev/dri synthetic dir fd.
const char g_drmDriDirTag[] = "BROOK_DRM_DRI_DIR";

const char* DrmSysAttrContent(const char* path) {
    for (auto* a = kDrmSysAttrs; a->path; ++a)
        if (StrEq(path, a->path)) return a->content;
    return nullptr;
}
bool DrmSysIsDir(const char* path) {
    for (auto** d = kDrmSysDirs; *d; ++d)
        if (StrEq(path, *d)) return true;
    return false;
}
const char* IntelDrmSysAttrContent(const char* path){
    for(auto* a=kIntelDrmSysAttrs;a->path;++a)if(StrEq(path,a->path))return a->content;
    return nullptr;
}
bool IntelDrmSysIsDir(const char* path){return StrEq(path,"/sys/dev/char/226:129/device") ||
    StrEq(path,"/sys/dev/char/226:129/device/drm");}

namespace {

static DrmMapTokenAllocator g_drmMapTokens;
static DrmBackingDiagnostics g_drmBackingDiagnostics;
static void DrmOwnerRef(DrmGpuOwner* owner) {
    __atomic_fetch_add(&owner->references,1,__ATOMIC_RELAXED);
}

} // namespace
void DrmOwnerUnref(DrmGpuOwner* owner) {
    if (__atomic_fetch_sub(&owner->references,1,__ATOMIC_ACQ_REL)!=1) return;
    const auto* ga=GpuAppGet();
    if (owner->ctxId>0 && ga && ga->CtxDestroy) ga->CtxDestroy(owner->ctxId);
    kfree(owner);
}
void DrmCaptureComplete(void* context,bool safe,bool captured) {
    auto* resource=static_cast<DrmResource*>(context);
    KMutexLock(&resource->reservationLock);
    resource->reservation.FinishCapture(safe,captured);
    KMutexUnlock(&resource->reservationLock);
}

namespace {

// Final callbacks only enqueue: GPU waits never enter mapping/FD cleanup locks.
// GEMs, PRIME capabilities, VMAs and capture requests independently own the
// canonical resource. Last-reference retirement now covers exported buffers,
// too. An uncertain capture keeps its source lease, so it cannot reach here.
static KMutex g_drmRetirementLock{};
static DrmResource* g_drmRetiredResources;
static DrmResource* g_drmQuarantinedResources;
static KSemaphore g_drmRetirementReady{};
static bool g_drmRetirementWorkerStarted;
static void DrmRetirementWorker(void*) {
    for (;;) {
        KSemaphoreWait(&g_drmRetirementReady);
        KMutexLock(&g_drmRetirementLock);
        auto* resource=g_drmRetiredResources;
        g_drmRetiredResources=resource->retiredNext;
        KMutexUnlock(&g_drmRetirementLock);
        const auto* ga=GpuAppGet();
        const bool safe=resource->reservation.Ready();
        const bool retired=safe && (!resource->gres || (ga && ga->RetireResource &&
            ga->RetireResource(resource->owner->ctxId,static_cast<int32_t>(resource->gres))==0));
        if (!retired) {
            __atomic_fetch_add(&g_drmBackingDiagnostics.quarantinedResources,1,__ATOMIC_RELAXED);
            const uint64_t bytes=(static_cast<uint64_t>(resource->backingBytes)+4095)&~uint64_t{4095};
            __atomic_fetch_add(&g_drmBackingDiagnostics.quarantinedPageBytes,bytes,__ATOMIC_RELAXED);
            __atomic_fetch_add(&g_drmBackingDiagnostics.failedRetirements,1,__ATOMIC_RELAXED);
            KMutexLock(&g_drmRetirementLock);
            resource->retiredNext=g_drmQuarantinedResources;
            g_drmQuarantinedResources=resource;
            KMutexUnlock(&g_drmRetirementLock);
            continue;
        }
        if (resource->backingKva) {
            const uint64_t pages=(static_cast<uint64_t>(resource->backingBytes)+4095)/4096;
            VmmFreePages(VirtualAddress(resource->backingKva),pages);
            __atomic_fetch_add(&g_drmBackingDiagnostics.releasedMappings,1,__ATOMIC_RELAXED);
            __atomic_fetch_add(&g_drmBackingDiagnostics.releasedPageBytes,pages*4096,__ATOMIC_RELAXED);
        }
        auto* owner=resource->owner;kfree(resource);DrmOwnerUnref(owner);
    }
}
static bool DrmEnsureRetirementWorker() {
    KMutexLock(&g_drmRetirementLock);
    if (g_drmRetirementWorkerStarted) { KMutexUnlock(&g_drmRetirementLock);return true; }
    auto* worker=KernelThreadCreate("drm_retire",DrmRetirementWorker,nullptr);
    if (!worker) { KMutexUnlock(&g_drmRetirementLock);return false; }
    g_drmRetirementWorkerStarted=true;
    KMutexUnlock(&g_drmRetirementLock);
    SchedulerAddProcess(worker);
    return true;
}
static void DrmResourceLastReference(void* object) {
    auto* resource=static_cast<DrmResource*>(object);
    KMutexLock(&g_drmRetirementLock);
    resource->retiredNext=g_drmRetiredResources;
    g_drmRetiredResources=resource;
    KMutexUnlock(&g_drmRetirementLock);
    KSemaphoreSignal(&g_drmRetirementReady);
}
static DrmResource* DrmResourceCreate(DrmGpuOwner* owner,uint32_t gres,uint32_t size,
                                    uint32_t stride,uint32_t width,uint32_t height,
                                    uint32_t target,uint32_t format) {
    if (!DrmEnsureRetirementWorker()) return nullptr;
    auto* resource=static_cast<DrmResource*>(kmalloc(sizeof(DrmResource)));
    if (!resource) return nullptr;
    memset(resource,0,sizeof(*resource));
    resource->owner=owner;resource->gres=gres;resource->size=size;
    resource->stride=stride;resource->width=width;resource->height=height;
    resource->target=target;resource->format=format;
    KMutexInit(&resource->backingLock);
    resource->lifetime=MappingBackingCreate(resource,DrmResourceLastReference);
    if (!resource->lifetime) { kfree(resource);return nullptr; }
    DrmOwnerRef(owner);
    return resource;
}
static uint32_t DrmGemAlloc(DrmCtx* c,uint32_t gres,uint32_t size,
                           uint32_t stride,uint32_t w,uint32_t h,uint32_t target,uint32_t format) {
    for (uint32_t i=0;i<DRM_MAX_GEM;++i) {
        if (c->gem[i].used) continue;
        auto* resource=DrmResourceCreate(c->owner,gres,size,stride,w,h,target,format);
        if (!resource) return 0;
        c->gem[i]={true,resource,0};return i+1;
    }
    return 0;
}
static uint32_t DrmGemAllocImported(DrmCtx* c,DrmResource* resource) {
    for (uint32_t i=0;i<DRM_MAX_GEM;++i) {
        if (c->gem[i].used) continue;
        MappingBackingRef(resource->lifetime);
        c->gem[i]={true,resource,0};return i+1;
    }
    return 0;
}

} // namespace
void DrmGemDrop(DrmGemObj* gem) {
    auto* resource=gem->resource;*gem={};
    MappingBackingUnref(resource->lifetime);
}
DrmGemObj* DrmGemGet(DrmCtx* c, uint32_t bo_handle) {
    if (bo_handle == 0 || bo_handle > DRM_MAX_GEM) return nullptr;
    DrmGemObj* g = &c->gem[bo_handle - 1];
    return g->used ? g : nullptr;
}

DrmBackingDiagnostics DrmBackingDiagnosticsRead() {
    const auto& d=g_drmBackingDiagnostics;
    return {__atomic_load_n(&d.allocatedMappings,__ATOMIC_RELAXED),
            __atomic_load_n(&d.allocatedPageBytes,__ATOMIC_RELAXED),
            __atomic_load_n(&d.releasedMappings,__ATOMIC_RELAXED),
            __atomic_load_n(&d.releasedPageBytes,__ATOMIC_RELAXED),
            __atomic_load_n(&d.gemCloseBackingAliases,__ATOMIC_RELAXED),
            __atomic_load_n(&d.gemCloseBackingAliasPageBytes,__ATOMIC_RELAXED),
            __atomic_load_n(&d.failedAllocations,__ATOMIC_RELAXED),
            __atomic_load_n(&d.failedRequestedBytes,__ATOMIC_RELAXED),
            __atomic_load_n(&d.quarantinedResources,__ATOMIC_RELAXED),
            __atomic_load_n(&d.quarantinedPageBytes,__ATOMIC_RELAXED),
            __atomic_load_n(&d.failedRetirements,__ATOMIC_RELAXED)};
}

namespace {

// ---------------------------------------------------------------------------
// virtio-gpu DRM render node (/dev/dri/renderD128) — GL shim, Milestone 0.
//
// Maps the Linux virtio-gpu DRM uABI onto Brook's native per-process GPU path
// (GpuAppOps) so unmodified Mesa's virgl driver can drive it. M0 implements the
// device-identification / capability ioctls Mesa issues during initialization
// (VERSION, GET_CAP, VIRTGPU_GETPARAM, VIRTGPU_GET_CAPS) — verified against a
// real virtio-gpu node via a host Mesa strace (see
// artifacts/gl-drm-enumeration-spike.md). M1 adds the resource/submit ioctls
// (CONTEXT_INIT, RESOURCE_CREATE, RESOURCE_INFO, TRANSFER_*, EXECBUFFER, WAIT,
// GEM_CLOSE) over GpuAppOps, with a per-fd GEM-handle->virgl-resid table.
//
// ioctl command words are _IOWR('d', nr, struct): the low byte is the request
// number (nr), the next byte is the type ('d' = 0x64); size/dir occupy the high
// bits and are ignored here (Linux drivers dispatch on type+nr).
// ---------------------------------------------------------------------------
constexpr uint8_t  DRM_NR_VERSION        = 0x00;  // DRM_IOCTL_VERSION
constexpr uint8_t  DRM_NR_GEM_CLOSE      = 0x09;  // DRM_IOCTL_GEM_CLOSE
constexpr uint8_t  DRM_NR_PRIME_HANDLE_TO_FD = 0x2d;  // DRM_IOCTL_PRIME_HANDLE_TO_FD
constexpr uint8_t  DRM_NR_PRIME_FD_TO_HANDLE = 0x2e;  // DRM_IOCTL_PRIME_FD_TO_HANDLE
constexpr uint8_t  DRM_NR_GET_CAP        = 0x0c;  // DRM_IOCTL_GET_CAP
constexpr uint8_t  DRM_COMMAND_BASE      = 0x40;  // driver-private ioctl base
constexpr uint8_t  DRM_NR_VIRTGPU_MAP            = DRM_COMMAND_BASE + 0x01; // 0x41
constexpr uint8_t  DRM_NR_VIRTGPU_EXECBUFFER     = DRM_COMMAND_BASE + 0x02; // 0x42
constexpr uint8_t  DRM_NR_VIRTGPU_GETPARAM       = DRM_COMMAND_BASE + 0x03; // 0x43
constexpr uint8_t  DRM_NR_VIRTGPU_RESOURCE_CREATE= DRM_COMMAND_BASE + 0x04; // 0x44
constexpr uint8_t  DRM_NR_VIRTGPU_RESOURCE_INFO  = DRM_COMMAND_BASE + 0x05; // 0x45
constexpr uint8_t  DRM_NR_VIRTGPU_TRANSFER_FROM_HOST = DRM_COMMAND_BASE + 0x06; // 0x46
constexpr uint8_t  DRM_NR_VIRTGPU_TRANSFER_TO_HOST   = DRM_COMMAND_BASE + 0x07; // 0x47
constexpr uint8_t  DRM_NR_VIRTGPU_WAIT           = DRM_COMMAND_BASE + 0x08; // 0x48
constexpr uint8_t  DRM_NR_VIRTGPU_GET_CAPS       = DRM_COMMAND_BASE + 0x09; // 0x49
constexpr uint8_t  DRM_NR_VIRTGPU_RESOURCE_CREATE_BLOB = DRM_COMMAND_BASE + 0x0a;
constexpr uint8_t  DRM_NR_VIRTGPU_CONTEXT_INIT   = DRM_COMMAND_BASE + 0x0b; // 0x4b

// virtio-gpu DRM driver identity (matches the upstream kernel driver).
constexpr int32_t  VIRTGPU_DRIVER_MAJOR  = 0;
constexpr int32_t  VIRTGPU_DRIVER_MINOR  = 1;
constexpr int32_t  VIRTGPU_DRIVER_PATCH  = 0;

// struct drm_version (64-bit layout; size_t/ptr are 8 bytes, with 4 bytes of
// padding after the three int fields to align name_len).
struct DrmVersion {
    int32_t  version_major;
    int32_t  version_minor;
    int32_t  version_patchlevel;
    int32_t  _pad;
    uint64_t name_len;
    uint64_t name;          // char*
    uint64_t date_len;
    uint64_t date;          // char*
    uint64_t desc_len;
    uint64_t desc;          // char*
};

struct DrmGetCap   { uint64_t capability; uint64_t value; };

// DRM_CAP_PRIME (capability id 0x5) bits: import/export support. Mesa queries
// this during init to decide whether dmabuf sharing is available.
constexpr uint64_t DRM_CAP_PRIME            = 0x5;
constexpr uint64_t DRM_PRIME_CAP_IMPORT     = 0x1;
constexpr uint64_t DRM_PRIME_CAP_EXPORT     = 0x2;

// struct drm_prime_handle — the argument to PRIME_HANDLE_TO_FD / FD_TO_HANDLE.
struct DrmPrimeHandle {
    uint32_t handle;   // GEM bo_handle (in for export, out for import)
    uint32_t flags;    // DRM_CLOEXEC | DRM_RDWR (export only)
    int32_t  fd;       // dmabuf fd (out for export, in for import)
};
struct DrmVirtgpuGetparam { uint64_t param; uint64_t value; }; // value = user u64*
struct DrmVirtgpuGetCaps {
    uint32_t cap_set_id; uint32_t cap_set_ver;
    uint64_t addr; uint32_t size; uint32_t pad;
};

// M1 ioctl structs (virtgpu_drm.h, exact layout).
struct DrmGemClose { uint32_t handle; uint32_t pad; };
struct DrmVirtgpuResourceCreate {
    uint32_t target, format, bind, width, height, depth, array_size;
    uint32_t last_level, nr_samples, flags;
    uint32_t bo_handle;   // in: recreate attached to this bo; out: GEM handle
    uint32_t res_handle;  // out: host resource id
    uint32_t size, stride;
};
struct DrmVirtgpuResourceInfo {
    uint32_t bo_handle, res_handle, size, blob_mem;
};
struct DrmVirtgpuCreateBlob {
    uint32_t memory, flags, bo_handle, res_handle;
    uint64_t size;
    uint32_t pad, cmd_size;
    uint64_t cmd, blob_id;
};
struct DrmVirtgpu3dBox { uint32_t x, y, z, w, h, d; };
struct DrmVirtgpu3dTransfer {     // to_host and from_host share this layout
    uint32_t bo_handle;
    DrmVirtgpu3dBox box;
    uint32_t level, offset, stride, layer_stride;
};
struct DrmVirtgpuExecbuffer {
    uint32_t flags, size;
    uint64_t command;        // void* to virgl dwords
    uint64_t bo_handles;     // u32* residency list
    uint32_t num_bo_handles;
    int32_t  fence_fd;
    uint32_t ring_idx, syncobj_stride, num_in_syncobjs, num_out_syncobjs;
    uint64_t in_syncobjs, out_syncobjs;
};
struct DrmVirtgpu3dWait { uint32_t handle, flags; };
struct DrmVirtgpuMap { uint64_t offset; uint32_t handle, pad; };
struct DrmVirtgpuContextSetParam { uint64_t param, value; };
struct DrmVirtgpuContextInit {
    uint32_t num_params, pad;
    uint64_t ctx_set_params;  // DrmVirtgpuContextSetParam*
};

// VIRTGPU_PARAM_* (virtgpu_drm.h).
constexpr uint64_t VIRTGPU_PARAM_3D_FEATURES        = 1;
constexpr uint64_t VIRTGPU_PARAM_CAPSET_QUERY_FIX   = 2;
constexpr uint64_t VIRTGPU_PARAM_RESOURCE_BLOB      = 3;
constexpr uint64_t VIRTGPU_PARAM_HOST_VISIBLE       = 4;
constexpr uint64_t VIRTGPU_PARAM_CROSS_DEVICE       = 5;
constexpr uint64_t VIRTGPU_PARAM_CONTEXT_INIT       = 6;
constexpr uint64_t VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs = 7;
constexpr uint64_t VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME  = 8;

} // namespace

// Copy a NUL-padded string into a user buffer following the DRM len/ptr
// protocol: always report the true length in *outLen; copy min(provided, true)
// bytes if the user gave a buffer. libdrm calls twice (first to learn the
// length, then with an allocated buffer).
static void DrmFillStr(uint64_t userPtr, uint64_t& userLen, const char* s)
{
    uint64_t actual = 0;
    while (s[actual]) ++actual;
    if (userPtr && userLen)
    {
        uint64_t n = userLen < actual ? userLen : actual;
        CopyToUser(userPtr, s, n);
    }
    userLen = actual;
}

// Ensure a GEM-backed resource has guest backing pages attached to its host
// virgl resource (an iov). virglrenderer requires src_res->iov != NULL for the
// COPY_TRANSFER3D (READ_FROM_HOST) that Mesa emits for glReadPixels — and that
// applies to the *render target* being read, not just the staging buffer, so we
// must attach backing to every resource at creation time, not lazily at MAP
// (Mesa never MAPs a render target). Idempotent: a second call is a no-op once
// attachment is verified, so MAP reuses the same pages. Ambiguous attachment
// retains its allocation for safe retirement/quarantine and returns false.
static bool DrmEnsureBacking(DrmGemObj* gem)
{
    auto* g=gem->resource;
    struct Guard {
        KMutex* lock;
        explicit Guard(KMutex* value):lock(value){KMutexLock(lock);}
        ~Guard(){KMutexUnlock(lock);}
    } guard(&g->backingLock);
    if (g->hostBlob) return g->hostMapping.length != 0;
    if (g->backingKva != 0) return g->backingAttached;
    uint32_t bytes = g->backingBytes ? g->backingBytes
                   : (g->size ? g->size
                              : g->stride * (g->height ? g->height : 1));
    if (bytes == 0) return false;
    uint64_t pages = (static_cast<uint64_t>(bytes) + 4095) / 4096;
    VirtualAddress kva = VmmAllocPages(pages, VMM_WRITABLE,
                                       MemTag::Device, KernelPid);
    if (!kva.raw()) {
        __atomic_fetch_add(&g_drmBackingDiagnostics.failedAllocations,1,__ATOMIC_RELAXED);
        __atomic_fetch_add(&g_drmBackingDiagnostics.failedRequestedBytes,bytes,__ATOMIC_RELAXED);
        return false;
    }
    __atomic_fetch_add(&g_drmBackingDiagnostics.allocatedMappings,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&g_drmBackingDiagnostics.allocatedPageBytes,pages*4096,__ATOMIC_RELAXED);
    auto* zp = reinterpret_cast<volatile uint8_t*>(kva.raw());
    for (uint64_t b = 0; b < pages * 4096; ++b) zp[b] = 0;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    const bool attached=ga && ga->ResourceAttachUser &&
        ga->ResourceAttachUser(g->owner->ctxId,(int32_t)g->gres,kva.raw(),bytes)>=0;
    g->backingKva = kva.raw();
    g->backingBytes = bytes;
    g->backingAttached=attached;
    return attached;
}

int CreateCompletedSyncFile(Process* process) {
    if(!process)return -ESRCH;
    auto* file=static_cast<SyncFileData*>(kmalloc(sizeof(SyncFileData)));
    if(!file)return -ENOMEM;
    *file={1,ClockNowNs(1),__atomic_add_fetch(&g_eventFdIdentity,1,__ATOMIC_RELAXED)};
    FdEntry owned{};owned.type=FdType::SyncFile;owned.handle=file;owned.fdFlags=1;owned.statusFlags=0;
    const int fd=FdTablePublishOwnedPinned(process->fds,ProcessFdLock(process),owned);
    if(fd<0)SyncFileUnref(file);
    return fd;
}
int CreateIntelSyncFile(Process* process,BdwFence* fence) {
    if(!process || !fence)return -EINVAL;
    auto* file=static_cast<SyncFileData*>(kmalloc(sizeof(SyncFileData)));
    if(!file)return -ENOMEM;
    BdwFenceRef(fence);
    *file={1,0,__atomic_add_fetch(&g_eventFdIdentity,1,__ATOMIC_RELAXED),fence};
    FdEntry owned{};owned.type=FdType::SyncFile;owned.handle=file;owned.fdFlags=1;
    const int fd=FdTablePublishOwnedPinned(process->fds,ProcessFdLock(process),owned);
    if(fd<0)SyncFileUnref(file);
    return fd;
}

int64_t DrmRenderIoctl(DrmCtx* dctx, uint8_t nr, uint64_t arg)
{
    DrmMappingGuard guard(dctx);
    switch (nr)
    {
    case DRM_NR_VERSION:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVersion)) ||
            !UserBufferWritable(arg, sizeof(DrmVersion)))
            return -EFAULT;
        DrmVersion v;
        for (uint64_t i = 0; i < sizeof(v); ++i)
            reinterpret_cast<char*>(&v)[i] = reinterpret_cast<char*>(arg)[i];
        v.version_major = VIRTGPU_DRIVER_MAJOR;
        v.version_minor = VIRTGPU_DRIVER_MINOR;
        v.version_patchlevel = VIRTGPU_DRIVER_PATCH;
        DrmFillStr(v.name, v.name_len, "virtio_gpu");
        DrmFillStr(v.date, v.date_len, "0");
        DrmFillStr(v.desc, v.desc_len, "Brook virtio-gpu GL shim");
        if (!CopyToUser(arg, &v, sizeof(v))) return -EFAULT;
        return 0;
    }
    case DRM_NR_GET_CAP:
    {
        if (!UserBufferReadable(arg, sizeof(DrmGetCap)) ||
            !UserBufferWritable(arg, sizeof(DrmGetCap)))
            return -EFAULT;
        DrmGetCap c = *reinterpret_cast<DrmGetCap*>(arg);
        // Advertise PRIME import+export so Mesa enables the dmabuf sharing path
        // (windowed GL presentation). Everything else (dumb buffers, etc.) stays
        // unadvertised — we only support the GPU-resource dmabuf path.
        c.value = (c.capability == DRM_CAP_PRIME)
                ? (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)
                : 0;
        return CopyToUser(arg, &c, sizeof(c)) ? 0 : -EFAULT;
    }
    case DRM_NR_VIRTGPU_GETPARAM:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuGetparam)))
            return -EFAULT;
        DrmVirtgpuGetparam p = *reinterpret_cast<DrmVirtgpuGetparam*>(arg);
        const auto* ga = GpuAppGet();
        if (!ga || !ga->Capabilities) return -ENODEV;
        const auto caps = ga->Capabilities();
        uint64_t val;
        switch (p.param)
        {
        case VIRTGPU_PARAM_3D_FEATURES:       val = caps.capsetMask != 0; break;
        case VIRTGPU_PARAM_CAPSET_QUERY_FIX:  val = 1; break;
        case VIRTGPU_PARAM_CONTEXT_INIT:      val = caps.contextInit; break;
        // capset id bitmask: VIRGL(1) | VIRGL2(2).
        case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs: val = caps.capsetMask; break;
        case VIRTGPU_PARAM_RESOURCE_BLOB:     val = caps.resourceBlob; break;
        case VIRTGPU_PARAM_HOST_VISIBLE:      val = caps.hostVisible; break;
        case VIRTGPU_PARAM_CROSS_DEVICE:      val = 0; break;
        case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME: val = 0; break;
        default: return -EINVAL;
        }
        if (!CopyToUser(p.value, &val, sizeof(val))) return -EFAULT;
        return 0;
    }
    case DRM_NR_VIRTGPU_GET_CAPS:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuGetCaps)))
            return -EFAULT;
        DrmVirtgpuGetCaps gc = *reinterpret_cast<DrmVirtgpuGetCaps*>(arg);
        if (gc.size == 0 || gc.size > 64u * 1024) return -EINVAL;
        if (!UserBufferWritable(gc.addr, gc.size)) return -EFAULT;
        // Fetch the REAL virgl capset blob from the host so Mesa binds the
        // hardware virgl screen (instead of falling back to llvmpipe). The
        // driver issues VIRTIO_GPU_CMD_GET_CAPSET(id, version) and copies the
        // blob into the user buffer. Zero the tail so any bytes the host didn't
        // supply read as 0 (Mesa reads a fixed-size union virgl_caps).
        const brook::GpuAppOps* ga = brook::GpuAppGet();
        if (!ga || !ga->GetCapset || !ga->Capabilities) return -ENODEV;
        if (gc.cap_set_id >= 64 ||
            !(ga->Capabilities().capsetMask & (uint64_t{1} << gc.cap_set_id))) return -EINVAL;
        for (uint32_t i = 0; i < gc.size; ++i)
            reinterpret_cast<char*>(gc.addr)[i] = 0;
        int32_t n = ga->GetCapset(gc.cap_set_id, gc.cap_set_ver,
                                  reinterpret_cast<void*>(gc.addr), gc.size);
        if (n < 0) return -EINVAL;
        return 0;
    }
    // --- M1: resource / submit path (per-fd GEM table -> host virgl ids) ---
    case DRM_NR_VIRTGPU_CONTEXT_INIT:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuContextInit))) return -EFAULT;
        DrmVirtgpuContextInit init{};
        if (!CopyFromUser(&init, arg, sizeof(init))) return -EFAULT;
        if (init.num_params > 4) return -EINVAL;
        GpuContextParam params[4]{};
        if (init.num_params && !CopyFromUser(params, init.ctx_set_params,
                                             init.num_params * sizeof(params[0]))) return -EFAULT;
        const auto* ga = GpuAppGet();
        if (!ga || !ga->CtxCreate || !ga->Capabilities) return -ENODEV;
        if (dctx->owner->ctxId > 0) return -17; // EEXIST
        GpuContextConfig config{};
        if (!GpuContextParse(params, init.num_params, ga->Capabilities(), config)) return -EINVAL;
        Process* proc = ProcessCurrent();
        const int32_t ctx = ga->CtxCreate(proc ? proc->pid : 0, config);
        if (ctx < 0) return -ENOMEM;
        dctx->owner->config = config;
        dctx->owner->ctxId = ctx;
        return 0;
    }
    case DRM_NR_VIRTGPU_RESOURCE_CREATE_BLOB:
    {
        if (!UserBufferReadable(arg,sizeof(DrmVirtgpuCreateBlob)) ||
            !UserBufferWritable(arg,sizeof(DrmVirtgpuCreateBlob))) return -EFAULT;
        DrmVirtgpuCreateBlob create{};
        if (!CopyFromUser(&create,arg,sizeof(create))) return -EFAULT;
        const auto* ga=GpuAppGet();
        if (!ga || !ga->ResourceCreateBlob || !ga->Capabilities || !ga->Capabilities().resourceBlob) return -EINVAL;
        if (create.memory!=2 || (create.flags&~3u) || !create.size || create.size>0xfffff000ULL) return -EINVAL;
        if(dctx->owner->ctxId<=0) {
            if(!ga->CtxCreate) return -ENODEV;
            auto* process=ProcessCurrent();
            const auto context=ga->CtxCreate(process?process->pid:0,{});
            if(context<0)return -ENOMEM;
            dctx->owner->ctxId=context;
        }
        uint32_t virglCreate[12]{};
        if(dctx->owner->config.capsetId==4) {
            if(create.cmd_size) return -EINVAL;
        } else {
            // Mesa's persistent-buffer path bundles one PIPE_RESOURCE_CREATE.
            if(create.cmd_size!=sizeof(virglCreate))return -EINVAL;
            if(!CopyFromUser(virglCreate,create.cmd,sizeof(virglCreate)))return -EFAULT;
            if(virglCreate[0]!=((11u<<16)|48u) || create.blob_id>0xffffffffULL ||
                virglCreate[11]!=create.blob_id || virglCreate[1]!=0 ||
                virglCreate[4]!=((create.size+4095)&~uint64_t{4095}) ||
                virglCreate[5]>1 || virglCreate[6]>1 || virglCreate[7]!=1 ||
                virglCreate[8] || virglCreate[9])return -EINVAL;
        }
        const uint32_t size=static_cast<uint32_t>((create.size+4095)&~uint64_t{4095});
        const uint32_t bo=DrmGemAlloc(dctx,0,size,0,0,0,0,0);
        if (!bo) return -ENOMEM;
        auto* gem=&dctx->gem[bo-1];auto* resource=gem->resource;
        resource->hostBlob=true;resource->blobFlags=create.flags;
        if(dctx->owner->config.capsetId!=4) {
            resource->target=virglCreate[1];resource->format=virglCreate[2];
            resource->width=virglCreate[4];resource->height=virglCreate[5]?virglCreate[5]:1;
            if(!ga->Submit3D || ga->Submit3D(dctx->owner->ctxId,virglCreate,12)<0) {DrmGemDrop(gem);return -EIO;}
        }
        const GpuBlobCreate parameters{create.size,create.blob_id,create.memory,create.flags};
        if (ga->ResourceCreateBlob(dctx->owner->ctxId,parameters,&resource->gres,&resource->hostMapping)<0) {
            DrmGemDrop(gem);return -ENOMEM;
        }
        create.bo_handle=bo;create.res_handle=resource->gres;
        if (!CopyToUser(arg,&create,sizeof(create))) {DrmGemDrop(gem);return -EFAULT;}
        return 0;
    }
    case DRM_NR_VIRTGPU_RESOURCE_CREATE:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuResourceCreate)) ||
            !UserBufferWritable(arg, sizeof(DrmVirtgpuResourceCreate)))
            return -EFAULT;
        DrmVirtgpuResourceCreate rc = *reinterpret_cast<DrmVirtgpuResourceCreate*>(arg);
        const brook::GpuAppOps* ga = brook::GpuAppGet();
        if (!ga || !ga->ResourceCreate3D) return -ENODEV;
        // Context is created lazily if the client skipped CONTEXT_INIT.
        if (dctx->owner->ctxId <= 0) {
            if (!ga->CtxCreate) return -ENODEV;
            Process* proc = ProcessCurrent();
            int32_t ctx = ga->CtxCreate(proc ? proc->pid : 0, {});
            if (ctx < 0) return -ENOMEM;
            dctx->owner->ctxId = ctx;
        }
        // target 0 = PIPE_BUFFER: a 1D linear buffer whose "width" is a byte
        // count (e.g. glReadPixels staging), so it can far exceed the 2D texture
        // dimension cap. Validate buffers by byte size, textures by dimension.
        bool isBuffer = (rc.target == 0);
        if (rc.width == 0) return -EINVAL;
        if (isBuffer) {
            if (rc.width > 256u * 1024 * 1024) return -EINVAL;   // 256 MB cap
        } else if (rc.width > 16384 || rc.height > 16384) {
            return -EINVAL;
        }
        uint32_t createH = rc.height ? rc.height : 1;   // PIPE_BUFFER is 1D (h=0)
        uint32_t stride = 0, size = 0;
        if (!DrmLegacyResourceLayout(isBuffer,rc.width,createH,rc.stride,rc.size,
                                     &stride,&size)) return -EINVAL;
        // Allocate owning metadata before creating a host resource, so every
        // successful host allocation has a tracked rollback/retirement owner.
        uint32_t bo = DrmGemAlloc(dctx,0,size,stride,rc.width,createH,rc.target,rc.format);
        if (bo == 0) {
            SerialPrintf("drm: GEM metadata allocation failed ctx=%d capacity=%u\n",
                         dctx->owner->ctxId,DRM_MAX_GEM);
            return -ENOMEM;
        }
        int32_t gres = ga->ResourceCreate3D(dctx->owner->ctxId, rc.target, rc.format,
                                            rc.bind, rc.width, createH,
                                            rc.depth, rc.array_size,
                                            rc.last_level, rc.nr_samples,
                                            rc.flags);
        if (gres < 0) {
            SerialPrintf("drm: resource create failed ctx=%d target=%u size=%ux%u\n",
                         dctx->owner->ctxId,rc.target,rc.width,createH);
            DrmGemDrop(&dctx->gem[bo-1]);
            return -ENOMEM;
        }
        dctx->gem[bo-1].resource->gres=static_cast<uint32_t>(gres);
        // Attach guest backing now (not lazily at MAP): a render target read by
        // Mesa's glReadPixels COPY_TRANSFER3D must already have an iov, or
        // virglrenderer rejects the command (check_copy_transfer3d_handles).
        DrmGemObj* gobj = DrmGemGet(dctx, bo);
        if (gobj && !DrmEnsureBacking(gobj)) {
            SerialPrintf("drm: resource backing allocation failed ctx=%d gres=%d bytes=%u\n",
                         dctx->owner->ctxId,gres,gobj->resource->size);
            DrmGemDrop(gobj);
            return -ENOMEM;
        }
        rc.res_handle = (uint32_t)gres;   // host id, used inside virgl streams
        rc.bo_handle  = bo;               // per-fd GEM handle, used in ioctls
        rc.size = size; rc.stride = stride;
        if (!CopyToUser(arg,&rc,sizeof(rc))) { DrmGemDrop(gobj);return -EFAULT; }
        return 0;
    }
    case DRM_NR_VIRTGPU_RESOURCE_INFO:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuResourceInfo)) ||
            !UserBufferWritable(arg, sizeof(DrmVirtgpuResourceInfo)))
            return -EFAULT;
        DrmVirtgpuResourceInfo ri = *reinterpret_cast<DrmVirtgpuResourceInfo*>(arg);
        DrmGemObj* g = DrmGemGet(dctx, ri.bo_handle);
        if (!g) return -ENOENT;
        ri.res_handle = g->resource->gres;
        ri.size = g->resource->size;
        ri.blob_mem = g->resource->hostBlob ? 2 : 0;
        return CopyToUser(arg, &ri, sizeof(ri)) ? 0 : -EFAULT;
    }
    case DRM_NR_VIRTGPU_TRANSFER_TO_HOST:
    case DRM_NR_VIRTGPU_TRANSFER_FROM_HOST:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpu3dTransfer)))
            return -EFAULT;
        DrmVirtgpu3dTransfer t = *reinterpret_cast<DrmVirtgpu3dTransfer*>(arg);
        DrmGemObj* g = DrmGemGet(dctx, t.bo_handle);
        if (!g) return -ENOENT;
        if (g->resource->owner->ctxId <= 0 || g->resource->hostBlob) return -EINVAL;
        const brook::GpuAppOps* ga = brook::GpuAppGet();
        if (!ga || !ga->Transfer3D) return -ENODEV;
        int dir = (nr == DRM_NR_VIRTGPU_TRANSFER_TO_HOST) ? 0 : 1;
        if (!t.box.w || !t.box.h || !t.box.d) return -EINVAL;
        const brook::GpuTransfer3D transfer{
            t.box.x,t.box.y,t.box.z,t.box.w,t.box.h,t.box.d,
            t.offset,t.level,t.stride,t.layer_stride};
        int32_t r = ga->Transfer3D(g->resource->owner->ctxId, (int32_t)g->resource->gres, dir,
                                   transfer);
        return r < 0 ? -EIO : 0;
    }
    case DRM_NR_VIRTGPU_EXECBUFFER:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuExecbuffer)))
            return -EFAULT;
        DrmVirtgpuExecbuffer eb = *reinterpret_cast<DrmVirtgpuExecbuffer*>(arg);
        if (dctx->owner->ctxId <= 0) return -EINVAL;
        if (dctx->owner->config.capsetId==4) {
            if ((eb.flags&~6u) || !(eb.flags&4u) || eb.ring_idx>=dctx->owner->config.numRings ||
                eb.syncobj_stride || eb.num_in_syncobjs || eb.num_out_syncobjs ||
                (eb.size&3u) || eb.size>60u*1024u-32u || eb.num_bo_handles>DRM_MAX_GEM) return -EINVAL;
            if ((eb.flags&2u) && !UserBufferWritable(arg,sizeof(eb))) return -EFAULT;
            struct BoSnapshot {
                uint32_t* data;
                ~BoSnapshot(){kfree(data);}
            } handles{eb.num_bo_handles?static_cast<uint32_t*>(kmalloc(eb.num_bo_handles*sizeof(uint32_t))):nullptr};
            if(eb.num_bo_handles && !handles.data)return -ENOMEM;
            if(eb.num_bo_handles && !CopyFromUser(handles.data,eb.bo_handles,eb.num_bo_handles*sizeof(uint32_t)))return -EFAULT;
            for(uint32_t i=0;i<eb.num_bo_handles;++i)if(!DrmGemGet(dctx,handles.data[i]))return -ENOENT;
            auto* commands=eb.size ? static_cast<uint32_t*>(kmalloc(eb.size)) : nullptr;
            if (eb.size && !commands) return -ENOMEM;
            if (eb.size && !CopyFromUser(commands,eb.command,eb.size)) {kfree(commands);return -EFAULT;}
            // BO reservations are also dependencies when no fd was requested.
            // Retain a pending marker until this exact selected timeline completes.
            uint32_t begun=0;
            for(;begun<eb.num_bo_handles;++begun) {
                auto* gem=DrmGemGet(dctx,handles.data[begun]);
                KMutexLock(&gem->resource->reservationLock);
                const bool admitted=gem->resource->reservation.Begin();
                KMutexUnlock(&gem->resource->reservationLock);
                if(!admitted)break;
            }
            const auto* ga=GpuAppGet();
            const bool admitted=begun==eb.num_bo_handles;
            const int result=admitted && ga && ga->SubmitVenus ?
                ga->SubmitVenus(dctx->owner->ctxId,commands,eb.size/4,eb.ring_idx,(eb.flags&2u)!=0 || eb.num_bo_handles!=0) : -1;
            for(uint32_t i=0;i<begun;++i) {
                auto* gem=DrmGemGet(dctx,handles.data[i]);
                if(gem) {
                    KMutexLock(&gem->resource->reservationLock);
                    gem->resource->reservation.Finish(!admitted || result==0);
                    KMutexUnlock(&gem->resource->reservationLock);
                }
            }
            kfree(commands);
            if (result<0) return -EIO;
            if (eb.flags&2u) {
                auto* process=ProcessCurrent();
                const int descriptor=CreateCompletedSyncFile(process);
                if(descriptor<0)return descriptor;
                SyncFilePublicationPin publication(process,descriptor);
                eb.fence_fd=descriptor;
                if(!CopyToUser(arg,&eb,sizeof(eb))) {CloseProcessFd(process,descriptor);return -EFAULT;}
            }
            return 0;
        }
        if (eb.size == 0 || (eb.size & 3) || eb.size > 1024u * 1024) return -EINVAL;
        if (!UserBufferReadable(eb.command, eb.size)) return -EFAULT;
        const brook::GpuAppOps* ga = brook::GpuAppGet();
        if (!ga || !ga->Submit3D) return -ENODEV;
        if(eb.num_bo_handles>DRM_MAX_GEM)return -EINVAL;
        // Snapshot and validate the entire BO list before publishing any GPU
        // command. The pinned DRM file + mappingLock retain these canonical
        // GEM references until the driver's shared completion fence returns.
        struct HandleSnapshot { uint32_t* data; ~HandleSnapshot(){kfree(data);} }
            handles{eb.num_bo_handles?static_cast<uint32_t*>(kmalloc(eb.num_bo_handles*sizeof(uint32_t))):nullptr};
        if(eb.num_bo_handles && !handles.data)return -ENOMEM;
        if(eb.num_bo_handles && !CopyFromUser(handles.data,eb.bo_handles,eb.num_bo_handles*sizeof(uint32_t)))return -EFAULT;
        for(uint32_t i=0;i<eb.num_bo_handles;++i)
            if(!DrmGemGet(dctx,handles.data[i]))return -ENOENT;
        uint32_t nDw = eb.size / 4;
        auto* cmd = static_cast<uint32_t*>(kmalloc(eb.size));
        if (!cmd) return -ENOMEM;
        if(!CopyFromUser(cmd,eb.command,eb.size)) {kfree(cmd);return -EFAULT;}
        int32_t r = ga->Submit3D(dctx->owner->ctxId, cmd, nDw);
        kfree(cmd);
        if (r < 0) return -EIO;
        // QEMU can acknowledge SUBMIT_3D while rendering is still in flight.
        // Complete a real fence on the app context before handing Mesa a
        // signaled fd: the compositor samples this buffer in another context.
        if (eb.flags & 0x02 /*FENCE_FD_OUT*/) {
            if (!ga->PresentBarrier || ga->PresentBarrier(dctx->owner->ctxId) < 0)
                return -EIO;
            Process* fp = ProcessCurrent();
            const int fenceFd=CreateCompletedSyncFile(fp);
            if(fenceFd<0)return fenceFd;
            SyncFilePublicationPin publication(fp,fenceFd);
            eb.fence_fd = fenceFd;
            if (!CopyToUser(arg, &eb, sizeof(eb))) {
                CloseProcessFd(fp,fenceFd);
                return -EFAULT;
            }
        }
        return 0;
    }
    case DRM_NR_VIRTGPU_WAIT:
    {
        if(!UserBufferReadable(arg,sizeof(DrmVirtgpu3dWait)))return -EFAULT;
        DrmVirtgpu3dWait wait{};
        if(!CopyFromUser(&wait,arg,sizeof(wait)))return -EFAULT;
        auto* gem=DrmGemGet(dctx,wait.handle);if(!gem)return -ENOENT;
        if(gem->resource->hostBlob) {
            if((wait.flags&~1u) || gem->resource->owner->config.capsetId==4)return -95;
            // GL contexts with host blobs complete their submissions before
            // returning, so NOWAIT may safely report idle in this bounded path.
            if(wait.flags&1u)return 0;
            const auto* ga=GpuAppGet();
            return ga && ga->PresentBarrier && ga->PresentBarrier(gem->resource->owner->ctxId)==0 ? 0 : -EIO;
        }
        // Preserve the legacy guest-backed path at this checkpoint.
        return 0;
    }
    case DRM_NR_PRIME_HANDLE_TO_FD:
    {
        // Export a GEM (render target) as a PRIME dmabuf fd: a capability that
        // wraps the GEM's host virgl resource (gres). The client passes this fd
        // over the Wayland socket so the compositor can import + present it.
        if (!UserBufferReadable(arg, sizeof(DrmPrimeHandle)) ||
            !UserBufferWritable(arg, sizeof(DrmPrimeHandle)))
            return -EFAULT;
        DrmPrimeHandle ph = *reinterpret_cast<DrmPrimeHandle*>(arg);
        DrmGemObj* g = DrmGemGet(dctx, ph.handle);
        if (!g) return -ENOENT;
        Process* proc = ProcessCurrent();
        if (!proc) return -ESRCH;
        auto* p = static_cast<DrmPrimeObj*>(kmalloc(sizeof(DrmPrimeObj)));
        if (!p) return -ENOMEM;
        p->resource=g->resource;
        MappingBackingRef(p->resource->lifetime);
        p->refCount=1;
        // Sticky authorization for the numeric WM_PRESENT_GRES ABI. Actual
        // lifetime is owned by GEM/PRIME/VMA/capture references, not this flag.
        __atomic_store_n(&p->resource->everExported,true,__ATOMIC_RELEASE);
        int fd = FdAlloc(proc, FdType::DrmPrime, p);
        if (fd < 0) { DrmPrimeUnref(p);return -EMFILE; }
        ph.fd = fd;
        if (!CopyToUser(arg, &ph, sizeof(ph))) {
            CloseProcessFd(proc, fd);
            return -EFAULT;
        }
        return 0;
    }
    case DRM_NR_PRIME_FD_TO_HANDLE:
    {
        // Import a PRIME dmabuf fd (received over the Wayland socket) into a GEM
        // handle aliasing the SAME gres. Possession of the fd IS the
        // authorization — a process cannot reach a gres it was never handed.
        if (!UserBufferReadable(arg, sizeof(DrmPrimeHandle)) ||
            !UserBufferWritable(arg, sizeof(DrmPrimeHandle)))
            return -EFAULT;
        DrmPrimeHandle ph = *reinterpret_cast<DrmPrimeHandle*>(arg);
        Process* proc = ProcessCurrent();
        if (!proc) return -ESRCH;
        // Keep the capability alive across metadata lookup and GEM creation.
        // FdGet drops the table lock and can race a concurrent close/reuse.
        FdRef primeRef(proc, ph.fd);
        FdEntry* fde = primeRef.get();
        if (!fde || fde->type != FdType::DrmPrime || !fde->handle) return -EINVAL;
        auto* p = static_cast<DrmPrimeObj*>(fde->handle);
        // Repeated PRIME imports into one DRM file describe one GEM handle,
        // including imports through separately exported FDs and self-import.
        // Compare shared object identity, independently of host ID or GEM-slot
        // reuse. The pinned PRIME capability retains that exact object.
        for (uint32_t i = 0; i < DRM_MAX_GEM; ++i) {
            if (dctx->gem[i].used && dctx->gem[i].resource == p->resource) {
                ph.handle = i + 1;
                return CopyToUser(arg, &ph, sizeof(ph)) ? 0 : -EFAULT;
            }
        }
        // This bounded import path supports legacy VirGL textures/buffers.
        // A VirGL fence does not certify a Venus queue or host-blob lifetime.
        if(p->resource->hostBlob || p->resource->owner->config.capsetId==4 ||
           dctx->owner->config.capsetId==4)return -95;
        const auto* ga=GpuAppGet();
        if(!ga || !ga->ImportResource || !ga->CtxCreate)return -ENODEV;
        if(dctx->owner->ctxId<=0) {
            const int32_t context=ga->CtxCreate(proc->pid,dctx->owner->config);
            if(context<0)return -ENOMEM;
            dctx->owner->ctxId=context;
        }
        if(ga->ImportResource(dctx->owner->ctxId,p->resource->owner->ctxId,p->resource->gres)<0)return -EIO;
        uint32_t bo = DrmGemAllocImported(dctx,p->resource);
        if (bo == 0) return -ENOMEM;
        ph.handle = bo;
        if (!CopyToUser(arg, &ph, sizeof(ph))) {
            // Drop only the newly inserted alias/reference. Existing shared
            // handles and the PRIME capability remain owned by their callers.
            DrmGemDrop(&dctx->gem[bo-1]);
            return -EFAULT;
        }
        return 0;
    }
    case DRM_NR_GEM_CLOSE:
    {
        if (!UserBufferReadable(arg, sizeof(DrmGemClose))) return -EFAULT;
        DrmGemClose gc = *reinterpret_cast<DrmGemClose*>(arg);
        DrmGemObj* g = DrmGemGet(dctx, gc.handle);
        if (!g) return -ENOENT;
        // Drop this alias; other handles, PRIME capabilities and VMAs retain
        // the shared resource. The last reference queues verified retirement.
        if (g->resource->backingKva) {
            const uint64_t pages=(static_cast<uint64_t>(g->resource->backingBytes)+4095)/4096;
            __atomic_fetch_add(&g_drmBackingDiagnostics.gemCloseBackingAliases,1,__ATOMIC_RELAXED);
            __atomic_fetch_add(&g_drmBackingDiagnostics.gemCloseBackingAliasPageBytes,pages*4096,__ATOMIC_RELAXED);
        }
        DrmGemDrop(g);
        return 0;
    }
    case DRM_NR_VIRTGPU_MAP:
    {
        // Map-for-CPU-access (e.g. glReadPixels staging buffers). Lazily
        // allocate guest backing for the resource and attach it to the host
        // virgl resource, so a later TRANSFER_FROM_HOST DMAs the rendered
        // pixels into pages that sys_mmap then maps into user space. The
        // returned offset identifies this GEM incarnation (see sys_mmap).
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuMap)) ||
            !UserBufferWritable(arg, sizeof(DrmVirtgpuMap)))
            return -EFAULT;
        DrmVirtgpuMap m = *reinterpret_cast<DrmVirtgpuMap*>(arg);
        DrmGemObj* g = DrmGemGet(dctx, m.handle);
        if (!g) return -ENOENT;
        if (g->resource->hostBlob && !g->resource->hostMapping.length) return -EINVAL;
        if (!DrmEnsureBacking(g)) return -ENOMEM;
        if (!g->mapToken) {
            g->mapToken = g_drmMapTokens.Allocate(g->resource->size ? g->resource->size : 4096);
            if (!g->mapToken) return -ENOMEM;
        }
        m.offset = g->mapToken;
        return CopyToUser(arg, &m, sizeof(m)) ? 0 : -EFAULT;
    }
    default:
        return -25; // ENOTTY
    }
}

// The Intel file type is not published yet. These operations provide the
// real GEM handle/mmap substrate for a future admitted i915 render node;
// unsupported execution and cache modes fail rather than report success.
int64_t IntelRenderIoctl(brook::BdwGpuFile* file,uint64_t cmd,uint64_t arg,uint64_t renderFd) {
    // Exec preparation and admission retain stack-owned ledger nodes and
    // snapshots across user copies and sleepable locks. A fatal signal must
    // unwind those objects before the scheduler may retire this thread.
    KernelOperationGuard operation(ProcessCurrent());
    brook::BdwI915UserAccess user{nullptr,
        [](void*,void* dst,uint64_t src,uint64_t n){return CopyFromUser(dst,src,n);},
        [](void*,uint64_t dst,uint64_t n){return UserBufferWritable(dst,n);},
        [](void*,uint64_t dst,const void* src,uint64_t n){return CopyToUser(dst,src,n);},
        [](void*){return ClockNowNs(1);},
        [](void*,uint64_t){auto* self=ProcessCurrent();if(!self || HasPendingSignals())return false;
            self->wakeupTick=ApicTickCount()+1;SchedulerBlockInterruptible(self);return !HasPendingSignals();}};
    // Legacy GEM_MMAP (VA-API phase 2a): map the buffer's token through this
    // same descriptor, exactly as the caller's own mmap(2) would.
    uint64_t mapFd=renderFd;user.context=&mapFd;
    user.mapBuffer=[](void* context,uint64_t token,uint64_t bytes,uint64_t& address)->int64_t{
        const int64_t mapped=sys_mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,*static_cast<uint64_t*>(context),token);
        if(mapped<0)return mapped;address=static_cast<uint64_t>(mapped);return 0;};
    if(auto* current=ProcessCurrent()){
        user.submitterPid=current->pid;
        user.submitterIncarnation=current->leaderIncarnation?current->leaderIncarnation:current->incarnation;
    }
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    if(brook::BdwRenderExperimentEnabled() || brook::DebugHardwareAutoInitializeIntel()){
        user.submit=brook::BdwSubmitRenderExperiment;user.renderExperiment=true;
        user.handoffSubmit=brook::BdwOffscreenHandoffSubmit;
    } else if(brook::BdwStoreProbeEnabled())user.submit=brook::BdwSubmitStoreProbe;
    if(user.submit){user.beginExec=brook::BdwBeginRenderPreparation;user.endExec=brook::BdwEndRenderPreparation;}
#endif
#ifdef BROOK_BDW_CACHE_PROBE
    if(cmd==BROOK_BDW_CACHE_PROBE_IOCTL){
        auto* proc=ProcessCurrent();
        if(!proc || proc->euid!=0)return -EPERM;
        if(!user.renderExperiment)return -ENODEV;
        BrookBdwCacheProbe request{};
        if(!CopyFromUser(&request,arg,sizeof(request)) || !UserBufferWritable(arg,sizeof(request)))return -EFAULT;
        if(request.reserved || request.skippedToDevice || request.skippedToCpu)return -EINVAL;
        if(request.operation==BROOK_BDW_CACHE_PROBE_QUERY){
            if(request.fd<0 || request.fd>=static_cast<int32_t>(MAX_FDS) || request.omit || request.bytes)return -EINVAL;
            FdRef prime(proc,request.fd);
            if(!prime || prime->type!=FdType::IntelPrime || !prime->handle)return -EBADF;
            auto* buffer=static_cast<brook::BdwGpuBuffer*>(prime->handle);
            brook::BdwCacheProbeCounters counters{};
            if(!brook::BdwGpuBufferCacheProbeCounters(buffer,counters))return -EINVAL;
            request.bytes=brook::BdwGpuBufferBytes(buffer);
            request.omit=counters.omit;
            request.skippedToDevice=counters.toDevice;
            request.skippedToCpu=counters.toCpu;
            return CopyToUser(arg,&request,sizeof(request))?0:-EFAULT;
        }
        if(request.operation!=BROOK_BDW_CACHE_PROBE_ALLOC || request.fd!=-1 ||
           !request.bytes || request.bytes>BROOK_BDW_CACHE_PROBE_MAX_BYTES || (request.omit&~3u))return -EINVAL;
        auto* buffer=brook::BdwGpuBufferCreateCacheProbe(request.bytes,request.omit);
        if(!buffer)return -ENOMEM;
        FdEntry owned{};owned.type=FdType::IntelPrime;owned.handle=buffer;owned.fdFlags=1;owned.statusFlags=2;
        const int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
        if(fd<0){brook::BdwGpuBufferUnref(buffer);return fd;}
        request.fd=fd;
        if(CopyToUser(arg,&request,sizeof(request)))return 0;
        CloseProcessFd(proc,fd);
        return -EFAULT;
    }
#endif
    // Plain syncobj descriptors retain the mutable object. Flag 1 instead
    // transfers an immutable fence through a sync-file descriptor.
    struct IntelSyncHandle {uint32_t handle,flags;int32_t fd;uint32_t pad;uint64_t point;};
    const uint32_t ioctlSize=uint32_t((cmd>>16)&0x3fff);
    if(user.renderExperiment && ((cmd>>8)&255)=='d' && ((cmd&255)==0xc1 || (cmd&255)==0xc2) &&
       (ioctlSize==16 || ioctlSize==sizeof(IntelSyncHandle))){
        IntelSyncHandle request{};
        if(!CopyFromUser(&request,arg,ioctlSize) || !UserBufferWritable(arg,ioctlSize))return -EFAULT;
        if((request.flags&~3u) || request.pad || (!(request.flags&1u) && request.point))return -EINVAL;
        const uint64_t point=(request.flags&2u)?request.point:0;
        auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
        if(request.flags==1 || request.flags==3){
            auto* sync=brook::BdwGpuFileGetSync(file,request.handle);if(!sync)return -ENOENT;
            if((cmd&255)==0xc1){
                auto* fence=point?brook::BdwGpuSyncFindTimeline(sync,point):brook::BdwGpuSyncAcquire(sync);
                brook::BdwGpuSyncUnref(sync);
                if(!fence)return -EINVAL;
                const int fd=CreateIntelSyncFile(proc,fence);brook::BdwFenceUnref(fence);
                if(fd<0)return fd;
                SyncFilePublicationPin publication(proc,fd);
                request.fd=fd;if(!CopyToUser(arg,&request,ioctlSize)){CloseProcessFd(proc,fd);return -EFAULT;}
                return 0;
            }
            FdRef reference(proc,request.fd);auto* entry=reference.get();
            if(!entry || entry->type!=FdType::SyncFile || !entry->handle){brook::BdwGpuSyncUnref(sync);return -EINVAL;}
            auto* source=static_cast<SyncFileData*>(entry->handle);
            BdwFence* fence=source->intelFence;
            if(fence)BdwFenceRef(fence);else fence=BdwFenceCreate(true,nullptr,0,nullptr,source->timestampNs);
            if(!fence){brook::BdwGpuSyncUnref(sync);return -ENOMEM;}
            const bool ok=point?brook::BdwIntelSyncPointAdmitted(sync,point,fence):brook::BdwIntelSyncReplaceAdmitted(sync,fence);
            BdwFenceUnref(fence);brook::BdwGpuSyncUnref(sync);return ok?0:-EINVAL;
        }
        if((cmd&255)==0xc1){
            auto* sync=brook::BdwGpuFileGetSync(file,request.handle);if(!sync)return -ENOENT;
            FdEntry owned{};owned.type=FdType::IntelSyncObj;owned.handle=sync;owned.fdFlags=1;
            const int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
            if(fd<0){brook::BdwGpuSyncUnref(sync);return -EMFILE;}
            request.fd=fd;if(!CopyToUser(arg,&request,ioctlSize)){CloseProcessFd(proc,fd);return -EFAULT;}
            return 0;
        }
        FdRef reference(proc,request.fd);auto* entry=reference.get();
        if(!entry || entry->type!=FdType::IntelSyncObj || !entry->handle)return -EINVAL;
        request.handle=brook::BdwGpuFileImportSync(file,static_cast<brook::BdwGpuSync*>(entry->handle));
        if(!request.handle)return -ENOMEM;
        if(!CopyToUser(arg,&request,ioctlSize)){brook::BdwGpuFileDestroySync(file,request.handle);return -EFAULT;}
        return 0;
    }
    // Keep Intel PRIME private to the explicit render experiment until its
    // synchronization and compositor consumers are implemented and tested.
    if(user.renderExperiment && ((cmd>>8)&255)=='d' && ((cmd>>16)&0x3fff)==sizeof(DrmPrimeHandle) &&
       ((cmd&255)==DRM_NR_PRIME_HANDLE_TO_FD || (cmd&255)==DRM_NR_PRIME_FD_TO_HANDLE)){
        DrmPrimeHandle request{};
        if(!CopyFromUser(&request,arg,sizeof(request)) || !UserBufferWritable(arg,sizeof(request)))return -EFAULT;
        auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
        if((cmd&255)==DRM_NR_PRIME_HANDLE_TO_FD){
            if(request.flags&~(0x80000u|2u))return -EINVAL; // DRM_CLOEXEC | DRM_RDWR
            auto* buffer=brook::BdwGpuFileGetBuffer(file,request.handle);if(!buffer)return -ENOENT;
            FdEntry owned{};owned.type=FdType::IntelPrime;owned.handle=buffer;
            owned.fdFlags=(request.flags&0x80000u)?1:0;owned.statusFlags=(request.flags&2)?2:0;
            const int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
            if(fd<0){brook::BdwGpuBufferUnref(buffer);return -EMFILE;}
            request.fd=fd;
            if(!CopyToUser(arg,&request,sizeof(request))){CloseProcessFd(proc,fd);return -EFAULT;}
            return 0;
        }
        if(request.flags)return -EINVAL;
        FdRef prime(proc,request.fd);auto* entry=prime.get();
        if(!entry || entry->type!=FdType::IntelPrime || !entry->handle)return -EINVAL;
        request.handle=brook::BdwGpuFileImportBuffer(file,static_cast<brook::BdwGpuBuffer*>(entry->handle));
        if(!request.handle)return -ENOMEM;
        // On an exceptional copyout failure the alias remains until the file
        // closes: removing it could revoke a concurrent successful import.
        return CopyToUser(arg,&request,sizeof(request))?0:-EFAULT;
    }
    const int64_t result=brook::BdwIntelRenderIoctl(file,cmd,arg,user);
    // An execbuffer refusal becomes VK_ERROR_DEVICE_LOST in Mesa: name it.
    if(result<0 && (cmd&0xff)==0x69){
        static uint64_t refusals=0;const char* why=brook::BdwTakeRefusal();
        if(brook::BdwRefusalLogDue(refusals))
            KPrintf("gpu-exec: execbuffer refused pid=%u errno=%ld reason=%s count=%lu\n",user.submitterPid,-result,
                why?why:"unrecorded",refusals);
    }else if((cmd&0xff)==0x69)(void)brook::BdwTakeRefusal();
    else if((cmd&0xff)==0x72){
        static uint64_t losses=0;
        if(const char* why=brook::BdwTakeRefusal())
            if(brook::BdwRefusalLogDue(losses))KPrintf("gpu-exec: pid=%u %s count=%lu\n",user.submitterPid,why,losses);
    }
    return result;
}

} // namespace brook
