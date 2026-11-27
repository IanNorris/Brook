// M5.1: the nouveau uAPI core (nv_drm_core.h) driven through the sequence Mesa NVK 26.0.6 runs
// when it creates a physical device (winsys/nouveau_device.c, nouveau_context.c,
// vulkan/runtime/vk_drm_syncobj.c), with user pointers as host addresses.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "nv_drm_core.h"
#include "nv_vram_heap.h"
#include <vector>
#include <new>
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
    struct Chan { uint32_t vas, engine; std::vector<std::pair<uint32_t, std::vector<uint64_t>>> subs; uint32_t completed = 0; bool error = false; bool full = false; };
    std::map<uint32_t, Chan> chans; uint32_t next_chan = 100, chans_destroyed = 0;
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
    [](void* c, uint32_t) { ++static_cast<Fake*>(c)->flushes; return true; },
    [](void* c, uint32_t vas, uint32_t engine, uint32_t* ch) { auto* f = static_cast<Fake*>(c); *ch = f->next_chan++; f->chans[*ch] = {vas, engine, {}}; return true; },
    [](void* c, uint32_t ch) { auto* f = static_cast<Fake*>(c); assert(f->chans.erase(ch) == 1); ++f->chans_destroyed; },
    [](void* c, uint32_t ch, const uint64_t* va, const uint32_t* dw, const uint8_t*, uint32_t n, uint32_t seq) {
        auto& x = static_cast<Fake*>(c)->chans.at(ch); if (x.full) return false;
        std::vector<uint64_t> v; for (uint32_t i = 0; i < n; ++i) { v.push_back(va[i]); v.push_back(dw[i]); }
        x.subs.push_back({seq, v}); return true; },
    [](void* c, uint32_t ch) { return static_cast<Fake*>(c)->chans.at(ch).completed; },
    [](void* c, uint32_t ch) { return static_cast<Fake*>(c)->chans.at(ch).error; },
    true};

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
    {   // a backend without CPU-visible VRAM places VRAM|MAPPABLE in GART (and plain VRAM stays VRAM)
        static Backend nomap = kFakeBe; nomap.vram_cpu_map = false;
        static File g; g.Init(&dev, &nomap);
        GemNew m{}; const uint32_t hm = NewBo(g, 8192, GEM_DOMAIN_VRAM | GEM_DOMAIN_MAPPABLE, 0, &m);
        GemNew v{}; const uint32_t hv = NewBo(g, 8192, GEM_DOMAIN_VRAM, 0, &v);
        assert(hm && m.info.domain == (GEM_DOMAIN_GART | GEM_DOMAIN_MAPPABLE) && hv && v.info.domain == GEM_DOMAIN_VRAM);
        g.Close();
    }
    auto& pt = g_fake.vas[f.vas];
    // sync MAP: PTEs with the BO's kind; flush once per batch
    const uint32_t fl0 = g_fake.flushes;
    assert(Bind(f, {Map(ha, 0x100000, 1 << 20)}) == 0 && g_fake.flushes == fl0 + 1 && pt.size() == 256);
    assert(pt[0x100000].aper == Aper::Vram && pt[0x100000].kind == 0x06 && pt[0x1ff000].pa == pt[0x100000].pa + 0xff000);
    assert(Bind(f, {Map(hg, 0x100000 + (512 << 10), 64 << 10)}) == kEINVAL);    // VA busy
    {   // NVK passes the PTE kind in op flags 7:0 (nouveau_bo_map / nouveau_uvmm); it wins over the BO's
        BindOp k = Map(hg, 0x3000000, 4096); k.flags = 0xfe;
        assert(Bind(f, {k}) == 0 && pt[0x3000000].kind == 0xfe);
        assert(Bind(f, {Unmap(0x3000000, 4096)}) == 0 && !pt.count(0x3000000));
        BindOp bad = Map(hg, 0x3000000, 4096); bad.flags = 0x200;
        assert(Bind(f, {bad}) == kEINVAL);
    }
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

