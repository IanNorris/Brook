#pragma once
// GET_GSP_STATIC_INFO (r570 GspStaticConfigInfo, 0x678 bytes) parser: the
// fields Brook needs after INIT_DONE.
//   - FB regions (NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO): which VRAM the
//     client may use. As nouveau r535_gsp_get_static_info_fb: a region that
//     is neither reserved nor protected belongs to the client; GSP-RM's own
//     heap is in the reserved/protected ones and the WPR2 carve-out.
//   - bar1PdeBase / bar2PdeBase: the VRAM page directories RM built for the
//     BAR1/BAR2 virtual apertures (nouveau r535_bar_bar1_init wraps the BAR1
//     one as its root and writes its own lower levels and PTEs).
//   - the GPU UUID (gidInfo, binary format: flags 2, 16 bytes), name, FB size.
//   - hInternalClient/Device/Subdevice: RM's internal objects, used for the
//     NV2080_CTRL_CMD_INTERNAL_* and FIFO/CE queries (nouveau gsp->internal).
#include <stdint.h>
#include <stddef.h>
#include "nvrm_r570_obj.h"

namespace brook::nvgsp {

struct FbRegion {
    uint64_t base = 0, limit = 0, reserved = 0;
    uint32_t performance = 0;
    bool compressed = false, iso = false, prot = false;
    bool usable() const { return !reserved && !prot; }
};

struct StaticInfo {
    bool ok = false;
    uint32_t gid_flags = 0, gid_len = 0;
    uint8_t uuid[16] = {};
    bool uuid_ok = false;
    uint32_t fb_regions = 0;
    FbRegion fb[rm570::static_info::kFbRegionCount];
    uint64_t fb_length = 0;
    uint32_t fb_bus_width = 0, fb_ram_type = 0, l2_cache = 0;
    char name[65] = {}, short_name[65] = {};
    uint64_t bar1_pde = 0, bar2_pde = 0;
    bool vbios_valid = false, split_vas = false, uefi = false, efi_init = false;
    uint32_t h_internal_client = 0, h_internal_device = 0, h_internal_subdevice = 0;
    uint64_t fw_non_wpr_heap_offset = 0, fw_frts_offset = 0;
    // derived: the client-usable VRAM span ([lo, hi) of the lowest/highest usable region)
    uint64_t usable_lo = 0, usable_hi = 0;
};

inline uint64_t SiRd64(const uint8_t* p) { uint64_t v; __builtin_memcpy(&v, p, 8); return v; }
inline uint32_t SiRd32(const uint8_t* p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }

// `p` = the payload after the rpc header, `n` its bytes.
inline bool ParseStaticInfo(const uint8_t* p, uint32_t n, StaticInfo* s) {
    namespace si = rm570::static_info;
    *s = StaticInfo{};
    if (!p || n < si::kBytes) return false;
    s->gid_flags = SiRd32(p + si::kGidInfo + 4);
    s->gid_len = SiRd32(p + si::kGidInfo + 8);
    if (s->gid_len == 16) { __builtin_memcpy(s->uuid, p + si::kGidInfo + 12, 16); s->uuid_ok = true; }
    const uint8_t* fr = p + si::kFbRegionInfo;
    s->fb_regions = SiRd32(fr);
    if (s->fb_regions > si::kFbRegionCount) return false;
    for (uint32_t i = 0; i < s->fb_regions; ++i) {
        const uint8_t* r = fr + si::kFbRegionFirst + i * si::kFbRegionStride;
        FbRegion& f = s->fb[i];
        f.base = SiRd64(r); f.limit = SiRd64(r + 8); f.reserved = SiRd64(r + 16);
        f.performance = SiRd32(r + 24); f.compressed = r[28]; f.iso = r[29]; f.prot = r[30];
        if (f.limit < f.base) return false;
        if (f.usable()) {
            if (!s->usable_hi || f.base < s->usable_lo) s->usable_lo = f.base;
            if (f.limit + 1 > s->usable_hi) s->usable_hi = f.limit + 1;
        }
    }
    s->fb_length = SiRd64(p + si::kFbLength);
    s->fb_bus_width = SiRd32(p + si::kFbBusWidth);
    s->fb_ram_type = SiRd32(p + si::kFbRamType);
    s->l2_cache = SiRd32(p + si::kL2CacheSize);
    for (uint32_t i = 0; i < 64; ++i) {
        const uint8_t c = p[si::kGpuName + i], d = p[si::kGpuShortName + i];
        s->name[i] = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? char(c) : (c ? '?' : 0);
        s->short_name[i] = (d >= 0x20 && d < 0x7f && d != '"' && d != '\\') ? char(d) : (d ? '?' : 0);
    }
    for (uint32_t i = 0; i < 64; ++i) { if (!p[si::kGpuName + i]) { s->name[i] = 0; break; } }
    for (uint32_t i = 0; i < 64; ++i) { if (!p[si::kGpuShortName + i]) { s->short_name[i] = 0; break; } }
    s->bar1_pde = SiRd64(p + si::kBar1PdeBase);
    s->bar2_pde = SiRd64(p + si::kBar2PdeBase);
    s->vbios_valid = p[si::kVbiosValid];
    s->split_vas = p[si::kSplitVas];
    s->h_internal_client = SiRd32(p + si::kInternalClient);
    s->h_internal_device = SiRd32(p + si::kInternalDevice);
    s->h_internal_subdevice = SiRd32(p + si::kInternalSubdevice);
    s->uefi = p[si::kIsGpuUefi];
    s->efi_init = p[si::kIsEfiInit];
    s->fw_non_wpr_heap_offset = SiRd64(p + si::kFwWprLayout);
    s->fw_frts_offset = SiRd64(p + si::kFwWprLayout + 8);
    s->ok = true;
    return true;
}

// "GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (nvidia-smi form), 41 bytes + NUL.
inline void FormatUuid(const uint8_t u[16], char out[42]) {
    static const char hx[] = "0123456789abcdef";
    const char* pre = "GPU-";
    uint32_t n = 0;
    for (; pre[n]; ++n) out[n] = pre[n];
    for (uint32_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-';
        out[n++] = hx[u[i] >> 4]; out[n++] = hx[u[i] & 15];
    }
    out[n] = 0;
}

// True when [lo, hi) of VRAM lies inside one client-usable FB region
// (i.e. GSP-RM does not allocate there for itself).
inline bool VramRangeIsClient(const StaticInfo& s, uint64_t lo, uint64_t hi) {
    for (uint32_t i = 0; i < s.fb_regions; ++i)
        if (s.fb[i].usable() && lo >= s.fb[i].base && hi <= s.fb[i].limit + 1) return true;
    return false;
}

} // namespace brook::nvgsp
