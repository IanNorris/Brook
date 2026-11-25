// M1 P3 (FWSEC-FRTS alone) and P3b (fini) against the recording fake
// register file: the exact sequence the live `nv-gsp p3` / `nv-gsp fini`
// bridge verbs run (nv_gsp_p3.h), for both VGA-workspace encodings:
//   0x625f04 = 0x1      (Brook under UEFI, bit 3 clear): FRTS 0x1ffe00000,
//                       137 MiB, expected WPR2 0x01ffe000 / 0x01ffee00
//   0x625f04 = 0x20e09  (the CSM Linux reference): FRTS 0x1ffee0000, 136 MiB
// plus the refusal paths (no write before the checks pass) and the failure
// capture (CPUCTL, mailboxes, 0x1438, WPR2, TRACEPC).
//
//   test_nv_gsp_p3 [--record raw.jsonl] [--vga 0x1|0x20e09]
//
// With BROOK_NV_GSP_FW_DIR, BROOK_NV_GSP_ACR_BL and BROOK_NV_GSP_VBIOS set it
// uses the real blobs and VBIOS; it also checks the pinned ChipOps hashes.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "nv_gsp/nv_gsp_p3.h"
#include "../nv_gsp_fake/fake_gpu.h"
#include "../nv_gsp_fake/synth_fw.h"
#include "../../shared/update/payload.h"

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
    Bytes rom, acr, unload;
    uint64_t elf_size = 28528288, boot_size = 0x1000;
    uint32_t interface_offset = 0;
};

static size_t Writes(const fake_gpu::FakeGpu& g) {
    size_t n = 0;
    for (auto& a : g.log) n += a.op == 'W';
    return n;
}

struct Rig {
    fake_gpu::FakeGpu gpu;
    std::vector<uint8_t> dmamem = std::vector<uint8_t>(16 * 4096);
    DmaBuf dma;
    P3Inputs in;
    Rig(const Inputs& i, uint32_t vga) {
        gpu.reg[0x625f04] = vga;
        gpu.fwsec_interface_offset = i.interface_offset;
        dma.cpu = dmamem.data(); dma.bus = 0x109d30000ull; dma.bytes = dmamem.size();
        gpu.MapDma(dma.bus, dmamem.data(), dmamem.size());
        in.vbios = S(i.rom); in.acr_bl = S(i.acr); in.elf_size = i.elf_size; in.boot_size = i.boot_size;
    }
};

