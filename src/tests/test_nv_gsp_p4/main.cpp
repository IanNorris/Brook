// M1 P4 (booter_load -> GSP_INIT_DONE -> GET_GSP_STATIC_INFO) and the
// RM-aware fini (UNLOADING_GUEST_DRIVER, FWSEC-SB, booter_unload) against
// the recording fake register file plus a fake GSP-RM peer (fake_rm.h) that
// walks the real plumbing: mailbox -> libos -> RMARGS -> queues, WPR meta ->
// radix3 -> image. The live `nv-gsp p4` verb runs exactly nv_gsp_rm.h.
//
//   test_nv_gsp_p4 [--record raw.jsonl] [--vga 0x1|0x20e09]
//
// Real inputs: BROOK_NV_GSP_FW_DIR, BROOK_NV_GSP_ACR_BL, BROOK_NV_GSP_VBIOS,
// and optionally BROOK_NV_GSP_SEQ (the 25 Nov RUN_CPU_SEQUENCER payload,
// notes/gsp-m0-artifacts/seq-4098.bin) for a trace-diffable full replay.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include "nv_gsp/nv_gsp_rm.h"
#include "nv_gsp/nak_m4_kernel.h"
#include "nv_gsp/nv_gsp_bar1.h"
#include "nv_gsp/nv_gsp_m2.h"
#include "nv_gsp/nv_gsp_m3.h"
#include "nv_gsp/nv_gsp_ce_present.h"
#include "nv_gsp/nv_gsp_m4.h"
using brook::NvCeJob; using brook::NvCeRect;
#include "../nv_gsp_fake/fake_gpu.h"
#include "../nv_gsp_fake/fake_rm.h"
#include "../nv_gsp_fake/synth_fw.h"

using namespace brook::nvgsp;
using synth_fw::Bytes;

static Span S(const Bytes& b) { return Span{b.data(), b.size()}; }
static bool Load(const std::string& path, Bytes* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out->empty();
}
static const char* Env(const char* n) { const char* v = std::getenv(n); return (v && *v) ? v : nullptr; }

struct Inputs {
    bool real = false;
    Bytes rom, acr, elf, rbl, load, unload, seq;
};

// Page arena standing in for physically contiguous DMA memory.
struct Arena {
    std::vector<uint8_t> mem;
    uint8_t* base = nullptr;
    uint64_t bus = 0x110000000ull, used = 0, size = 0;
    explicit Arena(uint64_t bytes) : mem(bytes + 0x1000), size(bytes) {
        base = reinterpret_cast<uint8_t*>((uintptr_t(mem.data()) + 0xfff) & ~uintptr_t(0xfff));
    }
    bool operator()(uint32_t bytes, DmaRegion* r) {
        const uint64_t n = (uint64_t(bytes) + 0xfff) & ~uint64_t(0xfff);
        if (used + n > size) return false;
        r->cpu = base + used; r->bus = bus + used; r->bytes = uint32_t(n);
        std::memset(r->cpu, 0, n);
        used += n;
        return true;
    }
};

struct NoSync { void operator()() {} };

