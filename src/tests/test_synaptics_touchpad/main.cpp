// Host tests for the Synaptics PS/2 decoder and gesture state machine.
#include "synaptics_touchpad.h"
#include <cstdio>
#include <cstdlib>
using namespace brook;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// Encode an absolute packet the way the pad does (inverse of the decoder).
static void Encode(int x, int y, int z, int w, bool left, bool clickpad, uint8_t b[6]) {
    b[0] = 0x80 | ((w & 0x0c) << 2) | ((w & 0x02) << 1);
    b[1] = uint8_t(((y >> 8) & 0x0f) << 4 | ((x >> 8) & 0x0f));
    b[2] = uint8_t(z);
    b[3] = 0xC0 | ((x >> 12) & 1) << 4 | ((y >> 12) & 1) << 5 | ((w & 1) << 2);
    b[4] = uint8_t(x);
    b[5] = uint8_t(y);
    if (left) { if (clickpad) b[3] |= 0x01; else b[0] |= 0x01; }
}
static SynapticsPacket P(int x, int y, int z, int w, bool left = false) {
    uint8_t b[6]; Encode(x, y, z, w, left, true, b);
    CHECK(SynapticsByteValid(0, b[0]) && SynapticsByteValid(3, b[3]));
    return SynapticsDecodePacket(b, true);
}

int main() {
    // Identify / caps decoding (values in the shape the 06CB:76AD part returns).
    const uint8_t id[3] = {0x01, 0x47, 0x18};
    auto ident = SynapticsDecodeIdentify(id);
    CHECK(ident.valid && ident.major == 8 && ident.minor == 1 && ident.model == 1);
    const uint8_t notSyn[3] = {0x00, 0x03, 0x64};   // plain PS/2 status reply
    CHECK(!SynapticsDecodeIdentify(notSyn).valid);
    const uint8_t cap[3] = {0xd0, 0x47, 0x23};
    auto caps = SynapticsDecodeCaps(cap);
    CHECK(caps.valid && caps.extended() && caps.extRequests() == 5 && caps.multiFinger());
    caps.ext0c = 1u << 20;
    CHECK(caps.clickpad());
    CHECK(SynapticsModeByte(caps) == (SynModeAbsolute | SynModeHighRate | SynModeDisableGesture | SynModeW));
    uint8_t sl[4]; SynapticsSlice(0xC5, sl);
    CHECK(sl[0] == 3 && sl[1] == 0 && sl[2] == 1 && sl[3] == 1);

    // Packet round trip, including 13-bit coordinates and every W value.
    for (int w = 0; w < 16; ++w) {
        auto p = P(5471, 4447, 60, w, w == 5);
        CHECK(p.x == 5471 && p.y == 4447 && p.z == 60 && p.w == w && p.left == (w == 5));
    }
    CHECK(!SynapticsByteValid(0, 0x08) && !SynapticsByteValid(3, 0x80));
    { uint8_t b[6]; Encode(3000, 2000, 50, 4, true, false, b); auto p = SynapticsDecodePacket(b, false); CHECK(p.left && !p.right); }

    // One finger: first touch anchors (no jump), then motion with y inverted.
    SynapticsGesture g;
    auto o = g.Feed(P(3000, 3000, 60, 6)); CHECK(o.dx == 0 && o.dy == 0);
    int sumX = 0, sumY = 0;
    for (int i = 1; i <= 20; ++i) { o = g.Feed(P(3000 + 10 * i, 3000 + 10 * i, 60, 6)); sumX += o.dx; sumY += o.dy; }
    CHECK(sumX > 0 && sumY < 0);              // pad up = screen up (dy negative)
    CHECK(sumX >= 200 * 42 / 256 - 1);        // fractional accumulation keeps slow motion
    // Faster motion gets more gain per unit.
    SynapticsGesture f; f.Feed(P(2000, 2000, 60, 6)); int fast = 0;
    for (int i = 1; i <= 2; ++i) fast += f.Feed(P(2000 + 100 * i, 2000, 60, 6)).dx;
    CHECK(fast * 1 > sumX);                    // 200 units fast > 200 units slow
    // Lift then touch elsewhere: no jump.
    g.Feed(P(0, 0, 0, 0)); o = g.Feed(P(5000, 1500, 60, 6)); CHECK(o.dx == 0 && o.dy == 0);
    // Large jump within a touch is ignored.
    o = g.Feed(P(5000 + 2000, 1500, 60, 6)); CHECK(o.dx == 0);

    // Two fingers: vertical scroll with fractional accumulation, no pointer motion.
    SynapticsGesture s;
    s.Feed(P(3000, 2000, 70, 0));
    int ticks = 0, moved = 0;
    for (int i = 1; i <= 10; ++i) { auto r = s.Feed(P(3000, 2000 + 30 * i, 70, 0)); ticks += r.scrollY; moved += r.dx + r.dy; }
    CHECK(ticks == 3 && moved == 0);          // 300 units up / 100 per tick
    for (int i = 1; i <= 5; ++i) ticks += s.Feed(P(3000 + 40 * i, 2300, 70, 0)).scrollY;
    CHECK(ticks == 3);
    // Natural scrolling flips the direction.
    SynapticsGestureConfig nc; nc.naturalScroll = true; SynapticsGesture n(nc);
    n.Feed(P(3000, 2000, 70, 0)); int nt = 0;
    for (int i = 1; i <= 4; ++i) nt += n.Feed(P(3000, 2000 + 50 * i, 70, 0)).scrollY;
    CHECK(nt == -2);
    // Horizontal scroll.
    SynapticsGesture hz; hz.Feed(P(3000, 2000, 70, 0)); int hx = 0;
    for (int i = 1; i <= 4; ++i) hx += hz.Feed(P(3000 + 50 * i, 2000, 70, 0)).scrollX;
    CHECK(hx == 2);
    // Switching 1 -> 2 fingers re-anchors, so the second finger landing is not a scroll.
    SynapticsGesture t; t.Feed(P(3000, 2000, 60, 6)); t.Feed(P(3010, 2010, 60, 6));
    auto r = t.Feed(P(3600, 2600, 70, 0)); CHECK(r.scrollY == 0 && r.dx == 0);

    // Clickpad: click with one finger = left, with two fingers resting = right.
    SynapticsGesture c;
    c.Feed(P(3000, 2000, 60, 6));
    CHECK(c.Feed(P(3000, 2000, 60, 6, true)).buttons == 1);
    CHECK(c.Feed(P(3000, 2000, 60, 6, false)).buttons == 0);
    c.Feed(P(3000, 2000, 70, 0));
    CHECK(c.Feed(P(3000, 2000, 70, 0, true)).buttons == 2);
    CHECK(c.Feed(P(3000, 2000, 60, 6, true)).buttons == 2);  // stays right until release
    CHECK(c.Feed(P(3000, 2000, 60, 6, false)).buttons == 0);

    // Palms and light touches produce nothing; extended packets are ignored.
    SynapticsGesture pm; pm.Feed(P(3000, 2000, 60, 14));
    CHECK(pm.Feed(P(3300, 2300, 60, 14)).dx == 0);
    CHECK(pm.Fingers(P(3000, 2000, 10, 6)) == 0 && pm.Fingers(P(3000, 2000, 250, 6)) == 0);
    CHECK(pm.Feed(P(3000, 2000, 60, 2)).dx == 0);
    CHECK(pm.fingerHistogram[0] >= 1);

    if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
    std::printf("test_synaptics_touchpad: all passed\n");
    return 0;
}
