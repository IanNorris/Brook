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
        const P4Result p = r.Run();
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
    std::printf("test_nv_gsp_p4: OK (%s inputs%s)\n", I.real ? "real" : "synthetic", I.seq.empty() ? "" : ", real sequencer");
    return 0;
}