struct Rig {
    fake_gpu::FakeGpu gpu;
    fake_gpu::FakeRm rm{gpu};
    Arena arena;
    std::vector<uint8_t> fwsec_mem = std::vector<uint8_t>(16 * 4096);
    DmaBuf fwsec;
    P3Inputs in;
    GspElf elf; RiscvBootloader rbl; BooterImage booter, unload;
    std::vector<uint8_t> booter_img, unload_img;
    RmObjects objs;
    std::unique_ptr<RpcChannel> ch = std::make_unique<RpcChannel>();
    std::vector<uint8_t> rx = std::vector<uint8_t>(kMaxElemBytes * 2);
    PciIdentity pci;
    uint32_t budget = 200000, polls = 0;
    NoSync sync;
    Rig(const Inputs& I, uint32_t vga) : arena(I.real ? (48ull << 20) : (8ull << 20)) {
        gpu.reg[0x625f04] = vga;
        fwsec.cpu = fwsec_mem.data(); fwsec.bus = 0x109d30000ull; fwsec.bytes = fwsec_mem.size();
        gpu.MapDma(fwsec.bus, fwsec_mem.data(), fwsec_mem.size());
        gpu.MapDma(arena.bus, arena.base, arena.size);
        Vbios vb; FwsecImage f;
        assert(ParseVbios(S(I.rom), &vb) == FwStatus::Ok && ParseFwsec(vb, &f) == FwStatus::Ok);
        gpu.fwsec_interface_offset = f.interface_offset;
        assert(ParseGspElf(S(I.elf), &elf) == FwStatus::Ok && ParseRiscvBootloader(S(I.rbl), &rbl) == FwStatus::Ok);
        assert(ParseBooter(S(I.load), &booter) == FwStatus::Ok && ParseBooter(S(I.unload), &unload) == FwStatus::Ok);
        booter_img.resize(booter.image.n); unload_img.resize(unload.image.n);
        assert(BuildBooterImage(booter, booter_img.data(), booter_img.size()));
        in.vbios = S(I.rom); in.acr_bl = S(I.acr); in.elf_size = elf.fwimage.n; in.boot_size = rbl.payload.n;
        rm.elf_image = elf.fwimage.p; rm.elf_bytes = elf.fwimage.n;
        rm.signature = elf.signature.p; rm.signature_bytes = elf.signature.n;
        rm.bootloader = rbl.payload.p; rm.bootloader_bytes = rbl.payload.n;
        rm.seq_payload = I.seq;
        // the PCI identity Linux reported on 25 Nov
        pci.bar0 = 0xde000000; pci.bar1 = 0xc0000000; pci.bar3 = 0xd0000000; pci.bus = 2;
        pci.vendor = 0x10de; pci.device = 0x1e87; pci.subvendor = 0x19da; pci.subdevice = 0x3500; pci.revision = 0xa1;
    }
    bool Wait() { ++polls; return rm.Poll() && polls < budget; }
    template <class Arm>
    P4Result Run(Arm arm) {
        auto prepare = [&](const FbLayout& L) -> const char* { return BuildRmObjects(arena, elf, rbl, L, &objs); };
        struct W { Rig* r; bool operator()() { return r->Wait(); } } wait{this};
        return RunP4(gpu, kChipTu104, in, fwsec, pci, booter, booter_img.data(), &objs, ch.get(), rx.data(),
                     uint32_t(rx.size()), prepare, arm, wait, sync);
    }
    P4Result Run() { return Run([] { return true; }); }
    // ---- after INIT_DONE: BAR1 handover and M2 against the fake RM
    struct PageAlloc { Rig* r; bool vram = false; uint64_t next = 0x10ff00000ull;
        bool operator()(uint64_t* bus, uint8_t** cpu) {
            if (vram) { *bus = next; *cpu = nullptr; next += 0x1000; return true; }   // VRAM table pages (live default since 27 Nov)
            DmaRegion d; if (!r->arena(0x1000, &d)) return false; *bus = d.bus; *cpu = d.cpu; return true; }
        void flush(uint8_t*) {} };
    // CPU read through BAR1: the walk the BAR1 host translation does from the bound PDB.
    struct Bar1Cpu {
        Rig* r;
        struct Mem { fake_gpu::FakeGpu& g;
            uint64_t rd64(Aperture a, uint64_t x) { if (a == Aperture::Video) return g.Vram64(x);
                const uint8_t* d = g.Dma(x, 8); uint64_t v = 0; if (d) std::memcpy(&v, d, 8); return v; } };
        bool operator()(uint64_t off, uint32_t* v) {
            const uint32_t blk = r->gpu.reg[0xb80f40];
            if (!(blk & 0x80000000u)) { *v = r->gpu.Vram32(off); return true; }
            const uint64_t pdb = r->gpu.Vram64((uint64_t(blk & 0x0fffffff) << 12) + 0x200) & ~0xfffull;
            Mem m{r->gpu};
            const Translation t = GmmuWalk(m, kGmmuV2, pdb, Aperture::Video, off);
            if (!t.ok || t.aper != Aperture::Video) return false;   // a fault: write dropped / read garbage
            *v = r->gpu.Vram32(t.pa); return true;
        }
    };
    Bar1Map Bar1(uint64_t pde, bool vram = false) {
        PageAlloc pa{this}; pa.vram = vram;
        const uint64_t probes[3] = {0x0, 0x4343000, 0xff13000};   // P4's faulting offsets (GOP surface, a surface, instmem)
        return MapBar1Identity(gpu, kChipTu104, pa, pde, 256ull << 20, probes, 3, Bar1Cpu{this});
    }
    M2Result M2(uint32_t chid = kM2Chid, bool parent_device = false) {
        struct W { Rig* r; bool operator()() { return r->Wait(); } } wait{this};
        GspDoorbell<fake_gpu::FakeGpu, NoSync> bell{gpu, kChipTu104, sync};
        Pramin<fake_gpu::FakeGpu> vram(gpu, kChipTu104);
        M2Inputs in;
        in.si = last.si;
        in.vram_lo = 0x100000000ull; in.vram_hi = 0x100100000ull;
        in.chid = chid; in.parent_device = parent_device;
        auto sys = [&](M2Sys* s) { DmaRegion d; assert(arena(0x1000, &d)); s->cpu = d.cpu; s->bus = d.bus; s->bytes = d.bytes; };
        sys(&in.gpfifo); sys(&in.push); sys(&in.sem);
        auto mthd = [&](uint32_t bytes, M2Sys* s) { DmaRegion d; if (!arena(bytes, &d)) return false;
            s->cpu = d.cpu; s->bus = d.bus; s->bytes = d.bytes; return true; };
        auto fence = [&] { return Wait(); };
        M2Result m = RunM2(gpu, kChipTu104, vram, in, ch.get(), rx.data(), uint32_t(rx.size()), wait, bell, mthd, fence, sync);
        vram.Restore();
        return m;
    }
    // M3: P5 + the copy-engine sequence on the same channel (small surface: the arena is 8 MiB).
    M2Result M3(M3Plan& pl, M3Result& res) {
        struct W { Rig* r; bool operator()() { return r->Wait(); } } wait{this};
        GspDoorbell<fake_gpu::FakeGpu, NoSync> bell{gpu, kChipTu104, sync};
        Pramin<fake_gpu::FakeGpu> vram(gpu, kChipTu104);
        M2Inputs in;
        in.si = last.si;
        in.vram_lo = 0x100000000ull; in.vram_hi = 0x100100000ull;
        auto sys = [&](M2Sys* s) { DmaRegion d; assert(arena(0x1000, &d)); s->cpu = d.cpu; s->bus = d.bus; s->bytes = d.bytes; };
        sys(&in.gpfifo); sys(&in.push); sys(&in.sem);
        auto mthd = [&](uint32_t bytes, M2Sys* s) { DmaRegion d; if (!arena(bytes, &d)) return false;
            s->cpu = d.cpu; s->bus = d.bus; s->bytes = d.bytes; return true; };
        auto fence = [&] { return Wait(); };
        auto flush = [](uint8_t*, uint64_t) {};
        M3Extra<decltype(flush)> ex{pl, res, flush};
        budget = polls + 100000000;
        M2Result m = RunM2(gpu, kChipTu104, vram, in, ch.get(), rx.data(), uint32_t(rx.size()), wait, bell, mthd, fence, sync, ex);
        vram.Restore();
        return m;
    }
    // CE presenter: a kept channel with the copy object bound (`nv-gsp ce-present on`).
    M2Result CeOn(CePresent& cp) {
        struct W { Rig* r; bool operator()() { return r->Wait(); } } wait{this};
        GspDoorbell<fake_gpu::FakeGpu, NoSync> bell{gpu, kChipTu104, sync};
        Pramin<fake_gpu::FakeGpu> vram(gpu, kChipTu104);
        cp.in = M2Inputs{};
        cp.in.keep = &cp.k;
        cp.in.si = last.si;
        cp.in.vram_lo = 0x108000000ull; cp.in.vram_hi = 0x108800000ull;
        auto sys = [&](M2Sys* s) { DmaRegion d; assert(arena(0x1000, &d)); s->cpu = d.cpu; s->bus = d.bus; s->bytes = d.bytes; };
        sys(&cp.in.gpfifo); sys(&cp.in.push); sys(&cp.in.sem);
        auto mthd = [&](uint32_t bytes, M2Sys* s) { DmaRegion d; if (!arena(bytes, &d)) return false;
            s->cpu = d.cpu; s->bus = d.bus; s->bytes = d.bytes; return true; };
        auto fence = [&] { return Wait(); };
        M2Result m = RunM2(gpu, kChipTu104, vram, cp.in, ch.get(), rx.data(), uint32_t(rx.size()), wait, bell, mthd, fence, sync, CeExtra{});
        vram.Restore();
        cp.active = m.ok() && cp.k.kept;
        return m;
    }
    int CePresentOnce(CePresent& cp, const NvCeJob& job, const DmaRegion& bb, uint64_t* bytes) {
        Pramin<fake_gpu::FakeGpu> vram(gpu, kChipTu104);
        auto v2p = [&](uintptr_t a) -> uint64_t {
            const uintptr_t b = reinterpret_cast<uintptr_t>(bb.cpu);
            return a >= b && a < b + bb.bytes ? bb.bus + (a - b) : 0; };
        auto poll = [&] { (void)Wait(); };
        const int r = CePresentRun(gpu, kChipTu104, vram, cp, job, v2p, sync, poll, bytes);
        vram.Restore();
        return r;
    }
    uint32_t CeFree(CePresent& cp) {
        struct W { Rig* r; bool operator()() { return r->Wait(); } } wait{this};
        GspDoorbell<fake_gpu::FakeGpu, NoSync> bell{gpu, kChipTu104, sync};
        static uint8_t scratch[kRmApiBuf];
        RmApi<W, GspDoorbell<fake_gpu::FakeGpu, NoSync>> api(ch.get(), rx.data(), uint32_t(rx.size()), wait, bell, scratch);
        cp.active = false;
        return M2FreeKept(cp.k, api);
    }
    // M4 step 2: golden context + scrubber against the fake RM's GR model.
    struct GrApiW { Rig* r; bool operator()() { return r->Wait(); } };
    using GrApi = RmApi<GrApiW, GspDoorbell<fake_gpu::FakeGpu, NoSync>>;
    GrResult GrGolden(GrState& st) {
        GrApiW wait{this};
        GspDoorbell<fake_gpu::FakeGpu, NoSync> bell{gpu, kChipTu104, sync};
        static uint8_t scratch[kRmApiBuf];
        GrApi api(ch.get(), rx.data(), uint32_t(rx.size()), wait, bell, scratch);
        Pramin<fake_gpu::FakeGpu> vram(gpu, kChipTu104);
        auto sysalloc = [&](uint32_t bytes, M2Sys* s) { DmaRegion d; if (!arena(bytes, &d)) return false;
            s->cpu = d.cpu; s->bus = d.bus; s->bytes = d.bytes; return true; };
        GrResult g = RunGrGolden(gpu, kChipTu104, vram, api, last.si, 0x110000000ull, 0x120000000ull, sysalloc, st);
        vram.Restore();
        return g;
    }
    GrResult GrClear(GrState& st, GrClearPlan& pl, GrClearResult& cr) {
        GrApiW wait{this};
        GspDoorbell<fake_gpu::FakeGpu, NoSync> bell{gpu, kChipTu104, sync};
        static uint8_t scratch[kRmApiBuf];
        GrApi api(ch.get(), rx.data(), uint32_t(rx.size()), wait, bell, scratch);
        Pramin<fake_gpu::FakeGpu> vram(gpu, kChipTu104);
        auto sys = [&](M2Sys* s, uint32_t bytes) { DmaRegion d; assert(arena(bytes, &d)); s->cpu = d.cpu; s->bus = d.bus; s->bytes = d.bytes; };
        sys(&pl.gpfifo, 0x1000); sys(&pl.push, 0x1000); sys(&pl.sem, 0x1000); sys(&pl.mthdbuf, st.mthdbuf_size);
        auto poll = [&] { (void)Wait(); };
        GrResult g = RunGrClear(gpu, kChipTu104, vram, api, st, pl, sync, poll, cr);
        vram.Restore();
        return g;
    }
    GrResult GrCompute(GrState& st, GrClearPlan& pl, const GrComputeKernel& k, uint32_t inv, GrComputeResult& cr) {
        GrApiW wait{this};
        GspDoorbell<fake_gpu::FakeGpu, NoSync> bell{gpu, kChipTu104, sync};
        static uint8_t scratch[kRmApiBuf];
        GrApi api(ch.get(), rx.data(), uint32_t(rx.size()), wait, bell, scratch);
        Pramin<fake_gpu::FakeGpu> vram(gpu, kChipTu104);
        auto sys = [&](M2Sys* s, uint32_t bytes) { DmaRegion d; assert(arena(bytes, &d)); s->cpu = d.cpu; s->bus = d.bus; s->bytes = d.bytes; };
        sys(&pl.gpfifo, 0x1000); sys(&pl.push, 0x1000); sys(&pl.sem, 0x1000); sys(&pl.mthdbuf, st.mthdbuf_size);
        auto poll = [&] { (void)Wait(); };
        GrResult g = RunGrCompute(gpu, kChipTu104, vram, api, st, pl, k, inv, sync, poll, cr);
        vram.Restore();
        return g;
    }
    uint32_t GrDown(GrState& st, GrResult& g) {
        GrApiW wait{this};
        GspDoorbell<fake_gpu::FakeGpu, NoSync> bell{gpu, kChipTu104, sync};
        static uint8_t scratch[kRmApiBuf];
        GrApi api(ch.get(), rx.data(), uint32_t(rx.size()), wait, bell, scratch);
        Pramin<fake_gpu::FakeGpu> vram(gpu, kChipTu104);
        const uint32_t f = GrStop(gpu, kChipTu104, vram, api, st, g);
        vram.Restore();
        return f;
    }
    P4Result last;
    RmFiniResult Unload() {
        struct W { Rig* r; bool operator()() { return r->Wait(); } } wait{this};
        return RunRmUnload(gpu, kChipTu104, ch.get(), rx.data(), uint32_t(rx.size()), wait, sync);
    }
};

