#pragma once
// Per-surface overlay cache (compositor refactor stage 1.4). The renderer's
// overlay capture used to re-rasterise every shell surface (taskbar,
// launcher, popups, boot summary) into the shared capture buffer and hash all
// of their pixels. Each surface now has a content key (its display list's
// hash, WmShellSurfaceKey), and the buffer remembers which surface, rectangle
// and key it holds. A capture rasterises only the surfaces whose key or
// rectangle changed, and anything that shares pixels with them, so an
// unchanged launcher costs nothing while the taskbar graph or a hover
// changes. Freestanding; the compositor holds the buffer lock around it.
#include <stdint.h>
#include "desktop_overlay_regions.h"

namespace brook {

static constexpr uint32_t DESKTOP_OVERLAY_SURFACE_LIMIT = 4;
// Up to 4 dropped cache slots, 4 redrawn surfaces and 4 foreign rectangles.
static constexpr uint32_t DESKTOP_OVERLAY_CLEAR_LIMIT = 12;

struct DesktopOverlaySurfaceSlot {
    uint32_t kind;
    DesktopOverlayRect rect;
    uint64_t key; // 0: unknown content, never kept
};

// What the buffer holds outside the zero background. valid=false: unknown
// (fresh or wiped buffer), so every surface is drawn. foreign: pixels no
// surface owns (a pointer composited into a debug capture), cleared and
// redrawn under next time.
struct DesktopOverlaySurfaceCache {
    DesktopOverlaySurfaceSlot slots[DESKTOP_OVERLAY_SURFACE_LIMIT]{};
    uint32_t count = 0;
    DesktopOverlayRect foreign[DESKTOP_OVERLAY_SURFACE_LIMIT]{};
    uint32_t foreignCount = 0;
    bool valid = false;
};

struct DesktopOverlaySurfacePlan {
    DesktopOverlayRect clears[DESKTOP_OVERLAY_CLEAR_LIMIT]{};
    uint32_t clearCount = 0;
    uint32_t draw = 0;    // bit i: rasterise current surface i (after the clears)
    bool wipe = false;    // clear the whole buffer instead of the rectangles
};

inline bool DesktopOverlayRectsOverlap(const DesktopOverlayRect& a, const DesktopOverlayRect& b) {
    if (!a.width || !a.height || !b.width || !b.height) return false;
    return int64_t(a.x) < int64_t(b.x) + b.width && int64_t(b.x) < int64_t(a.x) + a.width &&
           int64_t(a.y) < int64_t(b.y) + b.height && int64_t(b.y) < int64_t(a.y) + a.height;
}
inline bool DesktopOverlaySlotEqual(const DesktopOverlaySurfaceSlot& a, const DesktopOverlaySurfaceSlot& b) {
    return a.kind == b.kind && a.key == b.key && a.rect.x == b.rect.x && a.rect.y == b.rect.y &&
           a.rect.width == b.rect.width && a.rect.height == b.rect.height;
}

// Plan one capture of the current surfaces (back to front). A surface is kept
// when the buffer already holds it (same kind, rectangle and nonzero key) and
// none of its pixels are cleared: by a surface that left or changed, by a
// redrawn surface's own rectangle, or by foreign pixels. Keeping is decided
// to a fixed point, so a kept surface never overlaps a cleared rectangle and
// rasterising the drawn ones (back to front, onto cleared pixels) reproduces
// exactly what a full redraw onto a zero buffer would.
inline DesktopOverlaySurfacePlan DesktopOverlayPlanSurfaces(const DesktopOverlaySurfaceCache& cache,
                                                            const DesktopOverlaySurfaceSlot* current, uint32_t count) {
    DesktopOverlaySurfacePlan plan{};
    if (count > DESKTOP_OVERLAY_SURFACE_LIMIT) count = DESKTOP_OVERLAY_SURFACE_LIMIT;
    if (!cache.valid || cache.count > DESKTOP_OVERLAY_SURFACE_LIMIT ||
        cache.foreignCount > DESKTOP_OVERLAY_SURFACE_LIMIT) {
        plan.wipe = true;
        plan.draw = count ? (1u << count) - 1u : 0u;
        return plan;
    }
    bool kept[DESKTOP_OVERLAY_SURFACE_LIMIT]{}, slotUsed[DESKTOP_OVERLAY_SURFACE_LIMIT]{};
    for (uint32_t i = 0; i < count; ++i) {
        if (!current[i].key) continue;
        for (uint32_t j = 0; j < cache.count && !kept[i]; ++j)
            if (!slotUsed[j] && DesktopOverlaySlotEqual(current[i], cache.slots[j])) { kept[i] = slotUsed[j] = true; }
    }
    auto add = [&](const DesktopOverlayRect& r) {
        if (!r.width || !r.height) return;
        if (plan.clearCount < DESKTOP_OVERLAY_CLEAR_LIMIT) plan.clears[plan.clearCount++] = r;
        else plan.wipe = true;
    };
    for (uint32_t j = 0; j < cache.count; ++j) if (!slotUsed[j]) add(cache.slots[j].rect);
    for (uint32_t j = 0; j < cache.foreignCount; ++j) add(cache.foreign[j]);
    for (uint32_t i = 0; i < count; ++i) if (!kept[i]) add(current[i].rect);
    for (bool changed = true; changed && !plan.wipe;) {
        changed = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (!kept[i]) continue;
            for (uint32_t c = 0; c < plan.clearCount; ++c)
                if (DesktopOverlayRectsOverlap(current[i].rect, plan.clears[c])) {
                    kept[i] = false; add(current[i].rect); changed = true; break;
                }
        }
    }
    if (plan.wipe) {
        plan.clearCount = 0;
        plan.draw = count ? (1u << count) - 1u : 0u;
        return plan;
    }
    for (uint32_t i = 0; i < count; ++i) if (!kept[i]) plan.draw |= 1u << i;
    return plan;
}

