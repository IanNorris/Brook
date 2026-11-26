#pragma once
// M1 proof point P4: boot GSP-RM 570.144 to GSP_INIT_DONE (and the first
// RPC after it), plus the RM-aware fini (UNLOADING_GUEST_DRIVER, then
// FWSEC-SB and booter_unload). Host-testable over an abstract Bus, a DMA
// allocator and the RpcChannel; the live `nv-gsp p4` verb runs exactly this.
//
// Sysmem objects (nouveau r535_gsp_oneinit, tu102_gsp_oneinit/_init):
//   ELF .fwimage   copied page by page (any pages), mapped by a radix3 table
//   signature      .fwsignature_tu10x in a 256-byte-aligned buffer
//   bootloader     RISC-V bootloader payload (bin_hdr data)
//   WPR meta       GspFwWprMeta: booter's mailbox argument
//   libos args     LOGINIT/LOGINTR/LOGRM (64 KiB each, PTE array at +8) + RMARGS
//   RMARGS         GSP_ARGUMENTS_CACHED (r570: bDmemStack = 1)
//   shared memory  [PTE page][cmdq 256 KiB][msgq 256 KiB] (nv_gsp_queue.h)
// RPCs queued before boot (NOSEQ, no reply): GSP_SET_SYSTEM_INFO (0x3a0 B)
// and SET_REGISTRY, byte-identical to what nouveau sent on this card on
// 25 Nov except for the BAR addresses read from this boot's PCI config.
//
// Clean-room from Linux nouveau (MIT) master 6e4122f0:
//   rm/r535/gsp.c shared_init 1137-1186, libos_init 1510-1570, radix3 1659-1716,
//     oneinit 2155-2230, init 1805-1835, fini 1727-1786, unloading 331-352,
//     registry 413-555, msg_recv/rpc_poll (rpc.c 454-550)
//   rm/r570/gsp.c set_system_info 165-195, set_rmargs 197-222
//   tu102.c init 188-209, wpr_meta_init 211-259, fini 163-186
#include <stdint.h>
#include <stddef.h>
#include "nv_gsp_p3.h"
#include "nv_gsp_queue.h"
#include "nv_gsp_radix3.h"
#include "nv_gsp_seq.h"
#include "nv_gsp_static.h"