static size_t Writes(const fake_gpu::FakeGpu& g) { size_t n = 0; for (auto& a : g.log) n += a.op == 'W'; return n; }

// The 25 Nov Linux SET_SYSTEM_INFO / SET_REGISTRY payloads (non-zero bytes of
// the dmesg hex dump; nouveau.debug=gsp=trace).
static void CheckPayloads(const fake_gpu::FakeRm& rm) {
    struct NZ { uint32_t off; const char* hex; };
    static const NZ nz[] = {
        {0x00, "000000de00000000000000c000000000"}, {0x10, "000000d0"}, {0x20, "0002"},
        {0x48, "00f0ffffff7f0000"}, {0x50, "0080080000100000de10871eda190035"}, {0x60, "a1"},
        {0xa0, "01000000ffff"}, {0xec, "ffff"}, {0x33c, "ffff"}};
    std::vector<uint8_t> want(kSysInfoBytes, 0);
    for (const NZ& z : nz)
        for (size_t i = 0; z.hex[2 * i]; ++i) want[z.off + i] = uint8_t(std::strtoul(std::string(z.hex + 2 * i, 2).c_str(), nullptr, 16));
    assert(rm.sysinfo == want);
    static const char reg_hex[] =
        "7500000003000000380000000100000001000000040000004c00000001000000"
        "010000000400000062000000010000000100000004000000524d536563427573"
        "5265736574456e61626c6500524d466f72636550636965436f6e666967536176"
        "6500524d4465766964436865636b49676e6f726500";
    std::string h(reg_hex);
    std::vector<uint8_t> reg;
    for (size_t i = 0; i + 1 < h.size(); i += 2) reg.push_back(uint8_t(std::strtoul(h.substr(i, 2).c_str(), nullptr, 16)));
    assert(rm.registry == reg && reg.size() == 0x75);
}

