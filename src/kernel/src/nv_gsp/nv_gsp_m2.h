#pragma once
// M2 / proof point P5: Brook's first RM objects and a host-method NOP plus
// semaphore release on one GPFIFO channel.
//
//   client     NV01_ROOT                    (r570 client.c: processID ~0)
//   device     NV01_DEVICE_0                (hClientShare = client)
//   subdevice  NV20_SUBDEVICE_0
//   queries    on RM's internal subdevice (static info handles): the FIFO
//              device-info table (pick a copy engine + its runlist) and the CE
//              fault-method-buffer size (nouveau r535_fifo_runl_ctor)
//   VRAM       Brook's own allocation from a client-usable FB region (the GSP
//              client owns that VRAM; RM's heap is in the reserved regions):
//              instance block 4 KiB, USERD 512 B, and the GMMU tables
//   GMMU       a GMMU v2 tree (VRAM, written through `VramIo`) mapping the
//              sysmem GPFIFO, push buffer and semaphore pages at kM2Va
//   vaspace    FERMI_VASPACE_A, externally owned, + NV0080 SET_PAGE_DIRECTORY
//              (nouveau r535_mmu_vaspace_new, external branch)
//   tsg        KEPLER_CHANNEL_GROUP_A {hVASpace, engineType}
//   channel    TURING_CHANNEL_GPFIFO_A under the TSG (hVASpace 0), RAMFC in the
//              instance block, USERD/inst in VRAM, mthdbuf in sysmem
//              (nouveau r570_chan_alloc), then NVA06F BIND + GPFIFO_SCHEDULE
//   submit     push = NOP; SEM_ADDR_LO/HI, SEM_PAYLOAD_LO; SEM_EXECUTE RELEASE
//              (nvif chanc36f sem_release), GPFIFO[0] -> push, USERD GP_PUT = 1,
//              doorbell usermode+0x90 = runlist << 16 | chid
//   fence      poll the semaphore dword in sysmem
//   teardown   free in reverse (channel, TSG, UNSET_PAGE_DIRECTORY, vaspace,
//              subdevice, device, client)
#include <stdint.h>
#include <stddef.h>
#include "nv_gsp_chip.h"
#include "nv_gsp_rmapi.h"
#include "nv_gsp_static.h"
#include "nv_gmmu.h"