// After the plan was executed: the buffer holds exactly these surfaces.
inline void DesktopOverlayCommitSurfaces(DesktopOverlaySurfaceCache& cache,
                                         const DesktopOverlaySurfaceSlot* current, uint32_t count) {
    if (count > DESKTOP_OVERLAY_SURFACE_LIMIT) count = DESKTOP_OVERLAY_SURFACE_LIMIT;
    for (uint32_t i = 0; i < count; ++i) cache.slots[i] = current[i];
    cache.count = count;
    cache.foreignCount = 0;
    cache.valid = true;
}

// Pixels outside any surface's ownership were written (a composited pointer):
// they are cleared next time, and the surfaces beneath them redrawn.
inline void DesktopOverlayAddForeign(DesktopOverlaySurfaceCache& cache, const DesktopOverlayRect& r) {
    if (cache.foreignCount < DESKTOP_OVERLAY_SURFACE_LIMIT) cache.foreign[cache.foreignCount++] = r;
    else cache.valid = false;
}

// Band keys from the surface keys (replacing the per-pixel hash): a band's
// key folds its geometry and opacity with, in z order, the index, rectangle
// and key of every input rectangle that shares pixels with it. Equal keys
// therefore mean the band holds the same surfaces in the same state, which
// rasterise to identical pixels. A rectangle without a key (0: a composited
// pointer) makes the band fall back to hashing its pixels.
inline void DesktopOverlayKeyBandsFromRects(const uint32_t* pixels, uint32_t width, uint32_t height,
                                            DesktopOverlayBands& bands, const DesktopOverlayRect* rects,
                                            const uint64_t* rectKeys, uint32_t count) {
    constexpr uint64_t prime = 0x100000001b3ull;
    for (uint32_t i = 0; i < bands.count && i < 7; ++i) {
        const auto& b = bands.bands[i];
        const DesktopOverlayRect band{int32_t(b.x), int32_t(b.y), b.width, b.height};
        uint64_t h = 0x9e3779b97f4a7c15ull ^ (uint64_t(b.x) << 32 | b.y);
        h = (h ^ (uint64_t(b.width) << 32 | b.height)) * prime;
        h = (h ^ ((bands.opaque >> i) & 1u)) * prime;
        bool unkeyed = false;
        for (uint32_t r = 0; r < count; ++r) {
            if (!DesktopOverlayRectsOverlap(band, rects[r])) continue;
            if (!rectKeys[r]) unkeyed = true;
            h = (h ^ r) * prime;
            h = (h ^ (uint64_t(uint32_t(rects[r].x)) << 32 | uint32_t(rects[r].y))) * prime;
            h = (h ^ (uint64_t(rects[r].width) << 32 | rects[r].height)) * prime;
            h = (h ^ rectKeys[r]) * prime;
        }
        if (unkeyed) {
            DesktopOverlayBands one{};
            one.count = 1; one.bands[0] = b; one.opaque = (bands.opaque >> i) & 1u;
            DesktopOverlayKeyBands(pixels, width, height, one);
            h = (h ^ one.keys[0]) * prime;
        }
        bands.keys[i] = h ? h : 1;
    }
}

} // namespace brook
