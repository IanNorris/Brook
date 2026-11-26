#pragma once
// M4 step 2: GR on GSP-RM (TU104, 570.144), in nouveau's order (MIT: r535/gr.c
// r535_gr_get_ctxbuf_info, r535_gr_promote_ctx, r535_gr_oneinit; r570/gr.c
// r570_gr_scrubber_init/fini; r535/vmm.c r535_mmu_vaspace_new):
//
//   info      NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO on RM's
//             internal subdevice -> the context-buffer list (MAIN + 64 subctx header
//             pages, PATCH, BUNDLE_CB, PAGEPOOL, ATTRIBUTE_CB, RTV_CB_GLOBAL, FECS_EVENT,
//             PRIV_ACCESS_MAP + its UNRESTRICTED twin; global/init/ro as nouveau)
//   golden    own client/device/subdevice, a FERMI_VASPACE_A that RM manages (not
//             externally owned) with Brook's page tables handed over for RM's 512 MiB
//             window at 4 GiB (COPY_SERVER_RESERVED_PDES), a GR0 channel on RM's first
//             free chid, PROMOTE_CTX(golden: every buffer allocated, init ones zeroed),
//             then TURING_A on the channel: RM builds and caches the golden context.
//             The channel scope is freed; the global buffers stay for every later channel.
//   scrubber  (TU10x, BUG4208224 WAR) the same scope with RM's KGRAPHICS_SCRUBBER
//             handles: PROMOTE_CTX(non-golden), TURING_A, INIT_BUG4208224_WAR(false).
//             Kept until fini, which sends the WAR teardown first.
//
// All VRAM comes from one Brook arena in a client-usable FB region; per-scope page
// tables from the same arena. Nothing here submits work.
#include <stdint.h>
#include "nv_gsp_m2.h"
#include "nv_gsp_bar1.h"

namespace brook::nvgsp {

namespace grh {   // client handle bases; device/subdevice/VAS/channel/3D are base + small offsets
constexpr uint32_t kGolden = 0xc1d0b600, kScrubber = 0xc1d0b700, kUser = 0xc1d0b800;
constexpr uint32_t kDevice = 0x80, kSubdevice = 0x81, kVaspace = 0x90, kChannel = 0xa0, kThreed = 0xb0, kCompute = 0xb1;
}

struct GrBuf {
    uint16_t id = 0; uint32_t size = 0; uint8_t page = 12, align = 12;
    bool global = false, init = false, ro = false;
    uint64_t vram = 0;              // golden: the global buffer (kept)
};
struct GrBufs { GrBuf b[16]; uint32_t n = 0; uint32_t raw_sizes[rm570::gr::kEngineIdCount] = {}; };

inline uint8_t GrLog2Ceil(uint64_t v) { uint8_t s = 0; while ((1ull << s) < v) ++s; return s; }

// nouveau r535_gr_get_ctxbuf_info over engine slot 0.
inline void GrBuildBufs(const rm570::gr::GET_CONTEXT_BUFFERS_INFO_PARAMS& p, GrBufs& out) {
    namespace g = rm570::gr;
    struct Map { uint32_t id0; uint16_t id1; bool global, init, ro; };
    static const Map map[] = {
        {g::CTX_GRAPHICS, g::BUF_MAIN, false, true, false},
        {g::CTX_GRAPHICS_PATCH, g::BUF_PATCH, false, true, false},
        {g::CTX_GRAPHICS_BUNDLE_CB, g::BUF_BUNDLE_CB, true, false, false},
        {g::CTX_GRAPHICS_PAGEPOOL, g::BUF_PAGEPOOL, true, false, false},
        {g::CTX_GRAPHICS_ATTRIBUTE_CB, g::BUF_ATTRIBUTE_CB, true, false, false},
        {g::CTX_GRAPHICS_RTV_CB_GLOBAL, g::BUF_RTV_CB_GLOBAL, true, false, false},
        {g::CTX_GRAPHICS_FECS_EVENT, g::BUF_FECS_EVENT, true, true, false},
        {g::CTX_GRAPHICS_PRIV_ACCESS_MAP, g::BUF_PRIV_ACCESS_MAP, true, true, true},
    };
    out.n = 0;
    for (uint32_t i = 0; i < g::kEngineIdCount; ++i) {
        out.raw_sizes[i] = p.engine[0][i].size;
        const Map* m = nullptr;
        for (const Map& e : map) if (e.id0 == i) m = &e;
        if (!m || out.n + 2 > 16) continue;
        uint32_t size = p.engine[0][i].size;
        if (m->id1 == g::BUF_MAIN) size = ((size + 0xfff) & ~0xfffu) + 64 * 0x1000;   // per-subctx headers
        const uint8_t page = size >= (1u << 21) ? 21 : size >= (1u << 16) ? 16 : 12;
        const uint8_t align = m->id1 == g::BUF_ATTRIBUTE_CB ? GrLog2Ceil(size) : page;
        GrBuf& b = out.b[out.n++];
        b.id = m->id1; b.size = size; b.page = page; b.align = align; b.global = m->global; b.init = m->init; b.ro = m->ro;
        if (m->id1 == g::BUF_PRIV_ACCESS_MAP) { out.b[out.n] = b; out.b[out.n].id = g::BUF_UNRESTRICTED_PRIV_ACCESS_MAP; ++out.n; }
    }
}

// GR idle fault (26 Nov): a sysmem method buffer on a long-lived GR channel (the scrubber) leads,
// ~1 s later, to a PCIe Engine error, PMU halt and Xid 62; in VRAM it does not. Default VRAM;
// `gr mthdbuf sys` restores the nouveau-style sysmem buffer. Formerly: put the method buffer in VRAM instead of
// sysmem (nouveau uses sysmem; this only tests whether a sysmem DMA is the faulting one).
inline bool g_grMthdbufVram = true;

// One client scope with a GR0 channel.
struct GrScope {
    uint32_t base = 0, chid = 0;
    bool external = false, priv = true;
    uint32_t h_vas = 0, h_chan = 0, h_3d = 0;   // RM handles (scrubber uses RM's fixed ones)
    uint32_t live[8] = {}, parent[8] = {}, nlive = 0;
    bool unset_pd = false;
    uint64_t root = 0, inst = 0, userd = 0;
    uint64_t va_next = 0x0000004000000000ull;   // context-buffer VAs (clear of RM's window at 4 GiB)
    uint64_t lpt_page = 0; uint32_t lpt_off = 0x1000; Aperture lpt_aper = Aperture::Video;
    uint64_t buf_vram[16] = {}, buf_va[16] = {};
    uint32_t promoted = 0, pt_pages = 0;
    uint64_t gpfifo_va = 0;   // 0: the channel never submits (golden, scrubber)
    uint32_t token = 0;
};

struct GrResult {
    const char* stage = "check";
    const char* status = "ok";
    uint32_t rm_status = 0, rpc_result = 0;
    QStatus q = QStatus::Ok;
    GrBufs bufs;
    uint32_t golden_entries = 0, scrubber_entries = 0;
    bool golden_ok = false, scrubber_ok = false;
    uint64_t vram_used = 0;
    uint32_t allocated = 0, freed = 0;
    RmEvents ev;
    bool ok() const { return status[0] == 'o' && status[1] == 'k' && !status[2]; }
};

// The kept state after golden + scrubber (global buffers and the scrubber scope).
struct GrState {
    bool up = false;
    GrBufs bufs;
    GrScope scrub;
    VramArena arena;
    uint32_t mthdbuf_size = 0;
    uint64_t rt_vram = 0, rt_bytes = 0;   // the colour target, kept across `gr clear` runs
};

template <class Bus, class Api, class VramIo>
struct GrCtx {
    Bus& bus; Api& api; VramIo& vram; const ChipOps& c; VramArena& arena; GrResult& r;
    void (*trace)(const char* what, uint32_t handle, uint32_t cls, uint32_t status) = nullptr;   // M5 bring-up
    bool fail(const char* what) { r.status = what; r.rm_status = api.status; r.rpc_result = api.rpc_result; r.q = api.q; return false; }
    bool track(GrScope& s, uint32_t h, uint32_t parent) { if (s.nlive >= 8) return fail("too many objects");
        s.live[s.nlive] = h; s.parent[s.nlive] = parent; ++s.nlive; ++r.allocated; return true; }
    bool alloc(GrScope& s, uint32_t parent, uint32_t h, uint32_t cls, void* p, uint32_t n, const char* what) {
        const bool ok = api.Alloc(s.base, parent, h, cls, p, n);
        if (trace) trace("alloc", h, cls, ok ? 0 : (api.status ? api.status : 0xffffffffu));
        if (!ok) return fail(what);
        return track(s, h, parent);
    }
    bool zero(uint64_t a, uint64_t bytes) { for (uint64_t o = 0; o < bytes; o += 4) vram.wr32(a + o, 0); return true; }
    template <class F> bool builder(GrScope& s, F f) {
        M2PtMem<VramIo> ptm{vram, arena};
        GmmuBuilder<M2PtMem<VramIo>> b{ptm, *c.gmmu};
        if (s.root) { b.Adopt(s.root, Aperture::Video); b.lpt_page = s.lpt_page; b.lpt_off = s.lpt_off; b.lpt_aper = s.lpt_aper; }
        else if (!b.Init()) return fail("GMMU root");
        const bool ok = f(b);
        s.root = b.root; s.lpt_page = b.lpt_page; s.lpt_off = b.lpt_off; s.lpt_aper = b.lpt_aper; s.pt_pages += ptm.pages;
        if (!ok) return fail(b.err ? b.err : "GMMU");
        return true;
    }