static void CheckHashes(const char* fwdir, const char* acr) {
    // The kernel pins these (nv_gsp_chip.h); the nix derivation pins the same.
    const std::string d(fwdir);
    for (uint32_t r = 0; r < kFwCount; ++r) {
        const FirmwareFile& f = kChipTu104.fw[r];
        std::string path = r == kFwAcrBl ? std::string(acr) : d + "/" + (std::strrchr(f.path, '/') + 1);
        Bytes b;
        assert(Load(path, &b));
        uint8_t h[32];
        BrookUpdateHash(b.data(), BrookUpdateSize(b.size()), h);
        if (b.size() != f.bytes || std::memcmp(h, f.sha256, 32)) {
            std::fprintf(stderr, "pinned hash mismatch: %s\n", f.path);
            assert(false);
        }
    }
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
        assert(Load(vbios, &I.rom) && Load(acr, &I.acr) &&
               Load(std::string(fwdir) + "/booter_unload-570.144.bin", &I.unload));
        CheckHashes(fwdir, acr);
    } else {
        I.rom = synth_fw::Vbios(true, 2).rom;
        I.acr = synth_fw::AcrBl();
        I.unload = synth_fw::Booter();
    }
    {
        Vbios vb; FwsecImage f;
        assert(ParseVbios(S(I.rom), &vb) == FwStatus::Ok && ParseFwsec(vb, &f) == FwStatus::Ok);
        I.interface_offset = f.interface_offset;
        std::printf("fwsec: v%u desc 0x%x+0x%x ucode 0x%zx..0x%zx (rom %zu B)\n", f.version, f.desc_addr, f.desc_size,
                    size_t(f.ucode.p - I.rom.data()), size_t(f.ucode.p - I.rom.data()) + f.ucode.n, I.rom.size());
    }

    // ---- both encodings succeed with their own placement
    struct Case { uint32_t vga; uint64_t frts; uint32_t lo, hi; uint64_t reserved; };
    const Case cases[] = {{0x1, 0x1ffe00000ull, 0x01ffe000, 0x01ffee00, 137ull << 20},
                          {0x20e09, 0x1ffee0000ull, 0x01ffee00, 0x01fffc00, 136ull << 20}};
    for (const Case& c : cases) {
        Rig r(I, c.vga);
        const P3Result p = RunP3(r.gpu, kChipTu104, r.in, r.dma);
        if (p.status != BootStatus::Ok)
            std::fprintf(stderr, "p3 vga=%x: %s at %s (%s)\n", c.vga, BootStatusName(p.status), p.stage, r.gpu.model_error.c_str());
        assert(p.status == BootStatus::Ok && r.gpu.fwsec_runs == 1 && !p.diag_valid);
        assert(p.layout.frts.addr == c.frts && p.layout.frts.size == 0x100000);
        assert(p.run.wpr2_lo == c.lo && p.run.wpr2_hi == c.hi && p.run.err == 0 && p.run.mbox0 == 0);
        assert(p.layout.reserved_top == c.reserved);
        // first write is FWSEC's falcon reset sequence (no write in the preflight)
        size_t first = 0;
        while (r.gpu.log[first].op != 'W') ++first;
        assert(r.gpu.log[first].phase == "fwsec_frts" && r.gpu.log[first].off == 0x110048);
        // only GSP falcon registers are written (no SEC2, no display, no WPR2)
        for (auto& a : r.gpu.log)
            if (a.op == 'W') assert(a.off >= 0x110000 && a.off < 0x111000);
        if (record && c.vga == record_vga) assert(r.gpu.WriteRawLog(record));

        // ---- P3b in the same boot: FWSEC-SB then booter_unload clears WPR2
        std::vector<uint8_t> bimg(I.unload.size());
        const FiniResult f = RunFini(r.gpu, kChipTu104, r.in, S(I.unload), r.dma, bimg.data(), bimg.size());
        if (f.status != BootStatus::Ok) std::fprintf(stderr, "fini: %s at %s\n", BootStatusName(f.status), f.stage);
        assert(f.status == BootStatus::Ok && r.gpu.fwsec_sb_runs == 1 && r.gpu.unload_runs == 1);
        assert(f.wpr2_hi_before == c.hi && f.unload.wpr2_hi == 0);
        // and after that a second P3 would see an unlocked WPR2 again
        assert(RunPreflight(r.gpu, kChipTu104).status == BootStatus::Ok);
    }

    // ---- refusals: nothing written
    {
        Rig r(I, 0x1); r.gpu.reg[0x1fa828] = 0x01ffee00;           // stale WPR2
        const P3Result p = RunP3(r.gpu, kChipTu104, r.in, r.dma);
        assert(p.status == BootStatus::Wpr2AlreadySet && !p.wrote && Writes(r.gpu) == 0);
    }
    {
        Rig r(I, 0x1); r.gpu.reg[0x1fa824] = 0x1ffffe00;           // P1 reset encoding (lo > hi) is unlocked
        assert(RunP3(r.gpu, kChipTu104, r.in, r.dma).status == BootStatus::Ok);
    }
    {
        Rig r(I, 0x1); r.gpu.reg[0x110100] = 0;                    // GSP falcon not halted
        const P3Result p = RunP3(r.gpu, kChipTu104, r.in, r.dma);
        assert(p.status == BootStatus::FalconError && !p.wrote && Writes(r.gpu) == 0);
    }
    {
        Rig r(I, 0x1); r.gpu.reg[0x111240] = 1;                    // RISC-V running
        assert(RunP3(r.gpu, kChipTu104, r.in, r.dma).status == BootStatus::FalconError && Writes(r.gpu) == 0);
    }
    {
        Rig r(I, 0x1); r.gpu.reg[0x000000] = 0x172000a1;           // not TU104
        assert(RunP3(r.gpu, kChipTu104, r.in, r.dma).status == BootStatus::WrongChip && Writes(r.gpu) == 0);
    }
    {
        Rig r(I, 0x1); r.gpu.reg[0x118128] = 0xfffffffe;           // devinit not done
        assert(RunP3(r.gpu, kChipTu104, r.in, r.dma).status == BootStatus::DevinitIncomplete && Writes(r.gpu) == 0);
    }
    {
        Rig r(I, 0x1); Bytes bad = I.rom; bad.resize(0x10000);       // 64 KiB dump: no FWSEC
        r.in.vbios = S(bad);
        const P3Result p = RunP3(r.gpu, kChipTu104, r.in, r.dma);
        assert(p.status == BootStatus::BadInput && std::strcmp(p.stage, "vbios") == 0 && Writes(r.gpu) == 0);
    }
    {
        Rig r(I, 0x1); r.dma.bus |= 0x800;                           // DMA not page-aligned
        const P3Result p = RunP3(r.gpu, kChipTu104, r.in, r.dma);
        assert(p.status == BootStatus::BadInput && std::strcmp(p.stage, "patch") == 0 && Writes(r.gpu) == 0);
    }
    {
        Rig r(I, 0x1); r.dma.bytes = 4096;                           // too small for the ucode
        assert(RunP3(r.gpu, kChipTu104, r.in, r.dma).status == BootStatus::BadInput && Writes(r.gpu) == 0);
    }

    {
        Rig r(I, 0x1); bool armed = false;                           // arm refuses: nothing written
        const P3Result p = RunP3(r.gpu, kChipTu104, r.in, r.dma, [&] { armed = true; return false; });
        assert(armed && p.status == BootStatus::BadInput && std::strcmp(p.stage, "arm") == 0 && Writes(r.gpu) == 0);
    }
    {
        Rig r(I, 0x1); size_t at_arm = ~size_t(0);                   // arm runs after all reads, before writes
        const P3Result p = RunP3(r.gpu, kChipTu104, r.in, r.dma, [&] { at_arm = Writes(r.gpu); return true; });
        assert(p.status == BootStatus::Ok && at_arm == 0);
    }

    // ---- failures after the first write: captured, not retried
    {
        Rig r(I, 0x1); r.gpu.fwsec_frts_error = 0x0d; r.gpu.trace_pcs = {0xfd00, 0xfd24, 0x1234};
        const P3Result p = RunP3(r.gpu, kChipTu104, r.in, r.dma);
        assert(p.status == BootStatus::FwsecError && p.run.err == 0x0d && p.diag_valid && p.wrote);
        assert(p.diag.frts_err == 0x000d0000 && p.diag.cpuctl == 0x10 && p.diag.wpr2_hi == 0);
        assert(p.diag.trace_n == 3 && p.diag.trace[0] == 0xfd00 && p.diag.trace[2] == 0x1234);
        assert(r.gpu.fwsec_runs == 1);
    }
    {
        Rig r(I, 0x1); r.gpu.halt_reads = 1 << 30;                   // never halts: 2 s timeout
        const P3Result p = RunP3(r.gpu, kChipTu104, r.in, r.dma);
        assert(p.status == BootStatus::Timeout && std::strcmp(p.run.step, "boot") == 0 && p.diag_valid);
    }
    {   // FWSEC locked a region other than Brook's FRTS placement
        Rig r(I, 0x1); r.gpu.wpr2_skew = 0x20000;
        const P3Result p = RunP3(r.gpu, kChipTu104, r.in, r.dma);
        assert(p.status == BootStatus::Wpr2Mismatch && p.diag_valid && p.diag.wpr2_lo == 0x01ffe200);
    }
    {   // P3b when the unload does not clear WPR2
        Rig r(I, 0x1); r.gpu.unload_clears = false;
        assert(RunP3(r.gpu, kChipTu104, r.in, r.dma).status == BootStatus::Ok);
        std::vector<uint8_t> bimg(I.unload.size());
        const FiniResult f = RunFini(r.gpu, kChipTu104, r.in, S(I.unload), r.dma, bimg.data(), bimg.size());
        assert(f.status == BootStatus::Wpr2Mismatch && std::strcmp(f.stage, "booter_unload") == 0 && f.diag_valid);
    }
    {   // P3b with WPR2 already clear: SB runs, unload is skipped (nouveau's check)
        Rig r(I, 0x1);
        std::vector<uint8_t> bimg(I.unload.size());
        const FiniResult f = RunFini(r.gpu, kChipTu104, r.in, S(I.unload), r.dma, bimg.data(), bimg.size());
        assert(f.status == BootStatus::Ok && r.gpu.fwsec_sb_runs == 1 && r.gpu.unload_runs == 0);
    }
    std::printf("test_nv_gsp_p3: OK (%s inputs)\n", I.real ? "real" : "synthetic");
    return 0;
}
