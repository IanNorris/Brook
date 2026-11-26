// M5.1: the nouveau uAPI core (nv_drm_core.h) driven through the sequence Mesa NVK 26.0.6 runs
// when it creates a physical device (winsys/nouveau_device.c, nouveau_context.c,
// vulkan/runtime/vk_drm_syncobj.c), with user pointers as host addresses.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "nv_drm_core.h"
#include "nv_vram_heap.h"
#include <vector>
#include <map>

#include <cstdlib>
#include <thread>

// Host stubs for the kernel heap and mutex (as test_intel_bdw_buffer).
extern "C" void* kmalloc(uint64_t n) { return std::malloc(n); }
extern "C" void kfree(void* p) { std::free(p); }
namespace brook {
void KMutexInit(KMutex* m) { *m = {}; }
void KMutexLock(KMutex* m) { uint32_t e = 0; while (!__atomic_compare_exchange_n(&m->locked, &e, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { e = 0; std::this_thread::yield(); } }
void KMutexUnlock(KMutex* m) { __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE); }
}

using namespace brook;
using namespace brook::nvdrm;

static uint64_t g_now = 1000;
static const UserAccess U{nullptr,
    [](void*, void* d, uint64_t s, uint64_t n) { if (!s) return false; memcpy(d, reinterpret_cast<void*>(s), n); return true; },
    [](void*, uint64_t d, const void* s, uint64_t n) { if (!d) return false; memcpy(reinterpret_cast<void*>(d), s, n); return true; },
    [](void*) { return g_now; },
    [](void*) { g_now += 1000000; return true; }};
static uint64_t Cmd(uint32_t nr, uint32_t size) { return (3ull << 30) | (uint64_t(size) << 16) | ('d' << 8) | nr; }
template <class T> static int64_t Io(File& f, uint32_t nr, T& t) { return Ioctl(f, U, Cmd(nr, sizeof(T)), uint64_t(&t)); }
static int64_t NvIo(File& f, uint32_t nr, void* p, uint32_t n) { return Ioctl(f, U, Cmd(kDrmCommandBase + nr, n), uint64_t(p)); }


static void TestVramHeap() {
    static VramHeap h;
    const uint64_t G = VramHeap::kGranule, lo = 0x140000000ull, hi = 0x140000000ull + 4096 * G;   // 256 MiB
    h.Init(lo + 1, hi + 5);   // rounds inwards
    assert(h.Lo() == lo + G && h.Hi() == hi && h.Free() == 4095 * G && h.Extents() == 1);
    uint64_t a, b, c, big;
    assert(h.Alloc(1, 0, &a) && a == lo + G && h.Used() == G);                 // rounds to a granule
    assert(h.Alloc(2 * G, 0, &b) && b == lo + 2 * G);
    assert(h.Alloc(1 << 20, 2 << 20, &big) && (big & ((2 << 20) - 1)) == 0);  // 2 MiB aligned: splits a hole
    assert(h.Extents() == 2);
    assert(h.Alloc(G, 0, &c) && c == lo + 4 * G);                             // first fit takes the hole first
    assert(h.Release(b, 2 * G) && !h.Release(b, 2 * G));                       // double free refused
    assert(h.Release(a, G) && h.Release(c, G) && h.Release(big, 1 << 20));
    assert(h.Used() == 0 && h.Extents() == 1 && h.Free() == 4095 * G);        // fully coalesced
    assert(!h.Alloc(4096 * G, 0, &a) && h.Alloc(4095 * G, 0, &a) && h.Free() == 0 && !h.Alloc(G, 0, &b));
    assert(h.Release(a, 4095 * G));
    // randomised against a granule bitmap
    std::vector<uint8_t> used(4096, 0);
    struct A { uint64_t base, bytes; };
    std::vector<A> live;
    uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1103515245u + 12345u; return seed >> 8; };
    for (int it = 0; it < 20000; ++it) {
        if (live.empty() || rnd() % 3) {
            const uint64_t bytes = (1 + rnd() % 64) * G, align = (rnd() % 4 == 0) ? (2 << 20) : G;
            uint64_t base;
            if (!h.Alloc(bytes, align, &base)) continue;
            assert((base & (align - 1)) == 0 && base >= h.Lo() && base + bytes <= h.Hi());
            for (uint64_t g = (base - lo) / G; g < (base + bytes - lo) / G; ++g) { assert(!used[g]); used[g] = 1; }
            live.push_back({base, bytes});
        } else {
            const size_t k = rnd() % live.size();
            const A x = live[k]; live[k] = live.back(); live.pop_back();
            assert(h.Release(x.base, x.bytes));
            for (uint64_t g = (x.base - lo) / G; g < (x.base + x.bytes - lo) / G; ++g) used[g] = 0;
        }
        uint64_t sum = 0; for (auto& x : live) sum += x.bytes;
        assert(h.Used() == sum);
    }
    for (auto& x : live) assert(h.Release(x.base, x.bytes));
    assert(h.Used() == 0 && h.Extents() == 1);
}

