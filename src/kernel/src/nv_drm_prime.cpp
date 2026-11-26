// nv-drm PRIME glue (notes/m5/zero-copy-design.md): the fd side of nv_drm_prime.h.
//   renderD130  PRIME_HANDLE_TO_FD / PRIME_FD_TO_HANDLE (FdType::NvPrime, one Shared reference)
//               SYNCOBJ_HANDLE_TO_FD / FD_TO_HANDLE: sync files (SyncFile, GpuFence) and opaque
//               syncobj fds (FdType::IntelSyncObj: the GPU-neutral GpuSync, as the i915 node)
//   dmabuf fd   DMA_BUF_IOCTL_EXPORT_SYNC_FILE / IMPORT_SYNC_FILE (implicit sync), DMA_BUF_IOCTL_SYNC
//               (no-op: GART pages are coherent), mmap of GART BOs, fstat, SEEK_END.
#include "nv_drm.h"
#include "syscall_drm.h"
#include "syscall_internal.h"
#include "syscall.h"
#include "user_copy.h"
#include "process.h"
#include "memory/mapping_ranges.h"

namespace brook {
using nvdrm::Shared;

namespace {
constexpr uint32_t kDrmCloexec = 0x80000, kDrmRdwr = 2;   // DRM_CLOEXEC (O_CLOEXEC), DRM_RDWR (O_RDWR)
struct PrimeHandle { uint32_t handle, flags; int32_t fd; };   // struct drm_prime_handle
static_assert(sizeof(PrimeHandle) == 12);
Shared* S(void* h) { return static_cast<Shared*>(h); }

int PublishPrime(Process* proc, Shared* s, uint32_t flags) {
    FdEntry owned{}; owned.type = FdType::NvPrime; owned.handle = s; owned.fdFlags = (flags & kDrmCloexec) ? 1 : 0;
    const int fd = FdPublishNew(proc, owned, (flags & kDrmRdwr) ? 2 : 0);
    if (fd < 0) nvdrm::SharedUnref(s);
    return fd;
}
// Publish a sync file for `fence` (nullptr: an already-signalled one) and store its number.
int64_t PublishSyncFile(Process* proc, GpuFence* fence, int32_t* out) {
    const int fd = fence ? CreateGpuSyncFile(proc, fence, SyncFileDriver::Nouveau) : CreateCompletedSyncFile(proc);
    if (fd < 0) return fd;
    SyncFilePublicationPin publication(proc, fd);
    *out = fd;
    return 0;
}
}  // namespace

void NvPrimeRef(void* h) { nvdrm::SharedRef(S(h)); }
void NvPrimeUnref(void* h) { nvdrm::SharedUnref(S(h)); }
uint64_t NvPrimeBytes(void* h) { return S(h)->size; }
uint64_t NvPrimeIdentity(void* h) { return S(h)->identity; }
bool NvPrimeMapGart(void* h, uint64_t offset, uint64_t length, uint64_t* phys, MappingBacking** life) {
    Shared* s = S(h);
    if (!s || !length || s->aper() != nvdrm::Aper::Sys || offset + length > s->size || offset + length < offset) return false;
    const auto* pages = reinterpret_cast<const NvDrmSysPages*>(s->backing);
    *phys = pages->phys + offset; *life = pages->life; MappingBackingRef(pages->life);
    return true;
}

int64_t NvPrimeFdIoctl(void* h, uint64_t cmd, uint64_t arg) {
    Shared* s = S(h);
    Process* proc = ProcessCurrent();
    if (!s) return -EBADF;
    if (!proc || !proc->fds) return -ESRCH;
    struct FenceIoctl { uint32_t flags; int32_t fd; };
    constexpr uint64_t kSync = 0x40086200, kExport = 0xc0086202, kImport = 0x40086203;
    if (cmd == kSync) {   // struct dma_buf_sync { u64 flags }: READ 1 | WRITE 2, END 4
        uint64_t flags = 0;
        if (!CopyFromUser(&flags, arg, sizeof(flags))) return -EFAULT;
        return (flags & ~7ull) || !(flags & 3) ? -EINVAL : 0;
    }
    if (cmd != kExport && cmd != kImport) return -25;   // ENOTTY
    FenceIoctl r{};
    if (!CopyFromUser(&r, arg, sizeof(r))) return -EFAULT;
    if (!r.flags || (r.flags & ~3u)) return -EINVAL;
    if (cmd == kExport) {
        if (!UserBufferWritable(arg, sizeof(r))) return -EFAULT;
        GpuFence* fe = nullptr;
        if (!nvdrm::SharedFence(s, r.flags, &fe)) return -ENOMEM;
        const int64_t rc = PublishSyncFile(proc, fe, &r.fd);
        GpuFenceUnref(fe);
        if (rc) return rc;
        if (!CopyToUser(arg, &r, sizeof(r))) { CloseProcessFd(proc, r.fd); return -EFAULT; }
        return 0;
    }
    FdRef source(proc, r.fd);
    if (!source || source->type != FdType::SyncFile || !source->handle) return -EINVAL;
    GpuFence* fe = static_cast<SyncFileData*>(source->handle)->fence;
    GpuFence* done = fe ? nullptr : GpuFenceCreate(true);   // a completed (virtgpu-style) sync file
    if (!fe && !done) return -ENOMEM;
    const bool ok = nvdrm::SharedAddFence(s, fe ? fe : done, r.flags);
    GpuFenceUnref(done);
    return ok ? 0 : -ENOMEM;
}

int64_t NvDrmPrimeIoctl(NvDrmFile* f, uint64_t cmd, uint64_t arg) {
    Process* proc = ProcessCurrent();
    if (!proc || !proc->fds) return -ESRCH;
    const uint32_t nr = nvdrm::IocNr(cmd), size = nvdrm::IocSize(cmd);
    if (nr == 0x2d || nr == 0x2e) {
        if (size != sizeof(PrimeHandle)) return -EINVAL;
        PrimeHandle p{};
        if (!CopyFromUser(&p, arg, sizeof(p)) || !UserBufferWritable(arg, sizeof(p))) return -EFAULT;
        if (nr == 0x2d) {   // PRIME_HANDLE_TO_FD
            if (p.flags & ~(kDrmCloexec | kDrmRdwr)) return -EINVAL;
            Shared* s = nullptr;
            nvdrm::File* core = NvDrmFileLock(f);
            const int64_t rc = core ? nvdrm::PrimeExport(*core, p.handle, &s) : nvdrm::kENOTTY;
            NvDrmFileUnlock(f);
            if (rc) return rc;
            const int fd = PublishPrime(proc, s, p.flags);
            if (fd < 0) return fd;
            p.fd = fd;
            if (!CopyToUser(arg, &p, sizeof(p))) { CloseProcessFd(proc, fd); return -EFAULT; }
            return 0;
        }
        // PRIME_FD_TO_HANDLE
        if (p.flags) return -EINVAL;
        FdRef prime(proc, p.fd);
        if (!prime || prime->type != FdType::NvPrime || !prime->handle) return -EINVAL;
        uint32_t handle = 0;
        nvdrm::File* core = NvDrmFileLock(f);
        const int64_t rc = core ? nvdrm::PrimeImport(*core, S(prime->handle), &handle) : nvdrm::kENOTTY;
        NvDrmFileUnlock(f);
        if (rc) return rc;
        p.handle = handle;
        // A copy-out failure leaves the handle open until GEM_CLOSE or file close (as the
        // i915 node): closing it here could close a concurrent import of the same BO.
        return CopyToUser(arg, &p, sizeof(p)) ? 0 : -EFAULT;
    }
    // SYNCOBJ_HANDLE_TO_FD (0xc1) / FD_TO_HANDLE (0xc2): struct drm_syncobj_handle (16, or 24 with point)
    if (size != 16 && size != sizeof(nvdrm::DrmSyncobjHandle)) return -EINVAL;
    nvdrm::DrmSyncobjHandle r{};
    if (!CopyFromUser(&r, arg, size) || !UserBufferWritable(arg, size)) return -EFAULT;
    if ((r.flags & ~3u) || r.pad) return -EINVAL;
    const bool sync_file = r.flags & 1, timeline = r.flags & 2;
    if (timeline && (!sync_file || size != sizeof(r))) return -EINVAL;
    const uint64_t point = timeline ? r.point : 0;
    if (nr == 0xc1) {
        if (sync_file) {
            int64_t rc = 0;
            nvdrm::File* core = NvDrmFileLock(f);
            GpuFence* fe = core ? nvdrm::SyncobjExportFence(*core, r.handle, point, &rc) : nullptr;
            if (!core) rc = nvdrm::kENOTTY;
            NvDrmFileUnlock(f);
            if (!fe) return rc;
            rc = PublishSyncFile(proc, fe, &r.fd);
            GpuFenceUnref(fe);
            if (rc) return rc;
        } else {   // opaque: a descriptor retaining the sync object itself
            nvdrm::File* core = NvDrmFileLock(f);
            GpuSync* s = core ? core->syncs.Get(r.handle) : nullptr;
            NvDrmFileUnlock(f);
            if (!s) return -ENOENT;
            FdEntry owned{}; owned.type = FdType::IntelSyncObj; owned.handle = s; owned.fdFlags = 1;
            const int fd = FdPublishNew(proc, owned, 0);
            if (fd < 0) { GpuSyncUnref(s); return -EMFILE; }
            r.fd = fd;
        }
        if (!CopyToUser(arg, &r, size)) { CloseProcessFd(proc, r.fd); return -EFAULT; }
        return 0;
    }
    FdRef source(proc, r.fd);
    if (sync_file) {
        if (!source || source->type != FdType::SyncFile || !source->handle) return -EINVAL;
        nvdrm::File* core = NvDrmFileLock(f);
        const int64_t rc = core ? nvdrm::SyncobjImportFence(*core, r.handle, point, static_cast<SyncFileData*>(source->handle)->fence)
                                : nvdrm::kENOTTY;
        NvDrmFileUnlock(f);
        return rc;
    }
    if (!source || source->type != FdType::IntelSyncObj || !source->handle) return -EINVAL;
    nvdrm::File* core = NvDrmFileLock(f);
    const uint32_t handle = core ? core->syncs.Import(static_cast<GpuSync*>(source->handle)) : 0;
    NvDrmFileUnlock(f);
    if (!handle) return -ENOMEM;
    r.handle = handle;
    if (!CopyToUser(arg, &r, size)) {
        core = NvDrmFileLock(f); if (core) core->syncs.Destroy(handle); NvDrmFileUnlock(f);
        return -EFAULT;
    }
    return 0;
}
}  // namespace brook
