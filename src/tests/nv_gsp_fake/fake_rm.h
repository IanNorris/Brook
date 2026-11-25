#pragma once
// Fake GSP-RM for test_nv_gsp_p4: what the GSP's RISC-V side does after
// booter_load, driven by the CPU's msgq waiter (Poll()). It finds every
// sysmem object through the plumbing the real firmware uses:
//   GSP MAILBOX0/1 -> libos args -> RMARGS -> shared memory (cmdq/msgq),
//   booter's WPR meta -> radix3 -> ELF image pages (checked byte for byte),
//   signature and bootloader copies.
// Then it plays the 25 Nov Linux message order: consume SET_SYSTEM_INFO and
// SET_REGISTRY, send RUN_CPU_SEQUENCER (the real payload when given), and
// once the sequencer resumed SEC2, six POST_NOCAT_RECORDs and INIT_DONE.
// After that it answers GET_GSP_STATIC_INFO and UNLOADING_GUEST_DRIVER
// (MAILBOX0 = 0x80000000 afterwards, as RM does when it has unloaded).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "fake_gpu.h"
#include "nv_gsp/nvrm_r570.h"

namespace fake_gpu {

class FakeRm {
public:
    explicit FakeRm(FakeGpu& g) : g_(g) {}

    // inputs the test sets
    const uint8_t* elf_image = nullptr; size_t elf_bytes = 0;
    const uint8_t* signature = nullptr; size_t signature_bytes = 0;
    const uint8_t* bootloader = nullptr; size_t bootloader_bytes = 0;
    std::vector<uint8_t> seq_payload;          // RUN_CPU_SEQUENCER payload (rpc header stripped)
    bool hang_before_init_done = false;
    uint32_t static_result = 0;                // rpc_result for GET_GSP_STATIC_INFO

    // observations
    int stage = 0;
    std::string error;
    std::vector<uint8_t> sysinfo, registry;
    uint32_t rpcs = 0, static_requests = 0, unload_requests = 0;
    uint64_t libos = 0, shm = 0;

    // Called by the CPU-side waiter; returns false on a model error.
    bool Poll() {
        if (!error.empty()) return false;
        if (stage == 0) {
            if (!(g_.reg[0x111240] & 1)) return true;        // booter has not started RISC-V yet
            if (!Attach()) return false;
            stage = 1;
        }
        if (stage == 1) {
            std::vector<uint8_t> e; uint32_t fn = 0;
            while (sysinfo.empty() || registry.empty()) {
                if (!ReadRpc(&fn, &e)) return true;          // nothing queued (yet)
                if (fn == 72) sysinfo.assign(e.begin() + 32, e.end());
                else if (fn == 73) registry.assign(e.begin() + 32, e.end());
                else return Fail("unexpected rpc before boot: " + std::to_string(fn));
            }
            g_.reg[0x110040] = 0x80000000;                   // "ready for the sequencer"
            std::vector<uint8_t> seq = seq_payload.empty() ? SynthSeq() : seq_payload;
            if (!Write(0x1002, seq.data(), uint32_t(seq.size()), 0)) return false;
            stage = 2;
            return true;
        }
        if (stage == 2) {
            if (!g_.sec2_resumes || !(g_.reg[0x1180f8] & 0x04000000)) return true;
            if (hang_before_init_done) return true;
            std::vector<uint8_t> nocat(0x4bc, 0x5a);
            for (int i = 0; i < 6; ++i) if (!Write(0x1020, nocat.data(), uint32_t(nocat.size()), 0)) return false;
            if (!Write(0x1001, nullptr, 0, 0)) return false;
            stage = 3;
            return true;
        }
        if (stage == 3) {
            std::vector<uint8_t> e; uint32_t fn = 0;
            while (ReadRpc(&fn, &e)) {
                if (fn == 65) {
                    ++static_requests;
                    if (e.size() != 32 + 0x678) return Fail("static info request size");
                    std::vector<uint8_t> rep(0x678);
                    for (size_t i = 0; i < rep.size(); ++i) rep[i] = uint8_t(i * 7 + 1);
                    if (!Write(65, rep.data(), uint32_t(rep.size()), static_result)) return false;
                } else if (fn == 47) {
                    ++unload_requests;
                    if (e.size() != 32 + 8) return Fail("unloading request size");
                    if (!Write(47, nullptr, 0, 0)) return false;
                    g_.reg[0x110040] = 0x80000000;
                    g_.reg[0x111240] = 0;                    // RISC-V stopped
                    stage = 4;
                } else {
                    return Fail("unexpected rpc " + std::to_string(fn));
                }
            }
        }
        return true;
    }

private:
    FakeGpu& g_;
    uint8_t* cmdq_ = nullptr;
    uint8_t* msgq_ = nullptr;
    uint32_t msg_seq_ = 0, cmd_seq_seen_ = 0;

