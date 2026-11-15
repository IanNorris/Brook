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
static int Abs2(const SynapticsOutput& o) { return (o.dx < 0 ? -o.dx : o.dx) + (o.dy < 0 ? -o.dy : o.dy); }
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
    auto caps = SynapticsDecodeCaps(cap, ident);
    CHECK(caps.valid && caps.extended() && caps.extRequests() == 5 && caps.multiFinger());
    // Firmware >= 7.5: the middle byte is a submodel, not the 0x47 magic.
    const uint8_t capNew[3] = {0xd0, 0x01, 0x23};
    auto cn = SynapticsDecodeCaps(capNew, ident);
    CHECK(cn.valid && cn.extended() && cn.caps == 0xd00123u);
    SynapticsIdentity old; old.valid = true; old.major = 6; old.minor = 2;
    CHECK(!SynapticsDecodeCaps(capNew, old).valid && SynapticsDecodeCaps(cap, old).valid);
    // No capExtended: flags ignored (absolute mode without W).
    const uint8_t capNoExt[3] = {0x00, 0x01, 0x23};
    auto ne = SynapticsDecodeCaps(capNoExt, ident);
    CHECK(ne.valid && ne.caps == 0 && !ne.extended() && !(SynapticsModeByte(ne) & SynModeW));
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
    CHECK(sumX >= 200 * 42 * 80 / 100 / 256 - 1);   // fractional accumulation keeps slow motion (80% base)
    CHECK(sumX <= 200 * 42 / 256);                    // ~20% slower than the original tuning
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


    // Pointer speed setting scales motion; default 100 = the 80% base tuning.
    {
        auto run = [](int speed) {
            SynapticsGestureConfig sc; sc.speedPercent = speed; SynapticsGesture sg(sc);
            sg.Feed(P(2000, 2000, 60, 6)); int sum = 0;
            for (int i = 1; i <= 40; ++i) sum += sg.Feed(P(2000 + 10 * i, 2000, 60, 6)).dx;
            return sum;
        };
        const int s100 = run(100), s200 = run(200), s50 = run(50);
        CHECK(s200 >= 2 * s100 - 2 && s200 <= 2 * s100 + 2);
        CHECK(s50 >= s100 / 2 - 2 && s50 <= s100 / 2 + 2);
        CHECK(run(10) == run(25));                    // clamped to the minimum
    }

    // AGM packet decode (W=2, type 1): second finger, half resolution scaled up.
    {
        uint8_t b[6] = {0x80 | 0x04, 0x34, 0x56, 0xC0 | 0x10, 0x87, 0x10 | 0x05};
        // w: ((b0&0x30)>>2)|((b0&0x04)>>1)|((b3&0x04)>>2) = 2
        auto p = SynapticsDecodePacket(b, true);
        CHECK(p.w == 2 && p.agm);
        CHECK(p.agmX == (((0x87 & 0x0f) << 8 | 0x34) << 1) && p.agmY == (((0x87 & 0xf0) << 4 | 0x56) << 1));
        CHECK(p.agmZ == (((0xC0 | 0x10) & 0x30) | 0x05) << 1);
    }
    auto AGM = [](int x, int y, int z) {
        SynapticsPacket p; p.w = 2; p.agm = true; p.agmX = x; p.agmY = y; p.agmZ = z; return p;
    };

    // Click-drag with a second finger: press with one finger (left), add a
    // second finger; motion follows whichever finger moves, never scrolls.
    {
        SynapticsGesture d;
        d.Feed(P(3000, 1500, 60, 6));
        CHECK(d.Feed(P(3000, 1500, 60, 6, true)).buttons == 1);   // left press
        d.Feed(P(3000, 1500, 70, 0, true));                       // second finger lands (re-anchor)
        int mx = 0, sy = 0, sx = 0;
        // Primary (holding) finger resting: tiny jitter only.
        for (int i = 1; i <= 5; ++i) { auto r = d.Feed(P(3000 + (i & 1), 1500, 70, 0, true)); mx += r.dx; sy += r.scrollY; sx += r.scrollX; CHECK(r.buttons == 1); }
        CHECK(mx == 0 && sy == 0 && sx == 0);
        // The moving finger arrives in AGM packets.
        d.Feed(AGM(4000, 3000, 60));
        for (int i = 1; i <= 10; ++i) { auto r = d.Feed(AGM(4000 + 30 * i, 3000 + 30 * i, 60)); mx += r.dx; sy += r.scrollY; CHECK(r.buttons == 1); }
        CHECK(mx > 0 && sy == 0);
        // Without AGM, motion reported in the W=0 packets also drags.
        int my = 0;
        for (int i = 1; i <= 10; ++i) { auto r = d.Feed(P(3000, 1500 + 30 * i, 70, 0, true)); my += r.dy; sy += r.scrollY; }
        CHECK(my < 0 && sy == 0);                                  // pad up = screen up
        CHECK(d.Feed(P(3000, 1800, 70, 0, false)).buttons == 0);   // release
        // After release, two fingers scroll again.
        int st = 0; for (int i = 1; i <= 6; ++i) st += d.Feed(P(3000, 1800 + 50 * i, 70, 0)).scrollY;
        CHECK(st == 3);
    }
    // A press that starts with two fingers resting is still a right click and
    // does not drag or scroll while held.
    {
        SynapticsGesture rc; rc.Feed(P(3000, 2000, 70, 0)); rc.Feed(P(3000, 2000, 70, 0));
        CHECK(rc.Feed(P(3000, 2000, 70, 0, true)).buttons == 2);
        int sc = 0; for (int i = 1; i <= 5; ++i) sc += rc.Feed(P(3000, 2000 + 50 * i, 70, 0, true)).scrollY;
        CHECK(sc == 0);
    }

    // Click jitter: pressing and releasing with small centroid and pressure
    // shifts produces no motion; a deliberate drag past the dead zone moves.
    {
        SynapticsGesture j;
        j.Feed(P(3000, 2000, 50, 6)); j.Feed(P(3000, 2000, 50, 6));
        int m = 0;
        // Press: pressure rises and the centroid shifts by up to 25 units.
        m += Abs2(j.Feed(P(3010, 1990, 90, 6, true)));
        for (int i = 0; i < 12; ++i) m += Abs2(j.Feed(P(3010 + (i % 3) * 5, 1985 - (i % 2) * 5, 95, 6, true)));
        CHECK(m == 0);
        // Still held, jitter within the dead zone after the window: no motion.
        for (int i = 0; i < 6; ++i) m += Abs2(j.Feed(P(3020 + (i & 1) * 10, 1980, 95, 6, true)));
        CHECK(m == 0);
        // Deliberate drag past 40 units: motion resumes, button stays down.
        int dx = 0;
        for (int i = 1; i <= 10; ++i) { auto r = j.Feed(P(3020 + 20 * i, 1980, 95, 6, true)); dx += r.dx; CHECK(r.buttons == 1); }
        CHECK(dx > 0);
        // Release with a shift: no motion during the window.
        m = 0;
        m += Abs2(j.Feed(P(3240, 1990, 60, 6, false)));
        for (int i = 0; i < 9; ++i) m += Abs2(j.Feed(P(3245, 1995, 55, 6)));
        CHECK(m == 0);
        // Afterwards ordinary motion works again.
        int after = 0; for (int i = 1; i <= 5; ++i) after += j.Feed(P(3245 + 30 * i, 1995, 55, 6)).dx;
        CHECK(after > 0);
        // A pressure step alone (no button) is not motion.
        SynapticsGesture z; z.Feed(P(3000, 2000, 40, 6)); z.Feed(P(3000, 2000, 40, 6));
        CHECK(Abs2(z.Feed(P(3020, 1980, 90, 6))) == 0);
    }
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
