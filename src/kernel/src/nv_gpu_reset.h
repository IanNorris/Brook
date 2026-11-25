#pragma once
// Pre-reset GPU reset for NVIDIA Turing (TU10x), modelled on open-gpu-kernel-modules
// 570.144 kbifDoSecondaryBusResetOrFunctionLevelReset_TU102 / kbifDoFunctionLevelReset_TU102:
//
//   1. engines   PMC_ENABLE (0x200): clear the engine bits RM resets before an FLR
//                (kbifGetValidEnginesToReset_TU102: GR, media, CE0-8, FIFO, PWR, DISP,
//                NVDEC0-2, NVENC0-1, SEC, PERFMON, NVJPG0). Only when GSP-RM or Brook
//                ran engines this boot (`engines`); a display-only boot skips it.
//   2. quiesce   clear Bus Master Enable, then wait for DevSta.TransactionsPending = 0
//                (kbifStopSysMemRequests + kbifWaitForTransactionsComplete_TU102).
//   3. FLR       DevCtl.InitiateFLR on fn0 when DevCap.FLR is set, 100 ms, restore the
//                saved header (BARs, then memory decode; bus master stays off), check the
//                vendor ID (RM: NV_XVE_ID).
//   4. GFW boot  the reset re-runs the GPU's own boot (devinit/FWSEC). Wait for
//                NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK (0x118128) bit 0 and
//                GFW_BOOT_PROGRESS (0x118234[7:0]) == 0xff (RM gpuWaitForGfwBootComplete;
//                nouveau tu102_devinit_wait), so the platform reset never lands mid-boot.
//   5. SBR       if there is no FLR, the vendor ID is gone, or GFW boot does not
//                complete: Secondary Bus Reset on the upstream bridge (hold >= 1 ms),
//                wait, restore the header, wait for GFW boot again (RM falls back to
//                kbifDoSecondaryBusHotReset the same way).
//
// Without (4) a reset issued 100 ms after an FLR can reach the firmware's option-ROM
// POST while the GPU is still re-running its own boot; with engines left running by
// GSP-RM (P5's copy engine) Brook's bare FLR + immediate platform reset was followed by
// a POST hang on 26 Nov. Host-tested in src/tests/test_nv_gpu_reset.
#include <stdint.h>

namespace brook::nvreset {

constexpr uint32_t kPmcEnable = 0x200;
constexpr uint32_t kPmcEngineMask =
    (1u << 4) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) |
    (1u << 14) | (1u << 15) | (1u << 16) | (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22) |
    (1u << 23) | (1u << 24) | (1u << 28) | (1u << 30) | (1u << 31);
constexpr uint32_t kGfwPlm = 0x118128, kGfwProgress = 0x118234;

struct Plan {
    bool engines = false;           // GSP-RM / channels ran this boot: PMC engine reset first
    bool allow_sbr = true;
    uint32_t tp_timeout_us = 50000;
    uint32_t flr_wait_us = 100000;  // PCIe: 100 ms before config access after FLR
    uint32_t sbr_hold_us = 2000, sbr_wait_us = 200000;
    uint32_t gfw_timeout_us = 2500000;
};

struct Result {
    const char* path = "none";      // "flr", "flr+sbr", "sbr", "none"
    const char* status = "ok";
    bool found_cap = false, flr_cap = false, bridge = false;
    uint32_t pmc_before = 0, pmc_after = 0;
    bool tp_cleared = false; uint32_t tp_us = 0;
    bool vendor_ok = false;
    bool gfw_ok = false; uint32_t gfw_us = 0, gfw_plm = 0, gfw_progress = 0;
    bool sbr_gfw_ok = false;
    uint32_t total_us = 0;
    bool ok() const { return status[0] == 'o' && status[1] == 'k' && !status[2]; }
};

