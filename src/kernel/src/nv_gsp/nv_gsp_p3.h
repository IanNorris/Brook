#pragma once
// M1 proof points P3 (FWSEC-FRTS alone) and P3b (fini: FWSEC-SB then
// booter_unload), as one host-testable sequence over an abstract Bus. The
// live bridge verb (nv_gsp_live.cpp) and test_nv_gsp_p3 (fake register
// file, trace-diffed) both run exactly this code.
//
// P3 stages, each a refusal point before the first GPU write:
//   preflight  chip/devinit/WPR2 (RunPreflight) + both falcons halted,
//              GSP RISC-V not running
//   vbios      image walk, BIT 'p', FWSEC ucode descriptor
//   acr_bl     ACR bootloader (FWSEC desc v2 only)
//   layout     FB/WPR2 layout from 0x625f04 and the FB size (FRTS placement)
//   patch      FRTS command patched into a private copy in the DMA buffer
//   fwsec      the falcon run; on any failure the FalconDiag is captured
// Nothing is retried.
#include <stdint.h>
#include <stddef.h>
#include "nv_gsp_boot.h"

namespace brook::nvgsp {

struct DmaBuf {
    uint8_t* cpu = nullptr;   // kernel mapping
    uint64_t bus = 0;         // device-visible address (physical: no IOMMU translation)
    size_t bytes = 0;
};

struct P3Inputs {
    Span vbios;               // full ROM image (PROM read), with the 0xE0 image
    Span acr_bl;              // nvidia/<chip>/acr/bl.bin (desc v2)
    uint64_t elf_size = 0;    // .fwimage bytes (layout below FRTS; not used by FWSEC)
    uint64_t boot_size = 0;   // RISC-V bootloader payload bytes
};

struct P3Result {
    const char* stage = "preflight";
    BootStatus status = BootStatus::Ok;
    FwStatus fw = FwStatus::Ok;
    Preflight pre;
    FbLayout layout;
    FwsecImage fwsec;
    FwsecPatchInfo patch;
    FwsecRun run;
    bool diag_valid = false;
    FalconDiag diag;
    bool wrote = false;       // true once the first falcon write was issued
};

// Shared front half: preflight checks the falcons too (P3 and P4 both need
// an idle GSP falcon); `require_unlocked` false lets P3b run with WPR2 set.
template <class Bus>
inline bool P3Prepare(Bus& bus, const ChipOps& c, const P3Inputs& in, P3Result* r, bool require_unlocked) {
    r->stage = "preflight";
    r->pre = RunPreflight(bus, c);
    if (r->pre.status != BootStatus::Ok &&
        !(r->pre.status == BootStatus::Wpr2AlreadySet && !require_unlocked)) { r->status = r->pre.status; return false; }
    // BROOK: both falcons halted (CPUCTL bit 4) and GSP RISC-V inactive.
    if (!(r->pre.gsp_cpuctl & 0x10) || !(r->pre.sec2_cpuctl & 0x10) || (r->pre.gsp_riscv_cpuctl & 1)) {
        r->status = BootStatus::FalconError; return false;
    }
    r->stage = "vbios";
    Vbios vb;
    if ((r->fw = ParseVbios(in.vbios, &vb)) != FwStatus::Ok ||
        (r->fw = ParseFwsec(vb, &r->fwsec)) != FwStatus::Ok) { r->status = BootStatus::BadInput; return false; }
    if (r->fwsec.loc.entry.type != c.fwsec_ucode_type) { r->status = BootStatus::BadInput; return false; }
    r->stage = "layout";
    FbLayoutInput li;
    li.fb_size = FbSizeFromReg(r->pre.fb_size_reg);
    li.reg_625f04 = r->pre.vga_ws;
    li.elf_size = in.elf_size;
    li.boot_size = in.boot_size;
    if (!c.compute_layout(li, &r->layout) || r->layout.frts.size != c.frts_size) {
        r->status = BootStatus::BadInput; return false;
    }
    return true;
}

template <class Bus>
inline bool P3PatchInto(const FwsecImage& f, uint32_t cmd, const FbLayout& L, const DmaBuf& dma,
                        FwsecPatchInfo* pi, P3Result* r) {
    r->stage = "patch";
    if (!dma.cpu || dma.bytes < f.ucode.n || (dma.bus & 0xfff)) { r->status = BootStatus::BadInput; return false; }
    if ((r->fw = PatchFwsec(f, dma.cpu, dma.bytes, cmd, L.frts.addr, L.frts.size, pi)) != FwStatus::Ok) {
        r->status = BootStatus::BadInput; return false;
    }
    return true;
}

struct NoArm { bool operator()() const { return true; } };

// `arm()` runs after every check passed and before the first GPU write (the
// live path enables PCI bus mastering and latches "once per boot" there);
// returning false stops with stage "arm" and nothing written.
template <class Bus, class Arm = NoArm>
inline P3Result RunP3(Bus& bus, const ChipOps& c, const P3Inputs& in, const DmaBuf& dma, Arm arm = Arm()) {
    P3Result r;
    if (!P3Prepare(bus, c, in, &r, true)) return r;
    AcrBl bl;
    const AcrBl* blp = nullptr;
    if (r.fwsec.version == 2) {
        r.stage = "acr_bl";
        if ((r.fw = ParseAcrBl(in.acr_bl, &bl)) != FwStatus::Ok) { r.status = BootStatus::BadInput; return r; }
        blp = &bl;
    }
    if (!P3PatchInto<Bus>(r.fwsec, nvfw::kDmemMapperCmdFrts, r.layout, dma, &r.patch, &r)) return r;
    r.stage = "arm";
    if (!arm()) { r.status = BootStatus::BadInput; return r; }
    r.stage = "fwsec";
    r.wrote = true;
    r.run = RunFwsecFrts(bus, c, r.fwsec, blp, dma.cpu, dma.bus, r.layout);
    r.status = r.run.status;
    if (r.status != BootStatus::Ok) { r.diag = CaptureFalconDiag(bus, c, c.gsp_falcon); r.diag_valid = true; }
    return r;
}

// P3b, nouveau tu102_gsp_fini order without GSP-RM: GSP falcon reset,
// FWSEC-SB, then booter_unload on the booter falcon (mbox 0xff/0xff).
struct FiniResult {
    const char* stage = "preflight";
    BootStatus status = BootStatus::Ok;
    FwStatus fw = FwStatus::Ok;
    uint32_t wpr2_lo_before = 0, wpr2_hi_before = 0;
    FwsecRun sb;
    BooterRun unload;
    bool diag_valid = false;
    FalconDiag diag;
    bool wrote = false;
};
template <class Bus, class Arm = NoArm>
inline FiniResult RunFini(Bus& bus, const ChipOps& c, const P3Inputs& in, Span booter_unload, const DmaBuf& dma,
                          uint8_t* booter_img, size_t booter_cap, Arm arm = Arm()) {
    FiniResult f;
    P3Result r;
    if (!P3Prepare(bus, c, in, &r, false)) { f.stage = r.stage; f.status = r.status; f.fw = r.fw; return f; }
    f.wpr2_lo_before = r.pre.wpr2_lo; f.wpr2_hi_before = r.pre.wpr2_hi;
    AcrBl bl;
    const AcrBl* blp = nullptr;
    if (r.fwsec.version == 2) {
        f.stage = "acr_bl";
        if ((f.fw = ParseAcrBl(in.acr_bl, &bl)) != FwStatus::Ok) { f.status = BootStatus::BadInput; return f; }
        blp = &bl;
    }
    f.stage = "booter";
    BooterImage b;
    if ((f.fw = ParseBooter(booter_unload, &b)) != FwStatus::Ok || !BuildBooterImage(b, booter_img, booter_cap)) {
        f.status = BootStatus::BadInput; return f;
    }
    FwsecPatchInfo pi;
    if (!P3PatchInto<Bus>(r.fwsec, nvfw::kDmemMapperCmdSb, r.layout, dma, &pi, &r)) {
        f.stage = r.stage; f.status = r.status; f.fw = r.fw; return f;
    }
    f.stage = "arm";
    if (!arm()) { f.status = BootStatus::BadInput; return f; }
    f.stage = "fwsec_sb";
    f.wrote = true;
    f.sb = RunFwsecSb(bus, c, r.fwsec, blp, dma.cpu, dma.bus);
    if (f.sb.status != BootStatus::Ok) {
        f.status = f.sb.status; f.diag = CaptureFalconDiag(bus, c, c.gsp_falcon); f.diag_valid = true; return f;
    }
    f.stage = "booter_unload";
    f.unload = RunBooterUnload(bus, c, b, booter_img);
    f.status = f.unload.status;
    if (f.status != BootStatus::Ok) { f.diag = CaptureFalconDiag(bus, c, c.booter_falcon); f.diag_valid = true; }
    return f;
}

} // namespace brook::nvgsp
