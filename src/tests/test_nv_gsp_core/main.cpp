// Host test for nv_gsp_wpr.h (WPR2 layout + GspFwWprMeta), nv_gsp_radix3.h
// (radix3 build + software walk) and nv_gsp_seq.h (CPU sequencer
// interpreter on a fake register file).
//
// WPR2 goldens:
//  - "trace": 8 GiB, 0x625f04 = 0x00020e09 (bit 3 set, base below fb-1 MiB)
//    as read under Linux on 25 Nov. FWSEC then reported WPR2 0x01ffee00 -
//    0x01fffc00 (FRTS 0x1ffee0000) and RM's static info FB region 4 started
//    at 0x1f7800000 with 0x8800000 (136 MiB) reserved: both reproduced here.
//  - "design doc": 0x625f04 bit 3 clear (as read by Brook on 23 Nov) gives the
//    design doc's table (FRTS 0x1ffe00000, ~137 MiB).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include "nv_gsp/nv_gsp_radix3.h"
#include "nv_gsp/nv_gsp_seq.h"
#include "nv_gsp/nv_gsp_wpr.h"

using namespace brook::nvgsp;
namespace rm = brook::nvgsp::rm570;

constexpr uint64_t kElf570 = 28528288;   // .fwimage of gsp-570.144.bin
constexpr uint64_t kBoot570 = 0x1000;    // bootloader-570.144.bin data_size

static void TestWprTrace() {
    FbLayoutInput in;
    in.fb_size = FbSizeFromReg(0x208);
    in.reg_625f04 = 0x00020e09; in.elf_size = kElf570; in.boot_size = kBoot570;
    FbLayout L;
    assert(ComputeFbLayout(in, &L));
    assert(L.vga_workspace.addr == 0x1fffe0000ull && L.vga_workspace.size == 0x20000);
    assert(L.frts.addr == 0x1ffee0000ull && L.frts.size == 0x100000);
    assert(L.boot.addr == 0x1ffedf000ull && L.boot.size == 0x1000);
    assert(L.elf.addr == 0x1fe3a0000ull && L.elf.size == kElf570);
    assert(L.heap.addr == 0x1f7a00000ull && L.heap.size == 105 * kMiB);
    assert(L.wpr2.addr == 0x1f7900000ull && L.wpr2.end() == 0x1fffe0000ull);
    assert(L.non_wpr_heap.addr == 0x1f7800000ull && L.non_wpr_heap.size == kMiB);
    assert(L.wpr_end == 0x1fffe0000ull);
    assert(L.reserved_top == 0x8800000ull);                  // == RM FB region 4 rsvd
    // the WPR2 registers FWSEC-FRTS left (fwsec.c debug line in dmesg)
    assert(Wpr2RegToAddr(0x01ffee00) == L.frts.addr);
    assert(Wpr2HiToEnd(0x01fffc00) == L.frts.end());
    assert(Wpr2HiToEnd(0) == 0);

    WprMetaSysmem s;
    s.radix3_lvl0 = 0x123456000; s.bootloader = 0x2000; s.signature = 0x3000; s.signature_size = 0x1000;
    rm::GspFwWprMeta m;
    std::memset(&m, 0xcc, sizeof(m));
    FillWprMeta(L, s, &m);
    assert(m.magic == 0xdc3aae21371a60b3ULL && m.revision == 1);
    assert(m.sysmemAddrOfRadix3Elf == 0x123456000 && m.sizeOfRadix3Elf == kElf570);
    assert(m.sizeOfBootloader == 0x1000 && m.sizeOfSignature == 0x1000);
    assert(m.gspFwRsvdStart == 0x1f7800000ull && m.nonWprHeapOffset == 0x1f7800000ull &&
           m.nonWprHeapSize == kMiB && m.gspFwWprStart == 0x1f7900000ull &&
           m.gspFwHeapOffset == 0x1f7a00000ull && m.gspFwHeapSize == 105 * kMiB &&
           m.gspFwOffset == 0x1fe3a0000ull && m.bootBinOffset == 0x1ffedf000ull &&
           m.frtsOffset == 0x1ffee0000ull && m.frtsSize == kMiB && m.gspFwWprEnd == 0x1fffe0000ull &&
           m.fbSize == 8ull << 30 && m.vgaWorkspaceOffset == 0x1fffe0000ull && m.vgaWorkspaceSize == 0x20000);
    assert(m.bootCount == 0 && m.verified == 0 && m.flags == 0 && m.pmuReservedSize == 0);
    // byte offsets as GSP-RM/booter read them
    uint64_t q;
    std::memcpy(&q, reinterpret_cast<uint8_t*>(&m) + 0x70, 8); assert(q == 0x1f7900000ull);
    std::memcpy(&q, reinterpret_cast<uint8_t*>(&m) + 0x98, 8); assert(q == 0x1ffee0000ull);
    // r570 sr data size = FRTS end - non-WPR heap end
    assert((m.frtsOffset + m.frtsSize) - (m.nonWprHeapOffset + m.nonWprHeapSize) == 0x86e0000ull);
}

