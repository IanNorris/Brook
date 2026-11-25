#pragma once
// Recording fake TU104 register file for the GSP M0 host tests and the
// trace-replay harness. It logs every BAR0 access (op, offset, value, phase,
// step) and models just enough hardware to let Brook's planned boot sequence
// run to completion:
//   - falcon engine reset (0x3c0 1 -> 0) starts a memory scrub: the next
//     DMACTL reads return 0x7 a few times, then 0x1 (as in the 25 Nov trace)
//   - IMEMC/IMEMD/DMEMC/DMEMD PIO with auto-increment into IMEM/DMEM arrays
//   - CPUCTL start (0x100 = 2, or 0x130 alias) runs a model, then halts
//   - FWSEC model (GSP, BOOTVEC 0xfd00, DMEM holds flcn_bl_dmem_desc_v2 with
//     a sysmem ctx_dma): fetches the patched ucode through the descriptor's
//     data_dma_base, walks appif -> DMEM mapper -> FRTS command itself, and
//     programs WPR2 lo/hi from the region it finds (error word 0)
//   - booter model (SEC2): reads GspFwWprMeta through MAILBOX0/1, checks the
//     magic, extends WPR2 to gspFwWprStart, MAILBOX0 = 0, starts GSP RISC-V
//   - CPU-sequencer resume: SEC2 start via alias raises 0x1180f8 bit 26
// Reset values are the ones the Linux trace read on this card.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace fake_gpu {

struct Access { char op; uint32_t off; uint32_t val; std::string phase, step; };

class FakeGpu {
public:
    struct Falcon {
        uint32_t base;
        std::vector<uint32_t> imem = std::vector<uint32_t>(0x10000 / 4, 0);
        std::vector<uint32_t> dmem = std::vector<uint32_t>(0x10000 / 4, 0);
        uint32_t imemc = 0, dmemc = 0;
        int scrub = 0, run = 0;
        bool halted = true;
        uint32_t starts = 0;
    };

    FakeGpu() {
        static const uint32_t kFalconInit[][2] = {
            {0x12c, 0xca101136}, {0x108, 0x40420100}, {0x048, 0x00001004}, {0x10c, 0x00000001},
            {0x100, 0x00000010},
            {0x624, 0x00000110}, {0x600, 0x110}, {0x604, 0x110}, {0x608, 0x110}, {0x60c, 0x110},
            {0x610, 0x110}, {0x614, 0x110}, {0x618, 0x110}, {0x61c, 0x110},
        };
        for (uint32_t b : {0x110000u, 0x840000u})
            for (auto& kv : kFalconInit) reg[b + kv[0]] = kv[1];
        reg[0x000000] = 0x164000a1;
        reg[0x118128] = 0xffffff8f;
        reg[0x118234] = 0x000003ff;
        reg[0x625f04] = 0x00020e09;
        reg[0x100ce0] = 0x00000208;
        reg[0x1180f8] = 0x13300000;
        reg[0x840408] = 0;
    }

    // ------------------------------------------------------------- Bus
    uint32_t rd32(uint32_t off) {
        const uint32_t v = Read(off);
        log.push_back({'R', off, v, phase, step});
        return v;
    }
    void wr32(uint32_t off, uint32_t v) {
        log.push_back({'W', off, v, phase, step});
        Write(off, v);
    }
    void udelay(uint32_t us) { t_us += us; }
    uint64_t now_us() { return ++t_us; }
    void mark(const char* p, const char* s) { phase = p; step = s; }
    void seq_step(uint32_t op) {
        static const char* names[] = {"seq_wr32", "seq_mask", "seq_poll", "seq_usec", "seq_save",
                                      "seq_core_reset", "seq_core_start", "seq_core_wait_halt",
                                      "seq_core_resume"};
        step = op < 9 ? names[op] : "seq_unknown";
    }

    // sysmem visible to the fake engines (bus address -> host bytes)
    void MapDma(uint64_t bus, const uint8_t* p, size_t n) { dma.push_back({bus, {p, n}}); }
    const uint8_t* Dma(uint64_t bus, size_t n) const {
        for (auto& d : dma)
            if (bus >= d.first && bus + n <= d.first + d.second.second)
                return d.second.first + (bus - d.first);
        return nullptr;
    }

    bool WriteRawLog(const char* path) const {
        FILE* f = std::fopen(path, "w");
        if (!f) return false;
        for (size_t i = 0; i < log.size(); ++i) {
            const Access& a = log[i];
            std::fprintf(f, "{\"op\": \"%c\", \"off\": %u, \"val\": %u, \"phase\": \"%s\", \"step\": \"%s\"}\n",
                         a.op, a.off, a.val, a.phase.c_str(), a.step.c_str());
        }
        return std::fclose(f) == 0;
    }

