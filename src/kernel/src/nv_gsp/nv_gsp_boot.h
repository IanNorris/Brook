#pragma once
// Brook's planned TU104 GSP-RM 570.144 boot register sequence (M0: offline,
// not wired into boot). Written against an abstract Bus so the same code
// drives the recording fake register file of the trace-replay harness
// (test_nv_gsp_boot, diffed against the 25 Nov Linux mmiotrace) and, from
// M1, the gated live path.
//
// Bus: rd32(off), wr32(off, v), udelay(us), now_us(), mark(phase, step).
// Offsets are BAR0-relative.
//
// Clean-room from Linux nouveau (MIT), master 6e4122f0:
//   F1 nvkm/falcon/gm200.c   wait_mem_scrubbing 170-180, enable 183-210,
//                            disable 212-236, fw_boot 238-266, fw_load 268-330,
//                            pio imem/dmem 63-128
//   F2 nvkm/falcon/gp102.c   reset_eng 61-74 (0x3c0 toggle)
//   F3 nvkm/falcon/v1.c      start 93-101 (CPUCTL_ALIAS when bit6)
//   F4 nvkm/falcon/base.c    pio_wr 175-209 (IMEMT per 256 B), reset 243-252
//   G1 nvkm/subdev/gsp/tu102.c fwsec_load_bld 118-145, booter_load 70-74,
//                            oneinit tail 400-414 (reset + libos in mbox)
//   G2 nvkm/subdev/gsp/fwsec.c fwsec_frts 347-377 (0x1438 >> 16, WPR2 regs)
//   G3 nvkm/subdev/gsp/rm/r535/gsp.c init 1805-1815 (0x080, RISC-V active),
//                            sequencer core ops 1076-1125, postinit 0x110004
//   G4 nvkm/engine/sec2/tu102.c debug 0x408 (signature select)
// Deliberate differences from nouveau are marked BROOK below; the replay
// harness lists them as the expected diff.
#include <stdint.h>
#include <stddef.h>
#include "nv_gsp_fw.h"
#include "nv_gsp_wpr.h"
#include "nv_gsp_chip.h"

namespace brook::nvgsp {

// falcon registers (relative)
constexpr uint32_t kFIrqMClr = 0x014, kFMbox0 = 0x040, kFMbox1 = 0x044, kFItfen = 0x048;
constexpr uint32_t kFOs = 0x080, kFBoot0 = 0x084, kFCpuctl = 0x100, kFBootvec = 0x104;
constexpr uint32_t kFHwcfg = 0x108, kFDmactl = 0x10c, kFHwcfg1 = 0x12c, kFCpuctlAlias = 0x130;
constexpr uint32_t kFImemc = 0x180, kFImemd = 0x184, kFImemt = 0x188, kFDmemc = 0x1c0, kFDmemd = 0x1c4;
constexpr uint32_t kFEngReset = 0x3c0, kFSec2Debug = 0x408, kFTranscfg0 = 0x600, kFFbifCtl = 0x624;
constexpr uint32_t kFQueueHead0 = 0xc00;
constexpr uint32_t kRiscvCpuctl = 0x240;

constexpr uint32_t kFTracePcIdx = 0x148, kFTracePcVal = 0x14c, kFSctl = 0x240;
// Chip-specific bases and registers come from ChipOps (nv_gsp_chip.h).

enum class BootStatus : uint8_t {
    Ok, WrongChip, DevinitIncomplete, Wpr2AlreadySet, Timeout, FalconError, FwsecError,
    Wpr2Mismatch, BooterError, RiscvInactive, BadInput,
};
inline const char* BootStatusName(BootStatus s) {
    switch (s) {
    case BootStatus::Ok: return "ok";
    case BootStatus::WrongChip: return "wrong-chip";
    case BootStatus::DevinitIncomplete: return "devinit-incomplete";
    case BootStatus::Wpr2AlreadySet: return "wpr2-already-set";
    case BootStatus::Timeout: return "timeout";
    case BootStatus::FalconError: return "falcon-error";
    case BootStatus::FwsecError: return "fwsec-error";
    case BootStatus::Wpr2Mismatch: return "wpr2-mismatch";
    case BootStatus::BooterError: return "booter-error";
    case BootStatus::RiscvInactive: return "riscv-inactive";
    case BootStatus::BadInput: return "bad-input";
    }
    return "?";
}

template <class Bus>
class Falcon {
public:
    Falcon(Bus& bus, uint32_t base) : bus_(bus), base_(base) {}
    uint32_t base() const { return base_; }
    uint32_t rd(uint32_t r) { return bus_.rd32(base_ + r); }
    void wr(uint32_t r, uint32_t v) { bus_.wr32(base_ + r, v); }
    void mask(uint32_t r, uint32_t m, uint32_t v) { const uint32_t o = rd(r); wr(r, (o & ~m) | v); }