static int64_t Exe(File& f, int32_t ch, std::vector<ExecPush> p, std::vector<SyncDesc> waits = {}, std::vector<SyncDesc> sigs = {}) {
    Exec e{}; e.channel = uint32_t(ch); e.push_count = uint32_t(p.size()); e.push_ptr = uint64_t(p.data());
    e.wait_count = uint32_t(waits.size()); e.wait_ptr = uint64_t(waits.data()); e.sig_count = uint32_t(sigs.size()); e.sig_ptr = uint64_t(sigs.data());
    return NvIo(f, NV_EXEC, &e, sizeof(e));
}
static void TestExec(const DeviceInfo& dev) {
    g_fake.vram.Init(0x140000000ull, 0x140000000ull + (8ull << 20));
    static File f; f.Init(&dev, &kFakeBe);
    NvVmInit vm{0xff00000000ull, (1ull << 40) - 0xff00000000ull}; assert(NvIo(f, NV_VM_INIT, &vm, sizeof(vm)) == 0);
    NvChannelAlloc ca{}; ca.tt_ctxdma_handle = 0x01; assert(NvIo(f, NV_CHANNEL_ALLOC, &ca, sizeof(ca)) == 0);
    Channel* c = FindChannel(f, ca.channel); assert(c && g_fake.chans.count(c->hw) && g_fake.chans[c->hw].vas == f.vas && g_fake.chans[c->hw].engine == 1);
    auto& hw = g_fake.chans[c->hw];
    DrmSyncobjCreate s1{0, 0}, s2{0, 0}; assert(Io(f, NR_SYNCOBJ_CREATE, s1) == 0 && Io(f, NR_SYNCOBJ_CREATE, s2) == 0);
    auto wait = [&](uint32_t h) { DrmSyncobjWait w{uint64_t(&h), 0, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, 0, 0}; return Io(f, NR_SYNCOBJ_WAIT, w); };
    // submit: pushes in order, then the job's seqno; fence completes when the GPU passes it
    assert(Exe(f, ca.channel, {{0x100000, 64, 0}, {0x200000, 16, EXEC_PUSH_NO_PREFETCH}}, {}, {{SYNC_SYNCOBJ, s1.handle, 0}}) == 0);
    assert(hw.subs.size() == 1 && hw.subs[0].first == 1 && hw.subs[0].second == (std::vector<uint64_t>{0x100000, 16, 0x200000, 4}));
    assert(wait(s1.handle) == kETIME);
    CpuPrep cp{0, CPU_PREP_NOWAIT}; { GemNew n{}; n.info.size = 4096; n.info.domain = GEM_DOMAIN_GART; assert(NvIo(f, NV_GEM_NEW, &n, sizeof(n)) == 0); cp.handle = n.info.handle; }
    assert(NvIo(f, NV_GEM_CPU_PREP, &cp, sizeof(cp)) == kEBUSY);
    hw.completed = 1;
    {   // an unlocked (pump=false) ioctl must not touch the rings: the job stays in flight
        DrmSyncobjWait w{uint64_t(&s1.handle), 0, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, 0, 0};
        const uint64_t cmd = Cmd(NR_SYNCOBJ_WAIT, sizeof(w));
        assert(Ioctl(f, U, cmd, uint64_t(&w), false) == kETIME && c->in_count == 1);
    }
    assert(wait(s1.handle) == 0 && NvIo(f, NV_GEM_CPU_PREP, &cp, sizeof(cp)) == 0 && c->in_count == 0);
    // a job waiting on a pending fence is held back; the ring-full case requeues
    GpuFence* gate = GpuFenceCreate(false);
    { GpuSync* s = f.syncs.Get(s2.handle); assert(GpuSyncReplace(s, gate)); GpuSyncUnref(s); }
    assert(Exe(f, ca.channel, {{0x300000, 8, 0}}, {{SYNC_SYNCOBJ, s2.handle, 0}}, {{SYNC_SYNCOBJ, s1.handle, 0}}) == 0);
    assert(hw.subs.size() == 1 && c->job_count == 1 && wait(s1.handle) == kETIME);
    hw.full = true; assert(GpuFenceComplete(gate, true));
    NvGetparam g{GP_PCI_DEVICE, 0}; assert(NvIo(f, NV_GETPARAM, &g, sizeof(g)) == 0 && hw.subs.size() == 1 && c->job_count == 1);   // no room yet
    hw.full = false; assert(NvIo(f, NV_GETPARAM, &g, sizeof(g)) == 0 && hw.subs.size() == 2 && hw.subs[1].first == 2 && c->job_count == 0);
    hw.completed = 2; assert(wait(s1.handle) == 0);
    GpuFenceUnref(gate);
    // a failed wait fence fails the job without submitting it
    GpuFence* bad = GpuFenceCreate(false); assert(GpuFenceComplete(bad, false));
    { GpuSync* s = f.syncs.Get(s2.handle); assert(GpuSyncReplace(s, bad)); GpuSyncUnref(s); }
    assert(Exe(f, ca.channel, {{0x300000, 8, 0}}, {{SYNC_SYNCOBJ, s2.handle, 0}}, {{SYNC_SYNCOBJ, s1.handle, 0}}) == 0);
    assert(hw.subs.size() == 2 && wait(s1.handle) == -5);
    GpuFenceUnref(bad);
    // validation
    assert(Exe(f, ca.channel, {{0x300000, 6, 0}}) == kEINVAL && Exe(f, ca.channel, {{0x300000, 0, 0}}) == kEINVAL);
    assert(Exe(f, ca.channel, {{0x300000, 8, 2}}) == kEINVAL && Exe(f, 99, {{0x300000, 8, 0}}) == kENOENT);
    // timeline syncobjs: EXEC signals point 5; waits, query, CPU signal and transfer by point
    {
        DrmSyncobjCreate tl{0, 0}; assert(Io(f, NR_SYNCOBJ_CREATE, tl) == 0);
        const uint32_t before = uint32_t(hw.subs.size());
        assert(Exe(f, ca.channel, {{0x500000, 8, 0}}, {}, {{SYNC_TIMELINE_SYNCOBJ, tl.handle, 5}}) == 0 && hw.subs.size() == before + 1);
        uint32_t th = tl.handle; uint64_t p5 = 5, p3 = 3, p6 = 6;
        DrmTimelineWait w5{uint64_t(&th), uint64_t(&p5), 0, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, 0, 0};
        DrmTimelineWait w6{uint64_t(&th), uint64_t(&p6), 0, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, 0, 0};
        assert(Io(f, NR_SYNCOBJ_TIMELINE_WAIT, w5) == kETIME && Io(f, NR_SYNCOBJ_TIMELINE_WAIT, w6) == kEINVAL);   // 6 not submitted
        DrmTimelineWait wa{uint64_t(&th), uint64_t(&p5), 0, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, 0, 0, 0};
        assert(Io(f, NR_SYNCOBJ_TIMELINE_WAIT, wa) == 0);                                                    // 5 is available
        uint64_t q = 99; DrmTimelineArray qa{uint64_t(&th), uint64_t(&q), 1, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED};
        assert(Io(f, NR_SYNCOBJ_QUERY, qa) == 0 && q == 5);
        qa.flags = 0; assert(Io(f, NR_SYNCOBJ_QUERY, qa) == 0 && q == 0);                                   // nothing signalled yet
        hw.completed = hw.subs.back().first;
        assert(Io(f, NR_SYNCOBJ_TIMELINE_WAIT, w5) == 0);
        DrmTimelineWait w3{uint64_t(&th), uint64_t(&p3), 0, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, 0, 0};
        assert(Io(f, NR_SYNCOBJ_TIMELINE_WAIT, w3) == 0);                                                    // earlier points are covered
        assert(Io(f, NR_SYNCOBJ_QUERY, qa) == 0 && q == 5);
        DrmTimelineArray sg{uint64_t(&th), uint64_t(&p6), 1, 0}; assert(Io(f, NR_SYNCOBJ_TIMELINE_SIGNAL, sg) == 0 && Io(f, NR_SYNCOBJ_TIMELINE_WAIT, w6) == 0);
        DrmSyncobjCreate b2{0, 0}; assert(Io(f, NR_SYNCOBJ_CREATE, b2) == 0);
        DrmSyncobjTransfer tr{tl.handle, b2.handle, 6, 0, 0, 0}; assert(Io(f, NR_SYNCOBJ_TRANSFER, tr) == 0);
        assert(wait(b2.handle) == 0);
        DrmSyncobjTransfer bad{tl.handle, b2.handle, 9, 0, 0, 0}; assert(Io(f, NR_SYNCOBJ_TRANSFER, bad) == kEINVAL);
        assert(Exe(f, ca.channel, {{0x500000, 8, 0}}, {{SYNC_SYNCOBJ, tl.handle, 7}}) == kEINVAL);          // binary flag with a point
    }
    // Blocking CPU_PREP uses a referenced snapshot. Another ioctl can replace
    // the last EXEC while the caller sleeps; only the original EXEC is awaited.
    {
        CpuPrep prep{cp.handle, 0};
        CpuPrepFences fences;
        GpuFenceUnref(f.last_exec);
        f.last_exec = GpuFenceCreate(false);
        assert(SnapshotCpuPrep(f, prep, fences) == 0);
        GpuFenceUnref(f.last_exec);
        f.last_exec = GpuFenceCreate(false);
        UserAccess sleeper = U;
        sleeper.ctx = fences.exec;
        sleeper.sleep_tick = [](void* fence) {
            return GpuFenceComplete(static_cast<GpuFence*>(fence), true);
        };
        assert(WaitCpuPrep(sleeper, prep, fences) == 0);
        assert(GpuFenceState(f.last_exec) == 0);
    }
    // channel error: in-flight work fails, later EXECs are refused
    assert(Exe(f, ca.channel, {{0x400000, 8, 0}}, {}, {{SYNC_SYNCOBJ, s1.handle, 0}}) == 0 && hw.subs.size() == 4);
    hw.error = true;
    assert(wait(s1.handle) == -5 && c->dead && Exe(f, ca.channel, {{0x400000, 8, 0}}) == -19);
    // A synchronous bind holds the file lock, so it must reap the same
    // file's EXEC that an earlier async bind is waiting on.
    {
        NvChannelAlloc second{};
        second.tt_ctxdma_handle = 0x01;
        assert(NvIo(f, NV_CHANNEL_ALLOC, &second, sizeof(second)) == 0);
        auto& secondHw = g_fake.chans[FindChannel(f, second.channel)->hw];
        assert(Exe(f, second.channel, {{0x400000, 8, 0}}, {}, {{SYNC_SYNCOBJ, s1.handle, 0}}) == 0);
        assert(Bind(f, {Map(cp.handle, 0x700000, 4096)}, VM_BIND_RUN_ASYNC,
                    {{SYNC_SYNCOBJ, s1.handle, 0}}) == 0);
        assert(f.job_count == 1);
        BindOp op = Map(cp.handle, 0x800000, 4096);
        VmBind bind{};
        bind.op_count = 1;
        bind.op_ptr = uint64_t(&op);
        UserAccess sleeper = U;
        sleeper.ctx = &secondHw;
        sleeper.sleep_tick = [](void* ctx) {
            auto& channel = *static_cast<Fake::Chan*>(ctx);
            if (channel.completed) return false;
            channel.completed = channel.subs.back().first;
            return true;
        };
        assert(Ioctl(f, sleeper, Cmd(kDrmCommandBase + NV_VM_BIND, sizeof(bind)), uint64_t(&bind)) == 0);
        assert(f.job_count == 0 && g_fake.vas[f.vas].count(0x700000) && g_fake.vas[f.vas].count(0x800000));
        NvChannelFree freeSecond{second.channel};
        assert(NvIo(f, NV_CHANNEL_FREE, &freeSecond, sizeof(freeSecond)) == 0);
    }
    // CHANNEL_FREE and close destroy the backend channels
    NvChannelAlloc ce{}; ce.tt_ctxdma_handle = 0x30; assert(NvIo(f, NV_CHANNEL_ALLOC, &ce, sizeof(ce)) == 0 && g_fake.chans.size() == 2);
    NvChannelFree fr{ca.channel}; assert(NvIo(f, NV_CHANNEL_FREE, &fr, sizeof(fr)) == 0 && g_fake.chans.size() == 1);
    {   // first-submit schedule/bind failure (27 Nov panic): the submit fails with the channel in
        // error, so queued jobs fail at once, nothing stays half-queued, and later EXECs are refused
        Channel* e = FindChannel(f, ce.channel); assert(e);
        auto& eh = g_fake.chans[e->hw]; eh.full = true; eh.error = true;   // submit refused + channel error
        DrmSyncobjCreate s3{0, 0}; assert(Io(f, NR_SYNCOBJ_CREATE, s3) == 0);
        assert(Exe(f, ce.channel, {{0x600000, 8, 0}}, {}, {{SYNC_SYNCOBJ, s3.handle, 0}}) == 0);
        assert(e->dead && e->job_count == 0 && e->in_count == 0 && wait(s3.handle) == -5);
        assert(Exe(f, ce.channel, {{0x600000, 8, 0}}) == -19);
    }
    f.Close();
    assert(g_fake.chans.empty() && g_fake.chans_destroyed == 3 && g_fake.vram_used() == 0 && g_fake.sys.empty());
}