namespace brook::nvgsp {

constexpr uint64_t kM2Va = 0x0000002000000000ull;     // 128 GiB: clear of 0 and RM's 4 GiB split-VAS window
constexpr uint32_t kM2Payload = 0x5eedf00d;
// ChID: RM 570 reserves chid 0 (nouveau r570 fifo rsvd_chids = 1, golden
// context on 1); P5 on 25 Nov got NV_ERR_NO_MEMORY for chid 0. USERD page =
// chid / 8, index = chid % 8 (nouveau r535_chan_alloc).
constexpr uint32_t kM2Chid = 2;

namespace m2h {   // handles (nouveau rm/handles.h style)
constexpr uint32_t kClient = 0xc1d0b500, kDevice = 0xde1d0000, kSubdevice = 0x5d1d0000, kVaspace = 0x90f10000,
                   kTsg = 0xa06c0000, kChannel = 0xf1f00000;
}

// Sysmem DMA page (zeroed, contiguous, 4 KiB-aligned bus address).
struct M2Sys { uint8_t* cpu = nullptr; uint64_t bus = 0; uint32_t bytes = 0; };

// Simple bump allocator over one VRAM range the caller reserved for M2.
struct VramArena {
    uint64_t lo = 0, hi = 0, next = 0;
    bool alloc(uint64_t bytes, uint64_t align, uint64_t* out) {
        uint64_t a = (next + align - 1) & ~(align - 1);
        if (a < lo || a + bytes > hi) return false;
        *out = a; next = a + bytes; return true;
    }
};

// GMMU memory for M2: tables in VRAM (VramIo: rd32/wr32 of a VRAM address).
template <class VramIo>
struct M2PtMem {
    VramIo& v;
    VramArena& arena;
    uint32_t pages = 0;
    bool alloc(uint64_t* addr, Aperture* aper) {
        if (!arena.alloc(0x1000, 0x1000, addr)) return false;
        for (uint32_t i = 0; i < 0x1000; i += 4) v.wr32(*addr + i, 0);
        *aper = Aperture::Video; ++pages;
        return true;
    }
    uint64_t rd64(Aperture, uint64_t a) { return v.rd32(a) | (uint64_t(v.rd32(a + 4)) << 32); }
    void wr64(Aperture, uint64_t a, uint64_t x) { v.wr32(a + 4, uint32_t(x >> 32)); v.wr32(a, uint32_t(x)); }
};

// A channel RunM2 leaves alive (the CE presenter): what later submissions,
// mappings and the eventual teardown need.
struct M2Keep {
    bool kept = false;
    uint32_t live[12] = {}, parent[12] = {}, nlive = 0; bool unset_pd = false;
    uint32_t gp = 0, payload = 0, token = 0, ce_engine = 0;
    uint64_t userd = 0, root = 0;
    VramArena arena;
    uint32_t pt_pages = 0, ptes = 0;
    uint64_t lpt_page = 0; uint32_t lpt_off = 0x1000; Aperture lpt_aper = Aperture::Video;
};

struct M2Inputs {
    M2Keep* keep = nullptr;                   // non-null: on success keep the objects (no teardown)
    StaticInfo si;
    uint64_t vram_lo = 0, vram_hi = 0;        // VRAM reserved for M2 (inside a client-usable region)
    M2Sys gpfifo, push, sem, mthdbuf;         // sysmem pages; mthdbuf sized from RM's answer (allocated by caller later)
    uint32_t chid = kM2Chid;
    bool parent_device = false;               // nouveau style: channel under the device with hVASpace, no explicit TSG
};

struct M2Result {
    const char* stage = "check";
    const char* status = "ok";
    uint32_t rm_status = 0, rpc_result = 0;
    QStatus q = QStatus::Ok;
    uint32_t ce_engine = 0, runlist = 0, devinfo_entries = 0, mthdbuf_size = 0;
    uint64_t inst = 0, userd = 0, pd3 = 0;
    uint32_t pt_pages = 0, ptes = 0;
    uint32_t token = 0, token_rm = 0;
    bool token_ctrl_ok = false, schedule_on_tsg = false;
    uint32_t allocated = 0, freed = 0;       // objects
    uint32_t sem_value = 0;
    uint64_t fence_us = 0;
    bool fence_ok = false;
    uint32_t gp_get = 0;
    RmEvents ev;
    bool ok() const { return status[0] == 'o' && status[1] == 'k' && !status[2]; }
};

// `need_mthdbuf(bytes, M2Sys*)` allocates the CE fault-method buffer once its size is known.
// `fence_wait()` is one bounded poll step (false = budget exhausted).
struct M2NoExtra {
    template <class B> bool premap(B&) { return true; }          // before SET_PAGE_DIRECTORY (no TLB invalidate needed)
    template <class Ctx> bool operator()(Ctx&) { return true; }  // on the live channel after the P5 fence
};

// `extra(ctx)` runs on the live channel after the P5 fence and before teardown (M3);
// ctx.alloc / ctx.map / ctx.submit are tracked like P5's own objects and freed with them.
template <class Bus, class VramIo, class Waiter, class Doorbell, class MthdAlloc, class FenceWait, class Sync,
          class Extra = M2NoExtra>
inline M2Result RunM2(Bus& bus, const ChipOps& c, VramIo& vram, const M2Inputs& in, RpcChannel* ch, uint8_t* rx,
                      uint32_t rxcap, Waiter& wait, Doorbell& bell, MthdAlloc need_mthdbuf, FenceWait fence_wait,
                      Sync& sync, Extra extra = Extra{}) {
    namespace rm = rm570;
    M2Result r;
    alignas(8) static uint8_t scratch[kRmApiBuf];
    RmApi<Waiter, Doorbell> api(ch, rx, rxcap, wait, bell, scratch);
    auto fail = [&](const char* what) {
        r.status = what; r.rm_status = api.status; r.rpc_result = api.rpc_result; r.q = api.q; return false;
    };
    const StaticInfo& si = in.si;
    if (!si.ok || !si.h_internal_client || !si.h_internal_subdevice) { r.status = "static info"; return r; }
    if (!in.gpfifo.cpu || !in.push.cpu || !in.sem.cpu) { r.status = "sysmem"; return r; }
    if (!VramRangeIsClient(si, in.vram_lo, in.vram_hi)) { r.status = "M2 VRAM range not in a client FB region"; return r; }

    uint32_t live[12]; uint32_t parent[12]; uint32_t nlive = 0;
    auto alloc = [&](uint32_t hParent, uint32_t h, uint32_t cls, void* p, uint32_t n) {
        if (nlive >= 12) return fail("too many objects");
        if (!api.Alloc(m2h::kClient, hParent, h, cls, p, n)) return fail("RM_ALLOC failed");
        live[nlive] = h; parent[nlive] = hParent; ++nlive; ++r.allocated; return true;
    };
    bool unset_pd = false;
    auto teardown = [&] {
        while (nlive) {
            --nlive;
            if (live[nlive] == m2h::kVaspace && unset_pd) {
                rm::NV0080_CTRL_DMA_UNSET_PAGE_DIRECTORY_PARAMS u = {m2h::kVaspace, 0};
                (void)api.Control(m2h::kClient, m2h::kDevice, rm::NV0080_CTRL_CMD_DMA_UNSET_PAGE_DIRECTORY, &u, sizeof(u));
            }
            if (live[nlive] == m2h::kClient) { if (api.Free(m2h::kClient, 0, m2h::kClient)) ++r.freed; }
            else if (api.Free(m2h::kClient, parent[nlive], live[nlive])) ++r.freed;
        }
    };
    auto finish = [&]() -> M2Result& { const char* keep = r.status; teardown(); r.status = keep; r.ev = api.ev; return r; };

    // ---- client / device / subdevice
    r.stage = "client";
    {
        rm::NV0000_ALLOC_PARAMETERS a = {};
        a.hClient = m2h::kClient; a.processID = ~0u;
        const char* n = "brook-m2"; for (uint32_t i = 0; n[i]; ++i) a.processName[i] = n[i];
        if (!api.Alloc(m2h::kClient, 0, m2h::kClient, rm::NV01_ROOT, &a, sizeof(a))) { fail("RM_ALLOC client"); return finish(); }
        live[nlive] = m2h::kClient; parent[nlive] = 0; ++nlive; ++r.allocated;
    }
    r.stage = "device";
    { rm::NV0080_ALLOC_PARAMETERS d = {}; d.hClientShare = m2h::kClient;
      if (!alloc(m2h::kClient, m2h::kDevice, rm::NV01_DEVICE_0, &d, sizeof(d))) return finish(); }
    r.stage = "subdevice";
    { rm::NV2080_ALLOC_PARAMETERS s = {};
      if (!alloc(m2h::kDevice, m2h::kSubdevice, rm::NV20_SUBDEVICE_0, &s, sizeof(s))) return finish(); }

    // ---- engine choice (internal subdevice)
    r.stage = "devinfo";
    {
        static rm::NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS t;
        __builtin_memset(&t, 0, sizeof(t));
        if (!api.Control(si.h_internal_client, si.h_internal_subdevice, rm::NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE, &t,
                         sizeof(t))) { fail("device info table"); return finish(); }
        r.devinfo_entries = t.numEntries;
        uint32_t gr_runlist = ~0u;
        for (uint32_t i = 0; i < t.numEntries && i < rm::kDevInfoMaxEntries; ++i)
            if (t.entries[i].engineData[rm::ENGINE_INFO_TYPE_RM_ENGINE_TYPE] == rm::RM_ENGINE_TYPE_GR0)
                gr_runlist = t.entries[i].engineData[rm::ENGINE_INFO_TYPE_RUNLIST];
        for (uint32_t i = 0; i < t.numEntries && i < rm::kDevInfoMaxEntries && !r.ce_engine; ++i) {
            const uint32_t e = t.entries[i].engineData[rm::ENGINE_INFO_TYPE_RM_ENGINE_TYPE];
            const uint32_t rl = t.entries[i].engineData[rm::ENGINE_INFO_TYPE_RUNLIST];
            if (e >= rm::RM_ENGINE_TYPE_COPY0 && e <= rm::RM_ENGINE_TYPE_COPY9 && rl != gr_runlist) { r.ce_engine = e; r.runlist = rl; }
        }
        if (!r.ce_engine) { r.status = "no async copy engine in the device info table"; return finish(); }
    }
    r.stage = "mthdbuf";
    {
        rm::NV2080_CTRL_CE_GET_FAULT_METHOD_BUFFER_SIZE_PARAMS p = {};
        if (!api.Control(si.h_internal_client, si.h_internal_subdevice, rm::NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE,
                         &p, sizeof(p))) { fail("CE fault method buffer size"); return finish(); }
        r.mthdbuf_size = p.size;
    }
    M2Sys mb;
    if (!r.mthdbuf_size || !need_mthdbuf(r.mthdbuf_size, &mb) || !mb.cpu) { r.status = "mthdbuf alloc"; return finish(); }

    // ---- VRAM: instance block, USERD, page tables
    r.stage = "vram";
    VramArena arena{in.vram_lo, in.vram_hi, in.vram_lo};
    if (!arena.alloc(0x1000, 0x1000, &r.inst) || !arena.alloc(0x1000, 0x1000, &r.userd)) { r.status = "VRAM arena"; return finish(); }
    for (uint32_t i = 0; i < 0x1000; i += 4) { vram.wr32(r.inst + i, 0); vram.wr32(r.userd + i, 0); }
    r.stage = "gmmu";
    M2PtMem<VramIo> ptm{vram, arena};
    GmmuBuilder<M2PtMem<VramIo>> b{ptm, *c.gmmu};
    if (!b.Init() ||
        !b.Map(kM2Va, in.gpfifo.bus, 0x1000, 12, Aperture::SysCoh, 0, true) ||
        !b.Map(kM2Va + 0x1000, in.push.bus, 0x1000, 12, Aperture::SysCoh, 0, true) ||
        !b.Map(kM2Va + 0x2000, in.sem.bus, 0x1000, 12, Aperture::SysCoh, 0, true)) {
        r.status = b.err ? b.err : "gmmu"; return finish();
    }
    if (!extra.premap(b)) { r.status = b.err ? b.err : "M3 mappings"; return finish(); }
    r.pd3 = b.root; r.pt_pages = ptm.pages; r.ptes = b.ptes;
    for (uint64_t o = 0; o < 0x3000; o += 0x1000) {
        const Translation t = GmmuWalk(ptm, *c.gmmu, b.root, b.root_aper, kM2Va + o + 8);
        const uint64_t want = (o == 0 ? in.gpfifo.bus : o == 0x1000 ? in.push.bus : in.sem.bus) + 8;
        if (!t.ok || t.pa != want || t.aper != Aperture::SysCoh) { r.status = "GMMU self-walk"; return finish(); }
    }

    // ---- VA space
    r.stage = "vaspace";
    {
        rm::NV_VASPACE_ALLOCATION_PARAMETERS v = {};
        v.index = rm::NV_VASPACE_ALLOCATION_INDEX_GPU_NEW;
        v.flags = rm::NV_VASPACE_ALLOCATION_FLAGS_IS_EXTERNALLY_OWNED;
        if (!alloc(m2h::kDevice, m2h::kVaspace, rm::FERMI_VASPACE_A, &v, sizeof(v))) return finish();
        rm::NV0080_CTRL_DMA_SET_PAGE_DIRECTORY_PARAMS pd = {};
        pd.physAddress = b.root;
        pd.numEntries = 1u << c.gmmu->lv[0].bits;
        pd.flags = rm::SET_PAGE_DIRECTORY_APERTURE_VIDMEM;
        pd.hVASpace = m2h::kVaspace;
        if (!api.Control(m2h::kClient, m2h::kDevice, rm::NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY, &pd, sizeof(pd))) {
            fail("SET_PAGE_DIRECTORY"); return finish();
        }
        unset_pd = true;
    }

    // ---- TSG + channel
    r.stage = "tsg";
    if (!in.parent_device) {
        rm::NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS g = {};
        g.hVASpace = m2h::kVaspace; g.engineType = r.ce_engine;
        if (!alloc(m2h::kDevice, m2h::kTsg, rm::KEPLER_CHANNEL_GROUP_A, &g, sizeof(g))) return finish();
    }
    r.stage = "channel";
    {
        static rm::NV_CHANNEL_ALLOC_PARAMS a;
        __builtin_memset(&a, 0, sizeof(a));
        a.gpFifoOffset = kM2Va;
        a.gpFifoEntries = 0x1000 / 8;
        a.flags = rm::NVOS04_PRIVILEGED_CHANNEL_TRUE | ((in.chid % 8) << rm::NVOS04_USERD_INDEX_VALUE_SHIFT) |
                  ((in.chid / 8) << rm::NVOS04_USERD_INDEX_PAGE_VALUE_SHIFT) | rm::NVOS04_USERD_INDEX_PAGE_FIXED_TRUE;
        a.hVASpace = in.parent_device ? m2h::kVaspace : 0;   // TSG channel: VAS comes from the TSG
        a.engineType = r.ce_engine;
        a.instanceMem = {r.inst, 0x1000, rm::ADDR_FBMEM, 1};
        a.userdMem = {r.userd, 0x200, rm::ADDR_FBMEM, 1};
        a.ramfcMem = {r.inst, 0x200, rm::ADDR_FBMEM, 1};
        a.mthdbufMem = {mb.bus, r.mthdbuf_size, rm::ADDR_SYSMEM, 0};
        a.internalFlags = rm::KERNELCHANNEL_PRIVILEGE_ADMIN | (rm::KERNELCHANNEL_ERROR_NOTIFIER_NONE << 2) |
                          (rm::KERNELCHANNEL_ERROR_NOTIFIER_NONE << 4);
        if (!alloc(in.parent_device ? m2h::kDevice : m2h::kTsg, m2h::kChannel, rm::TURING_CHANNEL_GPFIFO_A, &a, sizeof(a)))
            return finish();
    }
    r.stage = "bind";
    {
        rm::NVA06F_CTRL_BIND_PARAMS bp = {r.ce_engine};
        if (!api.Control(m2h::kClient, m2h::kChannel, rm::NVA06F_CTRL_CMD_BIND, &bp, sizeof(bp))) { fail("BIND"); return finish(); }
        rm::NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS sp = {1, 0};
        if (!api.Control(m2h::kClient, m2h::kChannel, rm::NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &sp, sizeof(sp))) {
            // a TSG channel may need the group scheduled instead (NVA06C GPFIFO_SCHEDULE 0xa06c0101)
            rm::NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS tp = {1, 0};
            if (in.parent_device || !api.Control(m2h::kClient, m2h::kTsg, 0xa06c0101, &tp, sizeof(tp))) {
                fail("GPFIFO_SCHEDULE"); return finish();
            }
            r.schedule_on_tsg = true;
        }
    }
    r.token = (r.runlist << 16) | in.chid;
    {
        rm::NVC36F_CTRL_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS tk = {};
        r.token_ctrl_ok = api.Control(m2h::kClient, m2h::kChannel, rm::NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN, &tk, sizeof(tk));
        r.token_rm = tk.workSubmitToken;
        if (r.token_ctrl_ok) r.token = tk.workSubmitToken;
    }

    // ---- submit: NOP + semaphore release
    r.stage = "submit";
    {
        namespace h = rm::host;
        uint32_t* p = reinterpret_cast<uint32_t*>(in.push.cpu);
        uint32_t n = 0;
        p[n++] = h::IncHeader(0, h::NOP, 1); p[n++] = 0;
        p[n++] = h::IncHeader(0, h::SEM_ADDR_LO, 3);
        p[n++] = uint32_t(kM2Va + 0x2000); p[n++] = uint32_t((kM2Va + 0x2000) >> 32); p[n++] = kM2Payload;
        p[n++] = h::IncHeader(0, h::SEM_EXECUTE, 1); p[n++] = h::SEM_EXECUTE_RELEASE;
        uint32_t* sem = reinterpret_cast<uint32_t*>(in.sem.cpu);
        sem[0] = 0;
        uint32_t* gp = reinterpret_cast<uint32_t*>(in.gpfifo.cpu);
        const uint64_t pva = kM2Va + 0x1000;
        gp[0] = uint32_t(pva);
        gp[1] = uint32_t(pva >> 32) | (n << 10);
        sync();
        vram.wr32(r.userd + h::USERD_GP_PUT, 1);
        bus.wr32(c.usermode + h::kDoorbell, r.token);
        r.stage = "fence";
        const uint64_t t0 = bus.now_us();
        while (!(r.fence_ok = __atomic_load_n(&sem[0], __ATOMIC_ACQUIRE) == kM2Payload)) {
            if (!fence_wait()) break;
        }
        r.fence_us = bus.now_us() - t0;
        r.sem_value = __atomic_load_n(&sem[0], __ATOMIC_ACQUIRE);
        r.gp_get = vram.rd32(r.userd + h::USERD_GP_GET);
        if (!r.fence_ok) r.status = "semaphore not released";
    }
    if (r.fence_ok) {
        // ---- M3 hook: more objects, mappings and submissions on the same channel
        struct Ctx {
            Bus& bus; const ChipOps& c; VramIo& vram; const M2Inputs& in; M2Result& r; GmmuBuilder<M2PtMem<VramIo>>& b; Sync& sync;
            FenceWait& poll; decltype(alloc)& alloc_fn; const RmEvents& ev;
            uint32_t gp = 1, payload = kM2Payload;
            uint32_t ce_engine() const { return r.ce_engine; }
            uint32_t channel() const { return m2h::kChannel; }
            bool alloc(uint32_t hParent, uint32_t h, uint32_t cls, void* p, uint32_t n) { return alloc_fn(hParent, h, cls, p, n); }
            // Push `n` words (+ a host semaphore release after WFI), ring, wait. us = doorbell -> semaphore.
            bool submit(const uint32_t* w, uint32_t n, uint32_t budget_us, uint64_t* us) {
                namespace h = rm570::host;
                if (n + 8 > 1024 || gp + 1 >= 0x1000 / 8) return false;
                uint32_t* p = reinterpret_cast<uint32_t*>(in.push.cpu);
                for (uint32_t i = 0; i < n; ++i) p[i] = w[i];
                const uint32_t want = ++payload;
                uint32_t k = n;
                p[k++] = h::IncHeader(0, h::SEM_ADDR_LO, 3);
                p[k++] = uint32_t(kM2Va + 0x2000); p[k++] = uint32_t((kM2Va + 0x2000) >> 32); p[k++] = want;
                p[k++] = h::IncHeader(0, h::SEM_EXECUTE, 1); p[k++] = h::SEM_EXECUTE_RELEASE | h::SEM_EXECUTE_RELEASE_WFI;
                uint32_t* g = reinterpret_cast<uint32_t*>(in.gpfifo.cpu) + gp * 2;
                const uint64_t pva = kM2Va + 0x1000;
                g[0] = uint32_t(pva); g[1] = uint32_t(pva >> 32) | (k << 10);
                sync();
                ++gp;
                uint32_t* sem = reinterpret_cast<uint32_t*>(in.sem.cpu);
                const uint64_t t0 = bus.now_us();
                vram.wr32(r.userd + h::USERD_GP_PUT, gp);
                bus.wr32(c.usermode + h::kDoorbell, r.token);
                for (;;) {
                    if (__atomic_load_n(&sem[0], __ATOMIC_ACQUIRE) == want) { *us = bus.now_us() - t0; return true; }
                    if (bus.now_us() - t0 > budget_us) { *us = bus.now_us() - t0; return false; }
                    (void)poll();
                }
            }
        } ctx{bus, c, vram, in, r, b, sync, fence_wait, alloc, api.ev};
        r.stage = "extra";
        if (!extra(ctx)) { if (r.ok()) r.status = "M3 step failed"; return finish(); }
        if (in.keep && r.ok()) {
            M2Keep& k = *in.keep;
            for (uint32_t i = 0; i < nlive; ++i) { k.live[i] = live[i]; k.parent[i] = parent[i]; }
            k.nlive = nlive; k.unset_pd = unset_pd; k.gp = ctx.gp; k.payload = ctx.payload; k.token = r.token;
            k.ce_engine = r.ce_engine; k.userd = r.userd; k.root = b.root; k.arena = arena;
            k.pt_pages = ptm.pages; k.ptes = b.ptes; k.lpt_page = b.lpt_page; k.lpt_off = b.lpt_off; k.lpt_aper = b.lpt_aper;
            k.kept = true;
            r.stage = "kept"; r.ev = api.ev;
            return r;
        }
    }
    r.stage = r.fence_ok ? "done" : r.stage;
    return finish();
}

// Free a kept channel's objects in reverse (as RunM2's teardown). Returns how many were freed.
template <class Api>
inline uint32_t M2FreeKept(M2Keep& k, Api& api) {
    namespace rm = rm570;
    uint32_t freed = 0;
    while (k.nlive) {
        --k.nlive;
        if (k.live[k.nlive] == m2h::kVaspace && k.unset_pd) {
            rm::NV0080_CTRL_DMA_UNSET_PAGE_DIRECTORY_PARAMS u = {m2h::kVaspace, 0};
            (void)api.Control(m2h::kClient, m2h::kDevice, rm::NV0080_CTRL_CMD_DMA_UNSET_PAGE_DIRECTORY, &u, sizeof(u));
        }
        if (k.live[k.nlive] == m2h::kClient) { if (api.Free(m2h::kClient, 0, m2h::kClient)) ++freed; }
        else if (api.Free(m2h::kClient, k.parent[k.nlive], k.live[k.nlive])) ++freed;
    }
    k.kept = false;
    return freed;
}

// One submission on a kept channel: `n` words + a host semaphore release after WFI,
// GPFIFO ring wrap, ring the doorbell, wait (each submission waits, so the push page
// is reused from offset 0). us = doorbell -> semaphore.
template <class Bus, class VramIo, class Sync, class Poll>
inline bool M2KeptSubmit(Bus& bus, const ChipOps& c, VramIo& vram, M2Keep& k, const M2Inputs& in, const uint32_t* w,
                         uint32_t n, uint32_t budget_us, uint64_t* us, Sync& sync, Poll poll) {
    namespace h = rm570::host;
    constexpr uint32_t kEntries = 0x1000 / 8;
    if (!k.kept || n + 8 > 1024) return false;
    uint32_t* p = reinterpret_cast<uint32_t*>(in.push.cpu);
    for (uint32_t i = 0; i < n; ++i) p[i] = w[i];
    const uint32_t want = ++k.payload;
    uint32_t q = n;
    p[q++] = h::IncHeader(0, h::SEM_ADDR_LO, 3);
    p[q++] = uint32_t(kM2Va + 0x2000); p[q++] = uint32_t((kM2Va + 0x2000) >> 32); p[q++] = want;
    p[q++] = h::IncHeader(0, h::SEM_EXECUTE, 1); p[q++] = h::SEM_EXECUTE_RELEASE | h::SEM_EXECUTE_RELEASE_WFI;
    const uint32_t slot = k.gp % kEntries;
    uint32_t* g = reinterpret_cast<uint32_t*>(in.gpfifo.cpu) + slot * 2;
    const uint64_t pva = kM2Va + 0x1000;
    g[0] = uint32_t(pva); g[1] = uint32_t(pva >> 32) | (q << 10);
    sync();
    k.gp = (k.gp + 1) % kEntries;
    const uint32_t* sem = reinterpret_cast<const uint32_t*>(in.sem.cpu);
    const uint64_t t0 = bus.now_us();
    vram.wr32(k.userd + h::USERD_GP_PUT, k.gp);
    bus.wr32(c.usermode + h::kDoorbell, k.token);
    for (;;) {
        if (__atomic_load_n(&sem[0], __ATOMIC_ACQUIRE) == want) { *us = bus.now_us() - t0; return true; }
        if (bus.now_us() - t0 > budget_us) { *us = bus.now_us() - t0; return false; }
        poll();
    }
}

} // namespace brook::nvgsp
