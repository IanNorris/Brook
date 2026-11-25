// Trace-replay harness for Brook's planned TU104 GSP boot (nv_gsp_boot.h):
// preflight, queued SET_SYSTEM_INFO/SET_REGISTRY doorbells, FWSEC-FRTS on
// the GSP falcon, GSP reset into RISC-V, booter_load on SEC2, the
// app-version handshake, optionally the RUN_CPU_SEQUENCER replay, and the
// postinit interrupt allow, all against the recording fake register file
// (nv_gsp_fake/fake_gpu.h).
//
//   test_nv_gsp_boot [--record raw.jsonl] [--seq seq-payload.bin]
//
// With BROOK_NV_GSP_FW_DIR, BROOK_NV_GSP_ACR_BL and BROOK_NV_GSP_VBIOS set it
// uses the real 570.144 blobs and VBIOS; otherwise synthetic containers. The
// raw log is diffed against the Linux mmiotrace reference by
// scripts/nv_gsp_trace_diff.py (ordering and values, DMA masked).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include "nv_gsp/nv_gsp_boot.h"
#include "nv_gsp/nv_gsp_queue.h"
#include "nv_gsp/nv_gsp_seq.h"
#include "../nv_gsp_fake/fake_gpu.h"
#include "../nv_gsp_fake/synth_fw.h"

using namespace brook::nvgsp;
using synth_fw::Bytes;
namespace rm = brook::nvgsp::rm570;

static Span S(const Bytes& b) { return Span{b.data(), b.size()}; }
static bool Load(const std::string& path, Bytes* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out->empty();
}
static const char* Env(const char* n) { const char* v = std::getenv(n); return (v && *v) ? v : nullptr; }

// Bus adaptor for the sequencer: register ops to the fake, core ops to Brook.
struct SeqBus {
    fake_gpu::FakeGpu& g;
    GspCoreOps<fake_gpu::FakeGpu> ops;
    uint32_t rd32(uint32_t a) { return g.rd32(a); }
    void wr32(uint32_t a, uint32_t v) { g.wr32(a, v); }
    void udelay(uint32_t us) { g.udelay(us); }
    uint64_t now_us() { return g.now_us(); }
    void seq_step(uint32_t op) { g.seq_step(op); }
    bool core_reset() { return ops.core_reset(); }
    bool core_start() { return ops.core_start(); }
    bool core_wait_halt() { return ops.core_wait_halt(); }
    bool core_resume() { return ops.core_resume(); }
};

struct NoWait { int n = 0; bool operator()() { return ++n < 1000; } };
struct Doorbell {
    fake_gpu::FakeGpu& g;
    void operator()() { g.wr32(kChipTu104.gsp_falcon + kFQueueHead0, 0); }   // r535 rpc.c cmdq_push
};

