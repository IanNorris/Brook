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
                   NV_VM_INIT = 0x10;
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
struct Channel { bool used = false; int32_t id = 0; uint32_t engine = 0; uint32_t nobj = 0;
                 struct { uint64_t token; uint32_t handle; int32_t oclass; } obj[kMaxObjects] = {}; };

struct File {
    const DeviceInfo* dev = nullptr;
    bool device_obj = false; uint64_t device_token = 0;
    bool vm_init = false; uint64_t vm_kernel_addr = 0, vm_kernel_size = 0;
    Channel channels[kMaxChannels];
    int32_t next_channel = 1;
    GpuSyncTable<kMaxSyncobjs> syncs;
    void Init(const DeviceInfo* d) { dev = d; syncs.Init(); }
    void Close() { syncs.CloseAll(); }
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
    if (!v.kernel_managed_size || v.kernel_managed_addr + v.kernel_managed_size > (1ull << 40)) return kEINVAL;
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
    a.channel = c->id; a.pushbuf_domains = 0x6;   // VRAM | GART
    a.notifier_handle = 0; a.nr_subchan = 0;
    for (auto& s : a.subchan) s = {0, 0};
    if (!u.write(u.ctx, arg, &a, sizeof(a))) { c->used = false; return kEFAULT; }
    return 0;
}
inline int64_t ChannelFree(File& f, const UserAccess& u, uint64_t arg) {
    NvChannelFree r{};
    if (!u.read(u.ctx, &r, arg, sizeof(r))) return kEFAULT;
    Channel* c = FindChannel(f, r.channel);
    if (!c) return kENOENT;
    c->used = false;
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

inline int64_t Ioctl(File& f, const UserAccess& u, uint64_t cmd, uint64_t arg) {
    if (IocType(cmd) != 'd' || !f.dev || !f.dev->valid) return kENOTTY;
    const uint32_t nr = IocNr(cmd), size = IocSize(cmd);
    auto sized = [&](uint32_t want) { return size == want; };
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
        default: return kENOTTY;
    }
}

}  // namespace brook::nvdrm
