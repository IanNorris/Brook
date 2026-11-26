#pragma once
// nv-drm PRIME (M5 zero-copy, notes/m5/zero-copy-design.md): nouveau GEM BOs shared between
// renderD130 files and exported as dmabuf fds. Header-only core, included at the end of
// nv_drm_core.h; the host test drives it with the fake backend, the kernel glue
// (nv_drm_prime.cpp) owns the fd side (FdType::NvPrime holds one Shared reference).
//
// Ownership: a BO owns its backing (VRAM pa or sysmem cookie) until its first export. Export
// moves the backing into a refcounted Shared object; from then on every BO naming it (the
// exporter's and each importer's, at most one open handle per file, as Linux GEM) and every
// dmabuf fd holds a reference, and the last one frees the backing through the backend.
//
// Implicit sync (dma_resv in miniature): a write fence and a read fence, each a join of the
// fences added since it last completed. DMA_BUF_IOCTL_IMPORT_SYNC_FILE adds; EXPORT_SYNC_FILE
// and CPU_PREP read. Mesa's WSI imports the present semaphore's sync file (WRITE) before
// wl_surface.commit, so a compositor that waits on the buffer sees the frame's completion.
// Fallback for producers that never import a fence: while no write fence was ever imported,
// each EXEC of a file that has the BO mapped joins the BO's "bookkeep" fence (conservative:
// may wait for later work too, never for less).
#include <stdint.h>