int main(int argc, char** argv) {
    const char* record = nullptr;
    uint32_t record_vga = 0x1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
        else if (!std::strcmp(argv[i], "--vga") && i + 1 < argc) record_vga = uint32_t(std::strtoul(argv[++i], nullptr, 0));
        else { std::fprintf(stderr, "usage: %s [--record raw.jsonl] [--vga 0x1|0x20e09]\n", argv[0]); return 2; }
    }
    Inputs I;
    const char* fwdir = Env("BROOK_NV_GSP_FW_DIR");
    const char* acr = Env("BROOK_NV_GSP_ACR_BL");
    const char* vbios = Env("BROOK_NV_GSP_VBIOS");
    I.real = fwdir && acr && vbios;
    if (I.real) {
        const std::string d(fwdir);
        assert(Load(vbios, &I.rom) && Load(acr, &I.acr) && Load(d + "/gsp-570.144.bin", &I.elf) &&
               Load(d + "/bootloader-570.144.bin", &I.rbl) && Load(d + "/booter_load-570.144.bin", &I.load) &&
               Load(d + "/booter_unload-570.144.bin", &I.unload));
        if (const char* seq = Env("BROOK_NV_GSP_SEQ")) assert(Load(seq, &I.seq));
    } else {
        I.rom = synth_fw::Vbios(true, 2).rom;
        I.acr = synth_fw::AcrBl();
        I.elf = synth_fw::Elf(28528288 / 64, 0x1000);
        I.rbl = synth_fw::RiscvBl();
        I.load = synth_fw::Booter();
        I.unload = synth_fw::Booter();
    }

    // ---- full boot, both layouts
    for (uint32_t vga : {0x1u, 0x20e09u}) {
        Rig r(I, vga);
        const P4Result& p = (r.last = r.Run());
        if (p.status != BootStatus::Ok)
            std::fprintf(stderr, "p4 vga=%x: %s at %s (q=%d fake=%s model=%s seq=%s)\n", vga, BootStatusName(p.status), p.stage,
                         int(p.q), r.rm.error.c_str(), r.gpu.model_error.c_str(), SeqStatusName(p.seq.status));
        assert(p.status == BootStatus::Ok && p.init_done && p.static_ok && std::strcmp(p.stage, "running") == 0);
        assert(p.seq_runs == 1 && p.nocat == 6 && p.other_events == 0 && r.rm.static_requests == 1);
        assert(r.gpu.fwsec_runs == 1 && r.gpu.booter_runs == 1 && r.gpu.sec2_resumes == 1);
        assert(p.static_len == kRpcHdr + kStaticInfoBytes && p.static_head[0] == 1 && p.static_head[1] == 8);
        // WPR2 after booter: from the WPR2 start to the end of FRTS
        assert(Wpr2RegToAddr(p.booter.wpr2_lo) == p.p3.layout.wpr2.addr);
        assert(Wpr2HiToEnd(p.booter.wpr2_hi) == p.p3.layout.frts.end());
        if (I.real) CheckPayloads(r.rm);
        if (record && vga == record_vga) assert(r.gpu.WriteRawLog(record));
        // ---- static info parse (fake reply mirrors the r570 layout)
        assert(p.si.ok && p.si.bar1_pde == fake_gpu::FakeRm::kBar1Pde && p.si.bar2_pde == fake_gpu::FakeRm::kBar2Pde);
        assert(p.si.uuid_ok && p.si.uuid[0] == 0xa0 && p.si.fb_regions == 3 && p.si.usable_hi == 0x1f7700000ull);
        assert(p.si.h_internal_subdevice == fake_gpu::FakeRm::kIntSubdevice && !std::strcmp(p.si.short_name, "TU104"));
        // ---- BAR1 handover: CPU writes fault after RM's switch, the identity map restores them
        r.rm.SetupBar1();
        r.gpu.SetVram32(0x0, 0x11223344); r.gpu.SetVram32(0x4343000, 0x55667788); r.gpu.SetVram32(0xff13000, 0x99aabbcc);
        { uint32_t v; assert(!Rig::Bar1Cpu{&r}(0x4343000, &v)); }
        const size_t inv0 = r.gpu.invalidates;
        const Bar1Map bm = r.Bar1(p.si.bar1_pde);
        if (!bm.ok()) std::fprintf(stderr, "bar1: %s at %s\n", bm.status, bm.stage);
        assert(bm.ok() && bm.committed && bm.invalidated && r.gpu.invalidates == inv0 + 1 && !bm.rm_tree);
        assert(bm.probes == 3 && bm.probes_equal == 3 && bm.page_shift == 16 && bm.ptes == 4096);
        assert(r.gpu.reg[0x1700] == 0x1fff0);                      // PRAMIN window restored
        for (uint64_t off = 0; off < (256ull << 20); off += 0x7654321) {
            uint32_t v; r.gpu.SetVram32(off & ~3ull, uint32_t(off)); assert(Rig::Bar1Cpu{&r}(off & ~3ull, &v) && v == uint32_t(off));
        }
        { Bar1Map again = r.Bar1(p.si.bar1_pde); assert(again.ok() && again.rm_tree && again.rm_tree_sys); }   // previous tree: sysmem
        assert(UnmapBar1Identity(r.gpu, kChipTu104, bm));
        {   // an RM-built VRAM tree with three 4 KiB pages at BAR1 0x80000 is found, counted and replaced
            struct VMem { fake_gpu::FakeGpu& g; uint64_t next = 0x1f7600000ull;
                bool alloc(uint64_t* a, Aperture* ap) { *a = next; next += 0x1000; *ap = Aperture::Video;
                    for (uint32_t i = 0; i < 0x1000; i += 4) g.SetVram32(*a + i, 0); return true; }
                uint64_t rd64(Aperture, uint64_t x) { return g.Vram64(x); }
                void wr64(Aperture, uint64_t x, uint64_t v) { g.SetVram32(x, uint32_t(v)); g.SetVram32(x + 4, uint32_t(v >> 32)); } } vm{r.gpu};
            GmmuBuilder<VMem> rmb{vm, kGmmuV2};
            rmb.Adopt(p.si.bar1_pde, Aperture::Video);
            assert(rmb.Map(0x80000, 0x1f7000000ull, 0x3000, 12, Aperture::Video));
            const Bar1Map rb = r.Bar1(p.si.bar1_pde);
            assert(rb.ok() && rb.rm_tree && !rb.rm_tree_sys && rb.rm_valid == 3 && rb.rm_first_va[0] == 0x80000 && rb.probes_equal == 3);
            assert(UnmapBar1Identity(r.gpu, kChipTu104, rb));
            assert(r.gpu.Vram64(p.si.bar1_pde) == rb.pd3_old[0]);
            r.gpu.SetVram32(p.si.bar1_pde, 0); r.gpu.SetVram32(p.si.bar1_pde + 4, 0);
        }
        { uint32_t v; assert(!Rig::Bar1Cpu{&r}(0x4343000, &v)); }
        {   // VRAM-resident identity tables: same view, every table page in VRAM, RM's PD3[0] restored after
            const Bar1Map vb = r.Bar1(p.si.bar1_pde, true);
            if (!vb.ok()) std::fprintf(stderr, "bar1 vram: %s at %s\n", vb.status, vb.stage);
            assert(vb.ok() && vb.committed && vb.probes == 3 && vb.probes_equal == 3 && vb.ptes == 4096);
            assert(((vb.pd3_new >> 1) & 3) == 1);                         // PD3[0] target: VRAM (GMMU v2 PDE aperture 2:1 = 1)
            for (uint64_t off = 0; off < (256ull << 20); off += 0x7654321) {
                uint32_t v; r.gpu.SetVram32(off & ~3ull, uint32_t(off) ^ 0x5a5a5a5au);
                assert(Rig::Bar1Cpu{&r}(off & ~3ull, &v) && v == (uint32_t(off) ^ 0x5a5a5a5au));
            }
            assert(UnmapBar1Identity(r.gpu, kChipTu104, vb));
            uint32_t v; assert(!Rig::Bar1Cpu{&r}(0x4343000, &v));
        }
        { Bar1Map bad = r.Bar1(p.si.bar1_pde + 0x1000); assert(!bad.ok() && !bad.committed && !std::strcmp(bad.stage, "inst_pdb")); }
        // ---- M2: objects, VRAM, GMMU, VAS, TSG, channel, NOP + semaphore
        const M2Result m = r.M2();
        if (!m.ok()) std::fprintf(stderr, "m2: %s at %s (rm 0x%x q %d fake=%s)\n", m.status, m.stage, m.rm_status, int(m.q), r.rm.error.c_str());
        assert(m.ok() && m.fence_ok && m.sem_value == kM2Payload && r.rm.sem_releases == 1 && r.rm.nops == 1);
        assert(m.ce_engine == rm570::RM_ENGINE_TYPE_COPY0 + 2 && m.runlist == 2 && m.token == 0x20002 && m.token_ctrl_ok);
        assert(m.gp_get == 1 && m.allocated == 6 && m.freed == 6 && r.rm.objs.empty() && m.mthdbuf_size == 0x1000 && !m.schedule_on_tsg);
        // ---- RM-aware fini: unload RPC, then FWSEC-SB + booter_unload clear WPR2
        const RmFiniResult u = r.Unload();
        assert(u.status == BootStatus::Ok && u.halted && u.mbox0_after == 0x80000000 && r.rm.unload_requests == 1);
        const FiniResult f = RunFini(r.gpu, kChipTu104, r.in, S(I.unload), r.fwsec, r.unload_img.data(), r.unload_img.size(),
                                     NoArm(), false);
        if (f.status != BootStatus::Ok) std::fprintf(stderr, "fini: %s at %s\n", BootStatusName(f.status), f.stage);
        assert(f.status == BootStatus::Ok && r.gpu.unload_runs == 1 && f.unload.wpr2_hi == 0);
        assert(r.rm.error.empty());
        std::printf("p4 vga=%x: OK, %llu KiB sysmem, %u polls, init_done at %llu us, events %u\n", vga,
                    (unsigned long long)(r.objs.dma_bytes >> 10), r.polls, (unsigned long long)p.t_init_done_us, p.events);
    }
    // ---- refusals and failures
    {
        Rig r(I, 0x1); r.gpu.reg[0x1fa828] = 0x01ffee00;           // WPR2 set: nothing written
        const P4Result p = r.Run();
        assert(p.status == BootStatus::Wpr2AlreadySet && Writes(r.gpu) == 0 && !r.objs.dma_bytes);
    }
    {
        Rig r(I, 0x1);                                             // arm refuses after the objects exist
        const P4Result p = r.Run([] { return false; });
        assert(std::strcmp(p.stage, "arm") == 0 && Writes(r.gpu) == 0 && r.objs.dma_bytes);
    }
    {
        Rig r(I, 0x1); r.arena.size = 64 << 10;                    // out of DMA memory: refused before any write
        const P4Result p = r.Run();
        assert(p.status == BootStatus::BadInput && Writes(r.gpu) == 0);
    }
    {
        Rig r(I, 0x1); r.rm.hang_before_init_done = true; r.budget = 5000;   // RM never reports INIT_DONE
        const P4Result p = r.Run();
        assert(p.status == BootStatus::Timeout && std::strcmp(p.stage, "init_done") == 0 && p.seq_runs == 1 && p.diag_valid);
    }
    {
        Rig r(I, 0x1); r.rm.static_result = 0x56;                  // RPC error on the first RPC
        const P4Result p = r.Run();
        assert(p.init_done && p.q == QStatus::RpcError && std::strcmp(p.stage, "static_info") == 0);
    }
    // ---- M2 variants
    {
        Rig r(I, 0x1); r.last = r.Run(); assert(r.last.init_done);
        r.rm.token_ctrl_supported = false; r.rm.channel_schedule_supported = false;
        const M2Result m = r.M2();
        assert(m.ok() && !m.token_ctrl_ok && m.token == 0x20002 && m.schedule_on_tsg && r.rm.objs.empty());
    }
    {   // chid 0 is RM's: NV_ERR_NO_MEMORY at the channel, as on hardware; nothing left behind
        Rig r(I, 0x1); r.last = r.Run(); assert(r.last.init_done);
        const M2Result m = r.M2(0);
        assert(!m.ok() && !std::strcmp(m.stage, "channel") && m.rm_status == 0x51 && r.rm.objs.empty());
        const M2Result again = r.M2(3, true);       // retry in the same boot, nouveau-style parent
        assert(again.ok() && again.allocated == 5 && again.token == 0x20003 && r.rm.sem_releases == 1 && r.rm.objs.empty());
    }
    {
        Rig r(I, 0x1); r.last = r.Run(); assert(r.last.init_done);
        r.rm.fail_class = rm570::TURING_CHANNEL_GPFIFO_A;
        const M2Result m = r.M2();
        assert(!m.ok() && !std::strcmp(m.stage, "channel") && m.rm_status == 0x1f && r.rm.objs.empty() && r.rm.sem_releases == 0);
        assert(r.gpu.doorbells.empty());
    }
    {
        Rig r(I, 0x1); r.last = r.Run(); assert(r.last.init_done);
        r.last.si.fb[0].reserved = 1;                              // M2 VRAM not client-owned: refused before any RPC
        const uint32_t before = r.rm.rpcs;
        const M2Result m = r.M2();
        assert(!m.ok() && !std::strcmp(m.stage, "check") && r.rm.rpcs == before);
    }
    // ---- M3: copy object, mappings, full/rect/present copies through the fake CE, verified
    {
        Rig r(I, 0x1); r.last = r.Run(); assert(r.last.init_done);
        M3Plan pl; pl.width = 320; pl.height = 180; pl.sx = 10; pl.sy = 7; pl.dx = 200; pl.dy = 100; pl.rw = 100; pl.rh = 60;
        pl.pw = 320; pl.ph = 50; pl.repeats = 2;
        const uint64_t surf = uint64_t(pl.width) * 4 * pl.height;
        DmaRegion a, b; assert(r.arena(uint32_t(surf), &a) && r.arena(uint32_t(surf), &b));
        pl.s = {a.cpu, a.bus, a.bytes}; pl.r = {b.cpu, b.bus, b.bytes};
        pl.v1 = 0x101000000ull; pl.v2 = 0x102000000ull;
        M3Result res;
        const M2Result m = r.M3(pl, res);
        if (!m.ok()) std::printf("M3: %s at %s / %s at %s host=%s\n", m.status, m.stage, res.status, res.step, r.rm.host_error.c_str());
        assert(m.ok() && res.ok() && res.object && res.verify_full && res.verify_rect);
        assert(m.allocated == 7 && m.freed == 7 && r.rm.objs.empty());
        // 4 timed kinds x 2 repeats + 1 V2->R read-back
        assert(r.rm.ce_launches == 4 * 2 + 1 + 2);
        assert(res.s2v.n == 2 && res.s2v.bytes == surf && res.present.bytes == uint64_t(320) * 4 * 50);
        assert(r.rm.sem_releases == 1 + r.rm.ce_launches);
        // V1 holds the pattern, V2's rectangle came from V1(10,7)
        assert(r.gpu.Vram32(0x101000000ull) == M3Pattern(0));
        assert(r.gpu.Vram32(0x102000000ull + 100ull * 1280 + 200 * 4) == M3Pattern(7ull * 320 + 10));
    }
    {   // the copy object is refused: P5 objects still freed, nothing launched
        Rig r(I, 0x1); r.last = r.Run(); assert(r.last.init_done);
        r.rm.fail_class = 0xc5b5;
        M3Plan pl; pl.width = 64; pl.height = 32; pl.sx = pl.sy = pl.dx = pl.dy = 0; pl.rw = 8; pl.rh = 8; pl.pw = 64; pl.ph = 8;
        DmaRegion a, b; assert(r.arena(64 * 4 * 32, &a) && r.arena(64 * 4 * 32, &b));
        pl.s = {a.cpu, a.bus, a.bytes}; pl.r = {b.cpu, b.bus, b.bytes}; pl.v1 = 0x101000000ull; pl.v2 = 0x102000000ull;
        M3Result res;
        const M2Result m = r.M3(pl, res);
        assert(!m.ok() && !res.object && r.rm.ce_launches == 0 && r.rm.objs.empty() && m.fence_ok);
    }
    // ---- CE present: kept channel, back-buffer mapping + remap, rect copies into VRAM, timeout, teardown
    {
        Rig r(I, 0x1); r.last = r.Run(); assert(r.last.init_done);
        CePresent cp;
        const M2Result m = r.CeOn(cp);
        if (!m.ok()) std::printf("CE on: %s at %s host=%s\n", m.status, m.stage, r.rm.host_error.c_str());
        assert(m.ok() && !std::strcmp(m.stage, "kept") && cp.k.kept && cp.k.nlive == 7 && r.rm.objs.size() == 7);
        assert(r.rm.ce_class == 0xc5b5 && r.rm.ce_launches == 0);
        const uint32_t W = 256, H = 64, stride = 320;
        DmaRegion bb; assert(r.arena(stride * H * 4, &bb));
        uint32_t* px = reinterpret_cast<uint32_t*>(bb.cpu);
        for (uint32_t i = 0; i < stride * H; ++i) px[i] = 0x10000000u + i;
        const NvCeRect rects[2] = {{10, 5, 74, 21}, {200, 40, 300, 80}};   // the second is clipped to 256x64
        NvCeJob job{px, stride, W, H, 0x400000, 1280, rects, 2};
        uint64_t bytes = 0;
        assert(r.CePresentOnce(cp, job, bb, &bytes) == 1);
        assert(bytes == 64ull * 4 * 16 + 56ull * 4 * 24 && cp.st.remaps == 1 && cp.st.frames == 1 && r.rm.ce_launches == 2);
        assert(r.gpu.Vram32(0x400000 + 5 * 1280 + 10 * 4) == 0x10000000u + 5 * stride + 10);
        assert(r.gpu.Vram32(0x400000 + 63 * 1280 + 255 * 4) == 0x10000000u + 63 * stride + 255);
        assert(r.gpu.Vram32(0x400000 + 4 * 1280 + 10 * 4) == 0);                         // outside the damage
        assert(r.CePresentOnce(cp, job, bb, &bytes) == 1 && cp.st.remaps == 1);        // same buffer: no remap
        // zero-copy step 2: a client layer (contiguous sysmem, XRGB with junk X bytes) copied last,
        // alpha forced through the component remap, over a shadow rect it overlaps
        {
            const uint32_t LW = 32, LH = 16, lstride = 48 * 4;
            DmaRegion lb; assert(r.arena(lstride * LH, &lb));
            uint32_t* lp = reinterpret_cast<uint32_t*>(lb.cpu);
            for (uint32_t i = 0; i < lstride / 4 * LH; ++i) lp[i] = 0x00a00000u + i;   // X byte 0
            NvCeJob lj = job;
            lj.layer_phys = lb.bus; lj.layer_stride = lstride; lj.layer_dst = NvCeRect{20, 8, int32_t(20 + LW), int32_t(8 + LH)};
            const size_t launches = r.rm.ce_launches;
            r.rm.ce_order.clear();
            { const int k = r.CePresentOnce(cp, lj, bb, &bytes);
              if (k != 1) std::fprintf(stderr, "layer kick %d: %s refused=%u bus=0x%llx host=%s\n", k, cp.last_error, cp.st.layer_refused,
                                      (unsigned long long)lb.bus, r.rm.host_error.c_str());
              assert(k == 1); }
            assert(r.rm.ce_launches == launches + 3 && r.rm.ce_remaps == 1 && cp.st.layer_copies == 1 && cp.st.layer_maps == 1);
            assert(bytes == 64ull * 4 * 16 + 56ull * 4 * 24 + uint64_t(LW) * 4 * LH);
            // the layer copy is the last launch, so it wins where it overlaps rect 0 (10,5)-(74,21)
            assert(r.rm.ce_order.back() == kCeVaVram + 0x400000 + 8 * 1280 + 20 * 4);
            assert(r.gpu.Vram32(0x400000 + 8 * 1280 + 20 * 4) == (0xff000000u | 0x00a00000u));
            assert(r.gpu.Vram32(0x400000 + (8 + 15) * 1280 + (20 + 31) * 4) == (0xff000000u | (0x00a00000u + 15 * 48 + 31)));
            assert(r.gpu.Vram32(0x400000 + 8 * 1280 + 19 * 4) == 0x10000000u + 8 * stride + 19);   // rect 0, just left of the layer
            assert(r.gpu.Vram32(0x400000 + 20 * 1280 + 19 * 4) == 0x10000000u + 20 * stride + 19);
            // the same BO again: its 2 MiB chunk is already mapped (no MMU invalidate)
            assert(r.CePresentOnce(cp, lj, bb, &bytes) == 1 && cp.st.layer_maps == 1 && cp.st.layer_copies == 2);
            // a layer-only job (no damage outside the hole) still copies the layer
            NvCeJob lo = lj; lo.n = 0;
            assert(r.CePresentOnce(cp, lo, bb, &bytes) == 1 && bytes == uint64_t(LW) * 4 * LH);
            // a layer rectangle outside the target is refused (the CPU copies the frame)
            NvCeJob bad2 = lj; bad2.layer_dst = NvCeRect{250, 60, 270, 70};
            assert(r.CePresentOnce(cp, bad2, bb, &bytes) == 0 && cp.st.layer_refused == 1 && !cp.broken);
        }
        // the compositor reallocates its back buffer: remapped at a fresh VA slot
        DmaRegion bb2; assert(r.arena(stride * H * 4, &bb2));
        uint32_t* px2 = reinterpret_cast<uint32_t*>(bb2.cpu);
        for (uint32_t i = 0; i < stride * H; ++i) px2[i] = 0x20000000u + i;
        NvCeJob job2{px2, stride, W, H, 0x600000, 1280, rects, 1};
        assert(r.CePresentOnce(cp, job2, bb2, &bytes) == 1 && cp.st.remaps == 2 && cp.slot == 2);
        assert(r.gpu.Vram32(0x600000 + 20 * 1280 + 73 * 4) == 0x20000000u + 20 * stride + 73);
        // a destination past the VRAM window is refused (CPU copies)
        NvCeJob bad{px2, stride, W, H, kCeVramBytes - 1280, 1280, rects, 1};
        assert(r.CePresentOnce(cp, bad, bb2, &bytes) == 0 && cp.st.refused == 1);
        // the engine stops answering: timeout -> -1, then the path stays off
        r.rm.host_stall = true;
        assert(r.CePresentOnce(cp, job2, bb2, &bytes) == -1 && cp.broken && cp.st.timeouts == 1);
        assert(r.CePresentOnce(cp, job2, bb2, &bytes) == 0);
        r.rm.host_stall = false;
        assert(r.CeFree(cp) == 7 && r.rm.objs.empty() && !cp.k.kept);
    }
    // ---- CE present: async kick/wait and the 2 MiB fast path for a contiguous, aligned source
    {
        Rig r(I, 0x1); r.last = r.Run(); assert(r.last.init_done);
        CePresent cp;
        assert(r.CeOn(cp).ok());
        // a 2 MiB-aligned, contiguous "shadow": the arena is contiguous; align inside it
        const uint32_t W = 512, H = 1024;                     // exactly 2 MiB
        DmaRegion big; assert(r.arena(4u << 20, &big));
        const uint64_t pad = (0x200000 - (big.bus & 0x1fffff)) & 0x1fffff;
        DmaRegion sh{big.cpu + pad, big.bus + pad, 2u << 20};
        // align the CPU pointer too: the fake's v2p is cpu->bus linear, so pick a cpu base with the same low bits
        uint32_t* px = reinterpret_cast<uint32_t*>(sh.cpu);
        for (uint32_t i = 0; i < W * H; ++i) px[i] = 0x30000000u + i;
        const NvCeRect rect[1] = {{0, 0, int32_t(W), int32_t(H)}};
        NvCeJob job{px, W, W, H, 0x800000, W * 4, rect, 1};
        Pramin<fake_gpu::FakeGpu> vram(r.gpu, kChipTu104);
        // v2p reports a 2 MiB-aligned physical base for the 2 MiB-aligned kernel VA we pretend the shadow has
        const uintptr_t kva = 0x40000000u;   // fake kernel VA, 2 MiB aligned
        NvCeJob jv = job; jv.src = reinterpret_cast<const uint32_t*>(kva);
        auto v2p = [&](uintptr_t a) -> uint64_t { return a >= kva && a < kva + (2u << 20) ? sh.bus + (a - kva) : 0; };
        uint64_t bytes = 0; uint32_t ticket = 0;
        assert(CePresentKick(r.gpu, kChipTu104, vram, cp, jv, v2p, r.sync, &bytes, &ticket) == 1 && ticket && cp.big);
        assert(cp.inflight == ticket && bytes == uint64_t(W) * H * 4);
        auto poll = [&] { (void)r.Wait(); };
        assert(CePresentWait(r.gpu, cp, ticket, 0, poll) == 0);            // not yet: nothing has run the host model
        uint32_t t2 = 0;                                                    // a second kick while one is in flight is refused
        assert(CePresentKick(r.gpu, kChipTu104, vram, cp, jv, v2p, r.sync, &bytes, &t2) == 0);
        assert(CePresentWait(r.gpu, cp, ticket, 50000, poll) == 1 && !cp.inflight);
        vram.Restore();
        assert(r.gpu.Vram32(0x800000) == 0x30000000u && r.gpu.Vram32(0x800000 + (2u << 20) - 4) == 0x30000000u + W * H - 1);
        assert(cp.st.map_pages == 1);                                       // one 2 MiB PTE
        assert(r.CeFree(cp) == 7 && r.rm.objs.empty());
    }
    // ---- M4 step 2: context-buffer list, golden context, TU10x scrubber
    {
        // the list as nouveau builds it from the synthetic sizes
        rm570::gr::GET_CONTEXT_BUFFERS_INFO_PARAMS info = {};
        for (uint32_t i = 0; i < 0x1a; ++i) info.engine[0][i].size = fake_gpu::FakeRm::kCtxSizes[i];
        GrBufs b; GrBuildBufs(info, b);
        assert(b.n == 9 && b.b[0].id == rm570::gr::BUF_MAIN && b.b[0].size == 0x40000 + 0x40000 && b.b[0].page == 16);
        bool attrib = false, unres = false;
        for (uint32_t i = 0; i < b.n; ++i) {
            if (b.b[i].id == rm570::gr::BUF_ATTRIBUTE_CB) { attrib = true; assert(b.b[i].page == 21 && b.b[i].align == 22 && b.b[i].global); }
            if (b.b[i].id == rm570::gr::BUF_UNRESTRICTED_PRIV_ACCESS_MAP) { unres = true; assert(b.b[i].ro && b.b[i].init && b.b[i].page == 12); }
        }
        assert(attrib && unres);
    }
    {
        Rig r(I, 0x1); r.last = r.Run(); assert(r.last.init_done);
        GrState st;
        GrResult g = r.GrGolden(st);
        if (!g.ok()) std::fprintf(stderr, "GR: %s at %s (rm %x) fake: %s / %s\n", g.status, g.stage, g.rm_status, r.rm.gr_error.c_str(), r.rm.error.c_str());
        assert(g.ok() && g.golden_ok && g.scrubber_ok && st.up);
        assert(g.golden_entries == 9 && g.scrubber_entries == 8 && r.rm.golden_built && r.rm.scrub_war && r.rm.threed_allocs == 2);
        assert(r.rm.objs.size() == 6);                                 // the scrubber scope stays; the golden one is gone
        assert(st.bufs.b[1].id == rm570::gr::BUF_PAGEPOOL && st.bufs.b[1].vram && !st.bufs.b[2].vram && g.vram_used > 0x700000);   // globals kept
        // ---- step 3: a user GR channel clears a pitch-linear target (small: the fake writes pixel by pixel)
        GrClearPlan pl; pl.width = 64; pl.height = 32; pl.repeats = 3;
        GrClearResult cr;
        const GrResult c3 = r.GrClear(st, pl, cr);
        if (!c3.ok()) std::fprintf(stderr, "GR clear: %s at %s (rm %x) host: %s / %s\n", c3.status, c3.stage, c3.rm_status, r.rm.host_error.c_str(), r.rm.gr_error.c_str());
        assert(c3.ok() && cr.first_ok && cr.verify_ok && cr.n == 3 && r.rm.clears == 4 && cr.token == ((2u << 16) | kGrUserChid));
        assert(r.gpu.Vram32(cr.rt_vram) == 0xff00ff00u && r.gpu.Vram32(cr.rt_vram + 31ull * 256 + 63 * 4) == 0xff00ff00u);
        assert(r.rm.objs.size() == 6);                                 // the user scope is freed; the scrubber stays
        // block-linear variant: same target memory, GOB-swizzled (the fake swizzles independently)
        {
            GrClearPlan bl; bl.width = 64; bl.height = 32; bl.repeats = 2; bl.block_linear = true;
            GrClearResult cb;
            const GrResult c5 = r.GrClear(st, bl, cb);
            if (!c5.ok()) std::fprintf(stderr, "GR clear bl: %s at %s host: %s got %x want %x clears %zu\n", c5.status, c5.stage, r.rm.host_error.c_str(), cb.sample_got, cb.sample_want, r.rm.clears);
            assert(c5.ok() && cb.first_ok && cb.verify_ok && cb.rt_vram == cr.rt_vram && r.rm.clears == 7);
            assert(GrBlOffset(0, 0, 64) == 0 && GrBlOffset(1, 0, 64) == 4 && GrBlOffset(4, 0, 64) == 32 && GrBlOffset(8, 0, 64) == 256);
            assert(GrBlOffset(0, 1, 64) == 16 && GrBlOffset(0, 2, 64) == 64 && GrBlOffset(0, 8, 64) == 512);
            assert(GrBlOffset(16, 0, 64) == 8192 && GrBlOffset(0, 128, 64) == 4 * 8192);
            assert(r.rm.objs.size() == 6);
        }
        // ---- step 4: compute dispatch of the NAK kernel (the fake checks its bytes and runs its semantics)
        {
            namespace nk = nak_m4;
            const GrComputeKernel k{nk::kCode, uint32_t(sizeof(nk::kCode) / 4), nk::kQmd, uint32_t(sizeof(nk::kQmd) / 4),
                                    nk::kShaderVa, nk::kCb0Va, nk::kInvocations, nk::kLocalSize, nk::kStoreValue};
            assert(!GrComputeCheck(k));
            r.rm.compute_code.assign(nk::kCode, nk::kCode + sizeof(nk::kCode) / 4);
            r.rm.compute_store = nk::kStoreValue;
            GrClearPlan cp; cp.repeats = 3;
            GrComputeResult cc;
            const uint32_t inv = nk::kInvocations - nk::kLocalSize / 2 - 3;
            const GrResult c4 = r.GrCompute(st, cp, k, inv, cc);
            if (!c4.ok()) std::fprintf(stderr, "GR compute: %s at %s host: %s / %s (bad %u got %x)\n", c4.status, c4.stage,
                                       r.rm.host_error.c_str(), r.rm.gr_error.c_str(), cc.bad_index, cc.bad_got);
            assert(c4.ok() && cc.first_ok && cc.verify_ok && cc.n == 3 && r.rm.dispatches == 4 && cc.threads == nk::kInvocations);
            assert(r.gpu.Vram32(cc.block_vram + GrComputeLayout::data) == nk::kStoreValue);
            assert(r.gpu.Vram32(cc.block_vram + GrComputeLayout::data + 4ull * inv) == kGrComputeSentinel);
            assert(r.rm.objs.size() == 6);
            // a QMD that names another program address is refused before any RPC
            uint32_t bad[64]; std::memcpy(bad, nk::kQmd, sizeof(bad)); bad[48] ^= 0x100;
            GrComputeKernel kb = k; kb.qmd = bad;
            assert(GrComputeCheck(kb) && !std::strcmp(GrComputeCheck(kb), "QMD program address"));
            // too many invocations for the QMD grid
            GrComputeResult c2; GrClearPlan p2;
            assert(!r.GrCompute(st, p2, k, nk::kInvocations + 1, c2).ok());
        }
        assert(r.GrDown(st, g) == 6 && r.rm.objs.empty() && !r.rm.scrub_war && !st.up);
        GrState again; again.up = true;
        assert(!r.GrGolden(again).ok());                                 // once per boot
    }
    {   // RM refuses TURING_A: the golden scope is freed, no scrubber
        Rig r(I, 0x1); r.last = r.Run(); assert(r.last.init_done);
        r.rm.fail_class = rm570::gr::TURING_A;
        GrState st;
        const GrResult g = r.GrGolden(st);
        assert(!g.ok() && !std::strcmp(g.stage, "golden") && !g.golden_ok && !st.up && r.rm.objs.empty() && r.rm.promotes == 1);
    }
    {   // GR VRAM outside a client FB region: refused before any RPC
        Rig r(I, 0x1); r.last = r.Run(); assert(r.last.init_done);
        r.last.si.fb[0].reserved = 1;
        const uint32_t before = r.rm.rpcs;
        GrState st;
        assert(!r.GrGolden(st).ok() && r.rm.rpcs == before);
    }
    std::printf("test_nv_gsp_p4: OK (%s inputs%s)\n", I.real ? "real" : "synthetic", I.seq.empty() ? "" : ", real sequencer");
    return 0;
}
