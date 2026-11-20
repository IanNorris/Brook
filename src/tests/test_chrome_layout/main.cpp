// Hit-target conformance (DESIGN.md "Conformance test"): the window-frame
// part table against the WmHitTest spec, table-driven at 100, 150 and 200%.
// Expected zones are written from the spec's logical sizes, not from the
// implementation's arithmetic.
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "chrome_layout.h"

using namespace brook;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHROME_LAYOUT line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)

enum class Z { None, Close, Max, Min, Drag, Client, Border, L, T, R, B, TL, TR, BL, BR, Dock };
const char* Name(Z z) {
    static const char* names[] = {"none", "close", "max", "min", "drag", "client", "border",
                                  "L", "T", "R", "B", "TL", "TR", "BL", "BR", "dock"};
    return names[static_cast<int>(z)];
}
Z Classify(const ChromePart* p) {
    if (!p) return Z::None;
    switch (p->role) {
    case ChromePartRole::Close: return Z::Close;
    case ChromePartRole::Maximise: return Z::Max;
    case ChromePartRole::Minimise: return Z::Min;
    case ChromePartRole::Drag: return Z::Drag;
    case ChromePartRole::Client: return Z::Client;
    case ChromePartRole::Border: return Z::Border;
    case ChromePartRole::Dock: return Z::Dock;
    case ChromePartRole::Resize: break;
    }
    switch (p->edges) {
    case ChromeEdgeLeft: return Z::L;
    case ChromeEdgeTop: return Z::T;
    case ChromeEdgeRight: return Z::R;
    case ChromeEdgeBottom: return Z::B;
    case ChromeEdgeTop | ChromeEdgeLeft: return Z::TL;
    case ChromeEdgeTop | ChromeEdgeRight: return Z::TR;
    case ChromeEdgeBottom | ChromeEdgeLeft: return Z::BL;
    case ChromeEdgeBottom | ChromeEdgeRight: return Z::BR;
    default: return Z::None;
    }
}

struct Probe { int x, y; Z want; const char* why; };

// Probes are relative to the window's outer top-left corner.
void Run(const char* scene, const DesktopMetrics& m, const ChromeLayout& l, const Probe* probes, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const Probe& p = probes[i];
        const Z got = Classify(l.Hit(l.outer.x + p.x, l.outer.y + p.y));
        if (got != p.want) {
            std::fprintf(stderr, "%s @%u%%: (%d,%d) %s: got %s, want %s\n", scene, m.percent, p.x, p.y, p.why,
                         Name(got), Name(p.want));
            ++failures;
        }
    }
}

// Spec sizes in physical pixels.
struct Spec {
    int border, title, button, margin, corner, band, square;
    explicit Spec(const DesktopMetrics& m)
        : border(m.pixels(2)), title(m.pixels(40)), button(m.pixels(48)), margin(m.pixels(6)),
          corner(m.pixels(16)), band(m.pixels(2)), square(m.pixels(8)) {}
};

// The caption run is contiguous, has no dead pixels, each button spans the
// full titlebar height from y = 0, Close reaches the right outer edge, and
// every button is drawn exactly in today's cell (no pixel change).
void CheckButtons(const char* scene, const ChromeFrameState& s, const DesktopMetrics& m, const ChromeLayout& l, bool bleed) {
    const ChromePart* parts[3] = {l.Find(ChromePartRole::Close), l.Find(ChromePartRole::Maximise),
                                  l.Find(ChromePartRole::Minimise)};
    for (int i = 0; i < 3; ++i) {
        CHECK(parts[i]);
        const DesktopRect cell = DesktopCaptionRect(s.geometry, m, i), v = parts[i]->visual, h = parts[i]->hit;
        CHECK(v.x == cell.x && v.y == cell.y && v.w == cell.w && v.h == cell.h);
        if (bleed) {
            CHECK(h.y == l.outer.y && h.y + h.h == v.y + v.h);
            if (i == 0) CHECK(h.x + h.w == l.outer.x + l.outer.w);
            else CHECK(h.x + h.w == parts[i - 1]->hit.x);
        } else CHECK(h.x == v.x && h.y == v.y && h.w == v.w && h.h == v.h);
    }
    (void)scene;
}
} // namespace