static void TestWprDesignDoc() {
    FbLayoutInput in;
    in.fb_size = 8ull << 30; in.reg_625f04 = 0x00000001; in.elf_size = kElf570; in.boot_size = kBoot570;
    FbLayout L;
    assert(ComputeFbLayout(in, &L));
    assert(L.non_wpr_heap.addr == 0x1f7700000ull && L.wpr2.addr == 0x1f7800000ull &&
           L.heap.addr == 0x1f7900000ull && L.elf.addr == 0x1fe2c0000ull && L.boot.addr == 0x1ffdff000ull &&
           L.frts.addr == 0x1ffe00000ull && L.vga_workspace.addr == 0x1fff00000ull);
    assert(L.reserved_top == 137 * kMiB && L.heap.size == 105 * kMiB);
    assert(L.wpr2.size == 135 * kMiB);
    // bit 3 set with a base at/above fb-1 MiB: used as is
    assert(VgaWorkspaceAddr(8ull << 30, 0x01fff008, true) == 0x1fff00000ull);
    assert(VgaWorkspaceAddr(8ull << 30, 0x01fffe08, true) == 0x1fffe0000ull);
    // no display engine: register ignored
    assert(VgaWorkspaceAddr(8ull << 30, 0x00020e09, false) == 0x1fff00000ull);
    // heap sizing: 8 MiB base + 96 KiB/GB rounded + 96 MiB channels
    assert(WprHeapSize(8ull << 30) == 105 * kMiB && WprHeapSize(16ull << 30) == 106 * kMiB);
    assert(WprHeapSize(1ull << 30) == 105 * kMiB);
    // refusals
    in.elf_size = 0; assert(!ComputeFbLayout(in, &L));
    in.elf_size = kElf570; in.fb_size = 0x10000000; assert(!ComputeFbLayout(in, &L));
}

// ------------------------------------------------------------- radix3
struct FakeMem {
    std::map<uint64_t, std::vector<uint64_t>> pages;     // bus page -> 512 entries
    uint64_t* Page(uint64_t bus) { auto& v = pages[bus]; v.resize(512); return v.data(); }
    uint64_t Read64(uint64_t bus) {
        auto it = pages.find(bus & ~0xfffull);
        assert(it != pages.end());
        return it->second[(bus & 0xfff) / 8];
    }
};

