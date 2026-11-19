#pragma once
// Window-frame part table (compositor refactor stage 1.1). One table per
// window state holds every chrome part with its role, the rectangle it is
// drawn in and the (usually larger) rectangle it is hit in. WmHitTest walks
// the table front to back and the frame painter draws the same parts, so what
// is drawn and what is clickable cannot disagree.
//
// The zones follow DESIGN.md "Hit targets and the WmHitTest spec": an outer
// resize margin, caption buttons that own the top and right outer edges, a
// thin inner resize band, the titlebar, then the client. Metrics are today's
// (title 36, buttons 46); the Clearwater metrics are stage 2.1.
//
// Freestanding: depends only on desktop_metrics.h, so the kernel WM, host
// tests and a later user-mode shell share it.
#include <stdint.h>
#include "desktop_metrics.h"

namespace brook {

enum class ChromePartRole : uint8_t { Resize, Close, Maximise, Minimise, Drag, Client, Border };
enum : uint8_t { ChromeEdgeLeft = 1, ChromeEdgeTop = 2, ChromeEdgeRight = 4, ChromeEdgeBottom = 8 };

struct ChromePart {
    ChromePartRole role;
    uint8_t edges;      // Resize: the edges a drag moves
    DesktopRect visual; // where the part is drawn (empty for pure hit zones)
    DesktopRect hit;    // where it takes input; buttons bleed to the outer edges
};

struct ChromeFrameState {
    DesktopWindowGeometry geometry;
    bool maximised;
    // The output, for edges that touch it (a window tiled or pushed to the
    // screen edge loses that edge's resize zones). Empty: no screen edges.
    DesktopRect screen;
};

// Resize zone sizes in logical pixels (DESIGN.md token sheet, "Resize zones").
struct ChromeResizeMetrics {
    static constexpr int OuterBase = 6;        // outside the visible frame
    static constexpr int OuterCornerBase = 16; // along each edge, diagonal
    static constexpr int InnerBase = 2;        // inside the frame
    static constexpr int InnerCornerBase = 8;  // inside corner squares
};

static constexpr uint32_t ChromePartLimit = 32;

struct ChromeLayout {
    DesktopRect outer;  // the visible frame (CSD: the client surface)
    DesktopRect bounds; // union of every hit rect: outer plus the margin
    uint32_t count;
    ChromePart parts[ChromePartLimit];

