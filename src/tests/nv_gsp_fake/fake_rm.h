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
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "fake_gpu.h"
#include "nv_gsp/nvrm_r570.h"
#include "nv_gsp/nvrm_r570_obj.h"
#include "nv_gsp/nv_gmmu.h"

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
                    std::vector<uint8_t> rep = StaticReply();
                    if (!Write(65, rep.data(), uint32_t(rep.size()), static_result)) return false;
                } else if (fn == 47) {
                    ++unload_requests;
                    if (e.size() != 32 + 8) return Fail("unloading request size");
                    if (!Write(47, nullptr, 0, 0)) return false;
                    g_.reg[0x110040] = 0x80000000;
                    g_.reg[0x111240] = 0;                    // RISC-V stopped
                    stage = 4;
                } else if (fn == 103) {
                    if (!HandleAlloc(e)) return false;
                } else if (fn == 76) {
                    if (!HandleControl(e)) return false;
                } else if (fn == 10) {
                    if (!HandleFree(e)) return false;
                } else {
                    return Fail("unexpected rpc " + std::to_string(fn));
                }
            }
            if (!Host()) return false;
        }
        return true;
    }

    // ---- M2 model inputs/observations
    static constexpr uint32_t kIntClient = 0xc1e00004, kIntDevice = 0xde1d0001, kIntSubdevice = 0x5d1d0001;
    static constexpr uint64_t kBar1Pde = 0x1f76c3000ull, kBar2Pde = 0x1f76c4000ull, kBar1Inst = 0x1f76c0000ull;
    bool token_ctrl_supported = true, channel_schedule_supported = true;
    uint32_t fail_class = ~0u;                     // RM_ALLOC of this class returns NV_ERR_INVALID_ARGUMENT
    struct Obj { uint32_t parent, cls; std::vector<uint8_t> params; };
    std::map<uint32_t, Obj> objs;
    std::vector<std::string> calls;                // "alloc 0xa06c", "ctrl 0x801813", "free ..."
    uint64_t pdb = 0; uint32_t pdb_aper = 9;       // SET_PAGE_DIRECTORY
    bool scheduled = false, bound = false;
    uint32_t sem_releases = 0, nops = 0, frees = 0;
    std::string host_error;

    // Program BAR1 the way GSP-RM leaves it after INIT_DONE (virtual, empty PD3).
    void SetupBar1() {
        g_.reg[0xb80f40] = 0x80000000u | uint32_t(kBar1Inst >> 12);
        g_.reg[0xb80f48] = 0xc0000000u | uint32_t((kBar1Inst + 0x1000) >> 12);
        g_.SetVram32(kBar1Inst + 0x200, uint32_t(kBar1Pde) | (1u << 10) | (1u << 11));
        g_.SetVram32(kBar1Inst + 0x204, uint32_t(kBar1Pde >> 32));
        g_.reg[0x1700] = 0x1fff0;                  // where the firmware left PRAMIN
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
    // GspStaticConfigInfo as RM 570.144 fills it on this card (fields Brook
    // parses; everything else keeps the old filler so static_head checks hold).
    static std::vector<uint8_t> StaticReply() {
        namespace si = brook::nvgsp::rm570::static_info;
        std::vector<uint8_t> r(si::kBytes);
        for (size_t i = 0; i < r.size(); ++i) r[i] = uint8_t(i * 7 + 1);
        auto w32 = [&](uint32_t o, uint32_t v) { std::memcpy(r.data() + o, &v, 4); };
        auto w64 = [&](uint32_t o, uint64_t v) { std::memcpy(r.data() + o, &v, 8); };
        w32(si::kGidInfo, 0); w32(si::kGidInfo + 4, 2); w32(si::kGidInfo + 8, 16);
        for (uint32_t i = 0; i < 16; ++i) r[si::kGidInfo + 12 + i] = uint8_t(0xa0 + i);
        std::memset(r.data() + si::kFbRegionInfo, 0, 0x308);
        w32(si::kFbRegionInfo, 3);
        struct R { uint64_t b, l, rsv; uint8_t comp, iso, prot; } regs[3] = {
            {0x0, 0x1f76fffffull, 0, 1, 1, 0},                   // client VRAM
            {0x1f7700000ull, 0x1f7ffffffull, 0x900000, 0, 0, 0},  // RM reserved
            {0x1f8000000ull, 0x1ffffffffull, 0, 0, 0, 1}};        // WPR2 (protected)
        for (uint32_t i = 0; i < 3; ++i) {
            const uint32_t o = si::kFbRegionInfo + si::kFbRegionFirst + i * si::kFbRegionStride;
            w64(o, regs[i].b); w64(o + 8, regs[i].l); w64(o + 16, regs[i].rsv);
            r[o + 28] = regs[i].comp; r[o + 29] = regs[i].iso; r[o + 30] = regs[i].prot;
        }
        w64(si::kFbLength, 0x200000000ull);
        std::memset(r.data() + si::kGpuName, 0, 64);
        std::memcpy(r.data() + si::kGpuName, "NVIDIA GeForce RTX 2080", 23);
        std::memset(r.data() + si::kGpuShortName, 0, 64);
        std::memcpy(r.data() + si::kGpuShortName, "TU104", 5);
        w64(si::kBar1PdeBase, kBar1Pde); w64(si::kBar2PdeBase, kBar2Pde);
        w32(si::kInternalClient, kIntClient); w32(si::kInternalDevice, kIntDevice); w32(si::kInternalSubdevice, kIntSubdevice);
        r[si::kSplitVas] = 0; r[si::kIsGpuUefi] = 1;
        return r;
    }

    // ---- RM objects
    static constexpr uint32_t kErrNoMemory = 0x51;
    static constexpr uint32_t kErrInvalidArgument = 0x1f, kErrInvalidObjectParent = 0x2c, kErrNotSupported = 0x56,
                              kErrInvalidObjectHandle = 0x33;
    bool Reply(uint32_t fn, std::vector<uint8_t>& e, uint32_t status_off, uint32_t status) {
        W32(e.data() + 32 + status_off, status);
        return Write(fn, e.data() + 32, uint32_t(e.size() - 32), 0);
    }
    bool HandleAlloc(std::vector<uint8_t>& e) {
        namespace rm = brook::nvgsp::rm570;
        if (e.size() < 32 + 0x20) return Fail("alloc size");
        const uint8_t* a = e.data() + 32;
        const uint32_t hClient = R32(a), hParent = R32(a + 4), hObject = R32(a + 8), cls = R32(a + 12), n = R32(a + 20);
        if (e.size() != 32 + 0x20 + n) return Fail("alloc paramsSize");
        const uint8_t* p = a + 0x20;
        char b[48]; std::snprintf(b, sizeof(b), "alloc 0x%x", cls); calls.push_back(b);
        uint32_t st = 0;
        auto need = [&](uint32_t sz) { if (n != sz) st = kErrInvalidArgument; };
        auto parent_is = [&](uint32_t c) { auto it = objs.find(hParent); if (it == objs.end() || it->second.cls != c) st = kErrInvalidObjectParent; };
        if (objs.count(hObject)) st = kErrInvalidObjectHandle;
        else switch (cls) {
            case rm::NV01_ROOT: need(0x78); if (!st && (hObject != hClient || R32(p) != hClient)) { st = kErrInvalidArgument; }
                break;
            case rm::NV01_DEVICE_0: need(0x38); parent_is(rm::NV01_ROOT); if (!st && R32(p + 4) != hClient) { st = kErrInvalidArgument; }
                break;
            case rm::NV20_SUBDEVICE_0: need(4); parent_is(rm::NV01_DEVICE_0); break;
            case rm::FERMI_VASPACE_A: need(0x30); parent_is(rm::NV01_DEVICE_0);
                if (!st && R32(p + 4) != rm::NV_VASPACE_ALLOCATION_FLAGS_IS_EXTERNALLY_OWNED) { st = kErrInvalidArgument; }
                break;
            case rm::KEPLER_CHANNEL_GROUP_A: need(20); parent_is(rm::NV01_DEVICE_0);
                if (!st && (!objs.count(R32(p + 8)) || objs[R32(p + 8)].cls != rm::FERMI_VASPACE_A)) { st = kErrInvalidArgument; }
                break;
            case rm::TURING_CHANNEL_GPFIFO_A: {
                need(sizeof(rm::NV_CHANNEL_ALLOC_PARAMS));
                if (st) break;
                rm::NV_CHANNEL_ALLOC_PARAMS c; std::memcpy(&c, p, sizeof(c));
                auto par = objs.find(hParent);
                if (par == objs.end()) { st = kErrInvalidObjectParent; break; }
                if (par->second.cls == rm::KEPLER_CHANNEL_GROUP_A) {
                    const uint32_t tsg_eng = R32(par->second.params.data() + 12);
                    if (c.hVASpace != 0 || (c.engineType && c.engineType != tsg_eng)) st = kErrInvalidArgument;
                } else if (par->second.cls == rm::NV01_DEVICE_0) {
                    if (!objs.count(c.hVASpace) || !c.engineType) st = kErrInvalidArgument;
                } else { st = kErrInvalidObjectParent; break; }
                const uint32_t chid = ((c.flags >> 12) & 0x1ff) * 8 + ((c.flags >> 8) & 7);
                if (!st && chid < 1) st = kErrNoMemory;                     // RM keeps chid 0 (rsvd_chids = 1)
                if (c.instanceMem.addressSpace != rm::ADDR_FBMEM || c.userdMem.addressSpace != rm::ADDR_FBMEM ||
                    c.mthdbufMem.addressSpace != rm::ADDR_SYSMEM || !c.mthdbufMem.size || c.gpFifoEntries == 0) st = kErrInvalidArgument;
                if (!st && !g_.Dma(c.mthdbufMem.base, size_t(c.mthdbufMem.size))) st = kErrInvalidArgument;
                if (!st && pdb_aper == 9) st = kErrInvalidArgument;   // VAS without a page directory
                break;
            }
            case 0xc5b5: {   // TURING_DMA_COPY_A under the channel, {version 1, engineType = the channel's CE}
                need(8); parent_is(rm::TURING_CHANNEL_GPFIFO_A);
                if (st) break;
                rm::NV_CHANNEL_ALLOC_PARAMS c; std::memcpy(&c, objs[hParent].params.data(), sizeof(c));
                uint32_t eng = c.engineType;
                if (!eng) { auto g = objs.find(objs[hParent].parent); if (g != objs.end()) eng = R32(g->second.params.data() + 12); }
                if (R32(p) != 1 || R32(p + 4) != eng) st = kErrInvalidArgument;
                break;
            }
            default: st = kErrNotSupported; break;
        }
        if (cls == fail_class) st = kErrInvalidArgument;
        if (!st) objs[hObject] = Obj{hParent, cls, std::vector<uint8_t>(p, p + n)};
        return Reply(103, e, 16, st);
    }
    bool HandleControl(std::vector<uint8_t>& e) {
        namespace rm = brook::nvgsp::rm570;
        if (e.size() < 32 + 0x18) return Fail("control size");
        uint8_t* a = e.data() + 32;
        const uint32_t hClient = R32(a), hObject = R32(a + 4), cmd = R32(a + 8), n = R32(a + 16);
        if (e.size() != 32 + 0x18 + n) return Fail("control paramsSize");
        uint8_t* p = a + 0x18;
        char b[48]; std::snprintf(b, sizeof(b), "ctrl 0x%x", cmd); calls.push_back(b);
        uint32_t st = 0;
        const bool internal = hClient == kIntClient && hObject == kIntSubdevice;
        auto obj_is = [&](uint32_t c) { auto it = objs.find(hObject); return it != objs.end() && it->second.cls == c; };
        switch (cmd) {
            case rm::NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE: {
                if (!internal || n != sizeof(rm::NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS)) { st = kErrInvalidArgument; break; }
                rm::NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS t = {};
                struct { uint32_t eng, rl; const char* name; } d[] = {
                    {rm::RM_ENGINE_TYPE_GR0, 0, "GRAPHICS"}, {rm::RM_ENGINE_TYPE_COPY0, 0, "GRCE"},
                    {rm::RM_ENGINE_TYPE_COPY0 + 1, 0, "GRCE"}, {rm::RM_ENGINE_TYPE_COPY0 + 2, 2, "CE2"},
                    {rm::RM_ENGINE_TYPE_COPY0 + 3, 3, "CE3"}};
                t.numEntries = 5;
                for (uint32_t i = 0; i < 5; ++i) {
                    t.entries[i].engineData[rm::ENGINE_INFO_TYPE_RM_ENGINE_TYPE] = d[i].eng;
                    t.entries[i].engineData[rm::ENGINE_INFO_TYPE_RUNLIST] = d[i].rl;
                    std::strncpy(t.entries[i].engineName, d[i].name, 15);
                }
                std::memcpy(p, &t, sizeof(t));
                break;
            }
            case rm::NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE:
                if (!internal || n != 4) st = kErrInvalidArgument; else W32(p, 0x1000);
                break;
            case rm::NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY: {
                if (!obj_is(rm::NV01_DEVICE_0) || n != 0x20) { st = kErrInvalidArgument; break; }
                rm::NV0080_CTRL_DMA_SET_PAGE_DIRECTORY_PARAMS q; std::memcpy(&q, p, sizeof(q));
                if (!objs.count(q.hVASpace) || q.numEntries != 4 || (q.physAddress & 0xfff)) { st = kErrInvalidArgument; break; }
                pdb = q.physAddress; pdb_aper = q.flags & 3;
                break;
            }
            case rm::NV0080_CTRL_CMD_DMA_UNSET_PAGE_DIRECTORY:
                if (!obj_is(rm::NV01_DEVICE_0)) st = kErrInvalidArgument; else pdb_aper = 9;
                break;
            case rm::NVA06F_CTRL_CMD_BIND:
                if (!obj_is(rm::TURING_CHANNEL_GPFIFO_A) || n != 4) st = kErrInvalidArgument; else bound = true;
                break;
            case rm::NVA06F_CTRL_CMD_GPFIFO_SCHEDULE:
                if (!obj_is(rm::TURING_CHANNEL_GPFIFO_A) || !bound) st = kErrInvalidArgument;
                else if (!channel_schedule_supported) st = kErrNotSupported;
                else scheduled = p[0];
                break;
            case 0xa06c0101:
                if (!obj_is(rm::KEPLER_CHANNEL_GROUP_A)) st = kErrInvalidArgument; else scheduled = p[0];
                break;
            case rm::NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN:
                if (!obj_is(rm::TURING_CHANNEL_GPFIFO_A) || !token_ctrl_supported) st = kErrNotSupported;
                else W32(p, (2u << 16) | ChannelChid());
                break;
            default: st = kErrNotSupported; break;
        }
        return Reply(76, e, 12, st);
    }
    bool HandleFree(std::vector<uint8_t>& e) {
        if (e.size() != 32 + 16) return Fail("free size");
        const uint32_t hRoot = R32(e.data() + 32), hObj = R32(e.data() + 40);
        calls.push_back("free");
        uint32_t st = 0;
        if (!objs.count(hObj)) st = kErrInvalidObjectHandle;
        else {
            // freeing removes the object and its descendants
            std::vector<uint32_t> kill{hObj};
            for (size_t i = 0; i < kill.size(); ++i)
                for (auto& kv : objs) if (kv.second.parent == kill[i] && kv.first != kill[i]) kill.push_back(kv.first);
            for (uint32_t h : kill) objs.erase(h);
            ++frees;
        }
        (void)hRoot;
        return Reply(10, e, 12, st);
    }

    // ---- Host/PBDMA model: on each doorbell, fetch GPFIFO entries
    // GP_GET..GP_PUT (USERD in VRAM) through the VAS page tables, run host
    // methods (NOP, SEM_* release) and advance GP_GET.
    struct PtMem {
        fake_gpu::FakeGpu& g;
        uint64_t rd64(brook::nvgsp::Aperture a, uint64_t addr) {
            if (a == brook::nvgsp::Aperture::Video) return g.Vram64(addr);
            const uint8_t* d = g.Dma(addr, 8); uint64_t v = 0; if (d) std::memcpy(&v, d, 8); return v;
        }
    };
    uint32_t ChannelChid() {
        for (auto& kv : objs)
            if (kv.second.cls == brook::nvgsp::rm570::TURING_CHANNEL_GPFIFO_A) {
                const uint32_t f = R32(kv.second.params.data() + 0x14);
                return ((f >> 12) & 0x1ff) * 8 + ((f >> 8) & 7);
            }
        return ~0u;
    }
    size_t doorbells_seen = 0;
    bool HostFail(const std::string& s) { host_error = s; return Fail("host: " + s); }
    uint8_t* Va(uint64_t va, size_t n) {
        PtMem m{g_};
        const auto t = brook::nvgsp::GmmuWalk(m, brook::nvgsp::kGmmuV2, pdb,
                                              pdb_aper == 0 ? brook::nvgsp::Aperture::Video : brook::nvgsp::Aperture::SysCoh, va);
        if (!t.ok || t.aper != brook::nvgsp::Aperture::SysCoh) return nullptr;
        return Mut(t.pa, n);
    }
    // VA -> (aperture, physical); VRAM through the fake's VRAM pages, sysmem through DMA.
    bool Xlate(uint64_t va, brook::nvgsp::Aperture* a, uint64_t* pa) {
        PtMem m{g_};
        const auto t = brook::nvgsp::GmmuWalk(m, brook::nvgsp::kGmmuV2, pdb,
                                              pdb_aper == 0 ? brook::nvgsp::Aperture::Video : brook::nvgsp::Aperture::SysCoh, va);
        if (!t.ok) return false;
        *a = t.aper; *pa = t.pa; return true;
    }
    bool CeRead32(uint64_t va, uint32_t* v) {
        brook::nvgsp::Aperture a; uint64_t pa;
        if (!Xlate(va, &a, &pa)) return false;
        if (a == brook::nvgsp::Aperture::Video) { *v = g_.Vram32(pa); return true; }
        const uint8_t* d = g_.Dma(pa, 4); if (!d) return false; *v = R32(d); return true;
    }
    bool CeWrite32(uint64_t va, uint32_t v) {
        brook::nvgsp::Aperture a; uint64_t pa;
        if (!Xlate(va, &a, &pa)) return false;
        if (a == brook::nvgsp::Aperture::Video) { g_.SetVram32(pa, v); return true; }
        uint8_t* d = Mut(pa, 4); if (!d) return false; W32(d, v); return true;
    }
  public:
    uint32_t ce_class = 0; uint32_t ce_m[8] = {}; size_t ce_launches = 0; uint64_t ce_bytes = 0;
    bool CeMethod(uint32_t m, uint32_t d) {
        if (m == 0) { if (d != 0xc5b5) return HostFail("SET_OBJECT class");
            bool have = false; for (auto& kv : objs) if (kv.second.cls == 0xc5b5) have = true;
            if (!have) return HostFail("SET_OBJECT without a copy object"); ce_class = d; return true; }
        if (!ce_class) return HostFail("CE method before SET_OBJECT");
        if (m >= 0x400 && m <= 0x41c) { ce_m[(m - 0x400) / 4] = d; return true; }
        if (m != 0x300) return HostFail("CE method");
        // LAUNCH_DMA: only the pitch-linear, virtual, non-pipelined + flush, multi-line form Brook uses
        if (d != (2u | 4u | (1u << 7) | (1u << 8) | (1u << 9))) return HostFail("LAUNCH_DMA flags");
        const uint64_t src = (uint64_t(ce_m[0] & 0xff) << 32) | ce_m[1], dst = (uint64_t(ce_m[2] & 0xff) << 32) | ce_m[3];
        const uint32_t pin = ce_m[4], pout = ce_m[5], len = ce_m[6], lines = ce_m[7];
        if (len & 3) return HostFail("line length");
        for (uint32_t y = 0; y < lines; ++y)
            for (uint32_t x = 0; x < len; x += 4) {
                uint32_t v;
                if (!CeRead32(src + uint64_t(y) * pin + x, &v) || !CeWrite32(dst + uint64_t(y) * pout + x, v)) {
                    ++mmu_faults; return HostFail("CE address not mapped");
                }
            }
        ++ce_launches; ce_bytes += uint64_t(len) * lines;
        return true;
    }
    size_t mmu_faults = 0;
    bool host_stall = false;   // the engine stops making progress (doorbells stay queued)
    bool Host() {
        if (host_stall) return true;
        namespace rm = brook::nvgsp::rm570;
        while (doorbells_seen < g_.doorbells.size()) {
            const uint32_t token = g_.doorbells[doorbells_seen++];
            const Obj* ch = nullptr;
            for (auto& kv : objs) if (kv.second.cls == rm::TURING_CHANNEL_GPFIFO_A) ch = &kv.second;
            if (!ch || !scheduled) return HostFail("doorbell without a scheduled channel");
            rm::NV_CHANNEL_ALLOC_PARAMS c; std::memcpy(&c, ch->params.data(), sizeof(c));
            if (token != ((2u << 16) | ChannelChid())) return HostFail("doorbell token");
            const uint64_t userd = c.userdMem.base;
            uint32_t get = g_.Vram32(userd + rm::host::USERD_GP_GET);
            const uint32_t put = g_.Vram32(userd + rm::host::USERD_GP_PUT);
            while (get != put) {
                const uint8_t* gp = Va(c.gpFifoOffset + get * 8ull, 8);
                if (!gp) return HostFail("GPFIFO not mapped");
                const uint32_t lo = R32(gp), hi = R32(gp + 4);
                const uint64_t addr = lo | (uint64_t(hi & 0xff) << 32);
                const uint32_t words = (hi >> 10) & 0x1fffff;
                const uint8_t* pb = Va(addr, words * 4ull);
                if (!pb || !words) return HostFail("push buffer not mapped");
                uint32_t sem_lo = 0, sem_hi = 0, payload = 0;
                for (uint32_t i = 0; i < words;) {
                    const uint32_t h = R32(pb + 4 * i++);
                    if ((h >> 29) != 1) return HostFail("push header");
                    const uint32_t cnt = (h >> 16) & 0x1fff; uint32_t m = (h & 0xfff) << 2;
                    const uint32_t subc = (h >> 13) & 7;
                    for (uint32_t k = 0; k < cnt; ++k, m += 4) {
                        const uint32_t d = R32(pb + 4 * i++);
                        if (subc == 4) { if (!CeMethod(m, d)) return false; continue; }
                        if (subc != 0) return HostFail("subchannel");
                        if (m == rm::host::NOP) ++nops;
                        else if (m == rm::host::SEM_ADDR_LO) sem_lo = d;
                        else if (m == rm::host::SEM_ADDR_HI) sem_hi = d;
                        else if (m == rm::host::SEM_PAYLOAD_LO) payload = d;
                        else if (m == rm::host::SEM_EXECUTE) {
                            if ((d & 7) != rm::host::SEM_EXECUTE_RELEASE) return HostFail("sem op");
                            uint8_t* sp = Va(sem_lo | (uint64_t(sem_hi & 0xff) << 32), 4);
                            if (!sp) return HostFail("semaphore not mapped");
                            W32(sp, payload); ++sem_releases;
                        } else return HostFail("method");
                    }
                }
                get = (get + 1) % c.gpFifoEntries;
            }
            g_.SetVram32(userd + rm::host::USERD_GP_GET, get);
        }
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