namespace brook::nvgsp {

struct DmaRegion { uint8_t* cpu = nullptr; uint64_t bus = 0; uint32_t bytes = 0; };

constexpr uint32_t kLogBytes = 0x10000;
constexpr uint32_t kMaxLvl2Pages = 32;          // 32 * 512 * 4 KiB = 64 MiB of image
constexpr uint32_t kSysInfoBytes = 0x3a0;       // r570 GspSystemInfo
constexpr uint32_t kStaticInfoBytes = 0x678;    // r570 GspStaticConfigInfo

enum LogId : uint8_t { kLogInit = 0, kLogIntr, kLogRm, kLogCount };

struct RmObjects {
    DmaRegion meta, libos, log[kLogCount], rmargs, shm, sig, boot, lvl0, lvl1;
    DmaRegion lvl2[kMaxLvl2Pages];
    uint32_t lvl2_n = 0;
    uint64_t image_bytes = 0, image_pages = 0;
    QueueShm q;
    uint32_t app_version = 0;
    WprMetaSysmem sm;
    uint64_t dma_bytes = 0;                      // total sysmem handed to the GPU
};

inline uint64_t LibosId8(const char* name) {
    uint64_t id = 0;
    for (int i = 0; i < 8 && *name; ++i, ++name) id = (id << 8) | uint8_t(*name);
    return id;
}

// Alloc: bool operator()(uint32_t bytes, DmaRegion* out) -> page-aligned,
// physically contiguous, zeroed. ImagePage: bool operator()(DmaRegion* out)
// for one 4 KiB image page (may be any page). Both must stay valid for the
// boot (nothing is freed).
template <class Alloc>
inline const char* BuildRmObjects(Alloc& alloc, const GspElf& elf, const RiscvBootloader& rbl, const FbLayout& L,
                                  RmObjects* o) {
    auto get = [&](uint32_t bytes, DmaRegion* r) {
        if (!alloc(bytes, r) || !r->cpu || (r->bus & 0xfff) || r->bytes < bytes) return false;
        o->dma_bytes += r->bytes;
        return true;
    };
    // ELF image + radix3
    o->image_bytes = elf.fwimage.n;
    const Radix3Plan plan = PlanRadix3(elf.fwimage.n);
    if (!plan.ok || plan.lvl2_pages > kMaxLvl2Pages) return "radix3 plan";
    o->image_pages = plan.image_pages;
    if (!get(0x1000, &o->lvl0) || !get(0x1000, &o->lvl1)) return "radix3 alloc";
    o->lvl2_n = uint32_t(plan.lvl2_pages);
    Radix3Page l2[kMaxLvl2Pages];
    for (uint32_t j = 0; j < o->lvl2_n; ++j) {
        if (!get(0x1000, &o->lvl2[j])) return "radix3 alloc";
        l2[j] = Radix3Page{reinterpret_cast<uint64_t*>(o->lvl2[j].cpu), o->lvl2[j].bus};
    }
    bool image_ok = true;
    auto page = [&](uint64_t i) -> uint64_t {
        DmaRegion p;
        if (!get(0x1000, &p)) { image_ok = false; return 0x1000; }   // aligned dummy; fails below
        const uint64_t off = i * 0x1000, n = elf.fwimage.n - off < 0x1000 ? elf.fwimage.n - off : 0x1000;
        CopyBytes(p.cpu, elf.fwimage.p + off, size_t(n));
        return p.bus;
    };
    if (!BuildRadix3(plan, Radix3Page{reinterpret_cast<uint64_t*>(o->lvl0.cpu), o->lvl0.bus},
                     Radix3Page{reinterpret_cast<uint64_t*>(o->lvl1.cpu), o->lvl1.bus}, l2, page) || !image_ok)
        return "radix3 build";
    // signature, bootloader
    const uint32_t sig_bytes = uint32_t((elf.signature.n + 255) & ~size_t(255));
    if (!elf.signature.n || !get(sig_bytes, &o->sig)) return "signature alloc";
    CopyBytes(o->sig.cpu, elf.signature.p, elf.signature.n);
    if (!rbl.payload.n || !get(uint32_t(rbl.payload.n), &o->boot)) return "bootloader alloc";
    CopyBytes(o->boot.cpu, rbl.payload.p, rbl.payload.n);
    o->app_version = rbl.desc.appVersion;
    // WPR meta
    if (!get(0x1000, &o->meta)) return "meta alloc";
    o->sm.radix3_lvl0 = o->lvl0.bus;
    o->sm.bootloader = o->boot.bus;
    o->sm.signature = o->sig.bus;
    o->sm.signature_size = sig_bytes;            // nouveau: gsp->sig.size = ALIGN(size, 256)
    o->sm.bl_code_offset = rbl.desc.monitorCodeOffset;
    o->sm.bl_data_offset = rbl.desc.monitorDataOffset;
    o->sm.bl_manifest_offset = rbl.desc.manifestOffset;
    FillWprMeta(L, o->sm, reinterpret_cast<rm570::GspFwWprMeta*>(o->meta.cpu));
    // shared memory (queues), RMARGS
    if (!get(QueueShmBytes(), &o->shm) || !InitQueueShm(o->shm.cpu, o->shm.bus, o->shm.bytes, &o->q)) return "shm";
    if (!get(0x1000, &o->rmargs)) return "rmargs alloc";
    rm570::GSP_ARGUMENTS_CACHED* a = reinterpret_cast<rm570::GSP_ARGUMENTS_CACHED*>(o->rmargs.cpu);
    a->messageQueueInitArguments.sharedMemPhysAddr = o->q.bus;
    a->messageQueueInitArguments.pageTableEntryCount = o->q.pte_count;
    a->messageQueueInitArguments.cmdQueueOffset = o->q.cmdq_off;
    a->messageQueueInitArguments.statQueueOffset = o->q.msgq_off;
    a->srInitArguments.oldLevel = 0; a->srInitArguments.flags = 0; a->srInitArguments.bInPMTransition = 0;
    a->bDmemStack = 1;
    // libos args + log buffers
    if (!get(0x1000, &o->libos)) return "libos alloc";
    rm570::LibosMemoryRegionInitArgument* args = reinterpret_cast<rm570::LibosMemoryRegionInitArgument*>(o->libos.cpu);
    static const char* const names[kLogCount] = {"LOGINIT", "LOGINTR", "LOGRM"};
    for (uint32_t i = 0; i < kLogCount; ++i) {
        if (!get(kLogBytes, &o->log[i])) return "log alloc";
        args[i].id8 = LibosId8(names[i]);
        args[i].pa = o->log[i].bus; args[i].size = kLogBytes;
        args[i].kind = rm570::LIBOS_MEMORY_REGION_CONTIGUOUS; args[i].loc = rm570::LIBOS_MEMORY_REGION_LOC_SYSMEM;
        uint64_t* pte = reinterpret_cast<uint64_t*>(o->log[i].cpu) + 1;     // [0] = put pointer
        for (uint32_t p = 0; p < kLogBytes / 0x1000; ++p) pte[p] = o->log[i].bus + uint64_t(p) * 0x1000;
    }
    args[3].id8 = LibosId8("RMARGS");
    args[3].pa = o->rmargs.bus; args[3].size = 0x1000;
    args[3].kind = rm570::LIBOS_MEMORY_REGION_CONTIGUOUS; args[3].loc = rm570::LIBOS_MEMORY_REGION_LOC_SYSMEM;
    return nullptr;
}

// ---------------------------------------------------------------- RPC payloads
struct PciIdentity {
    uint64_t bar0 = 0, bar1 = 0, bar3 = 0;       // BAR0 PRI, BAR1 FB, BAR3 instance (64-bit bases)
    uint8_t bus = 0, dev = 0, fn = 0;
    uint16_t vendor = 0, device = 0, subvendor = 0, subdevice = 0;
    uint8_t revision = 0;
};
// r570 GspSystemInfo, as r570_gsp_set_system_info fills it. Offsets checked
// against the 25 Nov Linux SET_SYSTEM_INFO dump (test_nv_gsp_p4). The ACPI
// block mirrors Linux: bValid = 1 with DOD/JT/MUX method status 0xffff
// (methods not evaluated). bIsPrimary stays 0 as on Linux.
inline void BuildSystemInfo(const PciIdentity& p, uint8_t out[kSysInfoBytes]) {
    ZeroBytes(out, kSysInfoBytes);
    Wr64(out + 0x00, p.bar0);                    // gpuPhysAddr
    Wr64(out + 0x08, p.bar1);                    // gpuPhysFbAddr
    Wr64(out + 0x10, p.bar3);                    // gpuPhysInstAddr
    Wr64(out + 0x20, (uint64_t(p.bus) << 8) | (uint64_t(p.dev) << 3) | p.fn);   // nvDomainBusDeviceFunc
    Wr64(out + 0x48, 0x00007ffffffff000ull);     // maxUserVa (x86-64 TASK_SIZE)
    Wr32(out + 0x50, 0x00088000);                // pciConfigMirrorBase (TU102 cfg mirror)
    Wr32(out + 0x54, 0x00001000);                // pciConfigMirrorSize
    Wr32(out + 0x58, (uint32_t(p.device) << 16) | p.vendor);
    Wr32(out + 0x5c, (uint32_t(p.subdevice) << 16) | p.subvendor);
    Wr32(out + 0x60, p.revision);
    out[0xa0] = 1;                               // acpiMethodData.bValid
    Wr32(out + 0xa4, 0xffff);                    // dodMethodData.status
    Wr32(out + 0xec, 0xffff);                    // jtMethodData.status
    Wr32(out + 0x33c, 0xffff);                   // muxMethodData... status
}

// PACKED_REGISTRY_TABLE {u32 size, u32 numEntries, entries[] {u32 nameOffset,
// u8 type (+3), u32 data, u32 length}} then the NUL-terminated keys. DWORD
// entries only (nouveau's three defaults).
struct RegistryDword { const char* key; uint32_t value; };
inline const RegistryDword kDefaultRegistry[] = {
    {"RMSecBusResetEnable", 1}, {"RMForcePcieConfigSave", 1}, {"RMDevidCheckIgnore", 1}};
// Extra DWORD keys appended after the defaults (live: /etc/nv-gsp-registry, "KEY VALUE" lines),
// for registry A/Bs such as RMElpg=0 (26 Nov GR idle fault). Keys point into g_extraRegKeys.
constexpr uint32_t kMaxExtraReg = 16, kRegBytes = 1024;
inline RegistryDword g_extraReg[kMaxExtraReg] = {};
inline uint32_t g_extraRegN = 0;
inline char g_extraRegKeys[kMaxExtraReg][48] = {};
inline uint32_t BuildRegistry(const RegistryDword* e, uint32_t n, uint8_t* out, uint32_t cap) {
    uint32_t str = 8 + 16 * n;
    if (str > cap) return 0;
    ZeroBytes(out, str);
    Wr32(out + 4, n);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* ent = out + 8 + 16 * i;
        Wr32(ent + 0, str); ent[4] = 1; Wr32(ent + 8, e[i].value); Wr32(ent + 12, 4);
        for (const char* k = e[i].key;; ++k) { if (str >= cap) return 0; out[str++] = uint8_t(*k); if (!*k) break; }
    }
    Wr32(out + 0, str);
    return str;
}

