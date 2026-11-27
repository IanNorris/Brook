#pragma once
// M3: TURING_DMA_COPY_A (0xc5b5) on the P5 channel. Runs inside RunM2's extra
// hook (nv_gsp_m2.h), so the copy object, its mappings and the channel are freed
// with P5's objects before RM unload.
//
//   object     RM_ALLOC 0xc5b5 under the channel {version 1, engineType = the
//              channel's copy engine} (nouveau r535_ce_alloc); SET_OBJECT on
//              subchannel 4 (nouveau PUSH906F_SUBC_NVA0B5)
//   mappings   two sysmem buffers (S = source pattern, R = read-back) and two
//              VRAM buffers (V1, V2, in a client FB region) in Brook's VA space
//   copies     pitch layout, virtual addresses, non-pipelined + flush, each
//              followed by a host semaphore release after WFI (nouveau
//              nve0_bo_move_copy, cla0b5.h LAUNCH_DMA fields)
//   sequence   S->V1 (full surface), V1->R, verify R == S; V1->V2; a pitched
//              VRAM->VRAM rectangle V1(sx,sy)->V2(dx,dy); V2->R, verify the
//              rectangle and everything around it; a sysmem->VRAM rectangle
//              the size of the video-present damage; timed repeats of each
//   rates      doorbell -> semaphore per submit (MB/s = bytes / us)
#include <stdint.h>
#include "nv_gsp_m2.h"