    // client, device, subdevice, VAS (+ page directory hand-over), GR0 channel
    bool Open(GrScope& s, const M2Sys& mthdbuf, uint32_t mthdbuf_size) {
        return Open(s, mthdbuf, mthdbuf_size, [](auto&) { return true; });
    }
    // premap(builder): mappings made before the VA space is bound (no TLB invalidate needed)
    template <class Premap>
    bool Open(GrScope& s, const M2Sys& mthdbuf, uint32_t mthdbuf_size, Premap premap) {
        namespace rm = rm570;
        {
            rm::NV0000_ALLOC_PARAMETERS a = {};
            a.hClient = s.base; a.processID = ~0u;
            const char* n = "brook-gr"; for (uint32_t i = 0; n[i]; ++i) a.processName[i] = n[i];
            if (!api.Alloc(s.base, 0, s.base, rm::NV01_ROOT, &a, sizeof(a))) return fail("RM_ALLOC client");
            if (!track(s, s.base, 0)) return false;
        }
        { rm::NV0080_ALLOC_PARAMETERS d = {}; d.hClientShare = s.base;
          if (!alloc(s, s.base, s.base + grh::kDevice, rm::NV01_DEVICE_0, &d, sizeof(d), "RM_ALLOC device")) return false; }
        { rm::NV2080_ALLOC_PARAMETERS sd = {};
          if (!alloc(s, s.base + grh::kDevice, s.base + grh::kSubdevice, rm::NV20_SUBDEVICE_0, &sd, sizeof(sd), "RM_ALLOC subdevice")) return false; }
        if (!builder(s, [](auto&) { return true; })) return false;
        if (!builder(s, premap)) return false;
        {
            rm::NV_VASPACE_ALLOCATION_PARAMETERS v = {};
            v.index = rm::NV_VASPACE_ALLOCATION_INDEX_GPU_NEW;
            v.flags = s.external ? rm::NV_VASPACE_ALLOCATION_FLAGS_IS_EXTERNALLY_OWNED : 0;
            if (!alloc(s, s.base + grh::kDevice, s.h_vas, rm::FERMI_VASPACE_A, &v, sizeof(v), "RM_ALLOC vaspace")) return false;
        }
        if (s.external) {
            rm::NV0080_CTRL_DMA_SET_PAGE_DIRECTORY_PARAMS pd = {};
            pd.physAddress = s.root; pd.numEntries = 1u << c.gmmu->lv[0].bits;
            pd.flags = rm::SET_PAGE_DIRECTORY_APERTURE_VIDMEM; pd.hVASpace = s.h_vas;
            if (!api.Control(s.base, s.base + grh::kDevice, rm::NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY, &pd, sizeof(pd)))
                return fail("SET_PAGE_DIRECTORY");
            s.unset_pd = true;
        } else {
            // RM's window [4 GiB, 4.5 GiB): hand RM the directory path down to its 512 MiB level.
            namespace g = rm::gr;
            static g::COPY_SERVER_RESERVED_PDES_PARAMS cp;
            __builtin_memset(&cp, 0, sizeof(cp));
            const uint32_t depth = 3;   // PD3, PD2, PD1 (PD1 entries cover 512 MiB)
            uint64_t t[4]; Aperture ap[4];
            if (!builder(s, [&](auto& b) { return b.Path(g::SPLIT_VAS_SERVER_RM_MANAGED_VA_START, depth, t, ap); })) return false;
            cp.pageSize = 1ull << c.gmmu->lv[depth - 1].shift;
            cp.virtAddrLo = g::SPLIT_VAS_SERVER_RM_MANAGED_VA_START;
            cp.virtAddrHi = g::SPLIT_VAS_SERVER_RM_MANAGED_VA_START + g::SPLIT_VAS_SERVER_RM_MANAGED_VA_SIZE - 1;
            cp.numLevelsToCopy = depth;
            for (uint32_t i = 0; i < depth; ++i) {
                cp.levels[i].physAddress = t[i];
                cp.levels[i].size = (1ull << c.gmmu->lv[i].bits) * c.gmmu->lv[i].entry_bytes;
                cp.levels[i].aperture = 1;   // video memory
                cp.levels[i].pageShift = c.gmmu->lv[i].shift;
            }
            if (!api.Control(s.base, s.h_vas, g::NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES, &cp, sizeof(cp)))
                return fail("COPY_SERVER_RESERVED_PDES");
        }
        // channel memory: instance block + USERD (VRAM), method buffer (sysmem)
        if (!arena.alloc(0x1000, 0x1000, &s.inst) || !arena.alloc(0x1000, 0x1000, &s.userd)) return fail("VRAM arena");
        zero(s.inst, 0x1000); zero(s.userd, 0x1000);
        static rm::NV_CHANNEL_ALLOC_PARAMS a;
        __builtin_memset(&a, 0, sizeof(a));
        a.gpFifoOffset = s.gpfifo_va; a.gpFifoEntries = 0x1000 / 8;    // golden/scrubber: 0, they never submit (nouveau)
        a.flags = (s.priv ? rm::NVOS04_PRIVILEGED_CHANNEL_TRUE : 0) | ((s.chid % 8) << rm::NVOS04_USERD_INDEX_VALUE_SHIFT) |
                  ((s.chid / 8) << rm::NVOS04_USERD_INDEX_PAGE_VALUE_SHIFT) | rm::NVOS04_USERD_INDEX_PAGE_FIXED_TRUE;
        a.hVASpace = s.h_vas;
        a.engineType = rm::gr::RM_ENGINE_TYPE_GR0;
        a.instanceMem = {s.inst, 0x1000, rm::ADDR_FBMEM, 1};
        a.userdMem = {s.userd, 0x200, rm::ADDR_FBMEM, 1};
        a.ramfcMem = {s.inst, 0x200, rm::ADDR_FBMEM, 1};
        if (g_grMthdbufVram) {
            uint64_t mv = 0; const uint64_t mb = (uint64_t(mthdbuf_size) + 0xfff) & ~0xfffull;
            if (!arena.alloc(mb, 0x1000, &mv)) return fail("VRAM arena (mthdbuf)");
            zero(mv, mb);
            a.mthdbufMem = {mv, mthdbuf_size, rm::ADDR_FBMEM, 1};
        } else {
            a.mthdbufMem = {mthdbuf.bus, mthdbuf_size, rm::ADDR_SYSMEM, 0};
        }
        a.internalFlags = (s.priv ? rm::KERNELCHANNEL_PRIVILEGE_ADMIN : 0) | (rm::KERNELCHANNEL_ERROR_NOTIFIER_NONE << 2) |
                          (rm::KERNELCHANNEL_ERROR_NOTIFIER_NONE << 4);
        return alloc(s, s.base + grh::kDevice, s.h_chan, rm::TURING_CHANNEL_GPFIFO_A, &a, sizeof(a), "RM_ALLOC GR channel");
    }