// ---- M5.2 against a fake backend that records PTEs --------------------------------------
struct Pte { Aper aper; uint64_t pa; uint8_t kind; bool sparse; };
struct Fake {
    VramHeap vram;
    std::map<uint64_t, uint64_t> sys;   // cookie -> bytes
    uint64_t next_cookie = 0x1000;
    std::map<uint32_t, std::map<uint64_t, Pte>> vas;   // vas -> page va -> pte
    uint32_t next_vas = 1, flushes = 0, destroyed = 0;
    uint64_t vram_used() const { return vram.Used(); }
};
static Fake g_fake;
static const Backend kFakeBe{&g_fake,
    [](void* c, uint64_t bytes, uint64_t align, uint64_t* pa) { return static_cast<Fake*>(c)->vram.Alloc(bytes, align, pa); },
    [](void* c, uint64_t pa, uint64_t bytes) { assert(static_cast<Fake*>(c)->vram.Release(pa, bytes)); },
    [](void* c, uint64_t bytes, uint64_t* cookie) { auto* f = static_cast<Fake*>(c); *cookie = f->next_cookie; f->next_cookie += 0x1000; f->sys[*cookie] = bytes; return true; },
    [](void* c, uint64_t cookie) { assert(static_cast<Fake*>(c)->sys.erase(cookie) == 1); },
    [](void* c, uint64_t, uint64_t, uint32_t* v) { auto* f = static_cast<Fake*>(c); *v = f->next_vas++; f->vas[*v]; return true; },
    [](void* c, uint32_t v) { auto* f = static_cast<Fake*>(c); f->vas.erase(v); ++f->destroyed; },
    [](void* c, uint32_t v, uint64_t va, Aper a, uint64_t backing, uint64_t off, uint64_t bytes, uint8_t kind) {
        auto& t = static_cast<Fake*>(c)->vas[v];
        for (uint64_t o = 0; o < bytes; o += kPage) t[va + o] = Pte{a, a == Aper::Vram ? backing + off + o : (backing << 20) + off + o, kind, false};
        return true; },
    [](void* c, uint32_t v, uint64_t va, uint64_t bytes, bool sparse) {
        auto& t = static_cast<Fake*>(c)->vas[v];
        for (uint64_t o = 0; o < bytes; o += kPage) { if (sparse) t[va + o] = Pte{Aper::Vram, 0, 0, true}; else t.erase(va + o); }
        return true; },
    [](void* c, uint32_t) { ++static_cast<Fake*>(c)->flushes; return true; }};