    bool Fail(const std::string& s) { error = s; return false; }
    static uint32_t R32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    static uint64_t R64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
    static void W32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
    uint8_t* Mut(uint64_t bus, size_t n) { return const_cast<uint8_t*>(g_.Dma(bus, n)); }

    bool Attach() {
        namespace rm = brook::nvgsp::rm570;
        libos = uint64_t(g_.reg[0x110040]) | (uint64_t(g_.reg[0x110044]) << 32);
        const uint8_t* args = g_.Dma(libos, 4 * sizeof(rm::LibosMemoryRegionInitArgument));
        if (!args) return Fail("libos args not in sysmem");
        static const char* const names[4] = {"LOGINIT", "LOGINTR", "LOGRM", "RMARGS"};
        uint64_t rmargs = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t* a = args + 32 * i;
            uint64_t id = 0; for (const char* n = names[i]; *n; ++n) id = (id << 8) | uint8_t(*n);
            if (R64(a) != id || a[24] != 1 || a[25] != 1) return Fail(std::string("libos arg ") + names[i]);
            const uint64_t pa = R64(a + 8), size = R64(a + 16);
            if (!g_.Dma(pa, size_t(size))) return Fail(std::string("libos region not in sysmem: ") + names[i]);
            if (i < 3) {   // PTE array at +8
                const uint8_t* b = g_.Dma(pa, size_t(size));
                for (uint64_t p = 0; p < size / 0x1000; ++p) if (R64(b + 8 + 8 * p) != pa + p * 0x1000) return Fail("log PTE");
            } else rmargs = pa;
        }
        const uint8_t* ra = g_.Dma(rmargs, sizeof(rm::GSP_ARGUMENTS_CACHED));
        shm = R64(ra);
        const uint32_t ptes = R32(ra + 8);
        const uint64_t cmdq_off = R64(ra + 16), msgq_off = R64(ra + 24);
        if (ra[48] != 1) return Fail("rmargs bDmemStack");
        uint8_t* s = Mut(shm, size_t(msgq_off) + 0x40000);
        if (!s || ptes != 129 || cmdq_off != 0x1000 || msgq_off != 0x41000) return Fail("shared memory args");
        for (uint32_t i = 0; i < ptes; ++i) if (R64(s + 8 * i) != shm + uint64_t(i) * 0x1000) return Fail("shm PTE");
        cmdq_ = s + cmdq_off; msgq_ = s + msgq_off;
        rm::msgqTxHeader tx = {};
        tx.size = 0x40000; tx.msgSize = 0x1000; tx.msgCount = 63; tx.flags = 1; tx.rxHdrOff = 32; tx.entryOff = 0x1000;
        std::memcpy(msgq_, &tx, sizeof(tx));
        // WPR meta -> radix3 -> image; signature; bootloader
        const uint8_t* m = g_.Dma(g_.booter_meta, sizeof(rm::GspFwWprMeta));
        if (!m) return Fail("meta");
        const uint64_t lvl0 = R64(m + 16), elf_size = R64(m + 24), bl = R64(m + 32), bl_size = R64(m + 40);
        const uint64_t sig = R64(m + 72), sig_size = R64(m + 80);
        if (elf_image) {
            if (elf_size != elf_bytes) return Fail("meta elf size");
            const uint8_t* l0 = g_.Dma(lvl0, 8); if (!l0) return Fail("radix3 lvl0");
            const uint64_t l1 = R64(l0);
            for (uint64_t off = 0; off < elf_bytes; off += 0x1000) {
                const uint64_t pg = off / 0x1000;
                const uint8_t* e1 = g_.Dma(l1 + (pg / 512) * 8, 8); if (!e1) return Fail("radix3 lvl1");
                const uint8_t* e2 = g_.Dma(R64(e1) + (pg % 512) * 8, 8); if (!e2) return Fail("radix3 lvl2");
                const size_t n = elf_bytes - off < 0x1000 ? elf_bytes - off : 0x1000;
                const uint8_t* d = g_.Dma(R64(e2), n);
                if (!d || std::memcmp(d, elf_image + off, n)) return Fail("radix3 image page " + std::to_string(pg));
            }
        }
        if (signature) {
            const uint8_t* d = g_.Dma(sig, signature_bytes);
            if (!d || sig_size < signature_bytes || sig_size % 256 || std::memcmp(d, signature, signature_bytes)) return Fail("signature");
        }
        if (bootloader) {
            const uint8_t* d = g_.Dma(bl, bootloader_bytes);
            if (!d || bl_size != bootloader_bytes || std::memcmp(d, bootloader, bootloader_bytes)) return Fail("bootloader");
        }
        return true;
    }