    // M5: the VA space alone (client, device, subdevice, external VAS + page directory), for a
    // renderD130 file; channels join it with ChannelInVas.
    bool OpenVas(GrScope& s) {
        namespace rm = rm570;
        {
            rm::NV0000_ALLOC_PARAMETERS a = {};
            a.hClient = s.base; a.processID = ~0u;
            const char* n = "brook-nvk"; for (uint32_t i = 0; n[i]; ++i) a.processName[i] = n[i];
            if (!api.Alloc(s.base, 0, s.base, rm::NV01_ROOT, &a, sizeof(a))) return fail("RM_ALLOC client");
            if (!track(s, s.base, 0)) return false;
        }
        { rm::NV0080_ALLOC_PARAMETERS d = {}; d.hClientShare = s.base;
          if (!alloc(s, s.base, s.base + grh::kDevice, rm::NV01_DEVICE_0, &d, sizeof(d), "RM_ALLOC device")) return false; }
        { rm::NV2080_ALLOC_PARAMETERS sd = {};
          if (!alloc(s, s.base + grh::kDevice, s.base + grh::kSubdevice, rm::NV20_SUBDEVICE_0, &sd, sizeof(sd), "RM_ALLOC subdevice")) return false; }
        if (!builder(s, [](auto&) { return true; })) return false;
        rm::NV_VASPACE_ALLOCATION_PARAMETERS v = {};
        v.index = rm::NV_VASPACE_ALLOCATION_INDEX_GPU_NEW; v.flags = rm::NV_VASPACE_ALLOCATION_FLAGS_IS_EXTERNALLY_OWNED;
        if (!alloc(s, s.base + grh::kDevice, s.h_vas, rm::FERMI_VASPACE_A, &v, sizeof(v), "RM_ALLOC vaspace")) return false;
        rm::NV0080_CTRL_DMA_SET_PAGE_DIRECTORY_PARAMS pd = {};
        pd.physAddress = s.root; pd.numEntries = 1u << c.gmmu->lv[0].bits;
        pd.flags = rm::SET_PAGE_DIRECTORY_APERTURE_VIDMEM; pd.hVASpace = s.h_vas;
        if (!api.Control(s.base, s.base + grh::kDevice, rm::NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY, &pd, sizeof(pd)))
            return fail("SET_PAGE_DIRECTORY");
        s.unset_pd = true;
        return true;
    }
    // A GR0 channel in an existing VAS (s.base/h_vas/root from the VAS scope; s.h_chan, s.chid,
    // s.gpfifo_va set by the caller). Objects allocated here are freed by Close(s).
    bool ChannelInVas(GrScope& s, const M2Sys& mthdbuf, uint32_t mthdbuf_size) {
        namespace rm = rm570;
        if (!arena.alloc(0x1000, 0x1000, &s.inst) || !arena.alloc(0x1000, 0x1000, &s.userd)) return fail("VRAM arena");
        zero(s.inst, 0x1000); zero(s.userd, 0x1000);
        static rm::NV_CHANNEL_ALLOC_PARAMS a;
        __builtin_memset(&a, 0, sizeof(a));
        a.gpFifoOffset = s.gpfifo_va; a.gpFifoEntries = 0x1000 / 8;
        a.flags = (s.priv ? rm::NVOS04_PRIVILEGED_CHANNEL_TRUE : 0) | ((s.chid % 8) << rm::NVOS04_USERD_INDEX_VALUE_SHIFT) |
                  ((s.chid / 8) << rm::NVOS04_USERD_INDEX_PAGE_VALUE_SHIFT) | rm::NVOS04_USERD_INDEX_PAGE_FIXED_TRUE;
        a.hVASpace = s.h_vas;
        a.engineType = rm::gr::RM_ENGINE_TYPE_GR0;
        a.instanceMem = {s.inst, 0x1000, rm::ADDR_FBMEM, 1};
        a.userdMem = {s.userd, 0x200, rm::ADDR_FBMEM, 1};
        a.ramfcMem = {s.inst, 0x200, rm::ADDR_FBMEM, 1};
        if (g_grMthdbufVram) {
            uint64_t mv = 0; const uint64_t mb = (uint64_t(mthdbuf_size) + 0xfff) & ~0xfffull;
            if (!arena.alloc(mb, 0x1000, &mv)) return fail("VRAM arena (mthdbuf)");
            zero(mv, mb);
            a.mthdbufMem = {mv, mthdbuf_size, rm::ADDR_FBMEM, 1};
        } else {
            a.mthdbufMem = {mthdbuf.bus, mthdbuf_size, rm::ADDR_SYSMEM, 0};
        }
        a.internalFlags = (s.priv ? rm::KERNELCHANNEL_PRIVILEGE_ADMIN : 0) | (rm::KERNELCHANNEL_ERROR_NOTIFIER_NONE << 2) |
                          (rm::KERNELCHANNEL_ERROR_NOTIFIER_NONE << 4);
        return alloc(s, s.base + grh::kDevice, s.h_chan, rm::TURING_CHANNEL_GPFIFO_A, &a, sizeof(a), "RM_ALLOC GR channel");
    }
    // PROMOTE_CTX (nouveau r535_gr_promote_ctx). golden: allocate every buffer (globals kept in bufs).
    bool Promote(GrScope& s, GrBufs& bufs, bool golden, uint32_t* entries) {
        namespace g = rm570::gr;
        static g::PROMOTE_CTX_PARAMS p;
        __builtin_memset(&p, 0, sizeof(p));
        p.engineType = 1; p.hChanClient = s.base; p.hObject = s.h_chan;
        for (uint32_t i = 0; i < bufs.n; ++i) {
            GrBuf& b = bufs.b[i];
            const bool alloc_now = golden || !b.global;
            g::PROMOTE_CTX_BUFFER_ENTRY& e = p.promoteEntry[p.entryCount];
            e.bufferId = b.id; e.bInitialize = b.init && alloc_now;
            uint64_t mem = 0;
            const uint64_t ps = 1ull << b.page, bytes = (uint64_t(b.size) + ps - 1) & ~(ps - 1);
            if (alloc_now) {
                if (!arena.alloc(bytes, ps, &mem)) return fail("VRAM arena (context buffer)");
                if (b.init) zero(mem, bytes);
                if (golden && b.global) b.vram = mem;
                if (b.id == g::BUF_PRIV_ACCESS_MAP) e.bNonmapped = 1;
            } else {
                if (b.id == g::BUF_UNRESTRICTED_PRIV_ACCESS_MAP) continue;
                mem = b.vram;
            }
            s.buf_vram[i] = mem;
            if (!e.bNonmapped) {
                const uint64_t al = 1ull << (b.align > b.page ? b.align : b.page);
                const uint64_t va = (s.va_next + al - 1) & ~(al - 1);
                if (!builder(s, [&](auto& gb) { return gb.Map(va, mem, bytes, b.page, Aperture::Video, 0, false, true, b.ro); }))
                    return false;
                s.va_next = va + bytes; s.buf_va[i] = va; e.gpuVirtAddr = va;
            }
            if (e.bInitialize) { e.gpuPhysAddr = mem; e.size = b.size; e.physAttr = g::kPhysAttrNouveau; }
            ++p.entryCount;
        }
        if (!Bar1FlushInvalidate(bus, c, s.root)) return fail("MMU invalidate");
        *entries = p.entryCount;
        if (!api.Control(s.base, s.base + grh::kSubdevice, g::NV2080_CTRL_CMD_GPU_PROMOTE_CTX, &p, sizeof(p)))
            return fail("PROMOTE_CTX");
        ++s.promoted;
        return true;
    }
    uint32_t Close(GrScope& s) {
        namespace rm = rm570;
        uint32_t freed = 0;
        while (s.nlive) {
            --s.nlive;
            const uint32_t h = s.live[s.nlive];
            if (h == s.h_vas && s.unset_pd) {
                rm::NV0080_CTRL_DMA_UNSET_PAGE_DIRECTORY_PARAMS u = {s.h_vas, 0};
                (void)api.Control(s.base, s.base + grh::kDevice, rm::NV0080_CTRL_CMD_DMA_UNSET_PAGE_DIRECTORY, &u, sizeof(u));
            }
            bool ok;
            if (h == s.base) ok = api.Free(s.base, 0, s.base);
            else ok = api.Free(s.base, s.parent[s.nlive], h);
            if (ok) ++freed;
            if (trace) trace("free", h, 0, ok ? 0 : (api.status ? api.status : 0xffffffffu));
        }
        r.freed += freed;
        return freed;
    }
    bool Threed(GrScope& s, uint32_t h) {
        if (!alloc(s, s.h_chan, h, rm570::gr::TURING_A, nullptr, 0, "RM_ALLOC TURING_A")) return false;
        s.h_3d = h; return true;
    }
    bool Compute(GrScope& s, uint32_t h) {
        return alloc(s, s.h_chan, h, rm570::gr::TURING_COMPUTE_A, nullptr, 0, "RM_ALLOC TURING_COMPUTE_A");
    }
    bool ScrubberWar(GrScope& s, bool teardown) {
        rm570::gr::INIT_BUG4208224_WAR_PARAMS w = {uint8_t(teardown ? 1 : 0)};
        if (!api.Control(s.base, s.base + grh::kSubdevice, rm570::gr::NV2080_CTRL_CMD_INTERNAL_KGR_INIT_BUG4208224_WAR, &w, sizeof(w)))
            return fail("INIT_BUG4208224_WAR");
        return true;
    }
};

// Step 2: context-buffer info, golden context, scrubber. `sysalloc(bytes, M2Sys*)` gives
// zeroed contiguous sysmem (method buffers). On success `st` keeps the global buffers and
// the scrubber scope (GrStop frees it).
constexpr uint32_t kGrGoldenChid = 1;   // RM 570: rsvd_chids = 1 (chid 0 is RM's); nouveau's golden channel
constexpr uint32_t kGrScrubberChid = 3; // P5/CE present use chid 2
template <class Bus, class Api, class VramIo, class SysAlloc>
inline GrResult RunGrGolden(Bus& bus, const ChipOps& c, VramIo& vram, Api& api, const StaticInfo& si, uint64_t vram_lo,
                            uint64_t vram_hi, SysAlloc sysalloc, GrState& st) {
    namespace rm = rm570;
    GrResult r;
    if (st.up) { r.status = "GR already up this boot"; return r; }
    if (!si.ok || !si.h_internal_client || !si.h_internal_subdevice) { r.status = "static info"; return r; }
    if (!VramRangeIsClient(si, vram_lo, vram_hi)) { r.status = "GR VRAM range not in a client FB region"; return r; }
    st.arena = VramArena{vram_lo, vram_hi, vram_lo};
    GrCtx<Bus, Api, VramIo> x{bus, api, vram, c, st.arena, r};
    auto done = [&]() -> GrResult& { r.ev = api.ev; r.vram_used = st.arena.next - vram_lo; return r; };

    r.stage = "info";
    {
        rm::NV2080_CTRL_CE_GET_FAULT_METHOD_BUFFER_SIZE_PARAMS m = {};
        if (!api.Control(si.h_internal_client, si.h_internal_subdevice, rm::NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE,
                         &m, sizeof(m)) || !m.size) { x.fail("method buffer size"); return done(); }
        st.mthdbuf_size = m.size;
        static rm::gr::GET_CONTEXT_BUFFERS_INFO_PARAMS info;
        __builtin_memset(&info, 0, sizeof(info));
        if (!api.Control(si.h_internal_client, si.h_internal_subdevice,
                         rm::gr::NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO, &info, sizeof(info))) {
            x.fail("KGR context buffer info"); return done(); }
        GrBuildBufs(info, st.bufs);
        if (st.bufs.n < 9) { r.status = "context buffer list incomplete"; r.bufs = st.bufs; return done(); }
    }

    r.stage = "golden";
    {
        GrScope g; g.base = grh::kGolden; g.chid = kGrGoldenChid; g.external = false; g.priv = true;
        g.h_vas = g.base + grh::kVaspace; g.h_chan = g.base + grh::kChannel;
        M2Sys mb;
        bool ok = sysalloc(st.mthdbuf_size, &mb) && mb.cpu;
        if (!ok) r.status = "method buffer alloc";
        ok = ok && x.Open(g, mb, st.mthdbuf_size);
        ok = ok && x.Promote(g, st.bufs, true, &r.golden_entries);
        ok = ok && x.Threed(g, g.base + grh::kThreed);   // RM builds and caches the golden context here
        r.golden_ok = ok;
        const char* keep = r.status;
        x.Close(g);   // nouveau frees the golden channel; RM keeps the context
        r.status = keep;
        if (!ok) { r.bufs = st.bufs; return done(); }
    }

    r.stage = "scrubber";
    {
        GrScope& s = st.scrub;
        s = GrScope{}; s.base = grh::kScrubber; s.chid = kGrScrubberChid; s.external = false; s.priv = false;
        s.h_vas = rm::gr::KGRAPHICS_SCRUBBER_HANDLE_VAS; s.h_chan = rm::gr::KGRAPHICS_SCRUBBER_HANDLE_CHANNEL;
        M2Sys mb;
        bool ok = sysalloc(st.mthdbuf_size, &mb) && mb.cpu;
        if (!ok) r.status = "method buffer alloc";
        ok = ok && x.Open(s, mb, st.mthdbuf_size);
        ok = ok && x.Promote(s, st.bufs, false, &r.scrubber_entries);
        ok = ok && x.Threed(s, rm::gr::KGRAPHICS_SCRUBBER_HANDLE_3DOBJ);
        ok = ok && x.ScrubberWar(s, false);
        r.scrubber_ok = ok;
        if (!ok) { const char* keep = r.status; x.Close(s); r.status = keep; r.bufs = st.bufs; return done(); }
    }
    st.up = true;
    r.stage = "done";
    r.bufs = st.bufs;
    return done();
}

// ---- Step 3: a user GR channel (externally owned VAS, like nouveau's client channels),
// TURING_A on subchannel 0, and clears of a pitch-linear A8R8G8B8 colour target.
namespace gr3d {   // TURING_A (NVIDIA clc597.h, MIT)
constexpr uint32_t SET_COLOR_TARGET_A0 = 0x0800, SET_COLOR_CLEAR_VALUE0 = 0x0d80, SET_WINDOW_OFFSET_X = 0x0df8,
                   SET_SCISSOR_ENABLE0 = 0x0e00, SET_SURFACE_CLIP_HORIZONTAL = 0x0ff4, SET_CLEAR_SURFACE_CONTROL = 0x10f8,
                   SET_CT_SELECT = 0x121c, SET_ZT_SELECT = 0x1538, SET_RENDER_ENABLE_C = 0x1558, CLEAR_SURFACE = 0x19d0;
constexpr uint32_t FORMAT_A8R8G8B8 = 0xcf, MEMORY_LAYOUT_PITCH = 1u << 12, CLEAR_RGBA = 0x3c, RENDER_ENABLE_TRUE = 1;
// block-linear: 1 GOB wide, 16 GOBs high (BLOCK_HEIGHT log2 4), layout bit 12 clear; PTE kind GENERIC_MEMORY
constexpr uint32_t kBlHeightLog2 = 4, MEMORY_BLOCK_LINEAR = kBlHeightLog2 << 4;
constexpr uint8_t kKindPitch = 0x00, kKindGeneric = 0x06;   // tu102 dev_mmu.h NV_MMU_PTE_KIND_*
}
// Byte offset of 32-bit pixel (x, y) in a block-linear surface (64 B x 8-row GOBs, 2^kBlHeightLog2
// GOBs per block, blocks row-major), as NIL lays out a single-level 2D image.
inline uint64_t GrBlOffset(uint32_t x, uint32_t y, uint32_t width) {
    const uint32_t xb = x * 4, gob_rows = 8u << gr3d::kBlHeightLog2;
    const uint32_t blocks_per_row = (width * 4 + 63) / 64;
    const uint64_t block = uint64_t(y / gob_rows) * blocks_per_row + xb / 64;
    const uint32_t gob = (y % gob_rows) / 8, yy = y % 8, xx = xb % 64;
    const uint32_t in = ((xx / 32) * 256) + ((yy / 2) * 64) + (((xx % 32) / 16) * 32) + ((yy % 2) * 16) + (xx % 16);
    return block * (512ull << gr3d::kBlHeightLog2) + gob * 512ull + in;
}
constexpr uint64_t kGrVaGp = 0x0000002800000000ull, kGrVaRt = 0x0000002900000000ull;
constexpr uint32_t kGrUserChid = 4;
struct GrClearPlan {
    M2Sys gpfifo, push, sem, mthdbuf; // sysmem pages (mthdbuf: the channel's fault-method buffer, unused by GR)
    uint32_t width = 3840, height = 2160, repeats = 8;
    bool block_linear = false;
    uint32_t hold_ms = 0;   // bisect: keep the channel alive this long before freeing it
    bool no_work = false;   // bisect: create the channel, submit nothing
};
struct GrClearResult {
    const char* status = "ok"; const char* step = "start";
    uint64_t rt_vram = 0; uint32_t token = 0;
    bool first_ok = false, verify_ok = false;
    uint32_t sample_got = 0, sample_want = 0;
    uint32_t first_us = 0, best_us = 0, sum_us = 0, n = 0;
    uint32_t events_rc = 0, events_mmu = 0;
    uint64_t bytes = 0;
    bool ok() const { return status[0] == 'o' && status[1] == 'k' && !status[2]; }
};
inline uint32_t GrF(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }

// A user GR channel session (steps 3 and 4): open the channel in its own external VAS with
// the GPFIFO/push/semaphore pages at kGrVaGp (+ `extra(b)` mappings), promote non-golden
// context buffers, allocate TURING_A (and TURING_COMPUTE_A when `compute`), bind + schedule,
// then run `body(submit)` where submit(words, n, &us) appends a WFI semaphore release, rings
// the doorbell and waits up to 2 s. Everything is freed before returning.
template <class Bus, class Api, class VramIo, class Sync, class Poll, class Extra, class Body>
inline GrResult GrUserSession(Bus& bus, const ChipOps& c, VramIo& vram, Api& api, GrState& st, const GrClearPlan& pl,
                              Sync& sync, Poll poll, bool compute, uint32_t* token, Extra extra, Body body) {
    namespace rm = rm570;
    namespace h = rm::host;
    GrResult r;
    r.stage = "user";
    GrCtx<Bus, Api, VramIo> x{bus, api, vram, c, st.arena, r};
    GrScope u; u.base = grh::kUser; u.chid = kGrUserChid; u.external = true; u.priv = true;
    u.h_vas = u.base + grh::kVaspace; u.h_chan = u.base + grh::kChannel; u.gpfifo_va = kGrVaGp;
    auto premap = [&](auto& b) {
        return b.Map(kGrVaGp, pl.gpfifo.bus, 0x1000, 12, Aperture::SysCoh, 0, true) &&
               b.Map(kGrVaGp + 0x1000, pl.push.bus, 0x1000, 12, Aperture::SysCoh, 0, true) &&
               b.Map(kGrVaGp + 0x2000, pl.sem.bus, 0x1000, 12, Aperture::SysCoh, 0, true) && extra(b);
    };
    auto close = [&]() -> GrResult& { const char* keep = r.status; x.Close(u); r.status = keep; r.ev = api.ev; return r; };
    if (!x.Open(u, pl.mthdbuf, st.mthdbuf_size, premap)) return close();
    uint32_t entries = 0;
    if (!x.Promote(u, st.bufs, false, &entries)) return close();
    if (!x.Threed(u, u.base + grh::kThreed)) return close();
    if (compute && !x.Compute(u, u.base + grh::kCompute)) return close();
    r.stage = "schedule";
    {
        rm::NVA06F_CTRL_BIND_PARAMS bp = {rm::gr::RM_ENGINE_TYPE_GR0};
        if (!api.Control(u.base, u.h_chan, rm::NVA06F_CTRL_CMD_BIND, &bp, sizeof(bp))) { x.fail("BIND"); return close(); }
        rm::NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS sp = {1, 0};
        if (!api.Control(u.base, u.h_chan, rm::NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &sp, sizeof(sp))) { x.fail("GPFIFO_SCHEDULE"); return close(); }
        rm::NVC36F_CTRL_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS tk = {};
        if (!api.Control(u.base, u.h_chan, rm::NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN, &tk, sizeof(tk))) {
            x.fail("work submit token"); return close(); }
        *token = tk.workSubmitToken;
    }
    uint32_t gp = 0, payload = 0x6a000000;
    auto submit = [&](const uint32_t* w, uint32_t n, uint32_t* us) {
        uint32_t* p = reinterpret_cast<uint32_t*>(pl.push.cpu);
        for (uint32_t i = 0; i < n; ++i) p[i] = w[i];
        const uint32_t want = ++payload;
        uint32_t q = n;
        p[q++] = h::IncHeader(0, h::SEM_ADDR_LO, 3);
        p[q++] = uint32_t(kGrVaGp + 0x2000); p[q++] = uint32_t((kGrVaGp + 0x2000) >> 32); p[q++] = want;
        p[q++] = h::IncHeader(0, h::SEM_EXECUTE, 1); p[q++] = h::SEM_EXECUTE_RELEASE | h::SEM_EXECUTE_RELEASE_WFI;
        uint32_t* g = reinterpret_cast<uint32_t*>(pl.gpfifo.cpu) + (gp % 512) * 2;
        const uint64_t pva = kGrVaGp + 0x1000;
        g[0] = uint32_t(pva); g[1] = uint32_t(pva >> 32) | (q << 10);
        sync();
        gp = (gp + 1) % 512;
        const uint32_t* sem = reinterpret_cast<const uint32_t*>(pl.sem.cpu);
        const uint64_t t0 = bus.now_us();
        vram.wr32(u.userd + h::USERD_GP_PUT, gp);
        bus.wr32(c.usermode + h::kDoorbell, *token);
        for (;;) {
            if (__atomic_load_n(&sem[0], __ATOMIC_ACQUIRE) == want) { *us = uint32_t(bus.now_us() - t0); return true; }
            if (bus.now_us() - t0 > 2000000 || api.ev.rc || api.ev.mmu_fault) { *us = uint32_t(bus.now_us() - t0); return false; }
            poll();
        }
    };
    r.stage = "run";
    body(r, submit);
    return close();
}

template <class Bus, class Api, class VramIo, class Sync, class Poll>
inline GrResult RunGrClear(Bus& bus, const ChipOps& c, VramIo& vram, Api& api, GrState& st, const GrClearPlan& pl,
                           Sync& sync, Poll poll, GrClearResult& cr) {
    namespace rm = rm570;
    namespace h = rm::host;
    GrResult r0;
    if (!st.up) { r0.status = "gr golden has not run"; return r0; }
    if (!pl.gpfifo.cpu || !pl.push.cpu || !pl.sem.cpu || !pl.mthdbuf.cpu) { r0.status = "sysmem"; return r0; }
    const uint32_t pitch = pl.width * 4;
    const uint32_t gob_rows = 8u << gr3d::kBlHeightLog2;
    const uint64_t bl_bytes = uint64_t((pitch + 63) & ~63u) * ((pl.height + gob_rows - 1) / gob_rows * gob_rows);
    const uint64_t need = ((pl.block_linear ? bl_bytes : uint64_t(pitch) * pl.height) + 0x1fffff) & ~0x1fffffull;
    if (st.rt_vram && st.rt_bytes < need) { r0.status = "colour target too small"; return r0; }
    if (!st.rt_vram) {
        const uint64_t both = ((bl_bytes > uint64_t(pitch) * pl.height ? bl_bytes : uint64_t(pitch) * pl.height) + 0x1fffff) & ~0x1fffffull;
        if (!st.arena.alloc(both, 0x200000, &st.rt_vram)) { r0.stage = "user"; r0.status = "VRAM arena (colour target)"; return r0; }
        st.rt_bytes = both;
    }
    cr.rt_vram = st.rt_vram;
    const uint64_t rt_bytes = st.rt_bytes;
    const uint8_t kind = pl.block_linear ? gr3d::kKindGeneric : gr3d::kKindPitch;
    auto extra = [&](auto& b) { return b.Map(kGrVaRt, cr.rt_vram, rt_bytes, 21, Aperture::Video, kind); };
    auto px_off = [&](uint32_t x, uint32_t y) {
        return pl.block_linear ? GrBlOffset(x, y, pl.width) : uint64_t(y) * pitch + x * 4ull; };
    auto body = [&](GrResult& r, auto& submit) {
        uint32_t w[64]; uint32_t n = 0;
        auto m1 = [&](uint32_t mthd, uint32_t v) { w[n++] = h::IncHeader(0, mthd, 1); w[n++] = v; };
        auto clear_words = [&](float rr, float gg, float bb, float aa) {
            w[n++] = h::IncHeader(0, gr3d::SET_COLOR_CLEAR_VALUE0, 4); w[n++] = GrF(rr); w[n++] = GrF(gg); w[n++] = GrF(bb); w[n++] = GrF(aa);
            m1(gr3d::CLEAR_SURFACE, gr3d::CLEAR_RGBA);
        };
        auto hold = [&] {   // busy-wait (under the nv-aux lock): RM events show up on the frees afterwards
            const uint64_t t0 = bus.now_us();
            while (bus.now_us() - t0 < uint64_t(pl.hold_ms) * 1000) bus.udelay(1000);
        };
        if (pl.no_work) { r.stage = "idle hold"; hold(); r.stage = "done"; return; }
        r.stage = "first clear";
        w[n++] = h::IncHeader(0, 0, 1); w[n++] = rm::gr::TURING_A;                          // SET_OBJECT, subchannel 0
        w[n++] = h::IncHeader(0, gr3d::SET_COLOR_TARGET_A0, 8);
        w[n++] = uint32_t(kGrVaRt >> 32); w[n++] = uint32_t(kGrVaRt);
        w[n++] = pl.block_linear ? pl.width : pitch; w[n++] = pl.height;   // WIDTH: pixels (block-linear) or bytes (pitch)
        w[n++] = gr3d::FORMAT_A8R8G8B8; w[n++] = pl.block_linear ? gr3d::MEMORY_BLOCK_LINEAR : gr3d::MEMORY_LAYOUT_PITCH;
        w[n++] = 1; w[n++] = 0;
        m1(gr3d::SET_CT_SELECT, 1);                       // one target: CT0
        m1(gr3d::SET_ZT_SELECT, 0);                       // no depth/stencil target
        w[n++] = h::IncHeader(0, gr3d::SET_SURFACE_CLIP_HORIZONTAL, 2); w[n++] = pl.width << 16; w[n++] = pl.height << 16;
        w[n++] = h::IncHeader(0, gr3d::SET_WINDOW_OFFSET_X, 2); w[n++] = 0; w[n++] = 0;
        m1(gr3d::SET_SCISSOR_ENABLE0, 0);
        m1(gr3d::SET_CLEAR_SURFACE_CONTROL, 0);
        m1(gr3d::SET_RENDER_ENABLE_C, gr3d::RENDER_ENABLE_TRUE);
        clear_words(1.0f, 0.0f, 1.0f, 1.0f);              // magenta: 0xffff00ff
        uint32_t us = 0;
        if (!submit(w, n, &us)) { r.status = api.ev.rc ? "RC during the first clear" : api.ev.mmu_fault ? "MMU fault during the first clear" : "first clear did not complete"; return; }
        cr.first_ok = true; cr.first_us = us;
        auto check = [&](uint32_t want) {
            const uint32_t xs[3] = {0, pl.width / 2, pl.width - 1}, ys[3] = {0, pl.height / 2, pl.height - 1};
            for (uint32_t i = 0; i < 3; ++i) for (uint32_t j = 0; j < 3; ++j) {
                const uint32_t got = vram.rd32(cr.rt_vram + px_off(xs[i], ys[j]));
                if (got != want) { cr.sample_got = got; cr.sample_want = want; return false; }
            }
            return true;
        };
        if (!check(0xffff00ffu)) { r.status = "first clear: wrong pixels"; return; }
        r.stage = "clears";
        for (uint32_t k = 0; k < pl.repeats; ++k) {
            n = 0;
            if (k & 1) clear_words(1.0f, 0.0f, 1.0f, 1.0f); else clear_words(0.0f, 1.0f, 0.0f, 1.0f);   // green 0xff00ff00
            if (!submit(w, n, &us)) { r.status = "clear did not complete"; return; }
            if (!cr.best_us || us < cr.best_us) cr.best_us = us;
            cr.sum_us += us; ++cr.n;
        }
        cr.bytes = uint64_t(pitch) * pl.height;
        cr.verify_ok = check((pl.repeats & 1) ? 0xff00ff00u : 0xffff00ffu);
        if (!cr.verify_ok) { r.status = "last clear: wrong pixels"; return; }
        if (pl.hold_ms) { r.stage = "hold after work"; hold(); }
        r.stage = "done";
    };
    GrResult r = GrUserSession(bus, c, vram, api, st, pl, sync, poll, false, &cr.token, extra, body);
    cr.events_rc = r.ev.rc; cr.events_mmu = r.ev.mmu_fault;
    return r;
}

// ---- Step 4: TURING_COMPUTE_A on subchannel 1 runs the NAK-compiled kernel (nak_m4_kernel.h:
// Mesa NAK, SM75, built by nix/nak-m4-kernel.nix) from a QMD NAK filled for fixed VAs. One 2 MiB
// VRAM block at kGrVaCompute holds code, CB0, QMD and the data array; CPU access is via PRAMIN.
namespace grc {   // TURING_COMPUTE_A (NVIDIA clc5c0.h, MIT)
constexpr uint32_t SET_OBJECT = 0x0000, INVALIDATE_SHADER_CACHES = 0x021c, INVALIDATE_SKED_CACHES = 0x0298,
                   SET_SHADER_SHARED_MEMORY_WINDOW_A = 0x02a0, SEND_PCAS_A = 0x02b4, SEND_SIGNALING_PCAS_B = 0x02bc,
                   SET_SHADER_LOCAL_MEMORY_WINDOW_A = 0x07b0;
constexpr uint32_t PCAS_INVALIDATE = 1, PCAS_SCHEDULE = 2;
constexpr uint32_t kSubch = 1;
// QMD V02_02 fields (clc3c0qmd.h): word index and bit range
constexpr uint32_t kQmdRasterWidthW = 12, kQmdVersionW = 18, kQmdCbValidW = 20, kQmdCb0LoW = 32, kQmdCb0HiW = 33,
                   kQmdProgLoW = 48, kQmdProgHiW = 49;
}
constexpr uint64_t kGrVaCompute = 0x0000002a00000000ull;
struct GrComputeLayout {   // byte offsets inside the 2 MiB block (the kernel header's VAs must agree)
    static constexpr uint64_t code = 0, cb0 = 0x10000, qmd = 0x20000, data = 0x100000;
};
struct GrComputeKernel {
    const uint32_t* code; uint32_t code_words;
    const uint32_t* qmd; uint32_t qmd_words;
    uint64_t shader_va, cb0_va;
    uint32_t invocations_built, local_size, store_value;
};
struct GrComputeResult {
    const char* status = "ok";
    uint32_t token = 0; uint64_t block_vram = 0;
    uint32_t invocations = 0, threads = 0;
    bool first_ok = false, verify_ok = false;
    uint32_t bad_index = 0, bad_got = 0, bad_want = 0;
    uint32_t first_us = 0, best_us = 0, sum_us = 0, n = 0;
    uint32_t events_rc = 0, events_mmu = 0;
};
constexpr uint32_t kGrComputeSentinel = 0x5e5e5e5eu;

// Checks the kernel header against the layout and the QMD fields Brook relies on.
inline const char* GrComputeCheck(const GrComputeKernel& k) {
    if (k.shader_va != kGrVaCompute + GrComputeLayout::code || k.cb0_va != kGrVaCompute + GrComputeLayout::cb0) return "kernel VAs";
    if (k.qmd_words != 64 || !k.code_words || k.code_words * 4ull > GrComputeLayout::cb0 - 0x1000) return "kernel sizes";
    const uint32_t* q = k.qmd;
    if ((q[grc::kQmdVersionW] & 0xff) != 0x22) return "QMD version";
    if (q[grc::kQmdProgLoW] != uint32_t(k.shader_va) || (q[grc::kQmdProgHiW] & 0x1ffff) != uint32_t(k.shader_va >> 32)) return "QMD program address";
    if (!(q[grc::kQmdCbValidW] & 1) || q[grc::kQmdCb0LoW] != uint32_t(k.cb0_va) || (q[grc::kQmdCb0HiW] & 0x1ffff) != uint32_t(k.cb0_va >> 32)) return "QMD CB0";
    if (((q[grc::kQmdVersionW] >> 16) & 0xffff) != k.local_size) return "QMD block size";
    if (q[grc::kQmdRasterWidthW] * k.local_size < k.invocations_built) return "QMD grid";
    return nullptr;
}

template <class Bus, class Api, class VramIo, class Sync, class Poll>
inline GrResult RunGrCompute(Bus& bus, const ChipOps& c, VramIo& vram, Api& api, GrState& st, const GrClearPlan& pl,
                             const GrComputeKernel& k, uint32_t invocations, Sync& sync, Poll poll, GrComputeResult& cr) {
    namespace h = rm570::host;
    GrResult r0;
    if (!st.up) { r0.status = "gr golden has not run"; return r0; }
    if (!pl.gpfifo.cpu || !pl.push.cpu || !pl.sem.cpu || !pl.mthdbuf.cpu) { r0.status = "sysmem"; return r0; }
    if (const char* e = GrComputeCheck(k)) { r0.status = e; return r0; }
    const uint32_t raster = k.qmd[grc::kQmdRasterWidthW];
    cr.threads = raster * k.local_size;
    if (!invocations || invocations > cr.threads) { r0.status = "invocations"; return r0; }
    cr.invocations = invocations;
    if (!st.arena.alloc(0x200000, 0x200000, &cr.block_vram)) { r0.stage = "user"; r0.status = "VRAM arena (compute block)"; return r0; }
    const uint64_t blk = cr.block_vram, data_va = kGrVaCompute + GrComputeLayout::data;
    // Code (+ 4 KiB of zeros for instruction prefetch), CB0, QMD, data (sentinel-filled).
    for (uint32_t i = 0; i < k.code_words; ++i) vram.wr32(blk + GrComputeLayout::code + i * 4ull, k.code[i]);
    for (uint32_t i = 0; i < 1024; ++i) vram.wr32(blk + GrComputeLayout::code + k.code_words * 4ull + i * 4ull, 0);
    const uint32_t cb0[64] = {uint32_t(data_va), uint32_t(data_va >> 32), 4, invocations};
    for (uint32_t i = 0; i < 64; ++i) vram.wr32(blk + GrComputeLayout::cb0 + i * 4ull, cb0[i]);
    for (uint32_t i = 0; i < 64; ++i) vram.wr32(blk + GrComputeLayout::qmd + i * 4ull, k.qmd[i]);
    auto fill = [&]() { for (uint32_t i = 0; i < cr.threads + 32; ++i) vram.wr32(blk + GrComputeLayout::data + i * 4ull, kGrComputeSentinel); };
    auto verify = [&]() {
        for (uint32_t i = 0; i < cr.threads + 32; ++i) {
            const uint32_t want = i < invocations ? k.store_value : kGrComputeSentinel;
            const uint32_t got = vram.rd32(blk + GrComputeLayout::data + i * 4ull);
            if (got != want) { cr.bad_index = i; cr.bad_got = got; cr.bad_want = want; return false; }
        }
        return true;
    };
    auto extra = [&](auto& b) { return b.Map(kGrVaCompute, blk, 0x200000, 21, Aperture::Video); };
    auto body = [&](GrResult& r, auto& submit) {
        uint32_t w[48]; uint32_t n = 0;
        auto m = [&](uint32_t mthd, uint32_t cnt) { w[n++] = h::IncHeader(grc::kSubch, mthd, cnt); };
        auto launch = [&]() {
            m(grc::INVALIDATE_SHADER_CACHES, 1); w[n++] = 0x1011;   // instruction + data + constant
            m(grc::INVALIDATE_SKED_CACHES, 1); w[n++] = 0;
            m(grc::SEND_PCAS_A, 1); w[n++] = uint32_t((kGrVaCompute + GrComputeLayout::qmd) >> 8);
            m(grc::SEND_SIGNALING_PCAS_B, 1); w[n++] = grc::PCAS_INVALIDATE | grc::PCAS_SCHEDULE;
        };
        r.stage = "first dispatch";
        m(grc::SET_OBJECT, 1); w[n++] = rm570::gr::TURING_COMPUTE_A;
        m(grc::SET_SHADER_SHARED_MEMORY_WINDOW_A, 2); w[n++] = 0; w[n++] = 0xfe000000u;   // as NVK
        m(grc::SET_SHADER_LOCAL_MEMORY_WINDOW_A, 2); w[n++] = 0; w[n++] = 0xff000000u;
        launch();
        fill();
        uint32_t us = 0;
        if (!submit(w, n, &us)) { r.status = api.ev.rc ? "RC during the first dispatch" : api.ev.mmu_fault ? "MMU fault during the first dispatch" : "first dispatch did not complete"; return; }
        cr.first_us = us;
        cr.first_ok = verify();
        if (!cr.first_ok) { r.status = "first dispatch: wrong data"; return; }
        r.stage = "dispatches";
        for (uint32_t i = 0; i < pl.repeats; ++i) {
            fill();
            n = 0; launch();
            if (!submit(w, n, &us)) { r.status = "dispatch did not complete"; return; }
            if (!cr.best_us || us < cr.best_us) cr.best_us = us;
            cr.sum_us += us; ++cr.n;
        }
        cr.verify_ok = verify();
        if (!cr.verify_ok) { r.status = "last dispatch: wrong data"; return; }
        r.stage = "done";
    };
    GrResult r = GrUserSession(bus, c, vram, api, st, pl, sync, poll, true, &cr.token, extra, body);
    cr.events_rc = r.ev.rc; cr.events_mmu = r.ev.mmu_fault;
    return r;
}

// Before RM unload (and on `gr off`): WAR teardown, then free the scrubber scope.
template <class Bus, class Api, class VramIo>
inline uint32_t GrStop(Bus& bus, const ChipOps& c, VramIo& vram, Api& api, GrState& st, GrResult& r) {
    if (!st.up) return 0;
    GrCtx<Bus, Api, VramIo> x{bus, api, vram, c, st.arena, r};
    (void)x.ScrubberWar(st.scrub, true);
    const uint32_t freed = x.Close(st.scrub);
    st.up = false;
    return freed;
}

}  // namespace brook::nvgsp