static uint32_t NewBo(File& f, uint64_t size, uint32_t domain, uint8_t kind = 0, GemNew* out = nullptr) {
    GemNew n{}; n.info.size = size; n.info.domain = domain; n.info.tile_flags = uint32_t(kind) << 8;
    const int64_t r = NvIo(f, NV_GEM_NEW, &n, sizeof(n));
    if (out) *out = n;
    return r == 0 ? n.info.handle : 0;
}
static int64_t Bind(File& f, std::vector<BindOp> ops, uint32_t flags = 0, std::vector<SyncDesc> waits = {}, std::vector<SyncDesc> sigs = {}) {
    VmBind v{}; v.op_count = uint32_t(ops.size()); v.flags = flags; v.op_ptr = uint64_t(ops.data());
    v.wait_count = uint32_t(waits.size()); v.wait_ptr = uint64_t(waits.data()); v.sig_count = uint32_t(sigs.size()); v.sig_ptr = uint64_t(sigs.data());
    return NvIo(f, NV_VM_BIND, &v, sizeof(v));
}
static BindOp Map(uint32_t h, uint64_t va, uint64_t range, uint64_t off = 0) { return BindOp{VM_BIND_OP_MAP, 0, h, 0, va, off, range}; }
static BindOp Unmap(uint64_t va, uint64_t range, bool sparse = false) { return BindOp{VM_BIND_OP_UNMAP, sparse ? VM_BIND_SPARSE : 0u, 0, 0, va, 0, range}; }
static BindOp Sparse(uint64_t va, uint64_t range) { return BindOp{VM_BIND_OP_MAP, VM_BIND_SPARSE, 0, 0, va, 0, range}; }