static void TestRadix3() {
    const Radix3Plan p = PlanRadix3(kElf570);
    assert(p.ok && p.image_pages == 6965 && p.lvl2_pages == 14 && p.total_pages == 16);
    // nouveau: ALIGN(floor(size / 4K) * 8, 4K) / 4K = 14 pages too
    assert(((kElf570 / 4096) * 8 + 4095) / 4096 == p.lvl2_pages);
    assert(PlanRadix3(512ull * 512 * 4096).ok && !PlanRadix3(512ull * 512 * 4096 + 1).ok && !PlanRadix3(0).ok);
    assert(PlanRadix3(4097).image_pages == 2 && PlanRadix3(4097).lvl2_pages == 1);

    FakeMem mem;
    const uint64_t lvl0 = 0x7000000000ull, lvl1 = 0x7000001000ull;
    std::vector<Radix3Page> l2(p.lvl2_pages);
    for (uint64_t j = 0; j < p.lvl2_pages; ++j) {
        const uint64_t bus = 0x7100000000ull + j * 0x3000;      // scattered
        l2[j] = Radix3Page{mem.Page(bus), bus};
    }
    // image pages scattered: page i -> unique bus address (7919 is coprime with 8192)
    auto image_page = [](uint64_t i) { return 0x200000000ull + ((i * 7919) % 8192) * 0x1000; };
    assert(BuildRadix3(p, Radix3Page{mem.Page(lvl0), lvl0}, Radix3Page{mem.Page(lvl1), lvl1}, l2.data(), image_page));
    auto rd = [&](uint64_t a) { return mem.Read64(a); };
    for (uint64_t off = 0; off < kElf570; off += 0x1000 - 8)
        assert(Radix3Translate(lvl0, off, rd) == image_page(off / 0x1000) + off % 0x1000);
    assert(Radix3Translate(lvl0, kElf570 - 1, rd) == image_page(6964) + (kElf570 - 1) % 4096);
    assert(Radix3Translate(lvl0, 6965ull * 4096, rd) == 0);    // past the image: hole
    assert(mem.Read64(lvl0) == lvl1 && mem.Read64(lvl0 + 8) == 0);
    assert(mem.Read64(lvl1 + 13 * 8) == l2[13].bus && mem.Read64(lvl1 + 14 * 8) == 0);
    // unaligned table or image page addresses are refused
    assert(!BuildRadix3(p, Radix3Page{mem.Page(lvl0), lvl0 + 8}, Radix3Page{mem.Page(lvl1), lvl1}, l2.data(), image_page));
    assert(!BuildRadix3(p, Radix3Page{mem.Page(lvl0), lvl0}, Radix3Page{mem.Page(lvl1), lvl1}, l2.data(),
                        [](uint64_t i) { return 0x1000 * i + 4; }));
}

// ---------------------------------------------------------- sequencer
struct SeqBus {
    std::map<uint32_t, uint32_t> reg;
    std::vector<std::pair<char, uint32_t>> log;
    std::map<uint32_t, int> ready_after;          // reg -> reads until it reads `ready_value`
    std::map<uint32_t, uint32_t> ready_value;
    uint64_t t = 0, delayed = 0;
    int resets = 0, starts = 0, waits = 0, resumes = 0;
    bool fail_core = false;
    std::vector<uint32_t> steps;
    uint32_t rd32(uint32_t a) {
        log.push_back({'R', a});
        auto it = ready_after.find(a);
        if (it != ready_after.end() && --it->second <= 0) reg[a] = ready_value[a];
        return reg[a];
    }
    void wr32(uint32_t a, uint32_t v) { log.push_back({'W', a}); reg[a] = v; }
    void udelay(uint32_t us) { t += us; delayed += us; }
    uint64_t now_us() { return ++t; }
    void seq_step(uint32_t op) { steps.push_back(op); }
    bool core_reset() { ++resets; return !fail_core; }
    bool core_start() { ++starts; return true; }
    bool core_wait_halt() { ++waits; return true; }
    bool core_resume() { ++resumes; return true; }
};

static std::vector<uint8_t> SeqMsg(const std::vector<uint32_t>& cmds, uint32_t cmd_index = ~0u, uint32_t bufsz = 0x3fe2) {
    std::vector<uint8_t> m(40 + 4 * cmds.size(), 0);
    const uint32_t idx = cmd_index == ~0u ? uint32_t(cmds.size()) : cmd_index;
    std::memcpy(m.data(), &bufsz, 4);
    std::memcpy(m.data() + 4, &idx, 4);
    if (!cmds.empty()) std::memcpy(m.data() + 40, cmds.data(), 4 * cmds.size());
    return m;
}

