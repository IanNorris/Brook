#pragma once
// TU104 GSP-RM 570.144 framebuffer layout (WPR2 carve-out) and GspFwWprMeta.
// Pure arithmetic, host-tested with goldens from the Linux 25 Nov capture
// on this card (FWSEC-FRTS set WPR2 to 0x1ffee0000; RM reported FB region 4
// = 0x1f7800000..0x1ffffffff, 136 MiB reserved).
//
// Clean-room from Linux nouveau (MIT), master 6e4122f0:
//   nvkm/subdev/gsp/tu102.c  wpr_meta_init 212-259, wpr_heap_size 261-273,
//                            vga_workspace_addr 275-292, oneinit 330-398
//   nvkm/subdev/gsp/rm/r570/rm.c r570_wpr_libos2 (TU10x heap parameters)
#include <stdint.h>
#include <stddef.h>
#include "nvrm_r570.h"

namespace brook::nvgsp {

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
constexpr uint64_t AlignDown(uint64_t v, uint64_t a) { return v / a * a; }

// gp102_fb_vidmem_size (nvkm/subdev/fb/gp102.c), register 0x100ce0;
// the 2080 reads 0x208 = 8 GiB: (val bits 9:4) << (bits 3:0 + 20), x15/16 when bit 30.
constexpr uint64_t FbSizeFromReg(uint32_t v) {
    return (v & 0x40000000) ? ((uint64_t((v & 0x3f0) >> 4) << ((v & 0xf) + 20)) / 16 * 15)
                            : (uint64_t((v & 0x3f0) >> 4) << ((v & 0xf) + 20));
}

struct FbLayoutInput {
    uint64_t fb_size = 0;            // nvkm_fb_vidmem_size (8 GiB on the 2080)
    uint32_t reg_625f04 = 0;         // NV_PDISP VGA workspace base register
    bool     display_present = true; // nouveau reads 0x625f04 only with a disp engine
    uint64_t elf_size = 0;           // .fwimage size
    uint64_t boot_size = 0;          // RISC-V bootloader bin_hdr data_size
};

struct Region { uint64_t addr = 0, size = 0; uint64_t end() const { return addr + size; } };
struct FbLayout {
    uint64_t fb_size = 0;
    Region vga_workspace, bios;      // bios == vga_workspace on TU10x
    Region frts, boot, elf, heap;    // WPR2 contents (heap = GSP-RM heap)
    Region wpr2;                     // meta at wpr2.addr
    Region non_wpr_heap;             // 1 MiB below WPR2
    uint64_t wpr_end = 0;            // gspFwWprEnd = align_down(bios, 128 KiB)
    uint64_t reserved_top = 0;       // fb_size - non_wpr_heap.addr
};

// tu102.c 261-273 with r570_wpr_libos2: 0 + 8 MiB + per-GB + 2048 channels,
// at least 64 MiB.
inline uint64_t WprHeapSize(uint64_t fb_size) {
    const uint64_t gb = (fb_size + (1ull << 30) - 1) >> 30;
    uint64_t h = rm570::GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS2 + rm570::GSP_FW_HEAP_PARAM_BASE_RM_SIZE_TU10X +
                 AlignUp(rm570::GSP_FW_HEAP_PARAM_SIZE_PER_GB_FB * gb, kMiB) +
                 AlignUp(rm570::GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE, kMiB);
    const uint64_t min = rm570::GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS2_MIN_MB * kMiB;
    return h > min ? h : min;
}

// tu102.c 275-292: bit 3 set means "workspace base valid" in bits 31:8 (64 KiB
// units); a base below fb-1 MiB is treated as unusable and the workspace
// shrinks to the top 128 KiB. Our card reads 0x00020e09 under Linux.
inline uint64_t VgaWorkspaceAddr(uint64_t fb_size, uint32_t reg_625f04, bool display_present) {
    const uint64_t base = fb_size - 0x100000;
    const uint64_t r = display_present ? reg_625f04 : 0;
    if (!(r & 0x8)) return base;
    const uint64_t addr = (r & 0xffffff00ull) << 8;
    if (addr < base) return fb_size - 0x20000;
    return addr;
}

inline bool ComputeFbLayout(const FbLayoutInput& in, FbLayout* L) {
    if (in.fb_size < (1ull << 30) || in.elf_size == 0 || in.boot_size == 0) return false;
    L->fb_size = in.fb_size;
    L->vga_workspace.addr = VgaWorkspaceAddr(in.fb_size, in.reg_625f04, in.display_present);
    L->vga_workspace.size = in.fb_size - L->vga_workspace.addr;
    L->bios = L->vga_workspace;
    L->frts.size = 0x100000;
    L->frts.addr = AlignDown(L->bios.addr, 0x20000) - L->frts.size;
    L->boot.size = in.boot_size;
    L->boot.addr = AlignDown(L->frts.addr - L->boot.size, 0x1000);
    L->elf.size = in.elf_size;
    L->elf.addr = AlignDown(L->boot.addr - L->elf.size, 0x10000);
    L->heap.size = WprHeapSize(in.fb_size);
    if (L->heap.size + kMiB > L->elf.addr) return false;
    L->heap.addr = AlignDown(L->elf.addr - L->heap.size, kMiB);
    L->heap.size = AlignDown(L->elf.addr - L->heap.addr, kMiB);
    L->wpr2.addr = AlignDown(L->heap.addr - sizeof(rm570::GspFwWprMeta), kMiB);
    L->wpr2.size = L->frts.addr + L->frts.size - L->wpr2.addr;
    L->non_wpr_heap.size = kMiB;
    L->non_wpr_heap.addr = L->wpr2.addr - L->non_wpr_heap.size;
    L->wpr_end = AlignDown(L->bios.addr, 0x20000);
    L->reserved_top = in.fb_size - L->non_wpr_heap.addr;
    return true;
}

// The sysmem side of the boot handoff (DMA/bus addresses chosen by Brook).
struct WprMetaSysmem {
    uint64_t radix3_lvl0 = 0;
    uint64_t bootloader = 0;         // bootloader payload copy
    uint64_t signature = 0;          // .fwsignature_tu10x copy (256-aligned buffer)
    uint64_t signature_size = 0;
    uint64_t bl_code_offset = 0, bl_data_offset = 0, bl_manifest_offset = 0;
};

// tu102.c 212-259.
inline void FillWprMeta(const FbLayout& L, const WprMetaSysmem& s, rm570::GspFwWprMeta* m) {
    uint8_t* p = reinterpret_cast<uint8_t*>(m);
    for (size_t i = 0; i < sizeof(*m); ++i) p[i] = 0;
    m->magic = rm570::GSP_FW_WPR_META_MAGIC;
    m->revision = rm570::GSP_FW_WPR_META_REVISION;
    m->sysmemAddrOfRadix3Elf = s.radix3_lvl0;
    m->sizeOfRadix3Elf = L.elf.size;
    m->sysmemAddrOfBootloader = s.bootloader;
    m->sizeOfBootloader = L.boot.size;
    m->bootloaderCodeOffset = s.bl_code_offset;
    m->bootloaderDataOffset = s.bl_data_offset;
    m->bootloaderManifestOffset = s.bl_manifest_offset;
    m->sysmemAddrOfSignature = s.signature;
    m->sizeOfSignature = s.signature_size;
    m->gspFwRsvdStart = L.non_wpr_heap.addr;
    m->nonWprHeapOffset = L.non_wpr_heap.addr;
    m->nonWprHeapSize = L.non_wpr_heap.size;
    m->gspFwWprStart = L.wpr2.addr;
    m->gspFwHeapOffset = L.heap.addr;
    m->gspFwHeapSize = L.heap.size;
    m->gspFwOffset = L.elf.addr;
    m->bootBinOffset = L.boot.addr;
    m->frtsOffset = L.frts.addr;
    m->frtsSize = L.frts.size;
    m->gspFwWprEnd = L.wpr_end;
    m->fbSize = L.fb_size;
    m->vgaWorkspaceOffset = L.vga_workspace.addr;
    m->vgaWorkspaceSize = L.vga_workspace.size;
}

// NV_PFB_PRI_MMU_WPR2_ADDR_LO/HI (0x1fa824/0x1fa828): bits 31:4 hold
// address >> 12. HI names the start of the last 128 KiB granule of the
// region (observed: FRTS 0x1ffee0000..0x1fffe0000 reads lo 0x01ffee00,
// hi 0x01fffc00).
constexpr uint32_t kRegWpr2Lo = 0x1fa824;
constexpr uint32_t kRegWpr2Hi = 0x1fa828;
constexpr uint64_t Wpr2RegToAddr(uint32_t v) { return uint64_t(v >> 4) << 12; }
constexpr uint64_t Wpr2HiToEnd(uint32_t v) { return v ? Wpr2RegToAddr(v) + 0x20000 : 0; }

} // namespace brook::nvgsp
