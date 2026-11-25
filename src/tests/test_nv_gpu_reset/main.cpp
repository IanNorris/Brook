// Host test for nv_gpu_reset.h: the pre-reset GPU reset sequence against a fake TU104
// function (config space, BAR0 PMC/GFW registers) and its upstream bridge.
#include "nv_gpu_reset.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
using namespace brook::nvreset;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

struct Fake {
    uint8_t cfg[256]{};
    uint16_t bctl = 0;
    bool bridge = true, flr_cap = true;
    // behaviour knobs
    uint32_t tp_clears_after_us = 300;   // TransactionsPending clears this long after BME off
    bool flr_breaks_gfw = false;          // GFW never completes after an FLR (needs SBR)
    bool flr_loses_link = false;          // vendor ID reads all-ones after FLR
    uint32_t gfw_after_reset_us = 400000; // GFW boot duration after any reset
    // state
    uint64_t now = 0, bme_off_at = ~0ull, reset_at = ~0ull; bool gfw_broken = false, link_lost = false;
    uint32_t pmc = 0x5fecdff1u, flrs = 0, sbrs = 0;
    std::vector<std::string> ev;
    static constexpr uint8_t kCap = 0x60;
    Fake() {
        w32(0, 0x1e8710de); w32(4, 0x00100007); w32(0x10, 0xde000000); w32(0x14, 0xc000000c); w32(0x18, 0);
        w32(0x1c, 0xd000000c); w32(0x24, 0xe001); w32(0x34, 0x40); w32(0x3c, 0x010a);
        w32(0x40, 0x00000001 | (uint32_t(kCap) << 8)); // PM cap -> PCIe cap
        w32(kCap, 0x00020010); w32(kCap + 4, 0x10008fe1); w32(kCap + 8, 0x00002910);
    }
    void w32(uint8_t o, uint32_t v) { std::memcpy(cfg + o, &v, 4); }
    uint32_t r32raw(uint8_t o) const { uint32_t v; std::memcpy(&v, cfg + o, 4); return v; }
    bool decode() const { return (r32raw(4) & 2) && !link_lost; }
    bool gfw_done() const { return reset_at == ~0ull || (!gfw_broken && now - reset_at >= gfw_after_reset_us); }
    void reset_fn(bool sbr) {
        // config header back to power-on defaults (BARs and command cleared)
        uint32_t keep0 = r32raw(0);
        for (uint8_t o = 4; o < 0x28; o += 4) w32(o, 0);
        w32(0x3c, 0); w32(0, keep0);
        uint32_t dc = r32raw(kCap + 8); w32(kCap + 8, dc & 0xffff0000u);
        pmc = 0x40000020u; reset_at = now; bme_off_at = ~0ull;
        gfw_broken = !sbr && flr_breaks_gfw; link_lost = !sbr && flr_loses_link;
    }
};
struct Cfg {
    Fake& f;
    uint32_t rd32(uint8_t o) {
        if (f.link_lost) return 0xffffffffu;
        uint32_t v = f.r32raw(o);
        if (o == Fake::kCap + 4 && !f.flr_cap) v &= ~(1u << 28);
        if (o == Fake::kCap + 8) {   // DevSta.TP (bit 21 of the dword)
            const bool tp = f.bme_off_at == ~0ull || f.now - f.bme_off_at < f.tp_clears_after_us;
            v = (v & ~(1u << 21)) | (tp ? (1u << 21) : 0);
        }
        return v;
    }
    uint16_t rd16(uint8_t o) { return uint16_t(rd32(uint8_t(o & ~3)) >> ((o & 2) * 8)); }
    void wr32(uint8_t o, uint32_t v) { f.w32(o, v); }
    void wr16(uint8_t o, uint16_t v) {
        if (o == 4) {
            const bool was = f.r32raw(4) & 4;
            if (was && !(v & 4)) { f.bme_off_at = f.now; f.ev.push_back("bme_off"); }
        }
        if (o == Fake::kCap + 8 && (v & 0x8000)) {
            f.ev.push_back("flr"); ++f.flrs; f.reset_fn(false); return;
        }
        uint32_t d = f.r32raw(uint8_t(o & ~3)); const int sh = (o & 2) * 8;
        d = (d & ~(0xffffu << sh)) | (uint32_t(v) << sh); f.w32(uint8_t(o & ~3), d);
    }
    bool has_bridge() { return f.bridge; }
    uint16_t brd16(uint8_t) { return f.bctl; }
    void bwr16(uint8_t, uint16_t v) {
        if ((f.bctl & 0x40) && !(v & 0x40)) { f.ev.push_back("sbr"); ++f.sbrs; f.reset_fn(true); }
        f.bctl = v;
    }
};
struct Mmio {
    Fake& f;
    uint32_t rd32(uint32_t o) {
        if (!f.decode()) return 0xffffffffu;
        if (o == kPmcEnable) return f.pmc;
        if (o == kGfwPlm) return f.gfw_done() ? 0x1 : 0x0;
        if (o == kGfwProgress) return f.gfw_done() ? 0xff : 0x3c;
        return 0xbadf1100u;
    }
    void wr32(uint32_t o, uint32_t v) {
        if (!f.decode()) return;
        if (o == kPmcEnable) { f.pmc = v; f.ev.push_back("pmc"); }
    }
};
struct Clock { Fake& f; uint64_t now_us() { return f.now; } void delay_us(uint32_t us) { f.now += us; } };