// Cfg: rd32/wr32/rd16/wr16(offset) on the GPU's fn0, brd16/bwr16(offset) on the upstream
// bridge (has_bridge()). Mmio: rd32/wr32(BAR0 offset); valid only while memory decode is on.
// Clock: now_us(), delay_us(us).
template <class Cfg, class Mmio, class Clock>
inline Result RunGpuReset(Cfg& cfg, Mmio& mmio, Clock& clk, const Plan& plan) {
    Result r;
    const uint64_t t0 = clk.now_us();
    // PCIe capability (id 0x10)
    uint8_t cap = uint8_t(cfg.rd32(0x34) & 0xfc);
    for (int n = 0; cap && n < 48; ++n) {
        const uint32_t h = cfg.rd32(cap);
        if ((h & 0xff) == 0x10) break;
        cap = uint8_t((h >> 8) & 0xfc);
    }
    r.found_cap = cap != 0;
    r.flr_cap = cap && (cfg.rd32(uint8_t(cap + 4)) & (1u << 28));
    r.bridge = cfg.has_bridge();
    uint32_t hdr[16];
    for (uint32_t i = 0; i < 16; ++i) hdr[i] = cfg.rd32(uint8_t(i * 4));
    const uint16_t cmd = uint16_t(hdr[1]);
    const uint16_t devctl = cap ? cfg.rd16(uint8_t(cap + 8)) : 0;
    auto restore = [&] {
        for (uint32_t i = 4; i < 10; ++i) cfg.wr32(uint8_t(i * 4), hdr[i]);   // BAR0..BAR5
        cfg.wr32(0x3c, hdr[15]);
        if (cap) cfg.wr16(uint8_t(cap + 8), uint16_t(devctl & ~0x8000));
        cfg.wr16(0x04, uint16_t(cmd & ~0x0004));                              // memory decode, no bus master
    };
    auto gfw = [&](bool* ok) {
        const uint64_t g0 = clk.now_us();
        *ok = false;
        for (;;) {
            r.gfw_plm = mmio.rd32(kGfwPlm);
            r.gfw_progress = mmio.rd32(kGfwProgress);
            if (r.gfw_plm != 0xffffffffu && (r.gfw_plm & 1) && (r.gfw_progress & 0xff) == 0xff) { *ok = true; break; }
            if (clk.now_us() - g0 >= plan.gfw_timeout_us) break;
            clk.delay_us(1000);
        }
        r.gfw_us = uint32_t(clk.now_us() - g0);
    };
    auto vendor = [&] { return (cfg.rd32(0) & 0xffff) == 0x10de; };
    if ((hdr[0] & 0xffff) != 0x10de) { r.status = "not an NVIDIA function"; return r; }

    // 1. engines (needs memory decode, which is on while the display runs)
    if (plan.engines && (cmd & 2)) {
        r.pmc_before = mmio.rd32(kPmcEnable);
        if (r.pmc_before != 0xffffffffu) {
            mmio.wr32(kPmcEnable, r.pmc_before & ~kPmcEngineMask);
            r.pmc_after = mmio.rd32(kPmcEnable);
        }
    }
    // 2. no new sysmem requests; let the outstanding ones finish
    cfg.wr16(0x04, uint16_t(cmd & ~0x0004));
    if (cap) {
        const uint64_t w0 = clk.now_us();
        for (;;) {
            if (!(cfg.rd16(uint8_t(cap + 0x0a)) & (1u << 5))) { r.tp_cleared = true; break; }
            if (clk.now_us() - w0 >= plan.tp_timeout_us) break;
            clk.delay_us(100);
        }
        r.tp_us = uint32_t(clk.now_us() - w0);
    }
    // 3. FLR
    bool need_sbr = !r.flr_cap;
    if (r.flr_cap) {
        r.path = "flr";
        cfg.wr16(uint8_t(cap + 8), uint16_t(devctl | 0x8000));
        clk.delay_us(plan.flr_wait_us);
        r.vendor_ok = vendor();
        if (r.vendor_ok) {
            restore();
            gfw(&r.gfw_ok);   // 4.
        }
        need_sbr = !r.vendor_ok || !r.gfw_ok;
    }
    // 5. SBR fallback (or the only reset without FLR)
    if (need_sbr) {
        if (!plan.allow_sbr || !r.bridge) {
            r.status = !r.flr_cap ? "no FLR and no upstream bridge" : !r.vendor_ok ? "vendor ID lost after FLR" : "GFW boot did not complete after FLR";
            r.total_us = uint32_t(clk.now_us() - t0);
            return r;
        }
        r.path = r.flr_cap ? "flr+sbr" : "sbr";
        const uint16_t bc = cfg.brd16(0x3e);
        cfg.bwr16(0x3e, uint16_t(bc | 0x40));
        clk.delay_us(plan.sbr_hold_us);
        cfg.bwr16(0x3e, uint16_t(bc & ~0x40));
        clk.delay_us(plan.sbr_wait_us);
        uint32_t tries = 0;
        while (!(r.vendor_ok = vendor()) && tries++ < 20) clk.delay_us(50000);   // link retrain / CRS
        if (!r.vendor_ok) { r.status = "vendor ID lost after SBR"; r.total_us = uint32_t(clk.now_us() - t0); return r; }
        restore();
        gfw(&r.sbr_gfw_ok);
        r.gfw_ok = r.sbr_gfw_ok;
        if (!r.gfw_ok) r.status = "GFW boot did not complete after SBR";
    }
    r.total_us = uint32_t(clk.now_us() - t0);
    return r;
}

} // namespace brook::nvreset
