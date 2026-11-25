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
};

template <class Bus, class Api, class VramIo>
struct GrCtx {
    Bus& bus; Api& api; VramIo& vram; const ChipOps& c; VramArena& arena; GrResult& r;
    bool fail(const char* what) { r.status = what; r.rm_status = api.status; r.rpc_result = api.rpc_result; r.q = api.q; return false; }
    bool track(GrScope& s, uint32_t h, uint32_t parent) { if (s.nlive >= 8) return fail("too many objects");
        s.live[s.nlive] = h; s.parent[s.nlive] = parent; ++s.nlive; ++r.allocated; return true; }
    bool alloc(GrScope& s, uint32_t parent, uint32_t h, uint32_t cls, void* p, uint32_t n, const char* what) {
        if (!api.Alloc(s.base, parent, h, cls, p, n)) return fail(what);
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
        a.gpFifoOffset = 0; a.gpFifoEntries = 0x1000 / 8;    // nouveau: golden/scrubber never submit
        a.flags = (s.priv ? rm::NVOS04_PRIVILEGED_CHANNEL_TRUE : 0) | ((s.chid % 8) << rm::NVOS04_USERD_INDEX_VALUE_SHIFT) |
                  ((s.chid / 8) << rm::NVOS04_USERD_INDEX_PAGE_VALUE_SHIFT) | rm::NVOS04_USERD_INDEX_PAGE_FIXED_TRUE;
        a.hVASpace = s.h_vas;
        a.engineType = rm::gr::RM_ENGINE_TYPE_GR0;
        a.instanceMem = {s.inst, 0x1000, rm::ADDR_FBMEM, 1};
        a.userdMem = {s.userd, 0x200, rm::ADDR_FBMEM, 1};
        a.ramfcMem = {s.inst, 0x200, rm::ADDR_FBMEM, 1};
        a.mthdbufMem = {mthdbuf.bus, mthdbuf_size, rm::ADDR_SYSMEM, 0};
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
            if (h == s.base) { if (api.Free(s.base, 0, s.base)) ++freed; }
            else if (api.Free(s.base, s.parent[s.nlive], h)) ++freed;
        }
        r.freed += freed;
        return freed;
    }
    bool Threed(GrScope& s, uint32_t h) {
        if (!alloc(s, s.h_chan, h, rm570::gr::TURING_A, nullptr, 0, "RM_ALLOC TURING_A")) return false;
        s.h_3d = h; return true;
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