int main() {
    const DesktopRect screen{0, 0, 1280, 800};
    for (const DesktopMetrics* metrics : {&DesktopMetric100, &DesktopMetric150, &DesktopMetric200}) {
        const DesktopMetrics& m = *metrics;
        const Spec k(m);
        const int clientW = 400, clientH = 300;
        const int ow = clientW + 2 * k.border, oh = clientH + k.title + 2 * k.border;
        const int titleBottom = k.border + k.title, midY = k.border + k.title / 2;
        const int closeX = ow - k.border - k.button, maxX = closeX - k.button, minX = maxX - k.button;

        // --- Normal window, away from every screen edge.
        {
            const ChromeFrameState s{{100, 100, clientW, clientH, false}, false, screen};
            const ChromeLayout l = ChromeFrameLayout(s, m);
            CHECK(l.count <= ChromePartLimit);
            CheckButtons("normal", s, m, l, true);
            const Probe probes[] = {
                {ow - 1, 0, Z::Close, "outer top-right pixel"},
                {ow - 1, titleBottom - 1, Z::Close, "right edge, titlebar bottom row"},
                {ow - 1, titleBottom, Z::R, "right edge below the titlebar"},
                {closeX, midY, Z::Close, "close left edge"},
                {closeX - 1, midY, Z::Max, "max right edge"},
                {maxX, midY, Z::Max, "max left edge"},
                {maxX - 1, midY, Z::Min, "min right edge"},
                {minX, midY, Z::Min, "min left edge"},
                {minX - 1, midY, Z::Drag, "titlebar beside min"},
                {minX - 1, 0, Z::T, "top band beside min"},
                {closeX, -1, Z::T, "outer margin above close"},
                {ow - 1, -1, Z::TR, "outer margin above top-right corner"},
                {ow - k.corner, -1, Z::TR, "top margin, corner extent"},
                {ow - k.corner - 1, -1, Z::T, "top margin, past corner extent"},
                {ow + k.margin - 1, -k.margin, Z::TR, "outer top-right margin corner"},
                {ow + k.margin, -1, Z::None, "beyond right margin"},
                {ow / 2, -1, Z::T, "top margin"},
                {ow / 2, -k.margin, Z::T, "top margin, outermost"},
                {ow / 2, -k.margin - 1, Z::None, "above top margin"},
                {-1, oh / 2, Z::L, "left margin"},
                {-k.margin, oh / 2, Z::L, "left margin, outermost"},
                {-k.margin - 1, oh / 2, Z::None, "beyond left margin"},
                {ow, oh / 2, Z::R, "right margin"},
                {ow + k.margin - 1, oh / 2, Z::R, "right margin, outermost"},
                {ow + k.margin, oh / 2, Z::None, "beyond right margin"},
                {ow / 2, oh, Z::B, "bottom margin"},
                {ow / 2, oh + k.margin - 1, Z::B, "bottom margin, outermost"},
                {ow / 2, oh + k.margin, Z::None, "below bottom margin"},
                {-1, k.corner - 1, Z::TL, "left margin, corner extent"},
                {-1, k.corner, Z::L, "left margin, past corner extent"},
                {-k.margin, -k.margin, Z::TL, "outer top-left corner"},
                {k.corner - 1, oh, Z::BL, "bottom margin, corner extent"},
                {k.corner, oh, Z::B, "bottom margin, past corner extent"},
                {ow + k.margin - 1, oh + k.margin - 1, Z::BR, "outer bottom-right corner"},
                {-1, oh - 1, Z::BL, "left margin at bottom"},
                {0, oh / 2, Z::L, "inner left band"},
                {k.band - 1, oh / 2, Z::L, "inner left band, innermost"},
                {k.band, oh / 2, Z::Client, "client left edge"},
                {ow - 1, oh / 2, Z::R, "inner right band"},
                {ow - k.band, oh / 2, Z::R, "inner right band, innermost"},
                {ow - k.band - 1, oh / 2, Z::Client, "client right edge"},
                {ow / 2, oh - 1, Z::B, "inner bottom band"},
                {ow / 2, oh - k.band - 1, Z::Client, "client bottom edge"},
                {ow - 1, oh - 1, Z::BR, "inner bottom-right corner"},
                {ow - k.square, oh - k.square, Z::BR, "inner bottom-right square"},
                {k.square - 1, oh - k.square, Z::BL, "inner bottom-left square"},
                {k.square, oh - 1, Z::B, "past bottom-left square"},
                {0, 0, Z::TL, "inner top-left corner"},
                {k.square - 1, k.square - 1, Z::TL, "inner top-left square"},
                {k.square, k.band, Z::Drag, "titlebar below the top band"},
                {minX / 2, titleBottom - 1, Z::Drag, "titlebar bottom row"},
                {ow / 2, titleBottom, Z::Client, "client top edge"},
            };
            Run("normal", m, l, probes, sizeof(probes) / sizeof(probes[0]));
            // y = 0 across the whole titlebar: top band, then the buttons.
            for (int x = 0; x < ow; ++x) {
                const Z want = x < k.square ? Z::TL : x < minX ? Z::T : x < maxX ? Z::Min : x < closeX ? Z::Max : Z::Close;
                const Probe p{x, 0, want, "y = 0 sweep"};
                Run("normal", m, l, &p, 1);
            }
            CHECK(!l.Find(ChromePartRole::Dock));
        }
        // --- The side-dock button: a fourth cell left of minimise, the same
        // run rules; the title drag and top band stop at it.
        {
            ChromeFrameState s{{100, 100, clientW, clientH, false}, false, screen};
            s.geometry.dockButton = 2;
            const ChromeLayout l = ChromeFrameLayout(s, m);
            CheckButtons("dock", s, m, l, true);
            const ChromePart* dock = l.Find(ChromePartRole::Dock);
            CHECK(dock && l.dockRight);
            const int dockX = minX - k.button;
            CHECK(dock->visual.x == 100 + dockX && dock->visual.w == k.button);
            CHECK(dock->hit.x + dock->hit.w == l.Find(ChromePartRole::Minimise)->hit.x);
            const Probe probes[] = {
                {minX - 1, midY, Z::Dock, "dock right edge"},
                {dockX, midY, Z::Dock, "dock left edge"},
                {dockX - 1, midY, Z::Drag, "titlebar beside dock"},
                {dockX, 0, Z::Dock, "dock reaches the outer top"},
                {dockX - 1, 0, Z::T, "top band beside dock"},
            };
            Run("dock", m, l, probes, sizeof(probes) / sizeof(probes[0]));
            // Client-decorated windows never carry it, overlaid or not.
            DesktopWindowGeometry csd{0, 0, clientW, clientH, true, true};
            csd.dockButton = 1;
            CHECK(DesktopCaptionRect(csd, m, 3).w == 0);
            CHECK(!ChromeFrameLayout({csd, false, screen}, m).Find(ChromePartRole::Dock));
        }

        // --- Maximised: no resize zones; the buttons own y = 0 and the
        // rightmost column, so the screen's top-right pixel is Close.
        {
            const int mw = screen.w - 2 * k.border, mh = 720 - k.title - 2 * k.border;
            const ChromeFrameState s{{0, 0, mw, mh, false}, true, screen};
            const ChromeLayout l = ChromeFrameLayout(s, m);
            CheckButtons("maximised", s, m, l, true);
            const int w = screen.w, h = 720, cx = w - k.border - k.button, mx = cx - 2 * k.button;
            const Probe probes[] = {
                {w - 1, 0, Z::Close, "screen top-right pixel"},
                {w - 1, titleBottom - 1, Z::Close, "rightmost column, titlebar bottom row"},
                {w - 1, titleBottom, Z::Border, "rightmost column below the titlebar"},
                {0, h / 2, Z::Border, "no left band"},
                {w / 2, h - 1, Z::Border, "no bottom band"},
                {w / 2, h, Z::None, "no bottom margin"},
                {0, 0, Z::Drag, "top-left pixel drags"},
                {mx - 1, 0, Z::Drag, "y = 0 beside min"},
                {mx, 0, Z::Min, "y = 0 at min"},
            };
            Run("maximised", m, l, probes, sizeof(probes) / sizeof(probes[0]));
            for (int x = 0; x < w; ++x) {
                const Z want = x < mx ? Z::Drag : x < mx + k.button ? Z::Min : x < cx ? Z::Max : Z::Close;
                const Probe p{x, 0, want, "y = 0 sweep"};
                Run("maximised", m, l, &p, 1);
            }
        }

        // --- Pushed to the right screen edge (tiled right): that edge loses
        // its resize zones, so Close owns the screen's right column there.
        {
            const ChromeFrameState s{{screen.w - ow, 100, clientW, clientH, false}, false, screen};
            const ChromeLayout l = ChromeFrameLayout(s, m);
            CheckButtons("right-tiled", s, m, l, true);
            const Probe probes[] = {
                {ow - 1, 0, Z::Close, "outer top-right pixel"},
                {ow - 1, titleBottom, Z::Border, "no inner right band"},
                {ow - 1, oh / 2, Z::Border, "no inner right band"},
                {ow - 1, -1, Z::T, "top margin corner keeps only the top edge"},
                {ow - 1, oh, Z::B, "bottom margin corner keeps only the bottom edge"},
                {ow - 1, oh - 1, Z::B, "inner bottom-right square keeps only the bottom edge"},
                {-1, oh / 2, Z::L, "left margin unchanged"},
                {0, 0, Z::TL, "inner top-left unchanged"},
            };
            Run("right-tiled", m, l, probes, sizeof(probes) / sizeof(probes[0]));
        }

        // --- Client-decorated window with overlaid captions (chrome-csd):
        // button parts on a CSD window, hit exactly where they are drawn; no
        // WM resize zones (the client resizes from its own shadow margin).
        {
            const int bw = m.captionButton(), bh = m.title();
            const ChromeFrameState flush{{100, 100, 400, 300, true, true, 0, 0, 0}, false, screen};
            const ChromeLayout l = ChromeFrameLayout(flush, m);
            CheckButtons("csd", flush, m, l, false);
            const Probe probes[] = {
                {399, 0, Z::Close, "surface top-right pixel"},
                {400 - bw, bh - 1, Z::Close, "close bottom-left"},
                {400 - bw - 1, 0, Z::Max, "max right edge"},
                {400 - 3 * bw - 1, 0, Z::Client, "client beside min"},
                {399, bh, Z::Client, "client below close"},
                {400, 0, Z::None, "no WM margin"},
                {-1, 150, Z::None, "no WM margin"},
            };
            Run("csd", m, l, probes, sizeof(probes) / sizeof(probes[0]));
            // A transparent shadow margin stays the client's.
            const ChromeFrameState inset{{100, 100, 400, 300, true, true, 10, 12, 0}, false, screen};
            const ChromeLayout li = ChromeFrameLayout(inset, m);
            CheckButtons("csd-inset", inset, m, li, false);
            const Probe insetProbes[] = {
                {399, 0, Z::Client, "shadow margin corner"},
                {389, 11, Z::Client, "shadow margin above close"},
                {389, 12, Z::Close, "close top-right"},
            };
            Run("csd-inset", m, li, insetProbes, sizeof(insetProbes) / sizeof(insetProbes[0]));
            // Without overlaid captions the whole surface is client.
            const ChromeFrameState plain{{100, 100, 400, 300, true}, false, screen};
            const Probe plainProbe{399, 0, Z::Client, "no overlay"};
            Run("csd-plain", m, ChromeFrameLayout(plain, m), &plainProbe, 1);
        }
    }
    if (failures) {
        std::fprintf(stderr, "CHROME_LAYOUT: %d failures\n", failures);
        return 1;
    }
    std::puts("CHROME_LAYOUT_OK");
    return 0;
}