// ------------------------------------------------------------------ P4 run
struct P4Result {
    const char* stage = "preflight";
    BootStatus status = BootStatus::Ok;
    QStatus q = QStatus::Ok;
    P3Result p3;                     // preflight, layout, FWSEC-FRTS
    BooterRun booter;
    SeqResult seq;
    uint32_t seq_runs = 0, events = 0, nocat = 0, libos_print = 0, other_events = 0, last_other_fn = 0;
    bool init_done = false;
    uint64_t t_booter_us = 0, t_init_done_us = 0;      // from the arm point
    uint32_t static_len = 0;
    uint8_t static_head[64] = {};
    bool static_ok = false;
    uint8_t static_raw[kStaticInfoBytes] = {};    // the whole GspStaticConfigInfo reply payload
    StaticInfo si;                                 // parsed (nv_gsp_static.h)
    bool diag_valid = false;
    FalconDiag diag;
};

// Sequencer bus: register ops to `bus`, core ops to Brook's falcon model.
template <class Bus>
struct SeqBus {
    Bus& bus;
    GspCoreOps<Bus> ops;
    uint32_t rd32(uint32_t a) { return bus.rd32(a); }
    void wr32(uint32_t a, uint32_t v) { bus.wr32(a, v); }
    void udelay(uint32_t us) { bus.udelay(us); }
    uint64_t now_us() { return bus.now_us(); }
    void seq_step(uint32_t op) { (void)op; }
    bool core_reset() { return ops.core_reset(); }
    bool core_start() { return ops.core_start(); }
    bool core_wait_halt() { return ops.core_wait_halt(); }
    bool core_resume() { return ops.core_resume(); }
};