int main(int argc, char** argv) {
    const char* record = nullptr;
    const char* seq_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
        else if (!std::strcmp(argv[i], "--seq") && i + 1 < argc) seq_path = argv[++i];
        else { std::fprintf(stderr, "usage: %s [--record raw.jsonl] [--seq payload.bin]\n", argv[0]); return 2; }
    }
    const char* fwdir = Env("BROOK_NV_GSP_FW_DIR");
    const char* acr = Env("BROOK_NV_GSP_ACR_BL");
    const char* vbios = Env("BROOK_NV_GSP_VBIOS");
    const bool real = fwdir && acr && vbios;
    Bytes gsp_elf, booter_blob, rbl_blob, acr_blob, rom;
    if (real) {
        const std::string d(fwdir);
        assert(Load(d + "/gsp-570.144.bin", &gsp_elf) && Load(d + "/booter_load-570.144.bin", &booter_blob) &&
               Load(d + "/bootloader-570.144.bin", &rbl_blob) && Load(acr, &acr_blob) && Load(vbios, &rom));
    } else {
        gsp_elf = synth_fw::Elf(28528288 / 64, 0x1000);
        booter_blob = synth_fw::Booter();
        rbl_blob = synth_fw::RiscvBl();
        acr_blob = synth_fw::AcrBl();
        rom = synth_fw::Vbios(true, 2).rom;
    }
    GspElf elf; BooterImage booter; RiscvBootloader rbl; AcrBl bl; Vbios vb; FwsecImage fwsec;
    assert(ParseGspElf(S(gsp_elf), &elf) == FwStatus::Ok);
    assert(ParseBooter(S(booter_blob), &booter) == FwStatus::Ok);
    assert(ParseRiscvBootloader(S(rbl_blob), &rbl) == FwStatus::Ok);
    assert(ParseAcrBl(S(acr_blob), &bl) == FwStatus::Ok);
    assert(ParseVbios(S(rom), &vb) == FwStatus::Ok && ParseFwsec(vb, &fwsec) == FwStatus::Ok);

    fake_gpu::FakeGpu gpu;

    // ---- pre: preflight (BROOK), layout, queued RPCs
    const Preflight pf = RunPreflight(gpu, kChipTu104);
    assert(pf.status == BootStatus::Ok);
    FbLayoutInput in;
    in.fb_size = FbSizeFromReg(pf.fb_size_reg);
    in.reg_625f04 = pf.vga_ws;
    in.elf_size = elf.fwimage.n;
    in.boot_size = rbl.bin.data_size;
    FbLayout L;
    assert(ComputeFbLayout(in, &L));

    // Sysmem objects at fixed fake bus addresses (masked in the diff).
    const uint64_t kFwsecDma = 0x109d30000ull, kMetaDma = 0x10c000000ull, kLibosDma = 0x10c001000ull,
                   kQueueDma = 0x10c100000ull;
    std::vector<uint8_t> fwsec_img(fwsec.ucode.n);
    FwsecPatchInfo pi;
    assert(PatchFwsec(fwsec, fwsec_img.data(), fwsec_img.size(), nvfw::kDmemMapperCmdFrts, L.frts.addr, L.frts.size,
                      &pi) == FwStatus::Ok);
    gpu.MapDma(kFwsecDma, fwsec_img.data(), fwsec_img.size());
    gpu.fwsec_interface_offset = fwsec.interface_offset;
    rm::GspFwWprMeta meta;
    WprMetaSysmem sm;
    sm.radix3_lvl0 = 0x10c200000ull; sm.bootloader = 0x10c300000ull; sm.signature = 0x10c400000ull;
    sm.signature_size = elf.signature.n;
    sm.bl_code_offset = rbl.desc.monitorCodeOffset; sm.bl_data_offset = rbl.desc.monitorDataOffset;
    sm.bl_manifest_offset = rbl.desc.manifestOffset;
    FillWprMeta(L, sm, &meta);
    gpu.MapDma(kMetaDma, reinterpret_cast<const uint8_t*>(&meta), sizeof(meta));

    std::vector<uint8_t> shm(QueueShmBytes());
    QueueShm q;
    assert(InitQueueShm(shm.data(), kQueueDma, uint32_t(shm.size()), &q));
    auto ch = std::make_unique<RpcChannel>(q);
    gpu.mark("pre", "cmdq_doorbell");
    {
        NoWait w; Doorbell bell{gpu};
        std::vector<uint8_t> sysinfo(0x3a0, 0), registry(0x75, 0);
        assert(ch->Send(rm::NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO, sysinfo.data(), uint32_t(sysinfo.size()), w, bell,
                        true) == QStatus::Ok);
        assert(ch->Send(rm::NV_VGPU_MSG_FUNCTION_SET_REGISTRY, registry.data(), uint32_t(registry.size()), w, bell,
                        true) == QStatus::Ok);
    }

    // ---- FWSEC-FRTS
    const FwsecRun fr = RunFwsecFrts(gpu, kChipTu104, fwsec, &bl, fwsec_img.data(), kFwsecDma, L);
    if (fr.status != BootStatus::Ok) std::fprintf(stderr, "fwsec: %s (%s)\n", BootStatusName(fr.status), gpu.model_error.c_str());
    assert(fr.status == BootStatus::Ok && gpu.fwsec_runs == 1 && gpu.model_error.empty());
    assert(Wpr2RegToAddr(fr.wpr2_lo) == L.frts.addr && Wpr2HiToEnd(fr.wpr2_hi) == L.frts.end());

    // ---- GSP into RISC-V mode, booter_load
    assert(ResetGspRiscv(gpu, kChipTu104, kLibosDma) == BootStatus::Ok);
    std::vector<uint8_t> bimg(booter.image.n);
    assert(BuildBooterImage(booter, bimg.data(), bimg.size()));
    const BooterRun br = RunBooterLoad(gpu, kChipTu104, booter, bimg.data(), kMetaDma);
    if (br.status != BootStatus::Ok) std::fprintf(stderr, "booter: %s (%s)\n", BootStatusName(br.status), gpu.model_error.c_str());
    assert(br.status == BootStatus::Ok && gpu.booter_runs == 1 && br.mbox0 == 0 && br.mbox1 == uint32_t(kMetaDma >> 32));
    assert(Wpr2RegToAddr(br.wpr2_lo) == L.wpr2.addr && Wpr2HiToEnd(br.wpr2_hi) == L.frts.end());
    // the SEC2 DMEM copy carries the signature at dmem_sign (booter "patch dst")
    uint32_t w0; std::memcpy(&w0, booter.sig.p, 4);
    assert(gpu.sec2.dmem[booter.dmem_sign / 4] == w0);

    // ---- handshake
    assert(GspHandshake(gpu, kChipTu104, rbl.desc.appVersion) == BootStatus::Ok);

    // ---- CPU sequencer replay (GSP-RM's RUN_CPU_SEQUENCER payload)
    if (seq_path) {
        Bytes payload;
        assert(Load(seq_path, &payload));
        gpu.reg[kChipTu104.gsp_falcon + kFMbox0] = 0x80000000;    // GSP-RM: "ready for the sequencer"
        gpu.mark("cpu_sequencer", "seq");
        SeqBus sb{gpu, GspCoreOps<fake_gpu::FakeGpu>{gpu, kChipTu104, kLibosDma, rbl.desc.appVersion}};
        const SeqResult sr = RunSequencer(sb, payload.data(), uint32_t(payload.size()));
        std::printf("sequencer: %s, %u commands (%u wr, %u poll, %u store, %u core)\n", SeqStatusName(sr.status),
                    sr.commands, sr.writes, sr.polls, sr.stores, sr.core_ops);
        assert(sr.status == SeqStatus::Ok && gpu.seq_starts == 1 && gpu.sec2_resumes == 1);
    }

    // ---- postinit (r535_gsp_postinit: allow the GSP interrupt)
    gpu.mark("postinit", "intr_allow");
    gpu.wr32(kChipTu104.gsp_falcon + 0x004, 0x40);

    size_t pio = 0;
    for (auto& a : gpu.log) pio += a.op == 'W' && (a.off == 0x110184 || a.off == 0x8401c4 || a.off == 0x840184);
    std::printf("test_nv_gsp_boot: OK (%s inputs, %zu accesses, %zu PIO words, FRTS 0x%llx, WPR2 0x%llx..0x%llx)\n",
                real ? "real" : "synthetic", gpu.log.size(), pio, (unsigned long long)L.frts.addr,
                (unsigned long long)L.wpr2.addr, (unsigned long long)L.frts.end());
    if (record) assert(gpu.WriteRawLog(record));
    return 0;
}
