#pragma once
// Copy-engine present path (head 0): a TURING_DMA_COPY_A channel kept alive after
// RunM2, the nv-disp VRAM [0, 256 MiB) mapped once with 2 MiB pages (before the VA
// space is bound), and the compositor back buffer mapped page by page (4 KiB, its
// pages are scattered) at a fresh VA range whenever it moves or its first/last
// pages change, followed by an MMU invalidate. Each present: one pitch copy per
// damage rectangle, a host semaphore release after WFI, wait, then nv-disp flips.
#include <stdint.h>
#include "nv_gsp_m3.h"
#include "nv_gsp_bar1.h"
#include "../nv_ce_present_api.h"

namespace brook::nvgsp {

constexpr uint64_t kCeVaVram = 0x0000005000000000ull, kCeVramBytes = 256ull << 20;
constexpr uint64_t kCeVaBb = 0x0000006000000000ull, kCeVaBbSlot = 1ull << 32;
constexpr uint32_t kCeVaBbSlots = 64;
constexpr uint32_t kCeHandle = 0xce5b0001;
constexpr uint32_t kCeBudgetUs = 100000;

struct CeStats {
    uint64_t frames = 0, rects = 0, bytes = 0, us_sum = 0, submits = 0;
    uint32_t us_max = 0, remaps = 0, remap_us_max = 0, map_pages = 0, refused = 0, timeouts = 0;
};
struct CePresent {
    M2Keep k;
    M2Inputs in;                    // the kept channel's sysmem pages (gpfifo, push, sem)
    bool active = false, broken = false;
    uintptr_t bb_lo = 0, bb_hi = 0; // mapped back-buffer range (page aligned)
    uint64_t bb_va = 0, bb_pa_first = 0, bb_pa_last = 0;
    uint32_t slot = 0;
    CeStats st;
    const char* last_error = "";
};

template <class B>
inline bool CePremapVram(B& b) { return b.Map(kCeVaVram, 0, kCeVramBytes, 21, Aperture::Video); }

// RunM2 extra for `ce-present on`: VRAM window before the bind; on the live channel,
// the copy object and its SET_OBJECT.
struct CeExtra {
    template <class B> bool premap(B& b) { return CePremapVram(b); }
    template <class Ctx> bool operator()(Ctx& ctx) {
        ce::NVC0B5_ALLOCATION_PARAMETERS a = {1, ctx.ce_engine()};
        if (!ctx.alloc(ctx.channel(), kCeHandle, ce::kClass, &a, sizeof(a))) return false;
        const uint32_t w[2] = {rm570::host::IncHeader(ce::kSubc, ce::SET_OBJECT, 1), ce::kClass};
        uint64_t us = 0;
        return ctx.submit(w, 2, 1000000, &us);
    }
};

// Map [lo, hi) of the kernel's back buffer (v2p: kernel VA -> physical, 0 = unmapped)
// at the next VA slot, then flush + invalidate.
template <class Bus, class VramIo, class V2P>
inline bool CeMapBackBuffer(Bus& bus, const ChipOps& c, VramIo& vram, CePresent& cp, uintptr_t lo, uintptr_t hi, V2P v2p) {
    if (cp.slot >= kCeVaBbSlots || hi <= lo || hi - lo > kCeVaBbSlot) { cp.last_error = "back buffer: no VA slot"; return false; }
    const uint64_t t0 = bus.now_us();
    M2PtMem<VramIo> ptm{vram, cp.k.arena};
    GmmuBuilder<M2PtMem<VramIo>> b{ptm, *c.gmmu};
    b.Adopt(cp.k.root, Aperture::Video);
    b.lpt_page = cp.k.lpt_page; b.lpt_off = cp.k.lpt_off; b.lpt_aper = cp.k.lpt_aper;
    const uint64_t va = kCeVaBb + uint64_t(cp.slot) * kCeVaBbSlot;
    bool ok = true;
    uint32_t pages = 0;
    for (uintptr_t a = lo; a < hi && ok; a += 0x1000, ++pages) {
        const uint64_t pa = v2p(a);
        if (!pa || (pa & 0xfff)) { cp.last_error = "back buffer page not mapped"; ok = false; break; }
        ok = b.MapPage(va + (a - lo), pa, 12, Aperture::SysCoh, 0, true);
        if (!ok) cp.last_error = b.err ? b.err : "back buffer map";
    }
    cp.k.lpt_page = b.lpt_page; cp.k.lpt_off = b.lpt_off; cp.k.lpt_aper = b.lpt_aper;
    cp.k.pt_pages += ptm.pages; cp.k.ptes += b.ptes;
    ++cp.slot;   // never reuse a VA range: a half-written slot is simply abandoned
    if (!ok) return false;
    if (!Bar1FlushInvalidate(bus, c, cp.k.root)) { cp.last_error = "MMU invalidate did not complete"; return false; }
    cp.bb_lo = lo; cp.bb_hi = hi; cp.bb_va = va; cp.bb_pa_first = v2p(lo); cp.bb_pa_last = v2p(hi - 0x1000);
    ++cp.st.remaps; cp.st.map_pages += pages;
    const uint32_t us = uint32_t(bus.now_us() - t0);
    if (us > cp.st.remap_us_max) cp.st.remap_us_max = us;
    return true;
}

// The present: 1 copied, 0 not available, -1 failed (CE marked broken).
template <class Bus, class VramIo, class V2P, class Sync, class Poll>
inline int CePresentRun(Bus& bus, const ChipOps& c, VramIo& vram, CePresent& cp, const NvCeJob& job, V2P v2p, Sync& sync,
                        Poll poll, uint64_t* bytes_out) {
    *bytes_out = 0;
    if (!cp.active || cp.broken || !cp.k.kept) return 0;
    if (!job.src || !job.width || !job.height || job.src_stride_px < job.width || !job.n) return 0;
    if (job.dst_vram + uint64_t(job.dst_pitch) * job.height > kCeVramBytes || job.dst_pitch < job.width * 4) {
        ++cp.st.refused; return 0;
    }
    const uintptr_t s = reinterpret_cast<uintptr_t>(job.src);
    const uintptr_t end = s + (uint64_t(job.height - 1) * job.src_stride_px + job.width) * 4;
    const uintptr_t lo = s & ~uintptr_t(0xfff), hi = (end + 0xfff) & ~uintptr_t(0xfff);
    const bool inside = cp.bb_hi && lo >= cp.bb_lo && hi <= cp.bb_hi;
    if (!inside || v2p(cp.bb_lo) != cp.bb_pa_first || v2p(cp.bb_hi - 0x1000) != cp.bb_pa_last) {
        if (!CeMapBackBuffer(bus, c, vram, cp, lo, hi, v2p)) { cp.broken = true; return -1; }
    }
    static uint32_t w[1024];   // one presenter, called under the nv-aux lock
    uint32_t n = 0;
    uint64_t bytes = 0;
    auto flush = [&]() {
        if (!n) return true;
        uint64_t us = 0;
        const bool ok = M2KeptSubmit(bus, c, vram, cp.k, cp.in, w, n, kCeBudgetUs, &us, sync, poll);
        cp.st.us_sum += us; ++cp.st.submits;
        if (us > cp.st.us_max) cp.st.us_max = uint32_t(us);
        n = 0;
        if (!ok) { cp.last_error = "semaphore timeout"; ++cp.st.timeouts; cp.broken = true; }
        return ok;
    };
    for (uint32_t i = 0; i < job.n; ++i) {
        NvCeRect r = job.rects[i];
        if (r.x0 < 0) r.x0 = 0;
        if (r.y0 < 0) r.y0 = 0;
        if (r.x1 > int32_t(job.width)) r.x1 = int32_t(job.width);
        if (r.y1 > int32_t(job.height)) r.y1 = int32_t(job.height);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        const uintptr_t sp = s + (uint64_t(r.y0) * job.src_stride_px + uint32_t(r.x0)) * 4;
        CeRect cr;
        cr.src = cp.bb_va + (sp - cp.bb_lo);
        cr.dst = kCeVaVram + job.dst_vram + uint64_t(r.y0) * job.dst_pitch + uint32_t(r.x0) * 4;
        cr.src_pitch = job.src_stride_px * 4; cr.dst_pitch = job.dst_pitch;
        cr.line_bytes = uint32_t(r.x1 - r.x0) * 4; cr.lines = uint32_t(r.y1 - r.y0);
        if (n + 10 + 8 > 1024 && !flush()) return -1;
        n += CeCopyWords(w + n, cr, false);
        bytes += uint64_t(cr.line_bytes) * cr.lines;
        ++cp.st.rects;
    }
    if (!flush()) return -1;
    ++cp.st.frames; cp.st.bytes += bytes;
    *bytes_out = bytes;
    return 1;
}

} // namespace brook::nvgsp