    uint8_t* Slot(uint8_t* q, uint32_t i) { return q + 0x1000 + i * 0x1000; }
    // One element from cmdq (CPU -> GSP); rpc header + payload in *rpc.
    bool ReadElement(uint32_t* fn, std::vector<uint8_t>* rpc) {
        const uint32_t r = R32(msgq_ + 32), w = R32(cmdq_ + 16);
        if (r == w) return false;
        const uint32_t count = R32(Slot(cmdq_, r) + 40);
        if (count < 1 || count > 16 || (w + 63 - r) % 63 < count) return Fail("cmdq element count"), false;
        std::vector<uint8_t> e(count * 0x1000);
        for (uint32_t i = 0; i < count; ++i) std::memcpy(e.data() + i * 0x1000, Slot(cmdq_, (r + i) % 63), 0x1000);
        uint64_t c = 0, x;
        for (size_t i = 0; i < e.size(); i += 8) { std::memcpy(&x, e.data() + i, 8); c ^= x; }
        if ((uint32_t(c >> 32) ^ uint32_t(c)) != 0) return Fail("cmdq checksum"), false;
        if (R32(e.data() + 36) != cmd_seq_seen_++) return Fail("cmdq element sequence"), false;
        const uint32_t len = R32(e.data() + 48 + 8);
        if (R32(e.data() + 48) != 0x03000000 || R32(e.data() + 52) != brook::nvgsp::rm570::RPC_SIGNATURE) return Fail("rpc header"), false;
        *fn = R32(e.data() + 48 + 12);
        rpc->assign(e.begin() + 48, e.begin() + 48 + len);
        W32(msgq_ + 32, (r + count) % 63);
        ++rpcs;
        return true;
    }
    bool ReadRpc(uint32_t* fn, std::vector<uint8_t>* rpc) { return ReadElement(fn, rpc); }
    bool Write(uint32_t fn, const uint8_t* payload, uint32_t n, uint32_t result) {
        const uint32_t len = 32 + n, count = (48 + len + 0xfff) / 0x1000;
        std::vector<uint8_t> e(count * 0x1000, 0);
        W32(e.data() + 36, msg_seq_++); W32(e.data() + 40, count);
        W32(e.data() + 48, 0x03000000); W32(e.data() + 52, brook::nvgsp::rm570::RPC_SIGNATURE); W32(e.data() + 56, len);
        W32(e.data() + 60, fn); W32(e.data() + 64, result);
        if (n) std::memcpy(e.data() + 80, payload, n);
        uint64_t c = 0, x;
        for (size_t i = 0; i < e.size(); i += 8) { std::memcpy(&x, e.data() + i, 8); c ^= x; }
        W32(e.data() + 32, uint32_t(c >> 32) ^ uint32_t(c));
        const uint32_t wp = R32(msgq_ + 16);
        if (62 - (wp + 63 - R32(cmdq_ + 32)) % 63 < count) return Fail("msgq full");
        for (uint32_t i = 0; i < count; ++i) std::memcpy(Slot(msgq_, (wp + i) % 63), e.data() + i * 0x1000, 0x1000);
        W32(msgq_ + 16, (wp + count) % 63);
        return true;
    }
    // Minimal sequencer: the core ops the real 570.144 payload uses.
    static std::vector<uint8_t> SynthSeq() {
        const uint32_t cmds[] = {0, 0x110604, 4, 5, 6, 7, 8};   // WRITE FBIF_TRANSCFG1=4, RESET, START, WAIT, RESUME
        std::vector<uint8_t> p(40 + sizeof(cmds), 0);
        const uint32_t n = sizeof(cmds) / 4;
        std::memcpy(p.data(), &n, 4); std::memcpy(p.data() + 4, &n, 4);
        std::memcpy(p.data() + 40, cmds, sizeof(cmds));
        return p;
    }
};

}  // namespace fake_gpu