    // First part whose hit rect contains the point (test order), or null.
    const ChromePart* Hit(int px, int py) const {
        if (!bounds.contains(px, py)) return nullptr;
        for (uint32_t i = 0; i < count; ++i)
            if (parts[i].hit.contains(px, py)) return &parts[i];
        return nullptr;
    }
    const ChromePart* Find(ChromePartRole role) const {
        for (uint32_t i = 0; i < count; ++i)
            if (parts[i].role == role) return &parts[i];
        return nullptr;
    }
    void Add(ChromePartRole role, uint8_t edges, const DesktopRect& visual, const DesktopRect& hit) {
        if (count < ChromePartLimit && hit.w > 0 && hit.h > 0) parts[count++] = {role, edges, visual, hit};
    }
};

// Caption button roles in DesktopCaptionRect index order (0 close, 1
// maximise, 2 minimise).
inline constexpr ChromePartRole ChromeCaptionRole(int index) {
    return index == 0 ? ChromePartRole::Close : index == 1 ? ChromePartRole::Maximise : ChromePartRole::Minimise;
}
inline constexpr int ChromeCaptionIndex(ChromePartRole role) {
    return role == ChromePartRole::Close ? 0 : role == ChromePartRole::Maximise ? 1 :
           role == ChromePartRole::Minimise ? 2 : -1;
}
inline constexpr bool ChromePartIsButton(ChromePartRole role) { return ChromeCaptionIndex(role) >= 0; }

inline ChromeLayout ChromeFrameLayout(const ChromeFrameState& s, const DesktopMetrics& m) {
    ChromeLayout l{};
    const DesktopWindowGeometry& g = s.geometry;
    if (g.clientDecorated) {
        // The client draws its own frame and handles its own resize (its
        // shadow margin). Overlaid captions are the only WM parts: buttons
        // exactly where they are drawn, over the client's title strip.
        l.outer = l.bounds = {g.x, g.y, g.clientW, g.clientH};
        if (g.captionOverlay)
            for (int i = 0; i < 3; ++i) {
                const DesktopRect r = DesktopCaptionRect(g, m, i);
                l.Add(ChromeCaptionRole(i), 0, r, r);
            }
        l.Add(ChromePartRole::Client, 0, {}, l.outer);
        return l;
    }
    const int border = m.border(), titleBottom = m.border() + m.title();
    const int ow = g.clientW + 2 * border, oh = g.clientH + m.title() + 2 * border;
    const int x = g.x, y = g.y, right = x + ow, bottom = y + oh;
    l.outer = l.bounds = {x, y, ow, oh};
    uint8_t lost = s.maximised ? uint8_t(ChromeEdgeLeft | ChromeEdgeTop | ChromeEdgeRight | ChromeEdgeBottom) : 0;
    if (s.screen.w > 0 && s.screen.h > 0) {
        if (x <= s.screen.x) lost |= ChromeEdgeLeft;
        if (y <= s.screen.y) lost |= ChromeEdgeTop;
        if (right >= s.screen.x + s.screen.w) lost |= ChromeEdgeRight;
        if (bottom >= s.screen.y + s.screen.h) lost |= ChromeEdgeBottom;
    }
    auto resize = [&](uint8_t edges, const DesktopRect& r) {
        edges &= uint8_t(~lost);
        if (edges) l.Add(ChromePartRole::Resize, edges, {}, r);
    };

    // Zone 1: the outer margin. Each side exists only while its edge is
    // free; within the corner extent it resizes diagonally.
    const int mo = m.pixels(ChromeResizeMetrics::OuterBase), mc = m.pixels(ChromeResizeMetrics::OuterCornerBase);
    if (!(lost & ChromeEdgeTop)) {
        resize(ChromeEdgeTop | ChromeEdgeLeft, {x - mo, y - mo, mc + mo, mo});
        resize(ChromeEdgeTop, {x + mc, y - mo, ow - 2 * mc, mo});
        resize(ChromeEdgeTop | ChromeEdgeRight, {right - mc, y - mo, mc + mo, mo});
    }
    if (!(lost & ChromeEdgeBottom)) {
        resize(ChromeEdgeBottom | ChromeEdgeLeft, {x - mo, bottom, mc + mo, mo});
        resize(ChromeEdgeBottom, {x + mc, bottom, ow - 2 * mc, mo});
        resize(ChromeEdgeBottom | ChromeEdgeRight, {right - mc, bottom, mc + mo, mo});
    }
    if (!(lost & ChromeEdgeLeft)) {
        resize(ChromeEdgeLeft | ChromeEdgeTop, {x - mo, y, mo, mc});
        resize(ChromeEdgeLeft, {x - mo, y + mc, mo, oh - 2 * mc});
        resize(ChromeEdgeLeft | ChromeEdgeBottom, {x - mo, bottom - mc, mo, mc});
    }
    if (!(lost & ChromeEdgeRight)) {
        resize(ChromeEdgeRight | ChromeEdgeTop, {right, y, mo, mc});
        resize(ChromeEdgeRight, {right, y + mc, mo, oh - 2 * mc});
        resize(ChromeEdgeRight | ChromeEdgeBottom, {right, bottom - mc, mo, mc});
    }
    l.bounds = {x - (lost & ChromeEdgeLeft ? 0 : mo), y - (lost & ChromeEdgeTop ? 0 : mo), 0, 0};
    l.bounds.w = right + (lost & ChromeEdgeRight ? 0 : mo) - l.bounds.x;
    l.bounds.h = bottom + (lost & ChromeEdgeBottom ? 0 : mo) - l.bounds.y;

    // Zone 2: caption buttons, a contiguous run drawn in today's cells. Each
    // is hit from the outer top edge to the titlebar's bottom; Close also to
    // the outer right edge, so the top-right corner pixel is Close.
    int runLeft = right;
    for (int i = 0; i < 3; ++i) {
        const DesktopRect r = DesktopCaptionRect(g, m, i);
        if (r.w <= 0 || r.h <= 0) continue;
        const int hitRight = i == 0 ? right : r.x + r.w;
        l.Add(ChromeCaptionRole(i), 0, r, {r.x, y, hitRight - r.x, titleBottom});
        if (r.x < runLeft) runLeft = r.x;
    }

    // Zone 3: the inner band, corner squares first. No band above or to the
    // right of the buttons: the top-right inside corner is Close.
    const int bi = m.pixels(ChromeResizeMetrics::InnerBase), ic = m.pixels(ChromeResizeMetrics::InnerCornerBase);
    if (!s.maximised) {
        resize(ChromeEdgeTop | ChromeEdgeLeft, {x, y, ic, ic});
        resize(ChromeEdgeBottom | ChromeEdgeLeft, {x, bottom - ic, ic, ic});
        resize(ChromeEdgeBottom | ChromeEdgeRight, {right - ic, bottom - ic, ic, ic});
        resize(ChromeEdgeLeft, {x, y, bi, oh});
        resize(ChromeEdgeRight, {right - bi, y, bi, oh});
        resize(ChromeEdgeBottom, {x, bottom - bi, ow, bi});
        resize(ChromeEdgeTop, {x, y, runLeft - x, bi});
    }

    // Zones 4 and 5: the titlebar drags; the client; the remaining frame.
    l.Add(ChromePartRole::Drag, 0, {x + border, y + border, g.clientW, m.title()}, {x, y, ow, titleBottom});
    const DesktopRect client{x + border, y + titleBottom, g.clientW, g.clientH};
    l.Add(ChromePartRole::Client, 0, client, client);
    l.Add(ChromePartRole::Border, 0, {}, l.outer);
    return l;
}

} // namespace brook
