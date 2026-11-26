#pragma once
// M5.1: the nouveau DRM uAPI subset Mesa NVK (26.0.6) needs to create a physical device, on
// /dev/dri/renderD130. Header-only core (the host test drives it with fake user memory):
//   generic   VERSION ("nouveau" 1.4.0), GET_CAP, SYNCOBJ CREATE/DESTROY/WAIT/RESET/SIGNAL
//             (binary, on the GPU-neutral GpuSyncTable)
//   nouveau   GETPARAM, VM_INIT (records the split; the VAS is created with the first channel
//             at M5.3), CHANNEL_ALLOC/FREE (bookkeeping only until M5.3), NVIF device NEW, its
//             DEVICE_INFO/TIME methods, channel SCLASS, subchannel object NEW/DEL.
// Layouts: Mesa include/drm-uapi/nouveau_drm.h, src/nouveau/drm/nvif/{ioctl,cl0080,class}.h (MIT).
// Every other command returns -ENOTTY; M5.2 adds GEM/VM_BIND, M5.3 EXEC.
#include <stdint.h>
#include "gpu_sync.h"
#include "memory/heap.h"

namespace brook::nvdrm {

constexpr int64_t kEINVAL = -22, kENOENT = -2, kENOMEM = -12, kEFAULT = -14, kENOTTY = -25, kETIME = -62,
                  kEBUSY = -16, kENOSPC = -28, kEEXIST = -17;

// DRM ioctl number layout: dir 31:30, size 29:16, type 15:8 ('d'), nr 7:0.
constexpr uint32_t IocNr(uint64_t cmd) { return uint32_t(cmd & 0xff); }
constexpr uint32_t IocSize(uint64_t cmd) { return uint32_t((cmd >> 16) & 0x3fff); }
constexpr uint32_t IocType(uint64_t cmd) { return uint32_t((cmd >> 8) & 0xff); }
constexpr uint32_t kDrmCommandBase = 0x40;

// generic DRM
constexpr uint32_t NR_VERSION = 0x00, NR_GET_CAP = 0x0c, NR_SYNCOBJ_CREATE = 0xbf, NR_SYNCOBJ_DESTROY = 0xc0,
                   NR_SYNCOBJ_WAIT = 0xc3, NR_SYNCOBJ_RESET = 0xc4, NR_SYNCOBJ_SIGNAL = 0xc5;
constexpr uint64_t DRM_CAP_SYNCOBJ = 0x13, DRM_CAP_SYNCOBJ_TIMELINE = 0x14;
constexpr uint32_t DRM_SYNCOBJ_CREATE_SIGNALED = 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL = 1,
                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT = 2;
// nouveau (DRM_COMMAND_BASE + n)
constexpr uint32_t NV_GETPARAM = 0x00, NV_CHANNEL_ALLOC = 0x02, NV_CHANNEL_FREE = 0x03, NV_NVIF = 0x07,
                   NV_VM_INIT = 0x10, NV_VM_BIND = 0x11, NV_EXEC = 0x12, NV_GEM_NEW = 0x40, NV_GEM_CPU_PREP = 0x42,
                   NV_GEM_CPU_FINI = 0x43, NV_GEM_INFO = 0x44;
constexpr uint32_t NR_GEM_CLOSE = 0x09;
constexpr uint32_t VM_BIND_OP_MAP = 0, VM_BIND_OP_UNMAP = 1, VM_BIND_SPARSE = 1u << 8, VM_BIND_RUN_ASYNC = 1;
constexpr uint32_t SYNC_SYNCOBJ = 0, SYNC_TIMELINE_SYNCOBJ = 1, SYNC_TYPE_MASK = 0xf;
constexpr uint32_t CPU_PREP_NOWAIT = 1, CPU_PREP_WRITE = 4;
constexpr uint64_t kPage = 4096;
constexpr uint64_t GP_PCI_VENDOR = 3, GP_PCI_DEVICE = 4, GP_BUS_TYPE = 5, GP_FB_SIZE = 8, GP_CHIPSET_ID = 11,
                   GP_GRAPH_UNITS = 13, GP_PTIMER_TIME = 14, GP_HAS_BO_USAGE = 15, GP_HAS_PAGEFLIP = 16,
                   GP_EXEC_PUSH_MAX = 17, GP_VRAM_BAR_SIZE = 18, GP_VRAM_USED = 19, GP_HAS_VMA_TILEMODE = 20;
constexpr uint8_t NVIF_SCLASS = 0x01, NVIF_NEW = 0x02, NVIF_DEL = 0x03, NVIF_MTHD = 0x04;
constexpr int32_t NV_DEVICE = 0x80;
constexpr uint8_t NV_DEVICE_V0_INFO = 0x00, NV_DEVICE_V0_TIME = 0x01, NV_DEVICE_INFO_V0_PCIE = 0x03;

struct DrmVersion {   // struct drm_version (x86-64)
    int32_t major, minor, patchlevel, pad;
    uint64_t name_len, name, date_len, date, desc_len, desc;
};
static_assert(sizeof(DrmVersion) == 64);
struct DrmGetCap { uint64_t capability, value; };
struct DrmSyncobjCreate { uint32_t handle, flags; };
struct DrmSyncobjDestroy { uint32_t handle, pad; };
struct DrmSyncobjWait { uint64_t handles; int64_t timeout_nsec; uint32_t count_handles, flags, first_signaled, pad; uint64_t deadline_nsec; };
static_assert(sizeof(DrmSyncobjWait) == 40);
struct DrmSyncobjArray { uint64_t handles; uint32_t count_handles, pad; };
struct NvGetparam { uint64_t param, value; };
struct NvChannelAlloc {
    uint32_t fb_ctxdma_handle, tt_ctxdma_handle;
    int32_t channel; uint32_t pushbuf_domains, notifier_handle;
    struct { uint32_t handle, grclass; } subchan[8];
    uint32_t nr_subchan;
};
static_assert(sizeof(NvChannelAlloc) == 88);
struct NvChannelFree { int32_t channel; };
struct NvVmInit { uint64_t kernel_managed_addr, kernel_managed_size; };
struct NvifIoctl { uint8_t version, type, pad02[4], owner, route; uint64_t token, object; };
static_assert(sizeof(NvifIoctl) == 24);
struct NvifNew { uint8_t version, pad01[6], route; uint64_t token, object; uint32_t handle; int32_t oclass; };
static_assert(sizeof(NvifNew) == 32);
struct NvifSclass { uint8_t version, count, pad02[6]; };
struct NvifOclass { int32_t oclass; int16_t minver, maxver; };
struct NvifMthd { uint8_t version, method, pad02[6]; };
struct NvDeviceInfo {   // nv_device_info_v0
    uint8_t version, platform; uint16_t chipset; uint8_t revision, family, pad06[2];
    uint64_t ram_size, ram_user; char chip[16], name[64];
};
static_assert(sizeof(NvDeviceInfo) == 104);
struct NvDeviceTime { uint8_t version, pad01[7]; uint64_t time; };

// What the kernel publishes once GSP-RM is up (nv_gsp_live -> NvDrmPublish).
struct DeviceInfo {
    bool valid = false;
    uint16_t chipset = 0, pci_device = 0; uint8_t revision = 0;
    uint64_t vram_bytes = 0, vram_user = 0, bar1_bytes = 0;
    uint32_t gpc_count = 0, tpc_count = 0;
    uint32_t classes[8] = {}; uint32_t nclasses = 0;
    char chip[16] = {}, name[64] = {};
};

// User memory and the clock, injected (live: CopyFromUser/CopyToUser; host test: memcpy).
struct UserAccess {
    void* ctx;
    bool (*read)(void* ctx, void* dst, uint64_t src, uint64_t n);
    bool (*write)(void* ctx, uint64_t dst, const void* src, uint64_t n);
    uint64_t (*now_ns)(void* ctx);
    bool (*sleep_tick)(void* ctx);   // block ~1 ms; false if a signal is pending
};

constexpr uint32_t kMaxChannels = 16, kMaxObjects = 8, kMaxSyncobjs = 256;
constexpr uint32_t kMaxExecJobs = 16, kMaxInflight = 64, kMaxPushes = 512;
struct ExecPush { uint64_t va; uint32_t va_len, flags; };
static_assert(sizeof(ExecPush) == 16);
struct ExecJob {
    uint32_t npush = 0, nwaits = 0;
    ExecPush push[kMaxPushes];
    GpuFence* waits[16] = {};
    GpuFence* done = nullptr;
};
struct Channel { bool used = false; int32_t id = 0; uint32_t engine = 0; uint32_t nobj = 0;
                 struct { uint64_t token; uint32_t handle; int32_t oclass; } obj[kMaxObjects] = {};
                 uint32_t hw = 0;           // backend channel
                 bool dead = false;         // RC / fault: every later EXEC fails
                 uint32_t next_seqno = 1;
                 ExecJob* jobs[kMaxExecJobs] = {}; uint32_t job_head = 0, job_count = 0;   // waiting for fences / ring room
                 struct { uint32_t seqno; GpuFence* fence; } inflight[kMaxInflight] = {}; uint32_t in_head = 0, in_count = 0; };

// ---- M5.2 memory: the hardware side is a backend (live: VRAM heap + PMM pages + GMMU writes
// over PRAMIN + MMU invalidate; host test: a fake that records PTEs).
enum class Aper : uint8_t { Vram, Sys };
struct Backend {
    void* ctx;
    bool (*vram_alloc)(void* ctx, uint64_t bytes, uint64_t align, uint64_t* pa);
    void (*vram_free)(void* ctx, uint64_t pa, uint64_t bytes);
    bool (*sys_alloc)(void* ctx, uint64_t bytes, uint64_t* cookie);   // pinned pages; cookie names them
    void (*sys_free)(void* ctx, uint64_t cookie);
    bool (*vas_create)(void* ctx, uint64_t kernel_addr, uint64_t kernel_size, uint32_t* vas);
    void (*vas_destroy)(void* ctx, uint32_t vas);
    // Map [va, va+bytes) onto the BO backing at `offset` (VRAM: pa+offset; Sys: the cookie's pages).
    bool (*vas_map)(void* ctx, uint32_t vas, uint64_t va, Aper aper, uint64_t backing, uint64_t offset,
                    uint64_t bytes, uint8_t kind);
    // Unmap; sparse=true leaves the range sparse (reads 0, writes dropped) instead of invalid.
    bool (*vas_unmap)(void* ctx, uint32_t vas, uint64_t va, uint64_t bytes, bool sparse);
    bool (*vas_flush)(void* ctx, uint32_t vas);   // MMU invalidate after a batch of PTE writes
    // M5.3 channels: a GPFIFO channel in `vas` on the GR (0x01) or CE (0x30) runlist, with the
    // engine objects allocated (NVK binds classes with SET_OBJECT in its pushes).
    bool (*chan_create)(void* ctx, uint32_t vas, uint32_t engine, uint32_t* chan);
    void (*chan_destroy)(void* ctx, uint32_t chan);
    // Queue pushes [(va, dwords, no_prefetch)] then a kernel semaphore release of `seqno`, and
    // ring the doorbell. False when the ring has no room (the job stays queued).
    bool (*chan_submit)(void* ctx, uint32_t chan, const uint64_t* va, const uint32_t* dwords, const uint8_t* no_prefetch,
                        uint32_t n, uint32_t seqno);
    uint32_t (*chan_completed)(void* ctx, uint32_t chan);   // last released seqno
    bool (*chan_error)(void* ctx, uint32_t chan);           // RC / MMU fault on the channel
};

constexpr uint32_t kMaxBos = 2048, kMaxMappings = 2048, kMaxBindJobs = 16, kMaxBindOps = 64, kMaxJobSyncs = 16;
constexpr uint32_t GEM_DOMAIN_CPU = 1, GEM_DOMAIN_VRAM = 2, GEM_DOMAIN_GART = 4, GEM_DOMAIN_MAPPABLE = 8,
                   GEM_DOMAIN_COHERENT = 16, GEM_DOMAIN_NO_SHARE = 32;
struct Bo {
    uint32_t handle = 0;    // 0 = free slot
    uint32_t refs = 0;      // the handle (1) + each mapping
    bool handle_open = false;
    uint32_t domain = 0;    // VRAM or GART (+MAPPABLE/COHERENT/NO_SHARE as requested)
    uint64_t size = 0, backing = 0;   // VRAM pa or sysmem cookie
    uint32_t tile_mode = 0, tile_flags = 0;
    uint64_t map_token = 0;
    Aper aper() const { return (domain & GEM_DOMAIN_VRAM) ? Aper::Vram : Aper::Sys; }
    uint8_t kind() const { return uint8_t((tile_flags >> 8) & 0xff); }
};
struct Mapping { uint64_t va, range, bo_offset; uint32_t bo; bool sparse; };   // bo = handle, 0 for a sparse region
struct BindOp { uint32_t op, flags, handle, pad; uint64_t addr, bo_offset, range; };
static_assert(sizeof(BindOp) == 40);
struct SyncDesc { uint32_t flags, handle; uint64_t timeline_value; };
static_assert(sizeof(SyncDesc) == 16);
struct BindJob {
    bool used = false;
    uint32_t nops = 0, nwaits = 0;
    BindOp ops[kMaxBindOps];
    GpuFence* waits[kMaxJobSyncs];
    GpuFence* done = nullptr;   // installed in the signal syncobjs at submit, completed when the ops ran
};

struct File {
    const DeviceInfo* dev = nullptr;
    const Backend* be = nullptr;
    bool device_obj = false; uint64_t device_token = 0;
    bool vm_init = false; uint64_t vm_kernel_addr = 0, vm_kernel_size = 0; uint32_t vas = 0;
    Channel channels[kMaxChannels];
    int32_t next_channel = 1;
    GpuSyncTable<kMaxSyncobjs> syncs;
    Bo bos[kMaxBos];
    uint32_t next_handle = 1;
    Mapping maps[kMaxMappings];   // sorted by va, non-overlapping
    uint32_t nmaps = 0;
    BindJob jobs[kMaxBindJobs];   // async VM_BIND, run in submission order
    uint32_t job_head = 0, job_count = 0;
    GpuFence* last_exec = nullptr;   // CPU_PREP waits for it (M5.3 sets it)
    void Init(const DeviceInfo* d, const Backend* b = nullptr) { dev = d; be = b; syncs.Init(); }
    void Close();
};

inline bool CopyStr(const UserAccess& u, uint64_t dst, uint64_t& len, const char* s) {
    uint64_t n = 0; while (s[n]) ++n;
    if (dst && len) { const uint64_t c = len < n ? len : n; if (!u.write(u.ctx, dst, s, c)) return false; }
    len = n; return true;
}

inline int64_t Version(File&, const UserAccess& u, uint64_t arg) {
    DrmVersion v{};
    if (!u.read(u.ctx, &v, arg, sizeof(v))) return kEFAULT;
    v.major = 1; v.minor = 4; v.patchlevel = 0;   // NVK needs >= 1.3.1; compression needs 1.4.2 (left off)
    if (!CopyStr(u, v.name, v.name_len, "nouveau") || !CopyStr(u, v.date, v.date_len, "20261126") ||
        !CopyStr(u, v.desc, v.desc_len, "Brook nouveau uAPI on GSP-RM")) return kEFAULT;
    return u.write(u.ctx, arg, &v, sizeof(v)) ? 0 : kEFAULT;
}

inline int64_t GetCap(File&, const UserAccess& u, uint64_t arg) {
    DrmGetCap c{};
    if (!u.read(u.ctx, &c, arg, sizeof(c))) return kEFAULT;
    if (c.capability == DRM_CAP_SYNCOBJ) c.value = 1;
    else if (c.capability == DRM_CAP_SYNCOBJ_TIMELINE) c.value = 0;   // binary only until M5.3
    else return kEINVAL;
    return u.write(u.ctx, arg, &c, sizeof(c)) ? 0 : kEFAULT;
}

inline int64_t SyncobjCreate(File& f, const UserAccess& u, uint64_t arg) {
    DrmSyncobjCreate c{};
    if (!u.read(u.ctx, &c, arg, sizeof(c))) return kEFAULT;
    if (c.flags & ~DRM_SYNCOBJ_CREATE_SIGNALED) return kEINVAL;
    c.handle = f.syncs.Create((c.flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0);
    if (!c.handle) return kENOMEM;
    if (!u.write(u.ctx, arg, &c, sizeof(c))) { f.syncs.Destroy(c.handle); return kEFAULT; }
    return 0;
}
inline int64_t SyncobjDestroy(File& f, const UserAccess& u, uint64_t arg) {
    DrmSyncobjDestroy d{};
    if (!u.read(u.ctx, &d, arg, sizeof(d))) return kEFAULT;
    if (d.pad) return kEINVAL;
    return f.syncs.Destroy(d.handle) ? 0 : kEINVAL;
}
// Binary wait: state of each syncobj's current fence. WAIT_FOR_SUBMIT treats a missing fence as
// "not yet"; without it a missing fence is -EINVAL (Linux semantics). Absolute CLOCK_MONOTONIC.
inline int64_t SyncobjWait(File& f, const UserAccess& u, uint64_t arg) {
    DrmSyncobjWait w{};
    if (!u.read(u.ctx, &w, arg, sizeof(w))) return kEFAULT;
    if (w.flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | 4u)) return kEINVAL;
    if (!w.count_handles || w.count_handles > 64) return kEINVAL;
    uint32_t h[64];
    if (!u.read(u.ctx, h, w.handles, uint64_t(w.count_handles) * 4)) return kEFAULT;
    GpuSync* s[64] = {};
    for (uint32_t i = 0; i < w.count_handles; ++i)
        if (!(s[i] = f.syncs.Get(h[i]))) { for (uint32_t j = 0; j < i; ++j) GpuSyncUnref(s[j]); return kEINVAL; }
    const bool all = w.flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, submit = w.flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
    int64_t rc = 0;
    for (;;) {
        uint32_t done = 0, first = ~0u; bool missing = false, failed = false;
        for (uint32_t i = 0; i < w.count_handles; ++i) {
            GpuFence* fe = GpuSyncAcquire(s[i]);
            if (!fe) { missing = true; continue; }
            const uint32_t st = GpuFenceState(fe);
            GpuFenceUnref(fe);
            if (st) { ++done; if (first == ~0u) first = i; if (st == 2) failed = true; }
        }
        if (missing && !submit) { rc = kEINVAL; break; }
        if ((all && done == w.count_handles) || (!all && done)) { w.first_signaled = first; rc = failed ? -5 : 0; break; }
        if (u.now_ns(u.ctx) >= uint64_t(w.timeout_nsec < 0 ? 0 : w.timeout_nsec)) { rc = kETIME; break; }
        if (!u.sleep_tick(u.ctx)) { rc = -4; break; }   // -EINTR
    }
    for (uint32_t i = 0; i < w.count_handles; ++i) GpuSyncUnref(s[i]);
    if (rc == 0 && !u.write(u.ctx, arg, &w, sizeof(w))) return kEFAULT;
    return rc;
}
// RESET installs nothing (unsignalled, no fence); SIGNAL installs a completed fence.
inline int64_t SyncobjArray(File& f, const UserAccess& u, uint64_t arg, bool signal) {
    DrmSyncobjArray a{};
    if (!u.read(u.ctx, &a, arg, sizeof(a))) return kEFAULT;
    if (a.pad || !a.count_handles || a.count_handles > 64) return kEINVAL;
    uint32_t h[64];
    if (!u.read(u.ctx, h, a.handles, uint64_t(a.count_handles) * 4)) return kEFAULT;
    for (uint32_t i = 0; i < a.count_handles; ++i) {
        GpuSync* s = f.syncs.Get(h[i]);
        if (!s) return kEINVAL;
        GpuFence* fe = signal ? GpuFenceCreate(true) : nullptr;
        if (signal && !fe) { GpuSyncUnref(s); return kENOMEM; }
        const bool ok = GpuSyncReplace(s, fe);
        GpuFenceUnref(fe); GpuSyncUnref(s);
        if (!ok) return kEINVAL;
    }
    return 0;
}

inline int64_t Getparam(File& f, const UserAccess& u, uint64_t arg) {
    NvGetparam g{};
    if (!u.read(u.ctx, &g, arg, sizeof(g))) return kEFAULT;
    const DeviceInfo& d = *f.dev;
    switch (g.param) {
        case GP_PCI_VENDOR: g.value = 0x10de; break;
        case GP_PCI_DEVICE: g.value = d.pci_device; break;
        case GP_BUS_TYPE: g.value = 2; break;   // NV_PCIE
        case GP_FB_SIZE: g.value = d.vram_bytes; break;
        case GP_CHIPSET_ID: g.value = d.chipset; break;
        case GP_GRAPH_UNITS: g.value = (d.gpc_count & 0xff) | (uint64_t(d.tpc_count & 0xffff) << 8); break;
        case GP_PTIMER_TIME: g.value = u.now_ns(u.ctx); break;
        case GP_HAS_BO_USAGE: case GP_HAS_PAGEFLIP: g.value = 1; break;
        case GP_EXEC_PUSH_MAX: g.value = 512; break;   // NOUVEAU_GEM_MAX_PUSH
        case GP_VRAM_BAR_SIZE: g.value = d.bar1_bytes; break;
        case GP_VRAM_USED: g.value = 0; break;
        case GP_HAS_VMA_TILEMODE: g.value = 0; break;
        default: return kEINVAL;
    }
    return u.write(u.ctx, arg, &g, sizeof(g)) ? 0 : kEFAULT;
}

inline int64_t VmInit(File& f, const UserAccess& u, uint64_t arg) {
    NvVmInit v{};
    if (!u.read(u.ctx, &v, arg, sizeof(v))) return kEFAULT;
    if (f.vm_init) return kEBUSY;
    if (!v.kernel_managed_size || (v.kernel_managed_addr & (kPage - 1)) || v.kernel_managed_addr + v.kernel_managed_size > (1ull << 40))
        return kEINVAL;
    if (f.be && !f.be->vas_create(f.be->ctx, v.kernel_managed_addr, v.kernel_managed_size, &f.vas)) return kENOMEM;
    f.vm_init = true; f.vm_kernel_addr = v.kernel_managed_addr; f.vm_kernel_size = v.kernel_managed_size;
    return 0;
}

inline Channel* FindChannel(File& f, int64_t id) {
    for (auto& c : f.channels) if (c.used && c.id == id) return &c;
    return nullptr;
}
inline int64_t ChannelAlloc(File& f, const UserAccess& u, uint64_t arg) {
    NvChannelAlloc a{};
    if (!u.read(u.ctx, &a, arg, sizeof(a))) return kEFAULT;
    if (!f.vm_init) return kEINVAL;   // the new uAPI: channels live in the VM_INIT address space
    const uint32_t engine = a.tt_ctxdma_handle;
    if (engine != 0x01 && engine != 0x30) return kEINVAL;   // GR, CE
    Channel* c = nullptr;
    for (auto& x : f.channels) if (!x.used) { c = &x; break; }
    if (!c) return kENOSPC;
    *c = Channel{}; c->used = true; c->id = f.next_channel++; c->engine = engine;
    if (f.be && !f.be->chan_create(f.be->ctx, f.vas, engine, &c->hw)) { c->used = false; return kENOMEM; }
    a.channel = c->id; a.pushbuf_domains = 0x6;   // VRAM | GART
    a.notifier_handle = 0; a.nr_subchan = 0;
    for (auto& s : a.subchan) s = {0, 0};
    if (!u.write(u.ctx, arg, &a, sizeof(a))) { if (f.be) f.be->chan_destroy(f.be->ctx, c->hw); c->used = false; return kEFAULT; }
    return 0;
}
inline void ChannelTeardown(File& f, Channel& c);   // after the EXEC code
inline int64_t ChannelFree(File& f, const UserAccess& u, uint64_t arg) {
    NvChannelFree r{};
    if (!u.read(u.ctx, &r, arg, sizeof(r))) return kEFAULT;
    Channel* c = FindChannel(f, r.channel);
    if (!c) return kENOENT;
    ChannelTeardown(f, *c);
    return 0;
}

// NVIF: the device object (route NVIF, object 0) and per-channel subchannel objects
// (route 0xff, token = channel id).
inline int64_t Nvif(File& f, const UserAccess& u, uint64_t arg, uint32_t size) {
    if (size < sizeof(NvifIoctl) || size > 512) return kEINVAL;
    alignas(8) uint8_t buf[512];
    if (!u.read(u.ctx, buf, arg, size)) return kEFAULT;
    NvifIoctl io; __builtin_memcpy(&io, buf, sizeof(io));
    uint8_t* p = buf + sizeof(io);
    const uint32_t n = size - uint32_t(sizeof(io));
    const DeviceInfo& d = *f.dev;
    auto done = [&]() -> int64_t { return u.write(u.ctx, arg, buf, size) ? 0 : kEFAULT; };
    if (io.version != 0) return kEINVAL;
    switch (io.type) {
        case NVIF_NEW: {
            if (n < sizeof(NvifNew)) return kEINVAL;
            NvifNew nw; __builtin_memcpy(&nw, p, sizeof(nw));
            if (io.route == 0xff) {   // object on a channel
                Channel* c = FindChannel(f, int64_t(io.token));
                if (!c) return kENOENT;
                bool known = false;
                for (uint32_t i = 0; i < d.nclasses; ++i) known |= d.classes[i] == uint32_t(nw.oclass);
                if (!known) return kEINVAL;
                if (c->nobj >= kMaxObjects) return kENOSPC;
                for (uint32_t i = 0; i < c->nobj; ++i) if (c->obj[i].oclass == nw.oclass) return kEEXIST;
                c->obj[c->nobj++] = {nw.object, nw.handle, nw.oclass};
                return 0;
            }
            if (nw.oclass != NV_DEVICE || io.object != 0) return kEINVAL;
            if (f.device_obj) return kEEXIST;
            f.device_obj = true; f.device_token = nw.object;
            return 0;
        }
        case NVIF_DEL: {
            for (auto& c : f.channels) if (c.used)
                for (uint32_t i = 0; i < c.nobj; ++i) if (c.obj[i].token == io.object) {
                    c.obj[i] = c.obj[--c.nobj]; return 0; }
            if (f.device_obj && io.object == f.device_token) { f.device_obj = false; return 0; }
            return kENOENT;
        }
        case NVIF_MTHD: {
            if (!f.device_obj || io.object != f.device_token || n < sizeof(NvifMthd)) return kEINVAL;
            NvifMthd m; __builtin_memcpy(&m, p, sizeof(m));
            uint8_t* q = p + sizeof(m);
            const uint32_t qn = n - uint32_t(sizeof(m));
            if (m.method == NV_DEVICE_V0_INFO) {
                if (qn < sizeof(NvDeviceInfo)) return kEINVAL;
                NvDeviceInfo info{};
                info.version = 0; info.platform = NV_DEVICE_INFO_V0_PCIE; info.chipset = d.chipset; info.revision = d.revision;
                info.ram_size = d.vram_bytes; info.ram_user = d.vram_user;
                __builtin_memcpy(info.chip, d.chip, sizeof(info.chip)); __builtin_memcpy(info.name, d.name, sizeof(info.name));
                info.family = 0x0d;   // as nouveau reports Turing; NVK does not read it
                __builtin_memcpy(q, &info, sizeof(info));
                return done();
            }
            if (m.method == NV_DEVICE_V0_TIME) {
                if (qn < sizeof(NvDeviceTime)) return kEINVAL;
                NvDeviceTime t{}; t.time = u.now_ns(u.ctx);
                __builtin_memcpy(q, &t, sizeof(t));
                return done();
            }
            return kEINVAL;
        }
        case NVIF_SCLASS: {
            if (io.route != 0xff || n < sizeof(NvifSclass)) return kEINVAL;
            if (!FindChannel(f, int64_t(io.token))) return kENOENT;
            NvifSclass sc; __builtin_memcpy(&sc, p, sizeof(sc));
            const uint32_t room = (n - uint32_t(sizeof(sc))) / sizeof(NvifOclass);
            NvifOclass* list = reinterpret_cast<NvifOclass*>(p + sizeof(sc));
            for (uint32_t i = 0; i < room && i < sc.count; ++i) {
                NvifOclass o{}; if (i < d.nclasses) { o.oclass = int32_t(d.classes[i]); o.minver = -1; o.maxver = -1; }
                __builtin_memcpy(&list[i], &o, sizeof(o));
            }
            sc.count = uint8_t(d.nclasses);
            __builtin_memcpy(p, &sc, sizeof(sc));
            return done();
        }
        default: return kEINVAL;
    }
}


// ---- M5.2: GEM objects -------------------------------------------------------------------
struct GemInfo { uint32_t handle, domain; uint64_t size, offset, map_handle; uint32_t tile_mode, tile_flags; };
static_assert(sizeof(GemInfo) == 40);
struct GemNew { GemInfo info; uint32_t channel_hint, align; };
static_assert(sizeof(GemNew) == 48);
struct GemClose { uint32_t handle, pad; };
struct CpuPrep { uint32_t handle, flags; };
struct VmBind { uint32_t op_count, flags, wait_count, sig_count; uint64_t wait_ptr, sig_ptr, op_ptr; };
static_assert(sizeof(VmBind) == 40);
constexpr uint64_t kMapTokenBase = 1ull << 40;   // mmap offsets: kMapTokenBase + handle << 32 (page aligned, unique)

inline Bo* FindBo(File& f, uint32_t handle) {
    if (!handle || handle > kMaxBos) return nullptr;
    Bo& b = f.bos[handle - 1];
    return b.handle == handle ? &b : nullptr;
}
inline const Bo* FindBoByToken(File& f, uint64_t token) {
    if (token < kMapTokenBase || (token & 0xffffffffull)) return nullptr;
    Bo* b = FindBo(f, uint32_t((token - kMapTokenBase) >> 32));
    return b && b->map_token == token && b->handle_open ? b : nullptr;
}
inline void BoPut(File& f, Bo& b) {
    if (--b.refs) return;
    if (f.be) { if (b.aper() == Aper::Vram) f.be->vram_free(f.be->ctx, b.backing, b.size); else f.be->sys_free(f.be->ctx, b.backing); }
    b = Bo{};
}
inline int64_t GemNewIoctl(File& f, const UserAccess& u, uint64_t arg) {
    GemNew n{};
    if (!u.read(u.ctx, &n, arg, sizeof(n))) return kEFAULT;
    if (!f.be) return kENOTTY;
    const uint32_t d = n.info.domain;
    const bool vram = d & GEM_DOMAIN_VRAM, gart = d & GEM_DOMAIN_GART;
    if (!n.info.size || n.info.size > (8ull << 30) || (vram == gart && !(vram && gart))) return kEINVAL;
    if (d & ~(GEM_DOMAIN_VRAM | GEM_DOMAIN_GART | GEM_DOMAIN_MAPPABLE | GEM_DOMAIN_COHERENT | GEM_DOMAIN_NO_SHARE)) return kEINVAL;
    if (n.align & (n.align - 1)) return kEINVAL;
    // VRAM|GART: VRAM first, GART when VRAM is full (as TTM placement order)
    Bo* slot = nullptr; uint32_t h = 0;
    for (uint32_t i = 0; i < kMaxBos; ++i) { const uint32_t k = (f.next_handle - 1 + i) % kMaxBos; if (!f.bos[k].handle) { slot = &f.bos[k]; h = k + 1; break; } }
    if (!slot) return kENOSPC;
    const uint64_t size = (n.info.size + kPage - 1) & ~(kPage - 1);
    Bo b; b.handle = h; b.refs = 1; b.handle_open = true; b.size = size;
    b.tile_mode = n.info.tile_mode; b.tile_flags = n.info.tile_flags & 0x0000ff00u;
    const uint32_t extra = d & (GEM_DOMAIN_MAPPABLE | GEM_DOMAIN_COHERENT | GEM_DOMAIN_NO_SHARE);
    bool ok = false;
    if (vram) { ok = f.be->vram_alloc(f.be->ctx, size, n.align > 0x10000 ? n.align : 0x10000, &b.backing); if (ok) b.domain = GEM_DOMAIN_VRAM | extra; }
    if (!ok && gart) { ok = f.be->sys_alloc(f.be->ctx, size, &b.backing); if (ok) b.domain = GEM_DOMAIN_GART | extra; }
    if (!ok) return kENOMEM;
    b.map_token = kMapTokenBase + (uint64_t(h) << 32);
    *slot = b; f.next_handle = h % kMaxBos + 1;
    n.info.handle = h; n.info.domain = b.domain; n.info.size = size; n.info.offset = 0; n.info.map_handle = b.map_token;
    n.info.tile_flags = b.tile_flags;
    if (!u.write(u.ctx, arg, &n, sizeof(n))) { slot->handle_open = false; BoPut(f, *slot); return kEFAULT; }
    return 0;
}
inline int64_t GemInfoIoctl(File& f, const UserAccess& u, uint64_t arg) {
    GemInfo i{};
    if (!u.read(u.ctx, &i, arg, sizeof(i))) return kEFAULT;
    Bo* b = FindBo(f, i.handle);
    if (!b || !b->handle_open) return kENOENT;
    i.domain = b->domain; i.size = b->size; i.offset = 0; i.map_handle = b->map_token; i.tile_mode = b->tile_mode; i.tile_flags = b->tile_flags;
    return u.write(u.ctx, arg, &i, sizeof(i)) ? 0 : kEFAULT;
}
inline int64_t GemCloseIoctl(File& f, const UserAccess& u, uint64_t arg) {
    GemClose c{};
    if (!u.read(u.ctx, &c, arg, sizeof(c))) return kEFAULT;
    if (c.pad) return kEINVAL;
    Bo* b = FindBo(f, c.handle);
    if (!b || !b->handle_open) return kEINVAL;
    b->handle_open = false; b->map_token = 0;
    BoPut(f, *b);   // mappings keep the memory until they are unmapped
    return 0;
}
// CPU_PREP: the VM_BIND/EXEC uAPI has no per-BO busy tracking in userspace's view; nouveau
// waits on the BO's reservation, which holds the VM's EXEC fences. Brook waits for the file's
// last EXEC fence (conservative). CPU_FINI is a no-op.
inline int64_t CpuPrepIoctl(File& f, const UserAccess& u, uint64_t arg) {
    CpuPrep c{};
    if (!u.read(u.ctx, &c, arg, sizeof(c))) return kEFAULT;
    if (c.flags & ~(CPU_PREP_NOWAIT | CPU_PREP_WRITE)) return kEINVAL;
    Bo* b = FindBo(f, c.handle);
    if (!b || !b->handle_open) return kENOENT;
    GpuFence* fe = f.last_exec;
    if (!fe) return 0;
    const uint64_t deadline = u.now_ns(u.ctx) + 15000000000ull;   // nouveau: 15 s, then -EBUSY
    for (;;) {
        const uint32_t st = GpuFenceState(fe);
        if (st) return st == 2 ? -5 : 0;
        if (c.flags & CPU_PREP_NOWAIT) return kEBUSY;
        if (u.now_ns(u.ctx) >= deadline) return kEBUSY;
        if (!u.sleep_tick(u.ctx)) return -4;
    }
}

// ---- M5.2: the VA space ---------------------------------------------------------------------
// User range [0, kernel_managed_addr); mappings are 4 KiB aligned. MAP needs free VA or a
// sparse region; UNMAP splits mappings it partly covers. A memory-backed unmap inside a sparse
// region leaves the hole sparse; UNMAP|SPARSE removes a sparse region.
inline bool VaValid(const File& f, uint64_t va, uint64_t range) {
    return range && !((va | range) & (kPage - 1)) && va + range > va && va + range <= f.vm_kernel_addr;
}
// The list is sorted by va; memory-backed mappings may lie inside sparse regions (overlap).
inline bool Overlaps(const Mapping& m, uint64_t va, uint64_t end) { return m.va < end && m.va + m.range > va; }
inline bool InsertMapping(File& f, const Mapping& m) {
    if (f.nmaps >= kMaxMappings) return false;
    uint32_t i = 0; while (i < f.nmaps && f.maps[i].va < m.va) ++i;
    for (uint32_t j = f.nmaps; j > i; --j) f.maps[j] = f.maps[j - 1];
    f.maps[i] = m; ++f.nmaps; return true;
}
inline void EraseMapping(File& f, uint32_t i) { for (uint32_t j = i; j + 1 < f.nmaps; ++j) f.maps[j] = f.maps[j + 1]; --f.nmaps; }
// Structural checks for a whole batch at submit (flags, alignment, handles, BO bounds).
// VA-state checks run per op at apply time (a batch may unmap and remap the same range).
inline int64_t BindCheck(File& f, const BindOp& o) {
    if (o.pad || (o.flags & ~VM_BIND_SPARSE)) return kEINVAL;
    if (!VaValid(f, o.addr, o.range)) return kEINVAL;
    if (o.op == VM_BIND_OP_MAP) {
        const bool sparse = o.flags & VM_BIND_SPARSE;
        if (!sparse) {
            Bo* b = FindBo(f, o.handle);
            if (!b || !b->handle_open) return kENOENT;
            if ((o.bo_offset & (kPage - 1)) || o.bo_offset + o.range > b->size || o.bo_offset + o.range < o.bo_offset) return kEINVAL;
        } else if (o.handle || o.bo_offset) return kEINVAL;
        return 0;
    }
    if (o.op == VM_BIND_OP_UNMAP) return o.handle || o.bo_offset ? kEINVAL : 0;
    return kEINVAL;
}
inline int64_t BindState(const File& f, const BindOp& o) {
    const uint64_t end = o.addr + o.range;
    const bool sparse = o.flags & VM_BIND_SPARSE;
    if (o.op == VM_BIND_OP_MAP) {   // memory maps land in free VA or inside sparse regions; sparse regions in free VA
        for (uint32_t i = 0; i < f.nmaps; ++i) if (Overlaps(f.maps[i], o.addr, end) && (sparse || !f.maps[i].sparse)) return kEINVAL;
        return 0;
    }
    if (sparse)   // removing a sparse region: its memory mappings must go first (their PTEs would be lost)
        for (uint32_t i = 0; i < f.nmaps; ++i) if (!f.maps[i].sparse && Overlaps(f.maps[i], o.addr, end)) return kEBUSY;
    return 0;
}
inline int64_t BindApply(File& f, const BindOp& o) {
    if (int64_t e = BindState(f, o)) return e;
    const uint64_t va = o.addr, end = o.addr + o.range;
    if (o.op == VM_BIND_OP_MAP) {
        if (o.flags & VM_BIND_SPARSE) {
            if (!f.be->vas_unmap(f.be->ctx, f.vas, va, o.range, true)) return kENOMEM;   // sparse PTEs
            return InsertMapping(f, {va, o.range, 0, 0, true}) ? 0 : kENOSPC;
        }
        Bo* b = FindBo(f, o.handle);
        if (!b) return kENOENT;
        if (!f.be->vas_map(f.be->ctx, f.vas, va, b->aper(), b->backing, o.bo_offset, o.range, b->kind())) return kENOMEM;
        ++b->refs;
        return InsertMapping(f, {va, o.range, o.bo_offset, b->handle, false}) ? 0 : kENOSPC;
    }
    // UNMAP: one overlapped mapping of the requested kind at a time (memory-backed, or sparse
    // regions with UNMAP|SPARSE), splitting partial overlaps; restart after each change.
    const bool sparse_op = o.flags & VM_BIND_SPARSE;
    for (;;) {
        uint32_t i = 0;
        while (i < f.nmaps && !(f.maps[i].sparse == sparse_op && Overlaps(f.maps[i], va, end))) ++i;
        if (i == f.nmaps) return 0;
        const Mapping m = f.maps[i];
        const uint64_t a = m.va > va ? m.va : va, z = m.va + m.range < end ? m.va + m.range : end;
        // a memory hole inside a sparse region goes back to sparse; anything else becomes invalid
        bool inside_sparse = false;
        if (!m.sparse)
            for (uint32_t k = 0; k < f.nmaps; ++k)
                if (f.maps[k].sparse && f.maps[k].va <= a && f.maps[k].va + f.maps[k].range >= z) inside_sparse = true;
        if (!f.be->vas_unmap(f.be->ctx, f.vas, a, z - a, inside_sparse)) return kENOMEM;
        EraseMapping(f, i);
        uint32_t pieces = 0;
        if (m.va < a) { Mapping l = m; l.range = a - m.va; if (!InsertMapping(f, l)) return kENOSPC; ++pieces; }
        if (m.va + m.range > z) {
            Mapping r = m; r.va = z; r.range = m.va + m.range - z; if (!m.sparse) r.bo_offset = m.bo_offset + (z - m.va);
            if (!InsertMapping(f, r)) return kENOSPC; ++pieces;
        }
        if (!m.sparse) if (Bo* b = FindBo(f, m.bo)) { b->refs += pieces; BoPut(f, *b); }   // each surviving piece holds a reference
    }
}
// An async job holds a reference on each BO it maps (taken at submit, dropped after it ran).
inline void JobBoRefs(File& f, const BindJob& j, bool take) {
    for (uint32_t i = 0; i < j.nops; ++i)
        if (j.ops[i].op == VM_BIND_OP_MAP && !(j.ops[i].flags & VM_BIND_SPARSE))
            if (Bo* b = FindBo(f, j.ops[i].handle)) { if (take) ++b->refs; else BoPut(f, *b); }
}
inline int64_t RunOps(File& f, const BindOp* ops, uint32_t n) {
    int64_t rc = 0;
    for (uint32_t i = 0; i < n && !rc; ++i) rc = BindApply(f, ops[i]);
    if (!f.be->vas_flush(f.be->ctx, f.vas) && !rc) rc = -5;
    return rc;
}
// Runs queued async binds whose waits have completed, in order. Called on every ioctl and by
// the live glue's poller.
inline void Pump(File& f) {
    while (f.job_count) {
        BindJob& j = f.jobs[f.job_head];
        bool ready = true, failed = false;
        for (uint32_t i = 0; i < j.nwaits; ++i) { const uint32_t st = GpuFenceState(j.waits[i]); ready &= st != 0; failed |= st == 2; }
        if (!ready) return;
        const int64_t rc = failed ? -5 : RunOps(f, j.ops, j.nops);
        JobBoRefs(f, j, false);
        GpuFenceComplete(j.done, rc == 0);
        for (uint32_t i = 0; i < j.nwaits; ++i) GpuFenceUnref(j.waits[i]);
        GpuFenceUnref(j.done);
        j = BindJob{};
        f.job_head = (f.job_head + 1) % kMaxBindJobs; --f.job_count;
    }
}
inline int64_t ReadSyncs(File& f, const UserAccess& u, uint64_t ptr, uint32_t count, SyncDesc* out) {
    if (count > kMaxJobSyncs) return kEINVAL;
    if (count && !u.read(u.ctx, out, ptr, uint64_t(count) * sizeof(SyncDesc))) return kEFAULT;
    for (uint32_t i = 0; i < count; ++i) {
        if (out[i].flags != SYNC_SYNCOBJ) return kEINVAL;   // binary syncobjs only (no timeline cap yet)
        GpuSync* s = f.syncs.Get(out[i].handle);
        if (!s) return kEINVAL;
        GpuSyncUnref(s);
    }
    return 0;
}
inline int64_t VmBindIoctl(File& f, const UserAccess& u, uint64_t arg) {
    VmBind v{};
    if (!u.read(u.ctx, &v, arg, sizeof(v))) return kEFAULT;
    if (!f.vm_init || !f.be) return kEINVAL;
    if ((v.flags & ~VM_BIND_RUN_ASYNC) || !v.op_count || v.op_count > kMaxBindOps) return kEINVAL;
    const bool async = v.flags & VM_BIND_RUN_ASYNC;
    if (!async && (v.wait_count || v.sig_count)) return kEINVAL;   // sync binds take no syncs
    BindOp ops[kMaxBindOps];
    if (!u.read(u.ctx, ops, v.op_ptr, uint64_t(v.op_count) * sizeof(BindOp))) return kEFAULT;
    for (uint32_t i = 0; i < v.op_count; ++i) if (int64_t e = BindCheck(f, ops[i])) return e;
    if (!async) {   // in order: earlier async binds run first
        const uint64_t deadline = u.now_ns(u.ctx) + 15000000000ull;
        for (Pump(f); f.job_count; Pump(f)) {
            if (u.now_ns(u.ctx) >= deadline) return kEBUSY;
            if (!u.sleep_tick(u.ctx)) return -4;
        }
        return RunOps(f, ops, v.op_count);
    }
    SyncDesc waits[kMaxJobSyncs], sigs[kMaxJobSyncs];
    if (int64_t e = ReadSyncs(f, u, v.wait_ptr, v.wait_count, waits)) return e;
    if (int64_t e = ReadSyncs(f, u, v.sig_ptr, v.sig_count, sigs)) return e;
    if (f.job_count == kMaxBindJobs) return kEBUSY;
    BindJob& j = f.jobs[(f.job_head + f.job_count) % kMaxBindJobs];
    j = BindJob{}; j.used = true; j.nops = v.op_count;
    for (uint32_t i = 0; i < v.op_count; ++i) j.ops[i] = ops[i];
    for (uint32_t i = 0; i < v.wait_count; ++i) {
        GpuSync* s = f.syncs.Get(waits[i].handle);
        GpuFence* fe = GpuSyncAcquire(s); GpuSyncUnref(s);
        if (!fe) { for (uint32_t k = 0; k < j.nwaits; ++k) GpuFenceUnref(j.waits[k]); j = BindJob{}; return kEINVAL; }   // nothing submitted yet
        j.waits[j.nwaits++] = fe;
    }
    j.done = GpuFenceCreate(false);
    if (!j.done) { for (uint32_t k = 0; k < j.nwaits; ++k) GpuFenceUnref(j.waits[k]); j = BindJob{}; return kENOMEM; }
    for (uint32_t i = 0; i < v.sig_count; ++i) { GpuSync* s = f.syncs.Get(sigs[i].handle); GpuSyncReplace(s, j.done); GpuSyncUnref(s); }
    JobBoRefs(f, j, true);
    ++f.job_count;
    Pump(f);
    return 0;
}

// ---- M5.3: EXEC ----------------------------------------------------------------------------
struct Exec { uint32_t channel, push_count, wait_count, sig_count; uint64_t wait_ptr, sig_ptr, push_ptr; };
static_assert(sizeof(Exec) == 40);
constexpr uint32_t EXEC_PUSH_NO_PREFETCH = 1;

inline void FreeJob(ExecJob* j, bool ok) {
    for (uint32_t i = 0; i < j->nwaits; ++i) GpuFenceUnref(j->waits[i]);
    if (j->done) { GpuFenceComplete(j->done, ok); GpuFenceUnref(j->done); }
    kfree(j);
}
// Retire what the GPU finished; then submit queued jobs whose waits completed, in order.
inline void PumpChannel(File& f, Channel& c) {
    if (!c.dead && f.be->chan_error(f.be->ctx, c.hw)) c.dead = true;
    const uint32_t done = f.be->chan_completed(f.be->ctx, c.hw);
    while (c.in_count) {
        auto& x = c.inflight[c.in_head];
        if (!c.dead && int32_t(done - x.seqno) < 0) break;
        GpuFenceComplete(x.fence, !c.dead || int32_t(done - x.seqno) >= 0);
        GpuFenceUnref(x.fence); x = {};
        c.in_head = (c.in_head + 1) % kMaxInflight; --c.in_count;
    }
    while (c.job_count) {
        ExecJob* j = c.jobs[c.job_head];
        bool ready = true, failed = c.dead;
        for (uint32_t i = 0; i < j->nwaits; ++i) { const uint32_t st = GpuFenceState(j->waits[i]); ready &= st != 0; failed |= st == 2; }
        if (!ready && !failed) return;
        if (!failed) {
            if (c.in_count == kMaxInflight) return;
            uint64_t va[kMaxPushes]; uint32_t dw[kMaxPushes]; uint8_t np[kMaxPushes];
            for (uint32_t i = 0; i < j->npush; ++i) { va[i] = j->push[i].va; dw[i] = j->push[i].va_len / 4; np[i] = (j->push[i].flags & EXEC_PUSH_NO_PREFETCH) ? 1 : 0; }
            const uint32_t seq = c.next_seqno;
            if (!f.be->chan_submit(f.be->ctx, c.hw, va, dw, np, j->npush, seq)) return;   // ring full: later
            ++c.next_seqno;
            auto& x = c.inflight[(c.in_head + c.in_count) % kMaxInflight];
            x.seqno = seq; x.fence = j->done; j->done = nullptr; ++c.in_count;
        }
        FreeJob(j, false);   // a failed wait or dead channel fails its fence
        c.jobs[c.job_head] = nullptr; c.job_head = (c.job_head + 1) % kMaxExecJobs; --c.job_count;
    }
}
inline void PumpExec(File& f) { for (auto& c : f.channels) if (c.used && (c.job_count || c.in_count)) PumpChannel(f, c); }
inline void ChannelTeardown(File& f, Channel& c) {
    while (c.job_count) { FreeJob(c.jobs[c.job_head], false); c.jobs[c.job_head] = nullptr; c.job_head = (c.job_head + 1) % kMaxExecJobs; --c.job_count; }
    if (f.be) { c.dead = true; PumpChannel(f, c); f.be->chan_destroy(f.be->ctx, c.hw); }   // in-flight work is abandoned with the channel
    c.used = false;
}
inline int64_t ExecIoctl(File& f, const UserAccess& u, uint64_t arg) {
    Exec e{};
    if (!u.read(u.ctx, &e, arg, sizeof(e))) return kEFAULT;
    if (!f.be) return kENOTTY;
    Channel* c = FindChannel(f, e.channel);
    if (!c) return kENOENT;
    if (c->dead) return -19;   // -ENODEV as nouveau after a channel kill
    if (e.push_count > kMaxPushes || e.wait_count > 16 || e.sig_count > 16) return kEINVAL;
    SyncDesc waits[16], sigs[16];
    if (int64_t r = ReadSyncs(f, u, e.wait_ptr, e.wait_count, waits)) return r;
    if (int64_t r = ReadSyncs(f, u, e.sig_ptr, e.sig_count, sigs)) return r;
    PumpChannel(f, *c);
    if (c->job_count == kMaxExecJobs) return kEBUSY;
    auto* j = static_cast<ExecJob*>(kmalloc(sizeof(ExecJob)));
    if (!j) return kENOMEM;
    __builtin_memset(j, 0, sizeof(*j));
    j->npush = e.push_count;
    if (e.push_count && !u.read(u.ctx, j->push, e.push_ptr, uint64_t(e.push_count) * sizeof(ExecPush))) { kfree(j); return kEFAULT; }
    for (uint32_t i = 0; i < j->npush; ++i)
        if (!j->push[i].va_len || (j->push[i].va_len & 3) || (j->push[i].va & 3) || (j->push[i].flags & ~EXEC_PUSH_NO_PREFETCH) ||
            j->push[i].va_len / 4 >= (1u << 21)) { kfree(j); return kEINVAL; }
    for (uint32_t i = 0; i < e.wait_count; ++i) {
        GpuSync* s = f.syncs.Get(waits[i].handle);
        GpuFence* fe = GpuSyncAcquire(s); GpuSyncUnref(s);
        if (!fe) { FreeJob(j, false); return kEINVAL; }
        j->waits[j->nwaits++] = fe;
    }
    j->done = GpuFenceCreate(false);
    if (!j->done) { FreeJob(j, false); return kENOMEM; }
    for (uint32_t i = 0; i < e.sig_count; ++i) { GpuSync* s = f.syncs.Get(sigs[i].handle); GpuSyncReplace(s, j->done); GpuSyncUnref(s); }
    GpuFenceRef(j->done); GpuFenceUnref(f.last_exec); f.last_exec = j->done;
    c->jobs[(c->job_head + c->job_count) % kMaxExecJobs] = j; ++c->job_count;
    PumpChannel(f, *c);
    return 0;
}
inline void File::Close() {
    while (job_count) {   // drop queued binds (their fences fail)
        BindJob& j = jobs[job_head];
        if (be) JobBoRefs(*this, j, false);
        GpuFenceComplete(j.done, false); GpuFenceUnref(j.done);
        for (uint32_t i = 0; i < j.nwaits; ++i) GpuFenceUnref(j.waits[i]);
        j = BindJob{}; job_head = (job_head + 1) % kMaxBindJobs; --job_count;
    }
    for (auto& c : channels) if (c.used) ChannelTeardown(*this, c);
    if (be) {
        for (uint32_t i = 0; i < nmaps; ++i) if (!maps[i].sparse) { Bo* b = FindBo(*this, maps[i].bo); if (b) BoPut(*this, *b); }
        nmaps = 0;
        for (auto& b : bos) if (b.handle && b.handle_open) { b.handle_open = false; BoPut(*this, b); }
        if (vm_init) be->vas_destroy(be->ctx, vas);
    }
    GpuFenceUnref(last_exec); last_exec = nullptr;
    syncs.CloseAll();
}

inline int64_t Ioctl(File& f, const UserAccess& u, uint64_t cmd, uint64_t arg) {
    if (IocType(cmd) != 'd' || !f.dev || !f.dev->valid) return kENOTTY;
    const uint32_t nr = IocNr(cmd), size = IocSize(cmd);
    auto sized = [&](uint32_t want) { return size == want; };
    if (f.be && f.job_count) Pump(f);
    if (f.be) PumpExec(f);
    switch (nr) {
        case NR_VERSION: return sized(sizeof(DrmVersion)) ? Version(f, u, arg) : kEINVAL;
        case NR_GET_CAP: return sized(sizeof(DrmGetCap)) ? GetCap(f, u, arg) : kEINVAL;
        case NR_SYNCOBJ_CREATE: return sized(sizeof(DrmSyncobjCreate)) ? SyncobjCreate(f, u, arg) : kEINVAL;
        case NR_SYNCOBJ_DESTROY: return sized(sizeof(DrmSyncobjDestroy)) ? SyncobjDestroy(f, u, arg) : kEINVAL;
        case NR_SYNCOBJ_WAIT: return sized(sizeof(DrmSyncobjWait)) ? SyncobjWait(f, u, arg) : kEINVAL;
        case NR_SYNCOBJ_RESET: return sized(sizeof(DrmSyncobjArray)) ? SyncobjArray(f, u, arg, false) : kEINVAL;
        case NR_SYNCOBJ_SIGNAL: return sized(sizeof(DrmSyncobjArray)) ? SyncobjArray(f, u, arg, true) : kEINVAL;
        case kDrmCommandBase + NV_GETPARAM: return sized(sizeof(NvGetparam)) ? Getparam(f, u, arg) : kEINVAL;
        case kDrmCommandBase + NV_VM_INIT: return sized(sizeof(NvVmInit)) ? VmInit(f, u, arg) : kEINVAL;
        case kDrmCommandBase + NV_CHANNEL_ALLOC: return sized(sizeof(NvChannelAlloc)) ? ChannelAlloc(f, u, arg) : kEINVAL;
        case kDrmCommandBase + NV_CHANNEL_FREE: return sized(sizeof(NvChannelFree)) ? ChannelFree(f, u, arg) : kEINVAL;
        case kDrmCommandBase + NV_NVIF: return Nvif(f, u, arg, size);
        case NR_GEM_CLOSE: return sized(sizeof(GemClose)) ? GemCloseIoctl(f, u, arg) : kEINVAL;
        case kDrmCommandBase + NV_GEM_NEW: return sized(sizeof(GemNew)) ? GemNewIoctl(f, u, arg) : kEINVAL;
        case kDrmCommandBase + NV_GEM_INFO: return sized(sizeof(GemInfo)) ? GemInfoIoctl(f, u, arg) : kEINVAL;
        case kDrmCommandBase + NV_GEM_CPU_PREP: return sized(sizeof(CpuPrep)) ? CpuPrepIoctl(f, u, arg) : kEINVAL;
        case kDrmCommandBase + NV_GEM_CPU_FINI: return sized(sizeof(CpuPrep)) ? 0 : kEINVAL;
        case kDrmCommandBase + NV_VM_BIND: return sized(sizeof(VmBind)) ? VmBindIoctl(f, u, arg) : kEINVAL;
        case kDrmCommandBase + NV_EXEC: return sized(sizeof(Exec)) ? ExecIoctl(f, u, arg) : kEINVAL;
        default: return kENOTTY;
    }
}

}  // namespace brook::nvdrm