static void TestMemory(const DeviceInfo& dev) {
    g_fake.vram.Init(0x140000000ull, 0x140000000ull + (8ull << 20));   // 8 MiB of VRAM for the test
    static File f; f.Init(&dev, &kFakeBe);
    GemNew none{}; none.info.size = 4096; none.info.domain = GEM_DOMAIN_VRAM;
    assert(NvIo(f, NV_GEM_NEW, &none, sizeof(none)) == 0);   // GEM works before VM_INIT
    { GemClose c{none.info.handle, 0}; assert(Io(f, NR_GEM_CLOSE, c) == 0 && g_fake.vram_used() == 0); }
    NvVmInit vm{0xff00000000ull, (1ull << 40) - 0xff00000000ull}; assert(NvIo(f, NV_VM_INIT, &vm, sizeof(vm)) == 0 && f.vas == 1);
    // GEM: VRAM with a kind; GART; VRAM|GART falls back to GART when VRAM is full
    GemNew a{}; const uint32_t ha = NewBo(f, (1 << 20) + 5, GEM_DOMAIN_VRAM | GEM_DOMAIN_MAPPABLE, 0x06, &a);
    assert(ha && a.info.domain == (GEM_DOMAIN_VRAM | GEM_DOMAIN_MAPPABLE) && a.info.size == (1 << 20) + 4096 && a.info.map_handle);
    GemInfo gi{}; gi.handle = ha; assert(NvIo(f, NV_GEM_INFO, &gi, sizeof(gi)) == 0 && gi.size == a.info.size && ((gi.tile_flags >> 8) & 0xff) == 0x06);
    assert(FindBoByToken(f, gi.map_handle) && FindBoByToken(f, gi.map_handle)->handle == ha && !FindBoByToken(f, gi.map_handle + 4096));
    const uint32_t hg = NewBo(f, 64 << 10, GEM_DOMAIN_GART | GEM_DOMAIN_COHERENT); assert(hg && g_fake.sys.size() == 1);
    const uint32_t hbig = NewBo(f, 16 << 20, GEM_DOMAIN_VRAM | GEM_DOMAIN_GART); GemInfo bi{}; bi.handle = hbig;
    assert(hbig && NvIo(f, NV_GEM_INFO, &bi, sizeof(bi)) == 0 && (bi.domain & GEM_DOMAIN_GART) && g_fake.sys.size() == 2);
    assert(!NewBo(f, 16 << 20, GEM_DOMAIN_VRAM));   // no fallback without GART
    assert(!NewBo(f, 4096, 0) && !NewBo(f, 4096, GEM_DOMAIN_VRAM | 0x100));
    auto& pt = g_fake.vas[f.vas];
    // sync MAP: PTEs with the BO's kind; flush once per batch
    const uint32_t fl0 = g_fake.flushes;
    assert(Bind(f, {Map(ha, 0x100000, 1 << 20)}) == 0 && g_fake.flushes == fl0 + 1 && pt.size() == 256);
    assert(pt[0x100000].aper == Aper::Vram && pt[0x100000].kind == 0x06 && pt[0x1ff000].pa == pt[0x100000].pa + 0xff000);
    assert(Bind(f, {Map(hg, 0x100000 + (512 << 10), 64 << 10)}) == kEINVAL);    // VA busy
    assert(Bind(f, {Map(ha, 0x400000, 4096, (1 << 20) + 4096)}) == kEINVAL);     // past the BO
    assert(Bind(f, {Map(ha, 0xff00000000ull - 4096, 8192)}) == kEINVAL);         // into the kernel-managed range
    assert(Bind(f, {Map(ha, 0x400001, 4096)}) == kEINVAL);
    // GEM_CLOSE while mapped keeps the memory; unmapping frees it
    const uint64_t used = g_fake.vram_used();
    { GemClose c{ha, 0}; assert(Io(f, NR_GEM_CLOSE, c) == 0 && g_fake.vram_used() == used); }
    assert(Bind(f, {Map(ha, 0x800000, 4096)}) == kENOENT);                       // closed handle
    // split: unmap the middle, the edges stay (each with a reference), then the rest frees the BO
    assert(Bind(f, {Unmap(0x140000, 0x40000)}) == 0 && pt.size() == 192 && !pt.count(0x140000) && pt.count(0x13f000) && pt.count(0x180000));
    assert(pt[0x180000].pa - pt[0x100000].pa == 0x80000 && f.nmaps == 2);
    assert(Bind(f, {Unmap(0x100000, 0x40000)}) == 0 && g_fake.vram_used() == used);
    assert(Bind(f, {Unmap(0x180000, 0x80000)}) == 0 && pt.empty() && g_fake.vram_used() == used - (1 << 20) - VramHeap::kGranule);
    // sparse region: memory inside it, holes go back to sparse, removal needs the memory gone
    assert(Bind(f, {Sparse(0x10000000, 16 * kPage)}) == 0 && pt.size() == 16 && pt[0x10000000].sparse);
    assert(Bind(f, {Map(hg, 0x10002000, 2 * kPage)}) == 0 && !pt[0x10002000].sparse && pt[0x10002000].aper == Aper::Sys);
    assert(Bind(f, {Unmap(0x10000000, 16 * kPage, true)}) == kEBUSY);
    assert(Bind(f, {Unmap(0x10002000, 2 * kPage)}) == 0 && pt[0x10002000].sparse && pt.size() == 16);
    assert(Bind(f, {Sparse(0x10004000, kPage)}) == kEINVAL);                    // sparse over sparse
    assert(Bind(f, {Unmap(0x10000000, 16 * kPage, true)}) == 0 && pt.empty() && f.nmaps == 0);
    // one batch may unmap and remap the same range (state is checked per op)
    assert(Bind(f, {Map(hg, 0x20000000, kPage), Unmap(0x20000000, kPage), Map(hg, 0x20000000, kPage, kPage)}) == 0);
    assert(pt[0x20000000].pa == (g_fake.sys.begin()->first << 20) + kPage);
    // sync binds take no syncs; async binds wait for their fences, then signal
    DrmSyncobjCreate sa{0, 0}, sb{0, 0}; assert(Io(f, NR_SYNCOBJ_CREATE, sa) == 0 && Io(f, NR_SYNCOBJ_CREATE, sb) == 0);
    assert(Bind(f, {Unmap(0x20000000, kPage)}, 0, {}, {{SYNC_SYNCOBJ, sb.handle, 0}}) == kEINVAL);
    assert(Bind(f, {Unmap(0x20000000, kPage)}, VM_BIND_RUN_ASYNC, {{SYNC_SYNCOBJ, sa.handle, 0}}) == kEINVAL);   // nothing submitted on A
    GpuFence* gate = GpuFenceCreate(false);
    { GpuSync* s = f.syncs.Get(sa.handle); assert(GpuSyncReplace(s, gate)); GpuSyncUnref(s); }
    assert(Bind(f, {Unmap(0x20000000, kPage), Map(hbig, 0x30000000, 2 * kPage)}, VM_BIND_RUN_ASYNC,
                {{SYNC_SYNCOBJ, sa.handle, 0}}, {{SYNC_SYNCOBJ, sb.handle, 0}}) == 0);
    assert(f.job_count == 1 && pt.count(0x20000000) && !pt.count(0x30000000));
    uint32_t hb = sb.handle; DrmSyncobjWait w{uint64_t(&hb), 0, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, 0, 0};
    assert(Io(f, NR_SYNCOBJ_WAIT, w) == kETIME);                                  // B: the job's fence, still pending
    { GemClose c{hbig, 0}; assert(Io(f, NR_GEM_CLOSE, c) == 0); }                // the queued job keeps the BO alive
    assert(GpuFenceComplete(gate, true));
    NvGetparam g{GP_PCI_DEVICE, 0}; assert(NvIo(f, NV_GETPARAM, &g, sizeof(g)) == 0);   // any ioctl pumps the queue
    assert(f.job_count == 0 && !pt.count(0x20000000) && pt.count(0x30000000) && pt[0x30000000].aper == Aper::Sys);
    assert(Io(f, NR_SYNCOBJ_WAIT, w) == 0);
    GpuFenceUnref(gate);
    // CPU_PREP: nothing submitted -> idle; a pending EXEC fence -> NOWAIT busy
    CpuPrep cp{hg, 0}; assert(NvIo(f, NV_GEM_CPU_PREP, &cp, sizeof(cp)) == 0);
    f.last_exec = GpuFenceCreate(false);
    cp.flags = CPU_PREP_NOWAIT; assert(NvIo(f, NV_GEM_CPU_PREP, &cp, sizeof(cp)) == kEBUSY);
    GpuFenceComplete(f.last_exec, true); assert(NvIo(f, NV_GEM_CPU_PREP, &cp, sizeof(cp)) == 0);
    // close frees VRAM, sysmem and the VAS
    f.Close();
    assert(g_fake.vram_used() == 0 && g_fake.sys.empty() && g_fake.destroyed == 1 && !g_fake.vas.count(1));
}

