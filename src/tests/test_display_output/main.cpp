// Multi-output D0: the output registry, desktop union, rect damage, the
// pointer's union clamp and the per-output shell layout.
#include "display_output.h"
#include "desktop_pointer_motion.h"
#include "shell_output_layout.h"

#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>

using namespace brook;

namespace {

struct FakeBackend {
    uint32_t count = 0;
    OutputInfo infos[OutputMax]{};
};
FakeBackend g_a, g_b;

OutputInfo Make(int32_t x, int32_t y, uint32_t w, uint32_t h, bool enabled = true, bool connected = true)
{
    OutputInfo info{};
    info.connected = connected; info.enabled = enabled;
    info.x = x; info.y = y; info.w = w; info.h = h;
    info.refreshMhz = 60000; info.scalePercent = 100;
    return info;
}

uint32_t CountA() { return g_a.count; }
bool InfoA(uint32_t id, OutputInfo* out) { if (id >= g_a.count) return false; *out = g_a.infos[id]; out->id = 99; return true; }
uint32_t CountB() { return g_b.count; }
bool InfoB(uint32_t id, OutputInfo* out) { if (id >= g_b.count) return false; *out = g_b.infos[id]; return true; }

const OutputBackendOps kA{"a", CountA, InfoA, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
const OutputBackendOps kB{"b", CountB, InfoB, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

OutputLayout LayoutOf(std::initializer_list<OutputRect> rects)
{
    OutputLayout l;
    for (const OutputRect& r : rects) {
        l.outputs[l.count] = Make(r.x, r.y, r.w, r.h);
        l.outputs[l.count].id = uint8_t(l.count);
        ++l.count;
    }
    return l;
}

// Today's clamp: AdvanceDesktopPointer per axis against the framebuffer.
void SingleScreenClamp(int32_t& x, int32_t& y, uint32_t w, uint32_t h)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= int32_t(w)) x = int32_t(w) - 1;
    if (y >= int32_t(h)) y = int32_t(h) - 1;
}

void TestRegistry()
{
    OutputRegistry r;
    const uint64_t g0 = r.Generation();
    OutputLayout l;
    r.Enumerate(l);
    assert(l.count == 0 && l.generation == g0);
    assert(!r.Register(nullptr));
    g_a.count = 1; g_a.infos[0] = Make(0, 0, 3200, 1800);
    assert(r.Register(&kA));
    assert(!r.Register(&kA)); // once
    assert(r.Generation() > g0);
    r.Enumerate(l);
    assert(l.count == 1 && l.outputs[0].id == 0 && l.backends[0] == &kA && l.backendIds[0] == 0);
    assert(l.enabled(0) && l.rect(0) == (OutputRect{0, 0, 3200, 1800}));

    // A second backend's outputs follow the first's; disconnected ones are
    // skipped and ids renumber.
    g_b.count = 3;
    g_b.infos[0] = Make(0, 0, 0, 0, false, false);
    g_b.infos[1] = Make(3200, 0, 1920, 1080);
    g_b.infos[2] = Make(5120, 0, 1920, 1080, false);
    const uint64_t g1 = r.Generation();
    assert(r.Register(&kB) && r.Generation() > g1);
    r.Enumerate(l);
    assert(l.count == 3);
    assert(l.outputs[1].id == 1 && l.backends[1] == &kB && l.backendIds[1] == 1);
    assert(l.outputs[2].id == 2 && l.backendIds[2] == 2 && !l.enabled(2));
    const uint64_t g2 = r.Generation();
    r.NotifyChanged();
    assert(r.Generation() == g2 + 1);
    assert(r.Unregister(&kA) && !r.Unregister(&kA));
    r.Enumerate(l);
    assert(l.count == 2 && l.backends[0] == &kB && l.outputs[0].x == 3200);
}

void TestUnion()
{
    OutputLayout one = LayoutOf({{0, 0, 3200, 1800}});
    assert(OutputUnionRect(one) == (OutputRect{0, 0, 3200, 1800}));
    uint32_t w = 0, h = 0;
    OutputDesktopExtent(one, &w, &h);
    assert(w == 3200 && h == 1800);

    OutputLayout two = LayoutOf({{0, 0, 3840, 2160}, {3840, 0, 3840, 1080}});
    assert(OutputUnionRect(two) == (OutputRect{0, 0, 7680, 2160}));
    OutputDesktopExtent(two, &w, &h);
    assert(w == 7680 && h == 2160);
    two.outputs[1].enabled = false;
    assert(OutputUnionRect(two) == (OutputRect{0, 0, 3840, 2160}));

    OutputLayout none;
    assert(OutputUnionRect(none).empty());
    OutputDesktopExtent(none, &w, &h);
    assert(w == 0 && h == 0);
}

void TestOutputForRect()
{
    OutputLayout one = LayoutOf({{0, 0, 1920, 1080}});
    // Everything belongs to the only output, even off-screen rects.
    assert(OutputIndexForRect(one, {100, 100, 800, 600}) == 0);
    assert(OutputIndexForRect(one, {-5000, -5000, 10, 10}) == 0);
    assert(OutputIndexForRect(one, {1900, 1000, 800, 600}) == 0);

    OutputLayout two = LayoutOf({{0, 0, 3840, 2160}, {3840, 0, 3840, 2160}});
    assert(OutputIndexForRect(two, {100, 100, 800, 600}) == 0);
    assert(OutputIndexForRect(two, {4000, 100, 800, 600}) == 1);
    // Straddling: the centre decides.
    assert(OutputIndexForRect(two, {3500, 100, 800, 600}) == 1);  // centre 3900
    assert(OutputIndexForRect(two, {3200, 100, 800, 600}) == 0);  // centre 3600
    // Centre below both, overlap with output 1 only.
    assert(OutputIndexForRect(two, {4000, 2000, 400, 600}) == 1);
    // Nowhere: the primary.
    assert(OutputIndexForRect(two, {-9000, 0, 10, 10}) == 0);
    assert(OutputIndexForPoint(two, 3839, 0) == 0 && OutputIndexForPoint(two, 3840, 0) == 1);
    assert(OutputIndexForPoint(two, 7680, 0) == -1);
    assert(OutputRectOrPrimary(two, 3) == two.rect(0));
}

void TestDamage()
{
    // One output: the clipped rows are exactly the row span.
    const OutputRect out{0, 0, 3200, 1800};
    DamageRects d = DamageRects::Rows(100, 300, 3200);
    DamageRects local = DamageClipToOutput(d, out);
    DamageRect b{};
    assert(local.count == 1 && !local.full && local.Bounds(&b));
    assert(b == (DamageRect{0, 100, 3200, 300}));
    // Rows beyond the bottom are cut as `maxY = min(maxY, height)` was.
    local = DamageClipToOutput(DamageRects::Rows(1700, 4000, 3200), out);
    assert(local.Bounds(&b) && b.y0 == 1700 && b.y1 == 1800);
    DamageRects full = DamageRects::Full(3200, 1800);
    local = DamageClipToOutput(full, out);
    assert(local.full && local.Bounds(&b) && b == (DamageRect{0, 0, 3200, 1800}));
    assert(DamageClipToOutput(DamageRects{}, out).empty());

    // Two outputs: each gets its part in local coordinates.
    const OutputRect left{0, 0, 3840, 2160}, right{3840, 0, 3840, 1080};
    DamageRects two;
    two.Add({3000, 500, 4500, 700});   // across the edge
    two.Add({5000, 1500, 5100, 1600}); // under output 1, in no output
    DamageRects l = DamageClipToOutput(two, left), r = DamageClipToOutput(two, right);
    assert(l.count == 1 && l.rects[0] == (DamageRect{3000, 500, 3840, 700}));
    assert(r.count == 1 && r.rects[0] == (DamageRect{0, 500, 660, 700}));
    full = DamageRects::Full(7680, 2160);
    r = DamageClipToOutput(full, right);
    assert(r.full && r.rects[0] == (DamageRect{0, 0, 3840, 1080}));

    // Overflow collapses into the bounding rect; contained rects add nothing.
    DamageRects many;
    for (int i = 0; i < 12; ++i) many.Add({i * 10, i * 10, i * 10 + 5, i * 10 + 5});
    assert(many.count <= DamageRects::Max);
    assert(many.Bounds(&b) && b == (DamageRect{0, 0, 115, 115}));
    DamageRects inner;
    inner.Add({0, 0, 100, 100});
    inner.Add({10, 10, 20, 20});
    inner.Add({5, 5, 5, 50}); // empty
    assert(inner.count == 1);
}

void TestPointerClamp()
{
    // One output: identical to today's per-axis clamp for any point.
    const OutputRect one[] = {{0, 0, 3200, 1800}};
    for (int32_t x = -50; x < 3300; x += 37)
        for (int32_t y = -50; y < 1900; y += 41) {
            int32_t ax = x, ay = y, bx = x, by = y;
            SingleScreenClamp(ax, ay, 3200, 1800);
            DesktopClampToOutputs(one, 1, bx, by);
            assert(ax == bx && ay == by);
        }
    // The motion path: AdvanceDesktopPointer against the union extent, then
    // the union clamp, never moves a single-output position.
    int32_t fraction = 0, x = 3190;
    x = AdvanceDesktopPointer(x, 50, 3200, 100, fraction);
    int32_t y = 900;
    assert(DesktopClampToOutputs(one, 1, x, y) == 0 && x == 3199);

    // Two 3840x2160 side by side: the pointer crosses the edge.
    const OutputRect same[] = {{0, 0, 3840, 2160}, {3840, 0, 3840, 2160}};
    x = 3839; y = 1000;
    x = AdvanceDesktopPointer(x, 5, 7680, 100, fraction);
    assert(DesktopClampToOutputs(same, 2, x, y) == 0 && x == 3844 && y == 1000);
    x = 9000; y = 3000;
    assert(DesktopClampToOutputs(same, 2, x, y) == 3 && x == 7679 && y == 2159);

    // Non-equal heights: output 1 is 1080 tall. Below it is no output.
    const OutputRect mixed[] = {{0, 0, 3840, 2160}, {3840, 0, 3840, 1080}};
    x = 5000; y = 1500;   // nearest: output 1's bottom edge (420 away)
    assert(DesktopClampToOutputs(mixed, 2, x, y) == 2 && x == 5000 && y == 1079);
    x = 3900; y = 2000;   // nearest: output 0's right edge (61 away)
    assert(DesktopClampToOutputs(mixed, 2, x, y) == 1 && x == 3839 && y == 2000);
    x = 3850; y = 500;    // inside output 1
    assert(DesktopClampToOutputs(mixed, 2, x, y) == 0);
    // No rectangles: unchanged.
    x = -5; y = -5;
    assert(DesktopClampToOutputs(mixed, 0, x, y) == 0 && x == -5);
}

void TestShellLayout()
{
    const uint32_t tb = 72; // 48 at 150%
    // One output: the taskbar band is every row from its top, as
    // `my >= WmTaskbarTop(h)` was, without a column check.
    OutputLayout one = LayoutOf({{0, 0, 3200, 1800}});
    assert(ShellTaskbarRect(one.rect(0), tb) == (OutputRect{0, 1728, 3200, 72}));
    assert(ShellWorkAreaBase(one.rect(0), tb) == (OutputRect{0, 0, 3200, 1728}));
    for (int32_t y = 1700; y < 1810; ++y) {
        const auto hit = ShellTaskbarAt(one, tb, 0, 50, y);
        assert((hit.output == 0) == (y >= 1728));
        if (hit.output == 0) assert(hit.x == 50 && hit.y == y);
    }
    assert(ShellTaskbarAt(one, tb, 0, 99999, 1750).output == 0);
    assert(ShellTaskbarAt(one, tb, 1, 50, 1799).output == -1); // hidden: top = h
    assert(ShellTaskbarAt(one, tb, 1, 50, 1800).output == 0);
    assert(ShellDesktopHeight(50, tb) == 0 && ShellTaskbarTop(1800, tb, false) == 1728);

    // Two outputs, each with its own taskbar and work area.
    OutputLayout two = LayoutOf({{0, 0, 3840, 2160}, {3840, 0, 3840, 1080}});
    assert(ShellWorkAreaBase(two.rect(0), tb) == (OutputRect{0, 0, 3840, 2088}));
    assert(ShellWorkAreaBase(two.rect(1), tb) == (OutputRect{3840, 0, 3840, 1008}));
    assert(ShellTaskbarRect(two.rect(1), tb) == (OutputRect{3840, 1008, 3840, 72}));
    // A press on the second output's taskbar lands there, in its local
    // coordinates (the launcher button sits at its local left edge).
    auto hit = ShellTaskbarAt(two, tb, 0, 3840 + 20, 1050);
    assert(hit.output == 1 && hit.x == 20 && hit.y == 1050);
    hit = ShellTaskbarAt(two, tb, 0, 100, 2100);
    assert(hit.output == 0 && hit.x == 100 && hit.y == 2100);
    // Output 0's band row 1050 is desktop, not taskbar, and so is the
    // second output's band when a fullscreen window hides its taskbar.
    assert(ShellTaskbarAt(two, tb, 0, 100, 1050).output == -1);
    assert(ShellTaskbarAt(two, tb, 2u, 3860, 1050).output == -1);
    assert(ShellTaskbarAt(two, tb, 2u, 100, 2100).output == 0);
    assert(ShellOutputAt(two, 5000, 500, 0) == 1 && ShellOutputAt(two, 5000, 1500, 0) == 0);
}

} // namespace

int main()
{
    TestRegistry();
    TestUnion();
    TestOutputForRect();
    TestDamage();
    TestPointerClamp();
    TestShellLayout();
    puts("test_display_output: ok");
    return 0;
}