    bool Poll(uint32_t off, uint32_t m, uint32_t want, uint64_t timeout_us) {
        const uint64_t t0 = bus_.now_us();
        for (;;) {
            if ((bus_.rd32(off) & m) == want) return true;
            if (bus_.now_us() - t0 >= timeout_us) return false;
            bus_.udelay(1);
        }
    }
    // F1 170-180: nouveau's no-op mask of MAILBOX0 then DMACTL bits 1-2 clear.
    bool WaitScrub() { mask(kFMbox0, 0, 0); return Poll(base_ + kFDmactl, 0x6, 0, 10000); }
    bool ResetEng() {                     // F2
        mask(kFEngReset, 1, 1); bus_.udelay(10); mask(kFEngReset, 1, 0);
        return WaitScrub();
    }
    bool Disable() { mask(kFItfen, 3, 0); wr(kFIrqMClr, 0xffffffff); return ResetEng(); }
    bool Enable() {
        if (!ResetEng() || !WaitScrub()) return false;
        wr(kFBoot0, bus_.rd32(0x000000));            // PMC_BOOT_0 (all generations)
        return true;
    }
    bool Reset() { return Disable() && Enable(); }   // F4 243-252
    // F1 63-128 / F4 175-209
    void PioImem(uint32_t addr, const uint8_t* src, uint32_t len, uint32_t tag, bool secure) {
        wr(kFImemc, (secure ? (1u << 28) : 0) | (1u << 24) | addr);
        for (uint32_t off = 0; off < len; off += 0x100, ++tag) {
            wr(kFImemt, tag);
            const uint32_t n = len - off < 0x100 ? len - off : 0x100;
            for (uint32_t i = 0; i + 4 <= n; i += 4) wr(kFImemd, Le32(src + off + i));
        }
    }
    void PioDmem(uint32_t addr, const uint8_t* src, uint32_t len) {
        wr(kFDmemc, (1u << 24) | addr);
        for (uint32_t i = 0; i + 4 <= len; i += 4) wr(kFDmemd, Le32(src + i));
    }
    // F1 238-266; mbox1 written only when given.
    bool Boot(uint32_t mbox0, const uint32_t* mbox1, uint32_t boot_addr, uint32_t* out0, uint32_t* out1) {
        wr(kFMbox0, mbox0);
        if (mbox1) wr(kFMbox1, *mbox1);
        wr(kFBootvec, boot_addr);
        wr(kFCpuctl, 2);
        const bool ok = Poll(base_ + kFCpuctl, 0x10, 0x10, 2000000);
        *out0 = rd(kFMbox0); *out1 = rd(kFMbox1);
        return ok;
    }
    void Start() {                        // F3
        if (rd(kFCpuctl) & 0x40) wr(kFCpuctlAlias, 2); else wr(kFCpuctl, 2);
    }
    uint32_t ImemLimit() { return (rd(kFHwcfg) & 0x1ff) << 8; }
    static uint32_t Le32(const uint8_t* p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
private:
    Bus& bus_;
    uint32_t base_;
};

// ----------------------------------------------------------- preflight
// BROOK: nouveau reads none of WPR2/devinit before FWSEC; Brook refuses to
// run FRTS when WPR2 is already set (unclean restart: needs booter_unload
// or a power cycle) or devinit has not completed. Also the P1 read set.
struct Preflight {
    uint32_t boot0 = 0, devinit = 0, devinit_scratch = 0, vga_ws = 0, fb_size_reg = 0;
    uint32_t wpr2_lo = 0, wpr2_hi = 0;
    uint32_t gsp_hwcfg = 0, gsp_cpuctl = 0, sec2_hwcfg = 0, sec2_cpuctl = 0, gsp_riscv_cpuctl = 0;
    BootStatus status = BootStatus::Ok;
};
template <class Bus>
inline Preflight RunPreflight(Bus& bus, const ChipOps& c) {
    Preflight p;
    bus.mark("pre", "brook_preflight");
    p.boot0 = bus.rd32(c.reg_boot0);
    p.devinit = bus.rd32(c.reg_devinit);
    p.devinit_scratch = bus.rd32(c.reg_devinit_scratch);
    p.vga_ws = bus.rd32(c.reg_vga_ws);
    p.fb_size_reg = bus.rd32(c.reg_fb_size);
    p.wpr2_lo = bus.rd32(c.reg_wpr2_lo);
    p.wpr2_hi = bus.rd32(c.reg_wpr2_hi);
    p.gsp_cpuctl = bus.rd32(c.gsp_falcon + kFCpuctl);
    p.gsp_riscv_cpuctl = bus.rd32(c.gsp_riscv + kRiscvCpuctl);
    p.sec2_cpuctl = bus.rd32(c.booter_falcon + kFCpuctl);
    if (p.boot0 != c.boot0) p.status = BootStatus::WrongChip;
    else if (!(p.devinit & 1) || (p.devinit_scratch & 0xff) != 0xff) p.status = BootStatus::DevinitIncomplete;
    else if (p.wpr2_hi != 0) p.status = BootStatus::Wpr2AlreadySet;
    return p;
}

// --------------------------------------------------------- failure capture
// What P3/P4 report when a falcon step fails (no retry in the same boot):
// CPUCTL, mailboxes, the FWSEC error scratch, WPR2 and the TRACEPC ring
// (gm200_flcn_tracepc: TIDX 0x148 bits 23:16 = depth; write the index to
// 0x148, read the PC at 0x14c). The index writes are the only writes.
struct FalconDiag {
    uint32_t cpuctl = 0, mbox0 = 0, mbox1 = 0, sctl = 0, tidx = 0, dmactl = 0, hwcfg = 0;
    uint32_t frts_err = 0, sb_err = 0, wpr2_lo = 0, wpr2_hi = 0, riscv_cpuctl = 0;
    uint32_t trace_n = 0;
    uint32_t trace[32] = {};
};
template <class Bus>
inline FalconDiag CaptureFalconDiag(Bus& bus, const ChipOps& c, uint32_t falcon) {
    FalconDiag d;
    Falcon<Bus> f(bus, falcon);
    d.cpuctl = f.rd(kFCpuctl); d.mbox0 = f.rd(kFMbox0); d.mbox1 = f.rd(kFMbox1);
    d.sctl = f.rd(kFSctl); d.dmactl = f.rd(kFDmactl); d.hwcfg = f.rd(kFHwcfg);
    d.frts_err = bus.rd32(c.reg_frts_err); d.sb_err = bus.rd32(c.reg_sb_err);
    d.wpr2_lo = bus.rd32(c.reg_wpr2_lo); d.wpr2_hi = bus.rd32(c.reg_wpr2_hi);
    d.riscv_cpuctl = bus.rd32(c.gsp_riscv + kRiscvCpuctl);
    d.tidx = f.rd(kFTracePcIdx);
    uint32_t n = (d.tidx >> 16) & 0xff;
    if (n > 32) n = 32;
    for (uint32_t i = 0; i < n; ++i) { f.wr(kFTracePcIdx, i); d.trace[i] = f.rd(kFTracePcVal); }
    d.trace_n = n;
    return d;
}

// ------------------------------------------------------------ FWSEC (any command)
struct FwsecRun {
    uint32_t mbox0 = 0, mbox1 = 0, err = 0, wpr2_lo = 0, wpr2_hi = 0;
    BootStatus status = BootStatus::Ok;
    const char* step = "";             // last step entered (for the failure report)
};
// Build the flcn_bl_dmem_desc_v2 the ACR bootloader reads (G1 118-145).
inline nvfw::FlcnBlDmemDescV2 FwsecBlDesc(const FwsecImage& f, uint64_t dma) {
    nvfw::FlcnBlDmemDescV2 d = {};
    d.ctx_dma = nvfw::kFalconDmaIdxPhysSysNcoh;
    d.code_dma_base = dma;
    d.non_sec_code_off = f.nmem_base; d.non_sec_code_size = f.nmem_size;
    d.sec_code_off = f.imem_base; d.sec_code_size = f.imem_size;
    d.code_entry_point = 0;
    d.data_dma_base = dma + f.dmem_base_img;
    d.data_size = f.dmem_size;
    return d;
}

// Load and boot a patched FWSEC image on the GSP falcon in HS mode. `ucode`
// is the patched image (PatchFwsec) already copied to the sysmem DMA buffer
// at bus address `dma` (v2: the ACR bootloader pulls it in by DMA). v3
// loads IMEM/DMEM directly from `ucode`. `phase` names the trace phase.
template <class Bus>
inline FwsecRun BootFwsec(Bus& bus, const ChipOps& c, const char* phase, const FwsecImage& f, const AcrBl* bl,
                          const uint8_t* ucode, uint64_t dma) {
    FwsecRun r;
    Falcon<Bus> g(bus, c.gsp_falcon);
    bus.mark(phase, r.step = "acquire");
    (void)g.rd(kFHwcfg1);
    const uint32_t limit = g.ImemLimit();
    bus.mark(phase, r.step = "reset");
    if (!g.Reset()) { r.status = BootStatus::Timeout; return r; }
    bus.mark(phase, r.step = "load");
    g.mask(kFFbifCtl, 0x80, 0x80);
    g.wr(kFDmactl, 0);
    uint32_t boot_addr = f.boot_addr;
    if (f.version == 2) {
        if (!bl || bl->code.n > limit) { r.status = BootStatus::BadInput; return r; }
        g.PioImem(limit - uint32_t(bl->code.n), bl->code.p, uint32_t(bl->code.n), bl->boot_addr >> 8, false);
        const nvfw::FlcnBlDmemDescV2 d = FwsecBlDesc(f, dma);
        g.mask(kFTranscfg0 + d.ctx_dma * 4, 0x7, 0x5);
        g.PioDmem(0, reinterpret_cast<const uint8_t*>(&d), sizeof(d));
        boot_addr = bl->boot_addr;
    } else {
        g.PioImem(f.imem_base, ucode + f.imem_base_img, f.imem_size, f.imem_base >> 8, true);
        g.PioDmem(f.dmem_base, ucode + f.dmem_base_img, f.dmem_size);
    }
    bus.mark(phase, r.step = "boot");
    if (!g.Boot(0, nullptr, boot_addr, &r.mbox0, &r.mbox1)) { r.status = BootStatus::Timeout; return r; }
    if (r.mbox0 != 0) { r.status = BootStatus::FalconError; return r; }
    return r;
}

template <class Bus>
inline FwsecRun RunFwsecFrts(Bus& bus, const ChipOps& c, const FwsecImage& f, const AcrBl* bl, const uint8_t* ucode,
                             uint64_t dma, const FbLayout& L) {
    FwsecRun r = BootFwsec(bus, c, "fwsec_frts", f, bl, ucode, dma);
    if (r.status != BootStatus::Ok) return r;
    bus.mark("fwsec_frts", r.step = "verify");
    r.err = bus.rd32(c.reg_frts_err) >> 16;
    if (r.err) { r.status = BootStatus::FwsecError; return r; }
    r.wpr2_lo = bus.rd32(c.reg_wpr2_lo);
    r.wpr2_hi = bus.rd32(c.reg_wpr2_hi);
    // BROOK: check the region FWSEC locked against our FRTS placement.
    if (Wpr2RegToAddr(r.wpr2_lo) != L.frts.addr || Wpr2HiToEnd(r.wpr2_hi) != L.frts.end())
        r.status = BootStatus::Wpr2Mismatch;
    return r;
}

// FWSEC-SB (fini, tu102_gsp_fini): the falcon was reset first by the caller
// path (BootFwsec resets it anyway); error in the SB scratch bits 15:0.
template <class Bus>
inline FwsecRun RunFwsecSb(Bus& bus, const ChipOps& c, const FwsecImage& f, const AcrBl* bl, const uint8_t* ucode,
                           uint64_t dma) {
    FwsecRun r = BootFwsec(bus, c, "fwsec_sb", f, bl, ucode, dma);
    if (r.status != BootStatus::Ok) return r;
    bus.mark("fwsec_sb", r.step = "verify");
    r.err = bus.rd32(c.reg_sb_err) & 0xffff;
    r.wpr2_lo = bus.rd32(c.reg_wpr2_lo);
    r.wpr2_hi = bus.rd32(c.reg_wpr2_hi);
    if (r.err) r.status = BootStatus::FwsecError;
    return r;
}

// tu102_gsp_oneinit tail: GSP engine reset into RISC-V mode, libos args
// address in MAILBOX0/1.
template <class Bus>
inline BootStatus ResetGspRiscv(Bus& bus, const ChipOps& c, uint64_t libos) {
    Falcon<Bus> g(bus, c.gsp_falcon);
    bus.mark("gsp_riscv_reset", "reset");
    if (!g.ResetEng()) return BootStatus::Timeout;
    bus.mark("gsp_riscv_reset", "libos_args");
    g.wr(kFMbox0, uint32_t(libos));
    g.wr(kFMbox1, uint32_t(libos >> 32));
    return BootStatus::Ok;
}

// ------------------------------------------- booter_load / booter_unload
struct BooterRun {
    uint32_t debug = 0, mbox0 = 0, mbox1 = 0, wpr2_lo = 0, wpr2_hi = 0;
    BootStatus status = BootStatus::Ok;
    const char* step = "";
};
// `img` = booter image with the signature patched in (BuildBooterImage).
// Runs on the chip's booter falcon (SEC2 on TU10x) with mbox0/1 as given.
template <class Bus>
inline BooterRun BootBooter(Bus& bus, const ChipOps& c, const char* phase, const BooterImage& b, const uint8_t* img,
                            uint32_t mbox0, uint32_t mbox1) {
    BooterRun r;
    Falcon<Bus> s(bus, c.booter_falcon);
    bus.mark(phase, r.step = "acquire");
    (void)s.rd(kFHwcfg1);
    (void)s.rd(kFHwcfg);
    (void)s.rd(kFSec2Debug);
    // nouveau gm200_flcn_fw_signature: enable, then read the debug fuse.
    bus.mark(phase, r.step = "sig_select");
    if (!s.Enable()) { r.status = BootStatus::Timeout; return r; }
    r.debug = s.rd(kFSec2Debug);
    if (r.debug & c.booter_debug_fuse_bit) { r.status = BootStatus::BadInput; return r; }  // no dbg sig shipped
    bus.mark(phase, r.step = "reset");
    if (!s.Reset()) { r.status = BootStatus::Timeout; return r; }
    bus.mark(phase, r.step = "load");
    s.mask(kFFbifCtl, 0x80, 0x80);
    s.wr(kFDmactl, 0);
    s.PioImem(b.nmem_base, img + b.nmem_base_img, b.nmem_size, b.nmem_base >> 8, false);
    s.PioImem(b.imem_base, img + b.imem_base_img, b.imem_size, b.imem_base >> 8, true);
    s.PioDmem(b.dmem_base, img + b.dmem_base_img, b.dmem_size);
    bus.mark(phase, r.step = "boot");
    if (!s.Boot(mbox0, &mbox1, b.boot_addr, &r.mbox0, &r.mbox1)) { r.status = BootStatus::Timeout; return r; }
    if (r.mbox0 != 0) { r.status = BootStatus::BooterError; return r; }
    return r;
}
template <class Bus>
inline BooterRun RunBooterLoad(Bus& bus, const ChipOps& c, const BooterImage& b, const uint8_t* img, uint64_t wpr_meta) {
    BooterRun r = BootBooter(bus, c, "booter_load", b, img, uint32_t(wpr_meta), uint32_t(wpr_meta >> 32));
    if (r.status != BootStatus::Ok) return r;
    // BROOK: read back WPR2 after booter extended it (diagnostic only).
    bus.mark("booter_load", r.step = "brook_wpr2_check");
    r.wpr2_lo = bus.rd32(c.reg_wpr2_lo);
    r.wpr2_hi = bus.rd32(c.reg_wpr2_hi);
    return r;
}
// tu102_gsp_booter_unload: skipped when WPR2_HI is already 0; mbox 0xff/0xff
// outside suspend. Success = WPR2_HI reads 0 afterwards.
template <class Bus>
inline BooterRun RunBooterUnload(Bus& bus, const ChipOps& c, const BooterImage& b, const uint8_t* img) {
    BooterRun r;
    bus.mark("booter_unload", r.step = "wpr2_check");
    r.wpr2_hi = bus.rd32(c.reg_wpr2_hi);
    if (!r.wpr2_hi) { r.wpr2_lo = bus.rd32(c.reg_wpr2_lo); return r; }
    r = BootBooter(bus, c, "booter_unload", b, img, 0xff, 0xff);
    if (r.status != BootStatus::Ok) return r;
    bus.mark("booter_unload", r.step = "verify");
    r.wpr2_lo = bus.rd32(c.reg_wpr2_lo);
    r.wpr2_hi = bus.rd32(c.reg_wpr2_hi);
    if (r.wpr2_hi) r.status = BootStatus::Wpr2Mismatch;
    return r;
}

// r535_gsp_init head: app version to GSP OS register, RISC-V must be active.
template <class Bus>
inline BootStatus GspHandshake(Bus& bus, const ChipOps& c, uint32_t app_version) {
    bus.mark("gsp_handshake", "app_version");
    bus.wr32(c.gsp_falcon + kFOs, app_version);
    bus.mark("gsp_handshake", "riscv_active");
    return (bus.rd32(c.gsp_riscv + kRiscvCpuctl) & 1) ? BootStatus::Ok : BootStatus::RiscvInactive;
}

// Core operations the CPU sequencer asks for (G3 1076-1125). Plug into
// RunSequencer via a Bus adaptor that forwards rd32/wr32/... and these.
template <class Bus>
struct GspCoreOps {
    Bus& bus;
    const ChipOps& chip;
    uint64_t libos;
    uint32_t app_version;
    bool core_reset() {
        Falcon<Bus> g(bus, chip.gsp_falcon);
        if (!g.Reset()) return false;
        g.mask(kFFbifCtl, 0x80, 0x80);
        g.wr(kFDmactl, 0);
        return true;
    }
    bool core_start() { Falcon<Bus> g(bus, chip.gsp_falcon); g.Start(); return true; }
    bool core_wait_halt() {
        Falcon<Bus> g(bus, chip.gsp_falcon);
        return g.Poll(chip.gsp_falcon + kFCpuctl, 0x10, 0x10, 2000000);
    }
    bool core_resume() {
        Falcon<Bus> g(bus, chip.gsp_falcon), s(bus, chip.booter_falcon);
        if (!g.ResetEng()) return false;
        g.wr(kFMbox0, uint32_t(libos));
        g.wr(kFMbox1, uint32_t(libos >> 32));
        s.Start();
        if (!g.Poll(chip.reg_booter_boot_status, 0x04000000, 0x04000000, 2000000)) return false;
        if (s.rd(kFMbox0) != 0) return false;
        g.wr(kFOs, app_version);
        return (bus.rd32(chip.gsp_riscv + kRiscvCpuctl) & 1) != 0;
    }
};

} // namespace brook::nvgsp
