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
constexpr uint32_t kM2Chid = 0;                        // nouveau-style USERD page 0 / index 0

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

struct M2Inputs {
    StaticInfo si;
    uint64_t vram_lo = 0, vram_hi = 0;        // VRAM reserved for M2 (inside a client-usable region)
    M2Sys gpfifo, push, sem, mthdbuf;         // sysmem pages; mthdbuf sized from RM's answer (allocated by caller later)
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
template <class Bus, class VramIo, class Waiter, class Doorbell, class MthdAlloc, class FenceWait, class Sync>
inline M2Result RunM2(Bus& bus, const ChipOps& c, VramIo& vram, const M2Inputs& in, RpcChannel* ch, uint8_t* rx,
                      uint32_t rxcap, Waiter& wait, Doorbell& bell, MthdAlloc need_mthdbuf, FenceWait fence_wait,
                      Sync& sync) {
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

    uint32_t live[8]; uint32_t parent[8]; uint32_t nlive = 0;
    auto alloc = [&](uint32_t hParent, uint32_t h, uint32_t cls, void* p, uint32_t n) {
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
    {
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
        a.flags = rm::NVOS04_PRIVILEGED_CHANNEL_TRUE | ((kM2Chid % 8) << rm::NVOS04_USERD_INDEX_VALUE_SHIFT) |
                  ((kM2Chid / 8) << rm::NVOS04_USERD_INDEX_PAGE_VALUE_SHIFT) | rm::NVOS04_USERD_INDEX_PAGE_FIXED_TRUE;
        a.hVASpace = 0;                                 // TSG channel: VAS comes from the TSG
        a.engineType = r.ce_engine;
        a.instanceMem = {r.inst, 0x1000, rm::ADDR_FBMEM, 1};
        a.userdMem = {r.userd, 0x200, rm::ADDR_FBMEM, 1};
        a.ramfcMem = {r.inst, 0x200, rm::ADDR_FBMEM, 1};
        a.mthdbufMem = {mb.bus, r.mthdbuf_size, rm::ADDR_SYSMEM, 0};
        a.internalFlags = rm::KERNELCHANNEL_PRIVILEGE_ADMIN | (rm::KERNELCHANNEL_ERROR_NOTIFIER_NONE << 2) |
                          (rm::KERNELCHANNEL_ERROR_NOTIFIER_NONE << 4);
        if (!alloc(m2h::kTsg, m2h::kChannel, rm::TURING_CHANNEL_GPFIFO_A, &a, sizeof(a))) return finish();
    }
    r.stage = "bind";
    {
        rm::NVA06F_CTRL_BIND_PARAMS bp = {r.ce_engine};
        if (!api.Control(m2h::kClient, m2h::kChannel, rm::NVA06F_CTRL_CMD_BIND, &bp, sizeof(bp))) { fail("BIND"); return finish(); }
        rm::NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS sp = {1, 0};
        if (!api.Control(m2h::kClient, m2h::kChannel, rm::NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &sp, sizeof(sp))) {
            // a TSG channel may need the group scheduled instead (NVA06C GPFIFO_SCHEDULE 0xa06c0101)
            rm::NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS tp = {1, 0};
            if (!api.Control(m2h::kClient, m2h::kTsg, 0xa06c0101, &tp, sizeof(tp))) { fail("GPFIFO_SCHEDULE"); return finish(); }
            r.schedule_on_tsg = true;
        }
    }
    r.token = (r.runlist << 16) | kM2Chid;
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
    r.stage = r.fence_ok ? "done" : r.stage;
    return finish();
}

} // namespace brook::nvgsp
