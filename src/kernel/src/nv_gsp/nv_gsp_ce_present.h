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
    uint64_t frames = 0, rects = 0, bytes = 0, us_sum = 0, submits = 0, wait_us_sum = 0, waits_blocked = 0;
    uint32_t us_max = 0, remaps = 0, remap_us_max = 0, map_pages = 0, refused = 0, timeouts = 0;
};
struct CePresent {
    M2Keep k;
    M2Inputs in;                    // the kept channel's sysmem pages (gpfifo, push, sem)
    bool active = false, broken = false;
    uintptr_t bb_lo = 0, bb_hi = 0; // mapped back-buffer range (page aligned)
    uint64_t bb_va = 0, bb_pa_first = 0, bb_pa_last = 0;
    uint32_t slot = 0;
    bool big = false;              // the current source mapping uses 2 MiB PTEs
    uint32_t inflight = 0;         // payload of the kicked, not yet seen, submission (0 = none)
    uint64_t kick_us = 0;
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
    // Fast path: [lo, hi rounded up to 2 MiB) physically contiguous from a 2 MiB-aligned
    // base (nv-disp allocates its shadow that way) -> 2 MiB PTEs, one per 512 pages.
    const uintptr_t hi2 = (hi + 0x1fffff) & ~uintptr_t(0x1fffff);
    const uint64_t pa0 = v2p(lo);
    bool contig = pa0 && !(lo & 0x1fffff) && !(pa0 & 0x1fffff) && hi2 - lo <= kCeVaBbSlot;
    for (uintptr_t a = lo; contig && a < hi2; a += 0x1000) contig = v2p(a) == pa0 + (a - lo);
    if (contig) {
        for (uintptr_t a = lo; a < hi2 && ok; a += 0x200000, ++pages) {
            ok = b.MapPage(va + (a - lo), pa0 + (a - lo), 21, Aperture::SysCoh, 0, true);
            if (!ok) cp.last_error = b.err ? b.err : "back buffer map (2M)";
        }
        cp.big = true;
    } else cp.big = false;
    for (uintptr_t a = lo; !contig && a < hi && ok; a += 0x1000, ++pages) {
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

// Kick: build one submission (<= 100 rectangles) and ring the doorbell without
// waiting. 1 kicked (*ticket), 0 not available (CPU copies), -1 failed (CE broken).
template <class Bus, class VramIo, class V2P, class Sync>
inline int CePresentKick(Bus& bus, const ChipOps& c, VramIo& vram, CePresent& cp, const NvCeJob& job, V2P v2p, Sync& sync,
                         uint64_t* bytes_out, uint32_t* ticket) {
    *bytes_out = 0; *ticket = 0;
    if (!cp.active || cp.broken || !cp.k.kept || cp.inflight) return 0;
    if (!job.src || !job.width || !job.height || job.src_stride_px < job.width || !job.n || job.n > 100) return 0;
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
        n += CeCopyWords(w + n, cr, false);
        bytes += uint64_t(cr.line_bytes) * cr.lines;
        ++cp.st.rects;
    }
    if (!n) { *bytes_out = 0; return 0; }
    const uint32_t want = M2KeptKick(bus, c, vram, cp.k, cp.in, w, n, sync);
    if (!want) { cp.broken = true; cp.last_error = "kick"; return -1; }
    cp.inflight = want; cp.kick_us = bus.now_us(); ++cp.st.submits;
    cp.st.bytes += bytes; *bytes_out = bytes; *ticket = want;
    return 1;
}

// Wait for the kicked submission: 1 done, 0 not yet (budget spent, < kCeBudgetUs since the
// kick), -1 timed out (CE broken). budget_us 0 = just check.
template <class Bus, class Poll>
inline int CePresentWait(Bus& bus, CePresent& cp, uint32_t ticket, uint32_t budget_us, Poll poll) {
    if (!cp.inflight || cp.inflight != ticket) return cp.broken ? -1 : 1;
    const uint64_t t0 = bus.now_us();
    bool blocked = false;
    for (;;) {
        if (M2KeptDone(cp.k, cp.in, ticket)) {
            const uint32_t us = uint32_t(bus.now_us() - cp.kick_us);
            cp.st.us_sum += us; if (us > cp.st.us_max) cp.st.us_max = us;
            cp.st.wait_us_sum += bus.now_us() - t0; if (blocked) ++cp.st.waits_blocked;
            cp.inflight = 0; ++cp.st.frames;
            return 1;
        }
        const uint64_t now = bus.now_us();
        if (now - cp.kick_us > kCeBudgetUs) {
            cp.last_error = "semaphore timeout"; ++cp.st.timeouts; cp.broken = true; cp.inflight = 0; return -1;
        }
        if (now - t0 >= budget_us) { cp.st.wait_us_sum += now - t0; return 0; }
        blocked = true;
        poll();
    }
}

// Kick + wait (synchronous form; the host tests and simple callers).
template <class Bus, class VramIo, class V2P, class Sync, class Poll>
inline int CePresentRun(Bus& bus, const ChipOps& c, VramIo& vram, CePresent& cp, const NvCeJob& job, V2P v2p, Sync& sync,
                        Poll poll, uint64_t* bytes_out) {
    uint32_t t = 0;
    const int k = CePresentKick(bus, c, vram, cp, job, v2p, sync, bytes_out, &t);
    if (k != 1) return k;
    return CePresentWait(bus, cp, t, kCeBudgetUs + 1000, poll);
}

} // namespace brook::nvgsp