static void TestSequencer() {
    using namespace rm;
    const std::vector<uint32_t> cmds = {
        GSP_SEQ_BUF_OPCODE_REG_WRITE, 0x110040, 0x12345678,
        GSP_SEQ_BUF_OPCODE_REG_MODIFY, 0x110624, 0x80, 0x80,
        GSP_SEQ_BUF_OPCODE_REG_POLL, 0x1180f8, 0x04000000, 0x04000000, 0, 0,   // timeout 0 -> 4 s default
        GSP_SEQ_BUF_OPCODE_DELAY_US, 25,
        GSP_SEQ_BUF_OPCODE_REG_STORE, 0x110044, 3,
        GSP_SEQ_BUF_OPCODE_CORE_RESET,
        GSP_SEQ_BUF_OPCODE_CORE_START,
        GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT,
        GSP_SEQ_BUF_OPCODE_CORE_RESUME,
    };
    std::vector<uint8_t> m = SeqMsg(cmds);
    SeqBus b;
    b.reg[0x110624] = 0x110; b.reg[0x110044] = 0xabcd;
    b.ready_after[0x1180f8] = 3; b.ready_value[0x1180f8] = 0x17300000;
    SeqResult r = RunSequencer(b, m.data(), uint32_t(m.size()));
    assert(r.status == SeqStatus::Ok && r.commands == 9);
    assert(r.writes == 1 && r.modifies == 1 && r.polls == 1 && r.delays == 1 && r.stores == 1 && r.core_ops == 4);
    assert(b.reg[0x110040] == 0x12345678 && b.reg[0x110624] == 0x190 && r.save[3] == 0xabcd);
    assert(b.delayed >= 25 && b.resets == 1 && b.starts == 1 && b.waits == 1 && b.resumes == 1);
    assert(b.steps.size() == 9 && b.steps[2] == GSP_SEQ_BUF_OPCODE_REG_POLL && b.steps[8] == GSP_SEQ_BUF_OPCODE_CORE_RESUME);
    // poll: priming read + reads until ready (nouveau r535 gsp.c 1047-1051)
    int poll_reads = 0;
    for (auto& e : b.log) poll_reads += e.first == 'R' && e.second == 0x1180f8;
    assert(poll_reads == 3);
    assert(b.log[0] == std::make_pair('W', 0x110040u));

    // validation happens before anything executes
    SeqBus z;
    std::vector<uint32_t> bad = cmds; bad.push_back(9);
    m = SeqMsg(bad);
    r = RunSequencer(z, m.data(), uint32_t(m.size()));
    assert(r.status == SeqStatus::BadOpcode && r.fail_index == uint32_t(cmds.size()) && z.log.empty());
    bad = {GSP_SEQ_BUF_OPCODE_REG_STORE, 0x110040, 8};
    m = SeqMsg(bad);
    r = RunSequencer(z, m.data(), uint32_t(m.size()));
    assert(r.status == SeqStatus::BadSlot && z.log.empty());
    m = SeqMsg({GSP_SEQ_BUF_OPCODE_REG_WRITE, 0x110040, 1}, 2);   // payload crosses cmdIndex
    r = RunSequencer(z, m.data(), uint32_t(m.size()));
    assert(r.status == SeqStatus::Truncated && z.log.empty());
    m = SeqMsg({GSP_SEQ_BUF_OPCODE_REG_WRITE, 0x110040, 1}, 4);   // cmdIndex past the message
    r = RunSequencer(z, m.data(), uint32_t(m.size()));
    assert(r.status == SeqStatus::Truncated);
    m = SeqMsg({GSP_SEQ_BUF_OPCODE_REG_WRITE, 0x110040, 1}, 3, 2);  // past bufferSizeDWord
    r = RunSequencer(z, m.data(), uint32_t(m.size()));
    assert(r.status == SeqStatus::Truncated);
    r = RunSequencer(z, m.data(), 39);
    assert(r.status == SeqStatus::Truncated && z.log.empty());
    // a poll that never matches fails (nouveau would carry on)
    m = SeqMsg({GSP_SEQ_BUF_OPCODE_REG_POLL, 0x1180f8, 1, 1, 50, 0, GSP_SEQ_BUF_OPCODE_REG_WRITE, 0x110040, 1});
    SeqBus p;
    r = RunSequencer(p, m.data(), uint32_t(m.size()));
    assert(r.status == SeqStatus::PollTimeout && r.fail_index == 0 && p.reg.count(0x110040) == 0);
    // a failing core op stops the sequence
    m = SeqMsg({GSP_SEQ_BUF_OPCODE_CORE_RESET, GSP_SEQ_BUF_OPCODE_REG_WRITE, 0x110040, 1});
    SeqBus f; f.fail_core = true;
    r = RunSequencer(f, m.data(), uint32_t(m.size()));
    assert(r.status == SeqStatus::CoreFailed && f.reg.count(0x110040) == 0);
    // empty sequence is fine
    m = SeqMsg({});
    r = RunSequencer(z, m.data(), uint32_t(m.size()));
    assert(r.status == SeqStatus::Ok && r.commands == 0);
}

int main() {
    TestWprTrace();
    TestWprDesignDoc();
    TestRadix3();
    TestSequencer();
    std::printf("test_nv_gsp_core: OK\n");
    return 0;
}