namespace brook::nvgsp {

namespace ce {   // TURING_DMA_COPY_A methods (cla0b5.h / clc5b5.h)
constexpr uint32_t kClass = 0xc5b5, kSubc = 4;
constexpr uint32_t SET_OBJECT = 0x0000, LAUNCH_DMA = 0x0300, OFFSET_IN_UPPER = 0x0400;   // .. LINE_COUNT 0x041c
constexpr uint32_t LAUNCH_NON_PIPELINED = 2u, LAUNCH_FLUSH = 1u << 2, LAUNCH_SRC_PITCH = 1u << 7,
                   LAUNCH_DST_PITCH = 1u << 8, LAUNCH_MULTI_LINE = 1u << 9;
constexpr uint32_t kLaunchPitch2D =
    LAUNCH_NON_PIPELINED | LAUNCH_FLUSH | LAUNCH_SRC_PITCH | LAUNCH_DST_PITCH | LAUNCH_MULTI_LINE;
struct NVC0B5_ALLOCATION_PARAMETERS { uint32_t version, engineType; };
constexpr uint32_t kHandle = 0xce5b0000;
}

// One pitched copy: `lines` lines of `line_bytes`, at `src`/`dst` (GPU VAs) with pitches.
struct CeRect { uint64_t src, dst; uint32_t src_pitch, dst_pitch, line_bytes, lines; };

// Append the methods for `r` to `w` (10 words). `bind` adds SET_OBJECT first (2 words).
inline uint32_t CeCopyWords(uint32_t* w, const CeRect& r, bool bind) {
    namespace h = rm570::host;
    uint32_t n = 0;
    if (bind) { w[n++] = h::IncHeader(ce::kSubc, ce::SET_OBJECT, 1); w[n++] = ce::kClass; }
    w[n++] = h::IncHeader(ce::kSubc, ce::OFFSET_IN_UPPER, 8);
    w[n++] = uint32_t(r.src >> 32) & 0xff; w[n++] = uint32_t(r.src);
    w[n++] = uint32_t(r.dst >> 32) & 0xff; w[n++] = uint32_t(r.dst);
    w[n++] = r.src_pitch; w[n++] = r.dst_pitch; w[n++] = r.line_bytes; w[n++] = r.lines;
    w[n++] = h::IncHeader(ce::kSubc, ce::LAUNCH_DMA, 1); w[n++] = ce::kLaunchPitch2D;
    return n;
}

// The same copy of 32-bit pixels with alpha forced to 255 (zero-copy step 2: an XRGB client
// buffer, whose X byte is undefined, into an A8R8G8B8 scanout surface). Component remap
// (cl90b5.h): 4 one-byte components in and out, DST_X/Y/Z = SRC_X/Y/Z, DST_W = CONST_A; with
// remap on, LINE_LENGTH_IN counts pixels (NVK nvk_cmd_copy.c). 15 words.
namespace ce {
constexpr uint32_t SET_REMAP_CONST_A = 0x0700, LAUNCH_REMAP = 1u << 10;
constexpr uint32_t kRemapOpaque = 0u | (1u << 4) | (2u << 8) | (4u << 12) | (0u << 16) | (3u << 20) | (3u << 24);
}
inline uint32_t CeOpaqueCopyWords(uint32_t* w, const CeRect& r) {
    namespace h = rm570::host;
    uint32_t n = 0;
    w[n++] = h::IncHeader(ce::kSubc, ce::SET_REMAP_CONST_A, 3);
    w[n++] = 0xff; w[n++] = 0; w[n++] = ce::kRemapOpaque;
    w[n++] = h::IncHeader(ce::kSubc, ce::OFFSET_IN_UPPER, 8);
    w[n++] = uint32_t(r.src >> 32) & 0xff; w[n++] = uint32_t(r.src);
    w[n++] = uint32_t(r.dst >> 32) & 0xff; w[n++] = uint32_t(r.dst);
    w[n++] = r.src_pitch; w[n++] = r.dst_pitch; w[n++] = r.line_bytes / 4; w[n++] = r.lines;
    w[n++] = h::IncHeader(ce::kSubc, ce::LAUNCH_DMA, 1); w[n++] = ce::kLaunchPitch2D | ce::LAUNCH_REMAP;
    return n;
}

constexpr uint64_t kM3VaS =0x0000003000000000ull, kM3VaR = 0x0000003100000000ull,
                   kM3VaV1 = 0x0000003200000000ull, kM3VaV2 = 0x0000003300000000ull;

struct M3Buf { uint8_t* cpu = nullptr; uint64_t bus = 0; uint64_t bytes = 0; };
struct M3Plan {
    M3Buf s, r;                        // sysmem, >= surface bytes, contiguous
    uint64_t v1 = 0, v2 = 0;           // VRAM, >= surface bytes each, 2 MiB aligned
    uint32_t width = 3840, height = 2160, bpp = 4;
    uint32_t sx = 128, sy = 64, dx = 1792, dy = 1024, rw = 1600, rh = 900;   // VRAM->VRAM rectangle (pixels)
    uint32_t pw = 3840, ph = 1400;     // sysmem->VRAM "present" rectangle (~21.5 MB, the 26 Nov video damage)
    uint32_t repeats = 4;
    uint32_t budget_us = 2000000;
};
struct M3Rate { uint64_t bytes = 0; uint32_t best_us = 0, sum_us = 0, n = 0; uint32_t mbps() const { return best_us ? uint32_t(bytes / best_us) : 0; } };
struct M3Result {
    const char* status = "ok"; const char* step = "start";
    bool object = false; uint32_t rm_status = 0;
    uint32_t ptes = 0;
    M3Rate s2v, v2s, v2v, rect, present;
    bool verify_full = false, verify_rect = false;
    uint64_t mismatch_at = ~0ull; uint32_t mismatch_got = 0, mismatch_want = 0;
    uint32_t events_rc = 0, events_mmu = 0;
    bool ok() const { return status[0] == 'o' && status[1] == 'k' && !status[2]; }
};

inline uint32_t M3Pattern(uint64_t i) { return uint32_t(i * 2654435761u) ^ 0x5a5a0f0fu ^ uint32_t(i >> 7); }

// Map S, R, V1, V2 into the VA space; RunM2 calls this before SET_PAGE_DIRECTORY.
template <class B>
inline bool M3Premap(B& b, const M3Plan& pl) {
    const uint64_t surf = uint64_t(pl.width) * pl.bpp * pl.height;
    if (pl.s.bytes < surf || pl.r.bytes < surf || !pl.v1 || !pl.v2) { b.err = "M3 buffers"; return false; }
    auto map_sys = [&](uint64_t va, const M3Buf& m) {
        const uint8_t sh = (m.bus & 0x1fffff) ? 12 : 21;
        const uint64_t ps = 1ull << sh, bytes = (surf + ps - 1) & ~(ps - 1);
        if (bytes > m.bytes) { b.err = "M3 sysmem buffer too small for its page size"; return false; }
        return b.Map(va, m.bus, bytes, sh, Aperture::SysCoh, 0, true);
    };
    const uint64_t vbytes = (surf + 0x1fffff) & ~0x1fffffull;
    return map_sys(kM3VaS, pl.s) && map_sys(kM3VaR, pl.r) && b.Map(kM3VaV1, pl.v1, vbytes, 21, Aperture::Video) &&
           b.Map(kM3VaV2, pl.v2, vbytes, 21, Aperture::Video);
}

// `flush(p, n)`: write back + invalidate CPU caches for [p, p+n) (non-coherent safety).
template <class Ctx, class Flush>
inline bool RunM3(Ctx& ctx, const M3Plan& pl, M3Result& res, Flush flush) {
    const uint32_t pitch = pl.width * pl.bpp;
    const uint64_t surf = uint64_t(pitch) * pl.height;
    auto fail = [&](const char* s) { res.status = s; res.events_rc = ctx.ev.rc; res.events_mmu = ctx.ev.mmu_fault; return false; };
    res.ptes = ctx.b.ptes;
    if (pl.s.bytes < surf || pl.r.bytes < surf || !pl.s.cpu || !pl.r.cpu || !pl.v1 || !pl.v2) return fail("buffers");
    if (pl.sx + pl.rw > pl.width || pl.dx + pl.rw > pl.width || pl.sy + pl.rh > pl.height || pl.dy + pl.rh > pl.height ||
        pl.pw > pl.width || pl.ph > pl.height) return fail("rectangle");

    res.step = "object";
    ce::NVC0B5_ALLOCATION_PARAMETERS a = {1, ctx.ce_engine()};
    if (!ctx.alloc(ctx.channel(), ce::kHandle, ce::kClass, &a, sizeof(a))) { res.rm_status = ctx.r.rm_status; return fail("RM_ALLOC TURING_DMA_COPY_A"); }
    res.object = true;

    uint32_t* S = reinterpret_cast<uint32_t*>(pl.s.cpu);
    uint32_t* R = reinterpret_cast<uint32_t*>(pl.r.cpu);
    for (uint64_t i = 0; i < surf / 4; ++i) S[i] = M3Pattern(i);
    flush(pl.s.cpu, surf);
    auto clear_r = [&] { for (uint64_t i = 0; i < surf / 4; ++i) R[i] = 0xdeadbeefu; flush(pl.r.cpu, surf); };

    bool bound = false;
    uint32_t w[64];
    auto copy = [&](const CeRect& rc, M3Rate* rate, uint32_t reps) {
        for (uint32_t k = 0; k < reps; ++k) {
            const uint32_t n = CeCopyWords(w, rc, !bound);
            uint64_t us = 0;
            if (!ctx.submit(w, n, pl.budget_us, &us)) return false;
            bound = true;
            if (rate) {
                rate->bytes = uint64_t(rc.line_bytes) * rc.lines;
                if (!rate->best_us || us < rate->best_us) rate->best_us = uint32_t(us ? us : 1);
                rate->sum_us += uint32_t(us); ++rate->n;
            }
            if (ctx.ev.rc || ctx.ev.mmu_fault) return false;
        }
        return true;
    };
    const CeRect full_s_v1{kM3VaS, kM3VaV1, pitch, pitch, pitch, pl.height};
    const CeRect full_v1_r{kM3VaV1, kM3VaR, pitch, pitch, pitch, pl.height};
    const CeRect full_v1_v2{kM3VaV1, kM3VaV2, pitch, pitch, pitch, pl.height};
    const CeRect full_v2_r{kM3VaV2, kM3VaR, pitch, pitch, pitch, pl.height};
    const CeRect rect{kM3VaV1 + uint64_t(pl.sy) * pitch + uint64_t(pl.sx) * pl.bpp,
                      kM3VaV2 + uint64_t(pl.dy) * pitch + uint64_t(pl.dx) * pl.bpp, pitch, pitch, pl.rw * pl.bpp, pl.rh};
    const CeRect present{kM3VaS, kM3VaV2 + uint64_t(pl.height - pl.ph) * pitch, pitch, pitch, pl.pw * pl.bpp, pl.ph};

    res.step = "sysmem->vram";
    if (!copy(full_s_v1, &res.s2v, pl.repeats)) return fail("S->V1 copy");
    res.step = "vram->sysmem";
    clear_r();
    if (!copy(full_v1_r, &res.v2s, pl.repeats)) return fail("V1->R copy");
    flush(pl.r.cpu, surf);
    for (uint64_t i = 0; i < surf / 4; ++i)
        if (R[i] != S[i]) { res.mismatch_at = i * 4; res.mismatch_got = R[i]; res.mismatch_want = S[i]; return fail("V1 read-back differs"); }
    res.verify_full = true;

    res.step = "vram->vram";
    if (!copy(full_v1_v2, &res.v2v, pl.repeats)) return fail("V1->V2 copy");
    res.step = "rect";
    if (!copy(rect, &res.rect, pl.repeats)) return fail("rectangle copy");
    clear_r();
    if (!copy(full_v2_r, nullptr, 1)) return fail("V2->R copy");
    flush(pl.r.cpu, surf);
    const uint32_t wpx = pl.width;
    for (uint32_t y = 0; y < pl.height; ++y)
        for (uint32_t x = 0; x < wpx; ++x) {
            const bool in = x >= pl.dx && x < pl.dx + pl.rw && y >= pl.dy && y < pl.dy + pl.rh;
            const uint64_t si = in ? uint64_t(y - pl.dy + pl.sy) * wpx + (x - pl.dx + pl.sx) : uint64_t(y) * wpx + x;
            const uint64_t ri = uint64_t(y) * wpx + x;
            if (R[ri] != S[si]) { res.mismatch_at = ri * 4; res.mismatch_got = R[ri]; res.mismatch_want = S[si]; return fail("rectangle read-back differs"); }
        }
    res.verify_rect = true;

    res.step = "present";
    if (!copy(present, &res.present, pl.repeats)) return fail("present-rectangle copy");
    res.step = "done";
    res.events_rc = ctx.ev.rc; res.events_mmu = ctx.ev.mmu_fault;
    return true;
}

// RunM2 extra: maps before the VA space goes live, copies after the P5 fence.
template <class Flush>
struct M3Extra {
    const M3Plan& pl; M3Result& res; Flush flush;
    template <class B> bool premap(B& b) { return M3Premap(b, pl); }
    template <class Ctx> bool operator()(Ctx& ctx) { return RunM3(ctx, pl, res, flush); }
};

} // namespace brook::nvgsp