// ---- PRIME (nv_drm_prime.h): export/import between files, ownership, implicit fences --------
static void TestPrime(const DeviceInfo& dev) {
    g_fake.vram.Init(0x140000000ull, 0x140000000ull + (8ull << 20));
    static File a, b; a.Init(&dev, &kFakeBe); b.Init(&dev, &kFakeBe);
    NvVmInit vm{0xff00000000ull, (1ull << 40) - 0xff00000000ull};
    assert(NvIo(a, NV_VM_INIT, &vm, sizeof(vm)) == 0 && NvIo(b, NV_VM_INIT, &vm, sizeof(vm)) == 0);
    {   DrmGetCap c{DRM_CAP_PRIME, 0}; assert(Io(a, NR_GET_CAP, c) == 0 && c.value == (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)); }
    // a block-linear VRAM render target (kind 0x06, tile_mode = GOB height log2 5) and a linear GART buffer
    GemNew vn{}; vn.info.size = 1 << 20; vn.info.domain = GEM_DOMAIN_VRAM; vn.info.tile_flags = 0x06u << 8; vn.info.tile_mode = 5;
    assert(NvIo(a, NV_GEM_NEW, &vn, sizeof(vn)) == 0);
    const uint32_t hv = vn.info.handle, hg = NewBo(a, 64 << 10, GEM_DOMAIN_GART);
    const uint32_t hp = NewBo(a, 4096, GEM_DOMAIN_GART | GEM_DOMAIN_NO_SHARE);
    assert(hv && hg && hp);
    const uint64_t vram_used = g_fake.vram_used(); const size_t sys_used = g_fake.sys.size();
    Shared* sv = nullptr; Shared* sg = nullptr; Shared* sx = nullptr;
    assert(PrimeExport(a, hv, &sv) == 0 && sv && sv->refs == 2 && a.nshared == 1);   // the BO + the "fd"
    assert(sv->aper() == Aper::Vram && sv->size == (1u << 20) && sv->kind() == 0x06 && sv->tile_mode == 5);
    assert(PrimeExport(a, hv, &sx) == 0 && sx == sv && sv->refs == 3); SharedUnref(sx);   // second export: same object
    assert(PrimeExport(a, hg, &sg) == 0 && sg->aper() == Aper::Sys && a.nshared == 2);
    assert(PrimeExport(a, hp, &sx) == kEINVAL && !sx);                // NO_SHARE is VM-private
    assert(PrimeExport(a, 999, &sx) == kENOENT);
    // re-import into the exporter: the same handle (Linux GEM; Mesa's BO cache keys on it)
    uint32_t h = 0; assert(PrimeImport(a, sv, &h) == 0 && h == hv && sv->refs == 2);
    // import into another file: a new handle with the same placement, size and tiling
    uint32_t bv = 0, bg = 0; assert(PrimeImport(b, sv, &bv) == 0 && bv && sv->refs == 3);
    assert(PrimeImport(b, sv, &h) == 0 && h == bv && sv->refs == 3);   // again: same handle, no new reference
    assert(PrimeImport(b, sg, &bg) == 0 && bg != bv && b.nshared == 2);
    GemInfo gi{}; gi.handle = bv; assert(NvIo(b, NV_GEM_INFO, &gi, sizeof(gi)) == 0);
    assert(gi.domain == GEM_DOMAIN_VRAM && gi.size == (1u << 20) && ((gi.tile_flags >> 8) & 0xff) == 0x06 && gi.tile_mode == 5 && gi.map_handle);
    assert(FindBoByToken(b, gi.map_handle) && FindBoByToken(b, gi.map_handle)->backing == sv->backing);
    // the importer maps it in its own VAS: same backing, the BO's kind
    auto& pt = g_fake.vas[b.vas];
    assert(Bind(b, {Map(bv, 0x100000, 1 << 20)}) == 0 && pt[0x100000].aper == Aper::Vram && pt[0x100000].pa == sv->backing && pt[0x100000].kind == 0x06);
    // the exporter closes everything: the importer's handle, mapping and the fd keep the memory
    { GemClose c{hv, 0}; assert(Io(a, NR_GEM_CLOSE, c) == 0); }
    { GemClose c{hg, 0}; assert(Io(a, NR_GEM_CLOSE, c) == 0); }
    assert(g_fake.vram_used() == vram_used && g_fake.sys.size() == sys_used && a.nshared == 0);
    a.Close(); new (&a) File(); a.Init(&dev, &kFakeBe); assert(NvIo(a, NV_VM_INIT, &vm, sizeof(vm)) == 0);
    assert(g_fake.vram_used() == vram_used && sv->refs == 2);
    // implicit sync: a pending write fence blocks CPU_PREP (both READ and WRITE access) on every file
    GpuFence* w1 = GpuFenceCreate(false); GpuFence* w2 = GpuFenceCreate(false);
    assert(SharedAddFence(sv, w1, DMA_BUF_SYNC_WRITE) && SharedAddFence(sv, w2, DMA_BUF_SYNC_WRITE));   // joined
    CpuPrep cp{bv, CPU_PREP_NOWAIT}; assert(NvIo(b, NV_GEM_CPU_PREP, &cp, sizeof(cp)) == kEBUSY);
    GpuFence* got = nullptr; assert(SharedFence(sv, DMA_BUF_SYNC_READ, &got) && got && GpuFenceState(got) == 0);
    assert(GpuFenceComplete(w1, true) && GpuFenceState(got) == 0 && NvIo(b, NV_GEM_CPU_PREP, &cp, sizeof(cp)) == kEBUSY);
    assert(GpuFenceComplete(w2, true) && GpuFenceState(got) == 1 && NvIo(b, NV_GEM_CPU_PREP, &cp, sizeof(cp)) == 0);
    GpuFenceUnref(got);
    // readers: READ access ignores them, WRITE access waits for them
    GpuFence* r1 = GpuFenceCreate(false); assert(SharedAddFence(sv, r1, DMA_BUF_SYNC_READ));
    assert(NvIo(b, NV_GEM_CPU_PREP, &cp, sizeof(cp)) == 0);
    CpuPrep cw{bv, CPU_PREP_NOWAIT | CPU_PREP_WRITE}; assert(NvIo(b, NV_GEM_CPU_PREP, &cw, sizeof(cw)) == kEBUSY);
    assert(GpuFenceComplete(r1, true) && NvIo(b, NV_GEM_CPU_PREP, &cw, sizeof(cw)) == 0);
    // a failed producer fence is reported (-EIO), and a later successful one replaces it only once complete
    GpuFence* bad = GpuFenceCreate(false); assert(SharedAddFence(sv, bad, DMA_BUF_SYNC_WRITE) && GpuFenceComplete(bad, false));
    assert(NvIo(b, NV_GEM_CPU_PREP, &cp, sizeof(cp)) == -5);
    GpuFence* ok = GpuFenceCreate(true); assert(SharedAddFence(sv, ok, DMA_BUF_SYNC_WRITE) && NvIo(b, NV_GEM_CPU_PREP, &cp, sizeof(cp)) == -5);
    GpuFenceUnref(ok); GpuFenceUnref(bad); GpuFenceUnref(w1); GpuFenceUnref(w2); GpuFenceUnref(r1);
    // fallback: the GART buffer never had a write fence imported; an EXEC of a file that maps it
    // joins its bookkeep fence (and one that does not map it does not)
    {
        GpuFence* e1 = GpuFenceCreate(false);
        PrimeNoteExec(b, e1);   // b imported sg but has not mapped it
        GpuFence* f0 = nullptr; assert(SharedFence(sg, DMA_BUF_SYNC_READ, &f0) && !f0);
        assert(Bind(b, {Map(bg, 0x300000, 64 << 10)}) == 0);
        PrimeNoteExec(b, e1);
        CpuPrep cg{bg, CPU_PREP_NOWAIT}; assert(NvIo(b, NV_GEM_CPU_PREP, &cg, sizeof(cg)) == kEBUSY);
        assert(GpuFenceComplete(e1, true) && NvIo(b, NV_GEM_CPU_PREP, &cg, sizeof(cg)) == 0);
        // once a producer imports a write fence, EXECs stop feeding the fallback
        GpuFence* e2 = GpuFenceCreate(false); GpuFence* done = GpuFenceCreate(true);
        assert(SharedAddFence(sg, done, DMA_BUF_SYNC_WRITE));
        PrimeNoteExec(b, e2); assert(NvIo(b, NV_GEM_CPU_PREP, &cg, sizeof(cg)) == 0);
        assert(GpuFenceComplete(e2, true));
        GpuFenceUnref(e1); GpuFenceUnref(e2); GpuFenceUnref(done);
        assert(Bind(b, {Unmap(0x300000, 64 << 10)}) == 0);
    }
    // syncobj <-> sync-file fences: export the current fence, import into another syncobj
    {
        DrmSyncobjCreate s1{0, 0}, s2{0, 0}; assert(Io(b, NR_SYNCOBJ_CREATE, s1) == 0 && Io(b, NR_SYNCOBJ_CREATE, s2) == 0);
        int64_t rc = 0; assert(!SyncobjExportFence(b, s1.handle, 0, &rc) && rc == kEINVAL);   // nothing submitted
        assert(!SyncobjExportFence(b, 777, 0, &rc) && rc == kENOENT);
        GpuFence* pend = GpuFenceCreate(false);
        { GpuSync* s = b.syncs.Get(s1.handle); assert(GpuSyncReplace(s, pend)); GpuSyncUnref(s); }
        GpuFence* ex = SyncobjExportFence(b, s1.handle, 0, &rc); assert(ex == pend && rc == 0);
        assert(SyncobjImportFence(b, s2.handle, 0, ex) == 0);
        auto wait = [&](uint32_t hh) { DrmSyncobjWait w{uint64_t(&hh), 0, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, 0, 0}; return Io(b, NR_SYNCOBJ_WAIT, w); };
        assert(wait(s2.handle) == kETIME && GpuFenceComplete(pend, true) && wait(s2.handle) == 0);
        DrmSyncobjCreate s3{0, 0}; assert(Io(b, NR_SYNCOBJ_CREATE, s3) == 0);
        assert(SyncobjImportFence(b, s3.handle, 0, nullptr) == 0 && wait(s3.handle) == 0);   // a signalled sync file
        assert(SyncobjImportFence(b, 777, 0, nullptr) == kENOENT);
        GpuFenceUnref(ex); GpuFenceUnref(pend);
    }
    // teardown: the importer's file goes, then the last fd reference frees the backing
    b.Close();
    assert(sv->refs == 1 && sg->refs == 1 && g_fake.vram_used() == vram_used);
    SharedUnref(sv); SharedUnref(sg);
    a.Close();
    assert(g_fake.vram_used() == 0 && g_fake.sys.empty());
    // import of an fd from a file whose BO is gone still works (the fd owns the memory)
    new (&a) File(); new (&b) File(); a.Init(&dev, &kFakeBe); b.Init(&dev, &kFakeBe);
    const uint32_t h2 = NewBo(a, 8192, GEM_DOMAIN_GART); Shared* s2 = nullptr; assert(PrimeExport(a, h2, &s2) == 0);
    a.Close();
    uint32_t i2 = 0; assert(PrimeImport(b, s2, &i2) == 0 && g_fake.sys.size() == 1);
    SharedUnref(s2); assert(g_fake.sys.size() == 1);
    b.Close(); assert(g_fake.sys.empty());
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
        DrmGetCap t{DRM_CAP_SYNCOBJ_TIMELINE, 9}; assert(Io(f, NR_GET_CAP, t) == 0 && t.value == 1);
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
        uint8_t junk[64] = {}; assert(NvIo(f, 0x41, junk, 48) == kENOTTY && NvIo(f, 0x40, junk, 48) == kENOTTY);   // legacy PUSHBUF; GEM_NEW without a backend
        NvGetparam g{GP_PCI_DEVICE, 0}; assert(Ioctl(f, U, Cmd(kDrmCommandBase + NV_GETPARAM, 8), uint64_t(&g)) == kEINVAL);
        static File off; DeviceInfo none; off.Init(&none); assert(Io(off, NR_GET_CAP, g) == kENOTTY);   // unpublished device
    }
    f.Close();
    TestMemory(dev);
    TestExec(dev);
    TestPrime(dev);
    std::printf("test_nv_drm_core: OK\n");
    return 0;
}