static Result run(Fake& f, const Plan& p) { Cfg c{f}; Mmio m{f}; Clock k{f}; return RunGpuReset(c, m, k, p); }

int main() {
    {   // GSP boot: engines reset, BME off, TP drains, FLR, GFW completes
        Fake f; Plan p; p.engines = true;
        Result r = run(f, p);
        CHECK(r.ok()); CHECK(std::string(r.path) == "flr");
        CHECK(r.pmc_before == 0x5fecdff1u); CHECK((r.pmc_after & kPmcEngineMask) == 0);
        CHECK(r.tp_cleared && r.tp_us >= 300 && r.tp_us < 1000);
        CHECK(r.vendor_ok && r.gfw_ok && r.gfw_us >= 300000 && r.gfw_us < 310000);
        CHECK(f.flrs == 1 && f.sbrs == 0);
        CHECK(f.ev.size() >= 3 && f.ev[0] == "pmc" && f.ev[1] == "bme_off" && f.ev[2] == "flr");
        // header restored: BARs back, memory decode on, bus master off
        CHECK(f.r32raw(0x10) == 0xde000000u && f.r32raw(0x14) == 0xc000000cu && f.r32raw(0x1c) == 0xd000000cu);
        CHECK((f.r32raw(4) & 6) == 2);
    }
    {   // display-only boot: no PMC write
        Fake f; Plan p;
        Result r = run(f, p);
        CHECK(r.ok()); CHECK(r.pmc_before == 0); CHECK(f.ev[0] == "bme_off"); CHECK(f.pmc == 0x40000020u);
    }
    {   // GFW never completes after the FLR: SBR fallback, then GFW completes
        Fake f; f.flr_breaks_gfw = true; Plan p; p.engines = true;
        Result r = run(f, p);
        CHECK(r.ok()); CHECK(std::string(r.path) == "flr+sbr"); CHECK(f.flrs == 1 && f.sbrs == 1);
        CHECK(r.sbr_gfw_ok && r.gfw_ok); CHECK(f.bctl == 0);
        CHECK((f.r32raw(4) & 6) == 2 && f.r32raw(0x10) == 0xde000000u);
    }
    {   // link lost after FLR: SBR brings it back
        Fake f; f.flr_loses_link = true; Plan p;
        Result r = run(f, p);
        CHECK(r.ok()); CHECK(std::string(r.path) == "flr+sbr"); CHECK(f.sbrs == 1);
    }
    {   // no FLR capability: SBR only
        Fake f; f.flr_cap = false; Plan p; p.engines = true;
        Result r = run(f, p);
        CHECK(r.ok()); CHECK(std::string(r.path) == "sbr"); CHECK(f.flrs == 0 && f.sbrs == 1);
    }
    {   // no FLR, no bridge: refuse cleanly after quiescing
        Fake f; f.flr_cap = false; f.bridge = false; Plan p;
        Result r = run(f, p);
        CHECK(!r.ok()); CHECK(f.flrs == 0 && f.sbrs == 0); CHECK((f.r32raw(4) & 4) == 0);
    }
    {   // GFW broken after FLR and SBR not allowed: reported, no SBR
        Fake f; f.flr_breaks_gfw = true; Plan p; p.allow_sbr = false;
        Result r = run(f, p);
        CHECK(!r.ok()); CHECK(f.sbrs == 0); CHECK(r.total_us >= p.gfw_timeout_us);
    }
    {   // TransactionsPending stuck: bounded wait, reset still happens
        Fake f; f.tp_clears_after_us = 10000000; Plan p;
        Result r = run(f, p);
        CHECK(r.ok()); CHECK(!r.tp_cleared && r.tp_us >= p.tp_timeout_us); CHECK(f.flrs == 1);
    }
    {   // the worst case stays inside the budget the hook gives the reset watchdog
        Fake f; f.flr_breaks_gfw = true; f.gfw_after_reset_us = 2000000; Plan p; p.engines = true;
        Result r = run(f, p);
        CHECK(r.ok()); CHECK(r.total_us < 8000000);
    }
    std::printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