namespace brook::nvdrm {

struct Shared {
    uint32_t refs;              // BOs naming it + dmabuf fds (atomic)
    const Backend* be;
    uint64_t size, backing;     // VRAM pa or sysmem cookie (immutable)
    uint32_t domain, tile_mode, tile_flags;
    uint64_t identity;          // fstat st_ino
    KMutex lock;                // the fence fields
    GpuFence* write;            // implicit writers (joined)
    GpuFence* read;             // implicit readers (joined)
    GpuFence* bookkeep;         // EXEC fallback while !explicit_write
    bool explicit_write;        // a write fence was imported at least once
    Aper aper() const { return (domain & GEM_DOMAIN_VRAM) ? Aper::Vram : Aper::Sys; }
    uint8_t kind() const { return uint8_t((tile_flags >> 8) & 0xff); }
};

// The backend's allocator is not locked across files; a Shared object can be freed from any
// file's close or from an fd close. The kernel glue serialises the backend's alloc/free with
// one lock (nv_drm.cpp), the host test is single threaded.
inline uint64_t g_shared_identity = 0;

inline void SharedRef(Shared* s) { if (s) __atomic_fetch_add(&s->refs, 1, __ATOMIC_RELAXED); }
inline void SharedUnref(Shared* s) {
    if (!s || __atomic_fetch_sub(&s->refs, 1, __ATOMIC_ACQ_REL) != 1) return;
    if (s->be) { if (s->aper() == Aper::Vram) s->be->vram_free(s->be->ctx, s->backing, s->size); else s->be->sys_free(s->be->ctx, s->backing); }
    GpuFenceUnref(s->write); GpuFenceUnref(s->read); GpuFenceUnref(s->bookkeep);
    kfree(s);
}

// `slot` = slot join `add`: replace when the old fence completed successfully (bounded chains),
// otherwise a join that resolves when both have (a failure in either propagates).
inline bool FenceJoinInto(GpuFence*& slot, GpuFence* add) {
    if (!add) return true;
    if (!slot || GpuFenceState(slot) == 1) { GpuFenceRef(add); GpuFenceUnref(slot); slot = add; return true; }
    GpuFence* j = BdwFenceCreate(true, slot, 0, add);
    if (!j) return false;
    GpuFenceUnref(slot); slot = j;
    return true;
}
// DMA_BUF_SYNC_READ = 1, DMA_BUF_SYNC_WRITE = 2 (linux/dma-buf.h).
constexpr uint32_t DMA_BUF_SYNC_READ = 1, DMA_BUF_SYNC_WRITE = 2;
// IMPORT_SYNC_FILE: WRITE adds a writer, READ (only) adds a reader.
inline bool SharedAddFence(Shared* s, GpuFence* fence, uint32_t flags) {
    KMutexLock(&s->lock);
    bool ok;
    if (flags & DMA_BUF_SYNC_WRITE) { ok = FenceJoinInto(s->write, fence); s->explicit_write = true; }
    else ok = FenceJoinInto(s->read, fence);
    KMutexUnlock(&s->lock);
    return ok;
}
// EXPORT_SYNC_FILE / CPU_PREP: READ access waits for writers, WRITE access for readers too.
// *out = a referenced fence (nullptr = nothing to wait for); false on allocation failure.
inline bool SharedFence(Shared* s, uint32_t flags, GpuFence** out) {
    *out = nullptr;
    KMutexLock(&s->lock);
    GpuFence* w = s->explicit_write ? s->write : s->bookkeep;
    bool ok = FenceJoinInto(*out, w);
    if (ok && (flags & DMA_BUF_SYNC_WRITE)) ok = FenceJoinInto(*out, s->read);
    KMutexUnlock(&s->lock);
    if (!ok) { GpuFenceUnref(*out); *out = nullptr; }
    return ok;
}
inline bool SharedNoteExec(Shared* s, GpuFence* fence) {
    KMutexLock(&s->lock);
    const bool ok = s->explicit_write || FenceJoinInto(s->bookkeep, fence);
    KMutexUnlock(&s->lock);
    return ok;
}
// EXEC: every shared BO mapped in this file's VAS may be written by the job (the VM_BIND uAPI
// has no BO list). Called with the file lock held.
inline void PrimeNoteExec(File& f, GpuFence* done) {
    if (!f.nshared) return;
    for (auto& b : f.bos) if (b.handle && b.shared && b.refs > (b.handle_open ? 1u : 0u)) (void)SharedNoteExec(b.shared, done);
}

inline Bo* AllocBoSlot(File& f, uint32_t* h) {
    for (uint32_t i = 0; i < kMaxBos; ++i) {
        const uint32_t k = (f.next_handle - 1 + i) % kMaxBos;
        if (!f.bos[k].handle) { *h = k + 1; return &f.bos[k]; }
    }
    return nullptr;
}

// PRIME_HANDLE_TO_FD (core half): the Shared object behind `handle`, created on the first
// export. Returns a reference for the fd. NO_SHARE BOs are VM-private (nouveau: -EINVAL).
inline int64_t PrimeExport(File& f, uint32_t handle, Shared** out) {
    *out = nullptr;
    if (!f.be) return kENOTTY;
    Bo* b = FindBo(f, handle);
    if (!b || !b->handle_open) { f.why = "prime export: no such handle"; return kENOENT; }
    if (b->domain & GEM_DOMAIN_NO_SHARE) { f.why = "prime export: NO_SHARE BO"; return kEINVAL; }
    if (!b->shared) {
        auto* s = static_cast<Shared*>(kmalloc(sizeof(Shared)));
        if (!s) return kENOMEM;
        __builtin_memset(s, 0, sizeof(*s));
        s->refs = 1; s->be = f.be; s->size = b->size; s->backing = b->backing;
        s->domain = b->domain; s->tile_mode = b->tile_mode; s->tile_flags = b->tile_flags;
        s->identity = __atomic_add_fetch(&g_shared_identity, 1, __ATOMIC_RELAXED);
        KMutexInit(&s->lock);
        b->shared = s; ++f.nshared;   // the BO's reference: BoPut drops it instead of freeing the backing
    }
    SharedRef(b->shared);
    *out = b->shared;
    return 0;
}
// PRIME_FD_TO_HANDLE (core half): the file's open handle for `s` if it has one (Linux returns
// the same GEM handle for a re-import, Mesa's BO cache relies on it), else a new BO.
inline int64_t PrimeImport(File& f, Shared* s, uint32_t* handle) {
    *handle = 0;
    if (!f.be || !s) return kENOTTY;
    if (s->be != f.be) return kEINVAL;
    for (auto& b : f.bos) if (b.handle && b.handle_open && b.shared == s) { *handle = b.handle; return 0; }
    uint32_t h = 0;
    Bo* slot = AllocBoSlot(f, &h);
    if (!slot) return kENOSPC;
    Bo b; b.handle = h; b.refs = 1; b.handle_open = true; b.size = s->size; b.backing = s->backing;
    b.domain = s->domain; b.tile_mode = s->tile_mode; b.tile_flags = s->tile_flags;
    b.map_token = kMapTokenBase + (uint64_t(h) << 32);
    b.shared = s; SharedRef(s); ++f.nshared;
    *slot = b; f.next_handle = h % kMaxBos + 1;
    *handle = h;
    return 0;
}
// The Shared object behind an open handle (sync ioctls on a handle), referenced.
inline Shared* PrimeShared(File& f, uint32_t handle) {
    Bo* b = FindBo(f, handle);
    if (!b || !b->handle_open || !b->shared) return nullptr;
    SharedRef(b->shared);
    return b->shared;
}

// ---- syncobj <-> fd (DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD / FD_TO_HANDLE, core halves) ----------
struct DrmSyncobjHandle { uint32_t handle, flags; int32_t fd; uint32_t pad; uint64_t point; };
static_assert(sizeof(DrmSyncobjHandle) == 24);
constexpr uint32_t SYNCOBJ_FD_EXPORT_SYNC_FILE = 1, SYNCOBJ_FD_IMPORT_SYNC_FILE = 1, SYNCOBJ_FD_TIMELINE = 2;
// The fence a sync-file export carries: (handle, point) as a wait would see it. Referenced;
// nullptr with *rc set when there is none (-EINVAL as Linux for an unsubmitted point).
inline GpuFence* SyncobjExportFence(File& f, uint32_t handle, uint64_t point, int64_t* rc) {
    GpuSync* s = f.syncs.Get(handle);
    if (!s) { *rc = kENOENT; return nullptr; }
    GpuFence* fe = SyncPointFence(s, point);
    GpuSyncUnref(s);
    *rc = fe ? 0 : kEINVAL;
    return fe;
}
// Sync-file import: installs `fence` (nullptr = an already-signalled sync file) at (handle, point).
inline int64_t SyncobjImportFence(File& f, uint32_t handle, uint64_t point, GpuFence* fence) {
    GpuSync* s = f.syncs.Get(handle);
    if (!s) return kENOENT;
    GpuFence* owned = fence ? nullptr : GpuFenceCreate(true);
    if (!fence && !owned) { GpuSyncUnref(s); return kENOMEM; }
    const bool ok = SyncPointSignal(s, point, fence ? fence : owned);
    GpuFenceUnref(owned); GpuSyncUnref(s);
    return ok ? 0 : kEINVAL;
}

}  // namespace brook::nvdrm
