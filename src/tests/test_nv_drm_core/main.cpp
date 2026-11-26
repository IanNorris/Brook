// M5.1: the nouveau uAPI core (nv_drm_core.h) driven through the sequence Mesa NVK 26.0.6 runs
// when it creates a physical device (winsys/nouveau_device.c, nouveau_context.c,
// vulkan/runtime/vk_drm_syncobj.c), with user pointers as host addresses.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "nv_drm_core.h"
#include "nv_vram_heap.h"
#include <vector>

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
    std::printf("test_nv_drm_core: OK\n");
    return 0;
}