    std::map<uint32_t, uint32_t> reg;
    std::vector<Access> log;
    std::string phase = "pre", step = "probe";
    uint64_t t_us = 0;
    Falcon gsp{0x110000}, sec2{0x840000};
    std::vector<std::pair<uint64_t, std::pair<const uint8_t*, size_t>>> dma;
    // FWSEC model input: the DMEM-image offset of the application interface
    // table (descriptor InterfaceOffset), as ground truth from the VBIOS.
    uint32_t fwsec_interface_offset = 0;
    uint32_t fwsec_runs = 0, booter_runs = 0, seq_starts = 0, sec2_resumes = 0;
    uint32_t fwsec_sb_runs = 0, unload_runs = 0;
    uint32_t fwsec_frts_error = 0, fwsec_sb_error = 0;   // injected FWSEC failures
    bool unload_clears = true;
    uint64_t wpr2_skew = 0;                              // FWSEC locks a shifted region
    uint64_t booter_meta = 0;                            // WPR meta bus address booter_load was given
    std::vector<uint32_t> trace_pcs;                     // TRACEPC ring the falcon reports
    std::string model_error;
    int halt_reads = 3;

private:
    Falcon* Fl(uint32_t off) {
        if (off >= 0x110000 && off < 0x111000) return &gsp;
        if (off >= 0x840000 && off < 0x841000) return &sec2;
        return nullptr;
    }
    uint32_t Get(uint32_t off) const { auto it = reg.find(off); return it == reg.end() ? 0 : it->second; }

    uint32_t Read(uint32_t off) {
        Falcon* f = Fl(off);
        if (f) {
            const uint32_t r = off - f->base;
            if (r == 0x10c) {
                if (f->scrub > 0) { --f->scrub; return 0x7; }
                return 0x1;          // bit 0 reads back set on TU104 even after DMACTL = 0
            }
            if (r == 0x100) {
                if (f->run > 0 && --f->run == 0) Halt(*f);
                // SEC2 CPUCTL bit 6 (alias enable) reads set once GSP-RM is up
                // (0x10 after booter, 0x50 at the sequencer's core resume).
                if (f == &sec2 && booter_done && f->halted && phase == "cpu_sequencer") return 0x50;
                return Get(off);
            }
            if (r == 0x1c4) { const uint32_t v = f->dmem[(f->dmemc & 0xfffc) / 4]; f->dmemc += 4; return v; }
            if (r == 0x148) return (uint32_t(trace_pcs.size()) << 16) | (Get(off) & 0xffff);
            if (r == 0x14c) { const uint32_t i = Get(f->base + 0x148) & 0xff; return i < trace_pcs.size() ? trace_pcs[i] : 0; }
        }
        if (off == 0x1180f8 && resume_countdown > 0 && --resume_countdown == 0)
            reg[off] |= 0x04000000;
        return Get(off);
    }

    void Write(uint32_t off, uint32_t v) {
        Falcon* f = Fl(off);
        if (!f) { reg[off] = v; return; }
        const uint32_t r = off - f->base;
        switch (r) {
        case 0x3c0:
            if ((Get(off) & 1) && !(v & 1)) {
                // engine reset: scrub, mailboxes cleared, FBIF_CTL back to 0x110
                f->scrub = 4; f->halted = true;
                reg[f->base + 0x040] = 0; reg[f->base + 0x044] = 0; reg[f->base + 0x624] = 0x110;
            }
            reg[off] = v; return;
        case 0x180: f->imemc = v; reg[off] = v; return;
        case 0x184: f->imem[(f->imemc & 0xfffc) / 4] = v; f->imemc += 4; return;
        case 0x188: reg[off] = v; return;
        case 0x1c0: f->dmemc = v; reg[off] = v; return;
        case 0x1c4: f->dmem[(f->dmemc & 0xfffc) / 4] = v; f->dmemc += 4; return;
        case 0x100: case 0x130:
            if (v & 2) { Start(*f); return; }
            reg[off] = v; return;
        default: reg[off] = v; return;
        }
    }

    void Start(Falcon& f) {
        ++f.starts;
        f.halted = false;
        reg[f.base + 0x100] = 0;
        f.run = halt_reads;
        if (&f == &gsp) {
            const uint32_t ctx = f.dmem[8];
            if (phase != "cpu_sequencer" && Get(f.base + 0x104) == 0xfd00 && ctx == 4) { EmulateFwsec(); return; }
            ++seq_starts;           // sequencer-started boot stub (VRAM descriptor)
            return;
        }
        // SEC2
        if (f.starts == 1 || !booter_done || (Get(0x840040) == 0xff && Get(0x840044) == 0xff)) { EmulateBooter(); return; }
        ++sec2_resumes;
        resume_countdown = 3;
        reg[0x840040] = 0;
    }