int main() {
    TestVramHeap();
    DeviceInfo dev;
    dev.valid = true; dev.chipset = 0x164; dev.revision = 0xa1; dev.pci_device = 0x1e87;
    dev.vram_bytes = 8ull << 30; dev.vram_user = 7ull << 30; dev.bar1_bytes = 256ull << 20; dev.gpc_count = 6; dev.tpc_count = 46;
    const uint32_t cls[] = {0xc597, 0xc5c0, 0xc5b5, 0x902d, 0xa140};
    for (uint32_t i = 0; i < 5; ++i) dev.classes[i] = cls[i];
    dev.nclasses = 5;
    strcpy(dev.chip, "TU104"); strcpy(dev.name, "NVIDIA GeForce RTX 2080");
    static File f; f.Init(&dev);

    {   // drmGetVersion: >= 1.3.1, name "nouveau"
        char name[16] = {}; DrmVersion v{}; v.name = uint64_t(name); v.name_len = sizeof(name);
        assert(Io(f, NR_VERSION, v) == 0 && v.major == 1 && v.minor == 4 && !strcmp(name, "nouveau") && v.name_len == 7);
        const uint32_t version = uint32_t(v.major) << 24 | uint32_t(v.minor) << 8 | uint32_t(v.patchlevel);
        assert(version >= 0x01000301 && version < 0x01000402);   // VM_BIND yes, compression no
    }
    {   // vk_drm_syncobj_get_type: create signalled, wait(0), destroy; timeline cap off
        DrmGetCap c{DRM_CAP_SYNCOBJ, 0}; assert(Io(f, NR_GET_CAP, c) == 0 && c.value == 1);
        DrmGetCap t{DRM_CAP_SYNCOBJ_TIMELINE, 9}; assert(Io(f, NR_GET_CAP, t) == 0 && t.value == 0);
        DrmSyncobjCreate s{0, DRM_SYNCOBJ_CREATE_SIGNALED}; assert(Io(f, NR_SYNCOBJ_CREATE, s) == 0 && s.handle);
        uint32_t h = s.handle; DrmSyncobjWait w{uint64_t(&h), 0, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, 0, 0};
        assert(Io(f, NR_SYNCOBJ_WAIT, w) == 0 && w.first_signaled == 0);
        DrmSyncobjArray r{uint64_t(&h), 1, 0}; assert(Io(f, NR_SYNCOBJ_RESET, r) == 0);
        w.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL; assert(Io(f, NR_SYNCOBJ_WAIT, w) == kEINVAL);   // no fence, no WAIT_FOR_SUBMIT
        w.flags |= DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT; w.timeout_nsec = int64_t(g_now + 3000000);
        assert(Io(f, NR_SYNCOBJ_WAIT, w) == kETIME);
        assert(Io(f, NR_SYNCOBJ_SIGNAL, r) == 0 && Io(f, NR_SYNCOBJ_WAIT, w) == 0);
        DrmSyncobjDestroy d{h, 0}; assert(Io(f, NR_SYNCOBJ_DESTROY, d) == 0 && Io(f, NR_SYNCOBJ_DESTROY, d) == kEINVAL);
    }
    {   // nouveau_ws_device_new order: VM_INIT, NVIF device NEW, GETPARAMs, DEVICE_INFO
        NvVmInit vm{0xff00000000ull, (1ull << 40) - 0xff00000000ull};
        assert(NvIo(f, NV_VM_INIT, &vm, sizeof(vm)) == 0 && NvIo(f, NV_VM_INIT, &vm, sizeof(vm)) == kEBUSY);
        struct { NvifIoctl io; NvifNew nw; uint8_t dev[16]; } an{};
        an.io.type = NVIF_NEW; an.io.owner = 0xff; an.nw.oclass = NV_DEVICE; an.nw.object = 0x1234; an.nw.token = 0x1234;
        assert(NvIo(f, NV_NVIF, &an, sizeof(an)) == 0 && f.device_obj);
        NvGetparam g{GP_PCI_DEVICE, 0}; assert(NvIo(f, NV_GETPARAM, &g, sizeof(g)) == 0 && g.value == 0x1e87);
        g = {GP_GRAPH_UNITS, 0}; assert(NvIo(f, NV_GETPARAM, &g, sizeof(g)) == 0 && (g.value & 0xff) == 6 && ((g.value >> 8) & 0xffff) == 46);
        g = {GP_EXEC_PUSH_MAX, 0}; assert(NvIo(f, NV_GETPARAM, &g, sizeof(g)) == 0 && g.value == 512);
        g = {GP_VRAM_BAR_SIZE, 0}; assert(NvIo(f, NV_GETPARAM, &g, sizeof(g)) == 0 && g.value == (256ull << 20));
        g = {GP_HAS_VMA_TILEMODE, 7}; assert(NvIo(f, NV_GETPARAM, &g, sizeof(g)) == 0 && g.value == 0);
        g = {99, 0}; assert(NvIo(f, NV_GETPARAM, &g, sizeof(g)) == kEINVAL);
        struct { NvifIoctl io; NvifMthd m; NvDeviceInfo info; } mi{};
        mi.io.type = NVIF_MTHD; mi.io.owner = 0xff; mi.io.object = 0x1234; mi.m.method = NV_DEVICE_V0_INFO;
        assert(NvIo(f, NV_NVIF, &mi, sizeof(mi)) == 0);
        assert(mi.info.chipset == 0x164 && mi.info.platform == NV_DEVICE_INFO_V0_PCIE && mi.info.ram_user == (7ull << 30) &&
               !strcmp(mi.info.chip, "TU104"));
        mi.io.object = 0x999; assert(NvIo(f, NV_NVIF, &mi, sizeof(mi)) == kEINVAL);   // not the device object
    }
    {   // nouveau_ws_context_create(~0): CHANNEL_ALLOC (GR), SCLASS, NEW per engine class, DEL, FREE
        NvChannelAlloc ca{}; ca.fb_ctxdma_handle = 0xffffffff; ca.tt_ctxdma_handle = 0x01;
        assert(NvIo(f, NV_CHANNEL_ALLOC, &ca, sizeof(ca)) == 0 && ca.channel > 0);
        struct { NvifIoctl io; NvifSclass sc; NvifOclass list[16]; } sc{};
        sc.io.type = NVIF_SCLASS; sc.io.route = 0xff; sc.io.token = uint64_t(ca.channel); sc.sc.count = 16;
        assert(NvIo(f, NV_NVIF, &sc, sizeof(sc)) == 0 && sc.sc.count == 5);
        auto find = [&](uint8_t type) { uint32_t r = 0; for (auto& o : sc.list) if ((uint32_t(o.oclass) & 0xff) == type && uint32_t(o.oclass) > r) r = uint32_t(o.oclass); return r; };
        assert(find(0xb5) == 0xc5b5 && find(0x2d) == 0x902d && find(0x97) == 0xc597 && find(0x40) == 0xa140 && find(0xc0) == 0xc5c0);
        uint64_t objs[5] = {11, 12, 13, 14, 15}; const uint8_t types[5] = {0xb5, 0x2d, 0x97, 0x40, 0xc0};
        for (int i = 0; i < 5; ++i) {
            struct { NvifIoctl io; NvifNew nw; } nw{};
            nw.io.type = NVIF_NEW; nw.io.route = 0xff; nw.io.token = uint64_t(ca.channel);
            nw.nw.handle = 0xbeef0000u + uint32_t(i); nw.nw.object = objs[i]; nw.nw.oclass = int32_t(find(types[i]));
            assert(NvIo(f, NV_NVIF, &nw, sizeof(nw)) == 0);
            if (i == 2) { assert(NvIo(f, NV_NVIF, &nw, sizeof(nw)) == kEEXIST); }
        }
        { struct { NvifIoctl io; NvifNew nw; } bad{}; bad.io.type = NVIF_NEW; bad.io.route = 0xff; bad.io.token = uint64_t(ca.channel);
          bad.nw.oclass = 0xc397; assert(NvIo(f, NV_NVIF, &bad, sizeof(bad)) == kEINVAL); }   // not offered
        for (int i = 0; i < 5; ++i) { NvifIoctl d{}; d.type = NVIF_DEL; d.object = objs[i]; assert(NvIo(f, NV_NVIF, &d, sizeof(d)) == 0); }
        NvChannelFree fr{ca.channel}; assert(NvIo(f, NV_CHANNEL_FREE, &fr, sizeof(fr)) == 0 && NvIo(f, NV_CHANNEL_FREE, &fr, sizeof(fr)) == kENOENT);
        NvChannelAlloc ce{}; ce.tt_ctxdma_handle = 0x30; assert(NvIo(f, NV_CHANNEL_ALLOC, &ce, sizeof(ce)) == 0);
        NvChannelAlloc vp{}; vp.tt_ctxdma_handle = 0x02; assert(NvIo(f, NV_CHANNEL_ALLOC, &vp, sizeof(vp)) == kEINVAL);
    }
    {   // anything else (GEM, VM_BIND, EXEC) is not there yet; a wrong size is refused
        uint8_t junk[64] = {}; assert(NvIo(f, 0x40, junk, 48) == kENOTTY && NvIo(f, 0x12, junk, 48) == kENOTTY);
        NvGetparam g{GP_PCI_DEVICE, 0}; assert(Ioctl(f, U, Cmd(kDrmCommandBase + NV_GETPARAM, 8), uint64_t(&g)) == kEINVAL);
        static File off; DeviceInfo none; off.Init(&none); assert(Io(off, NR_GET_CAP, g) == kENOTTY);   // unpublished device
    }
    f.Close();
    TestMemory(dev);
    std::printf("test_nv_drm_core: OK\n");
    return 0;
}