// Doorbell: GSP falcon queue head 0 (r535 rpc.c cmdq_push), after `sync()`.
template <class Bus, class Sync>
struct GspDoorbell {
    Bus& bus; const ChipOps& c; Sync& sync;
    void operator()() { sync(); bus.wr32(c.gsp_falcon + kFQueueHead0, 0); }
};

// `in` carries the VBIOS/acr_bl and the real ELF/bootloader sizes (layout);
// `b`/`booter_img` = booter_load with its signature patched in;
// `prepare_objects(layout)` builds RmObjects for that layout (returns a
// failure string or nullptr) and syncs them to memory; `arm()` as for P3;
// `wait` is the msgq waiter (bool operator()(), false = budget exhausted);
// `sync()` makes CPU writes visible to the GPU before each doorbell/boot.
template <class Bus, class Prepare, class Arm, class Waiter, class Sync>
inline P4Result RunP4(Bus& bus, const ChipOps& c, const P3Inputs& in, const DmaBuf& fwsec_dma, const PciIdentity& pci,
                      const BooterImage& b, const uint8_t* booter_img, RmObjects* o, RpcChannel* ch, uint8_t* rxbuf,
                      uint32_t rxcap, Prepare prepare_objects, Arm arm, Waiter& wait, Sync& sync) {
    P4Result r;
    P3Result& p = r.p3;
    if (!P3Prepare(bus, c, in, &p, true)) { r.stage = p.stage; r.status = p.status; return r; }
    AcrBl bl; const AcrBl* blp = nullptr;
    if (p.fwsec.version == 2) {
        if ((p.fw = ParseAcrBl(in.acr_bl, &bl)) != FwStatus::Ok) { r.stage = "acr_bl"; r.status = BootStatus::BadInput; return r; }
        blp = &bl;
    }
    if (!P3PatchInto<Bus>(p.fwsec, nvfw::kDmemMapperCmdFrts, p.layout, fwsec_dma, &p.patch, &p)) {
        r.stage = p.stage; r.status = p.status; return r;
    }
    r.stage = "objects";
    if (const char* f = prepare_objects(p.layout)) { r.stage = f; r.status = BootStatus::BadInput; return r; }
    ch->Attach(o->q);
    uint8_t sysinfo[kSysInfoBytes]; BuildSystemInfo(pci, sysinfo);
    RegistryDword all[3 + kMaxExtraReg];
    for (uint32_t i = 0; i < 3; ++i) all[i] = kDefaultRegistry[i];
    for (uint32_t i = 0; i < g_extraRegN && i < kMaxExtraReg; ++i) all[3 + i] = g_extraReg[i];
    uint8_t reg[kRegBytes]; const uint32_t reglen = BuildRegistry(all, 3 + (g_extraRegN < kMaxExtraReg ? g_extraRegN : kMaxExtraReg), reg, sizeof(reg));
    if (!reglen) { r.stage = "registry"; r.status = BootStatus::BadInput; return r; }
    r.stage = "arm";
    if (!arm()) { r.status = BootStatus::BadInput; return r; }
    const uint64_t t0 = bus.now_us();
    p.wrote = true;
    GspDoorbell<Bus, Sync> bell{bus, c, sync};
    // pre: the two queued RPCs (doorbells while the GSP falcon is idle)
    r.stage = "cmdq";
    bus.mark("pre", "cmdq_doorbell");
    if ((r.q = ch->Send(rm570::NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO, sysinfo, kSysInfoBytes, wait, bell, true)) != QStatus::Ok ||
        (r.q = ch->Send(rm570::NV_VGPU_MSG_FUNCTION_SET_REGISTRY, reg, reglen, wait, bell, true)) != QStatus::Ok) {
        r.status = BootStatus::BadInput; return r;
    }
    // FWSEC-FRTS
    r.stage = "fwsec";
    p.run = RunFwsecFrts(bus, c, p.fwsec, blp, fwsec_dma.cpu, fwsec_dma.bus, p.layout);
    if ((r.status = p.run.status) != BootStatus::Ok) {
        r.diag = CaptureFalconDiag(bus, c, c.gsp_falcon); r.diag_valid = true; return r;
    }
    // GSP into RISC-V mode with the libos args, then booter_load on SEC2
    r.stage = "gsp_riscv_reset";
    if ((r.status = ResetGspRiscv(bus, c, o->libos.bus)) != BootStatus::Ok) return r;
    sync();
    r.stage = "booter_load";
    r.booter = RunBooterLoad(bus, c, b, booter_img, o->meta.bus);
    r.t_booter_us = bus.now_us() - t0;
    if ((r.status = r.booter.status) != BootStatus::Ok) {
        r.diag = CaptureFalconDiag(bus, c, c.booter_falcon); r.diag_valid = true; return r;
    }
    r.stage = "handshake";
    if ((r.status = GspHandshake(bus, c, o->app_version)) != BootStatus::Ok) {
        r.diag = CaptureFalconDiag(bus, c, c.gsp_falcon); r.diag_valid = true; return r;
    }
    // INIT_DONE, serving RUN_CPU_SEQUENCER and dropping notices on the way
    r.stage = "init_done";
    SeqBus<Bus> sb{bus, GspCoreOps<Bus>{bus, c, o->libos.bus, o->app_version}};
    auto on_event = [&](const uint8_t* m, uint32_t len) -> bool {
        ++r.events;
        const uint32_t fn = Get32(m + offsetof(rm570::rpc_message_header_v, function));
        if (fn == rm570::NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER) {
            bus.mark("cpu_sequencer", "seq");
            r.seq = RunSequencer(sb, m + kRpcHdr, len - kRpcHdr);
            ++r.seq_runs;
            sync();
            return r.seq.status == SeqStatus::Ok;
        }
        if (fn == rm570::NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD) { ++r.nocat; return true; }
        if (fn == rm570::NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT) { ++r.libos_print; return true; }
        ++r.other_events; r.last_other_fn = fn;
        return true;
    };
    uint32_t len = 0;
    r.q = ch->WaitFor(rm570::NV_VGPU_MSG_EVENT_GSP_INIT_DONE, rxbuf, rxcap, &len, 0, wait, on_event);
    if (r.q != QStatus::Ok) {
        r.status = (r.seq_runs && r.seq.status != SeqStatus::Ok) ? BootStatus::FalconError : BootStatus::Timeout;
        r.diag = CaptureFalconDiag(bus, c, c.gsp_falcon); r.diag_valid = true; return r;
    }
    r.init_done = true;
    r.t_init_done_us = bus.now_us() - t0;
    // first RPC: GET_GSP_STATIC_INFO (r570_gsp_get_static_info), zero request
    r.stage = "static_info";
    bus.mark("postinit", "static_info");
    static uint8_t req[kStaticInfoBytes];
    for (uint32_t i = 0; i < kStaticInfoBytes; ++i) req[i] = 0;
    if ((r.q = ch->Send(rm570::NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO, req, kStaticInfoBytes, wait, bell)) != QStatus::Ok ||
        (r.q = ch->WaitFor(rm570::NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO, rxbuf, rxcap, &len, 0, wait, on_event)) !=
            QStatus::Ok) {
        r.status = BootStatus::Timeout; return r;
    }
    r.static_len = len;
    for (uint32_t i = 0; i < 64 && kRpcHdr + i < len; ++i) r.static_head[i] = rxbuf[kRpcHdr + i];
    r.static_ok = len >= kRpcHdr + kStaticInfoBytes;
    if (r.static_ok) {
        for (uint32_t i = 0; i < kStaticInfoBytes; ++i) r.static_raw[i] = rxbuf[kRpcHdr + i];
        (void)ParseStaticInfo(r.static_raw, kStaticInfoBytes, &r.si);
    }
    r.stage = "running";
    r.status = r.static_ok ? BootStatus::Ok : BootStatus::BadInput;
    return r;
}