    void Halt(Falcon& f) {
        f.halted = true;
        reg[f.base + 0x100] = 0x10;
        // the sequencer-started GSP boot stub leaves MAILBOX0 = 0 when it halts
        if (&f == &gsp && phase == "cpu_sequencer") reg[0x110040] = 0;
    }

    static uint32_t U32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    static uint64_t U64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

    void EmulateFwsec() {
        ++fwsec_runs;
        // flcn_bl_dmem_desc_v2 in DMEM: words 9-10 code base, 16-17 data base, 18 data size
        const uint64_t data_base = uint64_t(gsp.dmem[16]) | (uint64_t(gsp.dmem[17]) << 32);
        const uint32_t data_size = gsp.dmem[18];
        const uint8_t* d = Dma(data_base, data_size);
        if (!d) { model_error = "fwsec: data_dma_base not in sysmem"; Fail(); return; }
        const uint32_t h = fwsec_interface_offset;
        if (h + 4 > data_size || d[h] != 1) { model_error = "fwsec: appif header"; Fail(); return; }
        const uint8_t hl = d[h + 1], el = d[h + 2], cnt = d[h + 3];
        for (uint32_t i = 0; i < cnt; ++i) {
            const uint8_t* e = d + h + hl + i * el;
            if (U32(e) != 4) continue;
            const uint32_t dm = U32(e + 4);
            const uint32_t init_cmd = U32(d + dm + 0x2c);
            const uint32_t cmd = U32(d + dm + 8);
            const uint8_t* c = d + cmd;
            if (init_cmd == 0x19 && U32(c) == 1 && U32(c + 4) == 24 && U32(c + 20) == 2) {
                ++fwsec_sb_runs;                       // FWSEC-SB: status word 0x1454 low half
                reg[0x001454] = fwsec_sb_error;
                reg[0x110040] = 0; reg[0x110044] = 0;
                return;
            }
            if (fwsec_frts_error) { reg[0x001438] = fwsec_frts_error << 16; reg[0x110040] = 0; return; }
            if (init_cmd != 0x15 || U32(c) != 1 || U32(c + 4) != 24 || U32(c + 20) != 2 ||
                U32(c + 24) != 1 || U32(c + 28) != 20 || U32(c + 40) != 2) {
                model_error = "fwsec: FRTS command not patched"; Fail(); return;
            }
            const uint64_t addr = (uint64_t(U32(c + 32)) << 12) + wpr2_skew, size = uint64_t(U32(c + 36)) << 12;
            reg[0x1fa824] = uint32_t((addr >> 12) << 4);
            reg[0x1fa828] = uint32_t(((addr + size - 0x20000) >> 12) << 4);
            reg[0x001438] = 0;
            reg[0x110040] = 0;
            reg[0x110044] = 0;
            frts_addr = addr; frts_size = size;
            return;
        }
        model_error = "fwsec: no DMEM mapper"; Fail();
    }

    void EmulateBooter() {
        if (Get(0x840040) == 0xff && Get(0x840044) == 0xff) {   // booter_unload (fini, not suspend)
            ++unload_runs;
            if (unload_clears) { reg[0x1fa824] = 0x1ffffe00; reg[0x1fa828] = 0; }
            reg[0x840040] = 0;
            return;
        }
        ++booter_runs;
        const uint64_t meta = uint64_t(Get(0x840040)) | (uint64_t(Get(0x840044)) << 32);
        const uint8_t* m = Dma(meta, 256);
        booter_meta = meta;
        if (!m || U64(m) != 0xdc3aae21371a60b3ULL || U64(m + 8) != 1) {
            model_error = "booter: bad WPR meta"; reg[0x840040] = 0xbad; return;
        }
        const uint64_t wpr = U64(m + 0x70), frts = U64(m + 0x98), fsz = U64(m + 0xa0);
        if (!Get(0x1fa828) || frts != frts_addr || fsz != frts_size) {
            model_error = "booter: FRTS/WPR2 mismatch"; reg[0x840040] = 0xbad2; return;
        }
        reg[0x1fa824] = uint32_t((wpr >> 12) << 4);
        reg[0x840040] = 0;               // MAILBOX1 keeps the meta high word
        reg[0x111240] = 0x5;             // GSP RISC-V running
        booter_done = true;
    }

    void Fail() { reg[0x001438] = 0xdead0000; }

    bool booter_done = false;
    int resume_countdown = 0;
    uint64_t frts_addr = 0, frts_size = 0;
};

}  // namespace fake_gpu
