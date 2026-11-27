#pragma once
// Zero-copy step 2 (head 0, CE present): a "hole" is the client rectangle of a window whose
// pixels come straight from its NVK buffer (a GART BO) instead of the compositor's back
// buffer. While a hole is set the presenter
//   - leaves it out of the shadow diff (the back buffer holds no valid pixels there), so the
//     shadow is stale inside it;
//   - leaves it out of every back-buffer/shadow copy into the target buffer;
//   - writes the whole hole from the BO into the target buffer on every flip (after the
//     other copies), so every buffer shows the BO frame it was flipped with.
// When the hole moves or goes away, the old rectangle is a "foreign" region: the target
// buffers hold BO pixels there and the shadow is stale. RepairHole refreshes the shadow from
// the back buffer over it and adds it to the changes, so AgeRepair rewrites it in every buffer
// as it is next used (no stale pixels, no full-head copy).
// Pure functions over plain memory: host-tested in test_nv_disp_driver.
#include <stdint.h>
#include "display_output.h"
#include "nv_disp_flipper.h"

namespace brook::nvdisp {

inline DamageRect ClipRect(const DamageRect& r, uint32_t width, uint32_t height) {
    DamageRect c{r.x0 < 0 ? 0 : r.x0, r.y0 < 0 ? 0 : r.y0, r.x1 > int32_t(width) ? int32_t(width) : r.x1,
                 r.y1 > int32_t(height) ? int32_t(height) : r.y1};
    if (c.empty()) c = DamageRect{};
    return c;
}
inline bool RectsIntersect(const DamageRect& a, const DamageRect& b) {
    return !a.empty() && !b.empty() && a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}
// The parts of r outside h (at most 4: the bands above and below, then left and right).
inline uint32_t SubtractRect(const DamageRect& r, const DamageRect& h, DamageRect out[4]) {
    if (r.empty()) return 0;
    if (!RectsIntersect(r, h)) { out[0] = r; return 1; }
    uint32_t n = 0;
    if (h.y0 > r.y0) out[n++] = DamageRect{r.x0, r.y0, r.x1, h.y0};
    if (h.y1 < r.y1) out[n++] = DamageRect{r.x0, h.y1, r.x1, r.y1};
    const int32_t y0 = h.y0 > r.y0 ? h.y0 : r.y0, y1 = h.y1 < r.y1 ? h.y1 : r.y1;
    if (h.x0 > r.x0) out[n++] = DamageRect{r.x0, y0, h.x0, y1};
    if (h.x1 < r.x1) out[n++] = DamageRect{h.x1, y0, r.x1, y1};
    return n;
}
// Calls f for each part of r outside the hole (all of r when the hole is empty).
template <typename F> inline void ForEachOutside(const DamageRect& r, const DamageRect& hole, F f) {
    DamageRect parts[4];
    const uint32_t n = hole.empty() ? (r.empty() ? 0u : (parts[0] = r, 1u)) : SubtractRect(r, hole, parts);
    for (uint32_t i = 0; i < n; ++i) f(parts[i]);
}
// The old hole `was` is no longer covered by `now`: refresh the shadow from src over it and
// report it changed (the caller's AgeRepair spreads it to every buffer). Returns true if it did.
inline bool RepairHole(const uint32_t* src, uint32_t srcStride, uint32_t* shadow, uint32_t shadowStride,
                       uint32_t width, uint32_t height, const DamageRect& was, const DamageRect& now,
                       DamageRects& changed) {
    if (was.empty() || was == now) return false;
    const DamageRect c = ClipRect(was, width, height);
    if (c.empty()) return false;
    for (int32_t y = c.y0; y < c.y1; ++y)
        __builtin_memcpy(shadow + uint64_t(y) * shadowStride + c.x0, src + uint64_t(y) * srcStride + c.x0,
                         uint64_t(c.x1 - c.x0) * 4);
    changed.Add(c);
    return true;
}
// The shadow diff of one present with a hole: each damaged rect minus the hole. Returns the
// pixels compared.
inline uint64_t HoleDiff(const uint32_t* src, uint32_t srcStride, uint32_t* shadow, uint32_t shadowStride,
                         uint32_t width, uint32_t height, const DamageRect& r, const DamageRect& hole,
                         DamageRects& changed) {
    uint64_t compared = 0;
    ForEachOutside(r, hole, [&](const DamageRect& part) {
        compared += ShadowDiff(src, srcStride, shadow, shadowStride, width, height, part, changed); });
    return compared;
}
// Copy the BO into the hole of a target buffer (CPU path), forcing alpha to 255 (the client
// buffer is XRGB, or ARGB the client declared opaque). bo is the hole's top-left pixel.
inline void CopyLayerRect(uint32_t* dst, uint32_t dstPitchPx, const DamageRect& hole, const uint32_t* bo,
                          uint32_t boStridePx) {
    if (hole.empty()) return;
    const uint32_t w = uint32_t(hole.x1 - hole.x0);
    for (int32_t y = hole.y0; y < hole.y1; ++y) {
        uint32_t* d = dst + uint64_t(y) * dstPitchPx + hole.x0;
        const uint32_t* s = bo + uint64_t(y - hole.y0) * boStridePx;
        for (uint32_t x = 0; x < w; ++x) d[x] = s[x] | 0xff000000u;
    }
}
} // namespace brook::nvdisp