// RM-aware fini (r535_gsp_fini + tu102_gsp_fini, not suspend):
// UNLOADING_GUEST_DRIVER {0, 0, level 0} with a reply, wait for GSP MAILBOX0
// = 0x80000000, then the P3b sequence (falcon reset inside BootFwsec,
// FWSEC-SB, booter_unload 0xff/0xff).
struct RmFiniResult {
    const char* stage = "unload_rpc";
    BootStatus status = BootStatus::Ok;
    QStatus q = QStatus::Ok;
    uint32_t mbox0_after = 0;
    bool halted = false;
    FiniResult fini;
};
template <class Bus, class Waiter, class Sync>
inline RmFiniResult RunRmUnload(Bus& bus, const ChipOps& c, RpcChannel* ch, uint8_t* rxbuf, uint32_t rxcap,
                                Waiter& wait, Sync& sync) {
    RmFiniResult f;
    bus.mark("fini", "unloading_guest_driver");
    uint8_t rpc[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // bInPMTransition, bGc6Entering, pad, newLevel = 0
    GspDoorbell<Bus, Sync> bell{bus, c, sync};
    auto drop = [](const uint8_t*, uint32_t) { return true; };
    uint32_t len = 0;
    if ((f.q = ch->Send(rm570::NV_VGPU_MSG_FUNCTION_UNLOADING_GUEST_DRIVER, rpc, sizeof(rpc), wait, bell)) != QStatus::Ok ||
        (f.q = ch->WaitFor(rm570::NV_VGPU_MSG_FUNCTION_UNLOADING_GUEST_DRIVER, rxbuf, rxcap, &len, 0, wait, drop)) !=
            QStatus::Ok) {
        f.status = BootStatus::Timeout; return f;
    }
    f.stage = "unload_wait";
    Falcon<Bus> g(bus, c.gsp_falcon);
    f.halted = g.Poll(c.gsp_falcon + kFMbox0, 0xffffffff, 0x80000000, 2000000);
    f.mbox0_after = g.rd(kFMbox0);
    if (!f.halted) f.status = BootStatus::Timeout;
    return f;
}

} // namespace brook::nvgsp
