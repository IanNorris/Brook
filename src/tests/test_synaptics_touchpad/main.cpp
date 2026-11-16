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
    for (int i = 0; i < 3; ++i) s.Feed(P(3000, 2000, 70, 0));   // touch-down + stability
    int ticks = 0, moved = 0;
    for (int i = 1; i <= 10; ++i) { auto r = s.Feed(P(3000, 2000 + 30 * i, 70, 0)); ticks += r.scrollY; moved += r.dx + r.dy; }
    CHECK(ticks == 3 && moved == 0);          // 300 units up / 100 per tick
    for (int i = 1; i <= 5; ++i) ticks += s.Feed(P(3000 + 40 * i, 2300, 70, 0)).scrollY;
    CHECK(ticks == 3);
    // Natural scrolling flips the direction.
    SynapticsGestureConfig nc; nc.naturalScroll = true; SynapticsGesture n(nc);
    for (int i = 0; i < 3; ++i) n.Feed(P(3000, 2000, 70, 0)); int nt = 0;
    for (int i = 1; i <= 4; ++i) nt += n.Feed(P(3000, 2000 + 50 * i, 70, 0)).scrollY;
    CHECK(nt == -2);
    // Horizontal scroll.
    SynapticsGesture hz; for (int i = 0; i < 3; ++i) hz.Feed(P(3000, 2000, 70, 0)); int hx = 0;
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
        // After release, two fingers scroll again (after settling).
        for (int i = 0; i < 3; ++i) d.Feed(P(3000, 1800, 70, 0));
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

    // Phantom-scroll guards.
    {
        // Lift-off: Z drops to 0 with W still 0 (no finger); no scroll even if x/y wander.
        SynapticsGesture l; for (int i = 0; i < 3; ++i) l.Feed(P(3000, 2000, 70, 0));
        int t = 0; for (int i = 1; i <= 4; ++i) t += l.Feed(P(3000, 2000 + 40 * i, 70, 0)).scrollY;
        CHECK(t > 0);
        int ph = 0;
        for (int i = 1; i <= 20; ++i) ph += l.Feed(P(3000, 2200 + 60 * i, 0, 0)).scrollY;        // Z=0 noise
        for (int i = 1; i <= 20; ++i) ph += l.Feed(P(3000, 3500 - 60 * i, 20, 0)).scrollY;       // just under threshold
        CHECK(ph == 0);
        // A brief two-finger blip (fewer than 3 stable packets) never scrolls.
        for (int k = 0; k < 10; ++k) {
            ph += l.Feed(P(3000, 2000 + 90 * k, 40, 0)).scrollY;
            ph += l.Feed(P(3000, 2090 + 90 * k, 40, 0)).scrollY;
            ph += l.Feed(P(3000, 2000, 0, 0)).scrollY;
        }
        CHECK(ph == 0);
        // Implausible jumps during a stable two-finger contact are dropped.
        SynapticsGesture jg; for (int i = 0; i < 4; ++i) jg.Feed(P(3000, 2000, 70, 0));
        CHECK(jg.Feed(P(3000, 2600, 70, 0)).scrollY == 0 && jg.scrollRejected == 1);
        // Resync reset: state is forgotten; a garbage packet after it can't scroll.
        SynapticsGesture rs; for (int i = 0; i < 4; ++i) rs.Feed(P(3000, 2000, 70, 0));
        rs.Reset();
        CHECK(rs.Feed(P(3000, 2200, 70, 0)).scrollY == 0);
        CHECK(rs.Feed(P(3000, 2240, 70, 0)).scrollY == 0);
        CHECK(rs.resets >= 1);
        // Low-pressure ticks are counted (diagnostic for the Dell report).
        SynapticsGesture lz; for (int i = 0; i < 3; ++i) lz.Feed(P(3000, 2000, 30, 0));
        int lt = 0; for (int i = 1; i <= 4; ++i) lt += lz.Feed(P(3000, 2000 + 50 * i, 30, 0)).scrollY;
        CHECK(lt == 2 && lz.scrollTicksLowZ == 2);
    }
    // Coordinate range queries (0x0d max, 0x0f min) and button areas.
    {
        SynapticsCaps rc; rc.caps = 0xf00123u; rc.ext0c = 0x12e800u;   // extRequests 7, max/min/clickpad/image sensor bits
        CHECK(rc.extRequests() == 7 && rc.maxCoords() && rc.minCoords() && rc.clickpad() && rc.agm());
        rc.caps = 0xd00123u; CHECK(rc.maxCoords() && !rc.minCoords());  // 5 requests: max only
        rc.ext0c = 0; CHECK(!rc.maxCoords());
        // x = r0<<5 | (r1&0x0f)<<1, y = r2<<5 | (r1&0xf0)>>3
        const uint8_t mx[3] = {0xb1, 0x5a, 0x94};   // x 5684, y 4746
        const uint8_t mn[3] = {0x27, 0x36, 0x22};   // x 1260, y 1094
        int32_t x = 0, y = 0; SynapticsDecodeCoords(mx, x, y); CHECK(x == 5684 && y == 4746);
        auto rg = SynapticsDecodeRange(mx, mn);
        CHECK(rg.xMin == 1260 && rg.yMin == 1094 && rg.xMax == 5684 && rg.yMax == 4746 && rg.reportedMax && rg.reportedMin);
        auto nominal = SynapticsDecodeRange(nullptr, nullptr);
        CHECK(nominal.xMin == 1472 && nominal.xMax == 5472 && nominal.yMin == 1408 && nominal.yMax == 4448 && !nominal.reportedMax);
        const uint8_t zero[3] = {0, 0, 0};
        CHECK(SynapticsDecodeRange(zero, nullptr).xMax == 5472);          // implausible: nominal
        auto maxOnly = SynapticsDecodeRange(mx, nullptr); CHECK(maxOnly.xMin == 1472 && maxOnly.xMax == 5684);
        SynapticsGestureConfig bc; SynapticsSetButtonAreas(bc, rg, true);
        CHECK(bc.softButtons && bc.buttonTopY == 1094 + (4746 - 1094) * 15 / 100 && bc.buttonSplitX == (1260 + 5684) / 2);
        SynapticsSetButtonAreas(bc, rg, false); CHECK(!bc.softButtons);
        SynapticsGestureConfig dc;   // defaults equal the nominal range
        SynapticsGestureConfig nc2; SynapticsSetButtonAreas(nc2, nominal, true);
        CHECK(dc.buttonTopY == nc2.buttonTopY && dc.buttonSplitX == nc2.buttonSplitX);
    }

    // Clickpad software buttons (nominal range: strip y < 1864, split x 3472).
    // Packet shapes follow the 9343 pad: a light touch, then the press packet
    // with higher pressure and a wider contact, held, then release.
    {
        auto press = [](SynapticsGesture& gg, int x, int y, int z = 110, int w = 8) {
            gg.Feed(P(x, y, 50, 6)); gg.Feed(P(x, y, 60, 6));
            return gg.Feed(P(x, y, z, w, true)).buttons;
        };
        // Hard click in the bottom-right area: right, held, then released.
        SynapticsGesture a;
        CHECK(press(a, 4800, 1600) == 2);
        for (int i = 0; i < 5; ++i) CHECK(a.Feed(P(4800, 1600, 120, 8, true)).buttons == 2);
        CHECK(a.Feed(P(4800, 1600, 60, 6)).buttons == 0);
        CHECK(a.clicksRight == 1 && a.clicksRightArea == 1 && a.clicksLeft == 0);
        // Bottom-left area: left. Centre of the pad: left. Right half above the strip: left.
        { SynapticsGesture b; CHECK(press(b, 2000, 1600) == 1); CHECK(b.clicksLeft == 1); }
        { SynapticsGesture b; CHECK(press(b, 3400, 3000) == 1); }
        { SynapticsGesture b; CHECK(press(b, 4800, 2400) == 1); }
        // Right at the bottom edge (Synaptics reports y below yMin there).
        { SynapticsGesture b; CHECK(press(b, 5300, 1200) == 2); }
        // A forceful press that trips the palm guards (W >= 12, Z >= 200)
        // still chooses by position.
        { SynapticsGesture b; CHECK(press(b, 4800, 1600, 230, 13) == 2); }
        { SynapticsGesture b; CHECK(press(b, 2000, 1600, 230, 13) == 1); }

        // Button held while the finger drifts out of its area.
        SynapticsGesture d;
        CHECK(press(d, 4800, 1600) == 2);
        for (int i = 1; i <= 30; ++i) CHECK(d.Feed(P(4800 - 60 * i, 1600 + 50 * i, 110, 8, true)).buttons == 2);
        CHECK(d.Feed(P(3000, 3100, 60, 6)).buttons == 0);
        SynapticsGesture dl;
        CHECK(press(dl, 2000, 1600) == 1);
        int dragX = 0;
        for (int i = 1; i <= 30; ++i) { auto r = dl.Feed(P(2000 + 100 * i, 1600, 110, 8, true)); dragX += r.dx; CHECK(r.buttons == 1); }
        CHECK(dragX > 0);                                         // left-area press still drags
        CHECK(dl.Feed(P(5000, 1600, 60, 6)).buttons == 0);

        // Press packet with Z=0 (no contact): the last contact decides.
        SynapticsGesture z0;
        z0.Feed(P(4800, 1600, 60, 6)); z0.Feed(P(4800, 1600, 70, 6));
        CHECK(z0.Feed(P(0, 0, 0, 0, true)).buttons == 2);
        CHECK(z0.Feed(P(4800, 1600, 120, 8, true)).buttons == 2);
        CHECK(z0.Feed(P(4800, 1600, 60, 6)).buttons == 0);
        CHECK(z0.clicksStalePos == 1 && z0.clicksRightArea == 1);
        SynapticsGesture z1;
        z1.Feed(P(2000, 1600, 60, 6));
        CHECK(z1.Feed(P(0, 0, 0, 0, true)).buttons == 1);
        // No contact seen at all: left.
        SynapticsGesture z2;
        CHECK(z2.Feed(P(0, 0, 0, 0, true)).buttons == 1 && z2.clicksNoPos == 1);

        // Ian's case: a finger resting in the left area, another finger
        // presses in the right area. Either finger may be the one the
        // primary (W=0) packet reports; the other arrives in AGM packets.
        auto rest = [&](SynapticsGesture& gg, int sx, int sy, int ax, int ay) {
            gg.Feed(P(2000, 1500, 60, 6)); gg.Feed(P(2000, 1500, 60, 6));   // thumb alone
            for (int i = 0; i < 3; ++i) { gg.Feed(P(sx, sy, 70, 0)); gg.Feed(AGM(ax, ay, 60)); }
            return gg.Feed(P(sx, sy, 140, 0, true)).buttons;
        };
        { SynapticsGesture t1; CHECK(rest(t1, 2000, 1500, 4800, 1600) == 2); CHECK(t1.clicksRightArea == 1); }
        { SynapticsGesture t2; CHECK(rest(t2, 4800, 1600, 2000, 1500) == 2); }
        // Thumb resting in the left area, click with another finger on the
        // main surface: the thumb's area decides (left), not a two-finger click.
        { SynapticsGesture t3; CHECK(rest(t3, 2000, 1500, 3400, 3000) == 1); CHECK(t3.clicksThumbLeft == 1); }
        { SynapticsGesture t4; CHECK(rest(t4, 3400, 3000, 2000, 1500) == 1); }
        // Second finger landed but no AGM position yet: two-finger click (right),
        // as before (this is what the old code did for every two-finger press).
        {
            SynapticsGesture t5; t5.Feed(P(2000, 1500, 60, 6)); t5.Feed(P(2000, 1500, 60, 6));
            t5.Feed(P(2000, 1500, 70, 0));
            CHECK(t5.Feed(P(2000, 1500, 140, 0, true)).buttons == 2 && t5.clicksTwoFinger == 1);
        }
        // Thumb resting in the right area, click elsewhere: right.
        { SynapticsGesture t6; t6.Feed(P(4800, 1500, 60, 6)); t6.Feed(P(4800, 1500, 60, 6));
          for (int i = 0; i < 3; ++i) { t6.Feed(P(4800, 1500, 70, 0)); t6.Feed(AGM(3000, 3000, 60)); }
          CHECK(t6.Feed(P(4800, 1500, 140, 0, true)).buttons == 2); }

        // Two-finger click on the main surface: right; no scroll or motion while held.
        {
            SynapticsGesture tf;
            for (int i = 0; i < 3; ++i) { tf.Feed(P(3000, 3000, 70, 0)); tf.Feed(AGM(3600, 3000, 60)); }
            CHECK(tf.Feed(P(3000, 3000, 90, 0, true)).buttons == 2 && tf.clicksTwoFinger == 1);
            CHECK(!tf.Dragging());
            int sc = 0, mv = 0;
            for (int i = 1; i <= 5; ++i) { auto r = tf.Feed(P(3000, 3000 + 50 * i, 90, 0, true)); sc += r.scrollY; mv += Abs2(r); CHECK(r.buttons == 2); }
            CHECK(sc == 0 && mv == 0);
            CHECK(tf.Feed(P(3000, 3250, 60, 0)).buttons == 0);
        }
        // Right-area press held by one finger while another moves: a right drag.
        {
            SynapticsGesture rd;
            CHECK(press(rd, 4800, 1600) == 2);
            CHECK(rd.Dragging());
            rd.Feed(P(4800, 1600, 110, 0, true));
            rd.Feed(AGM(3000, 3000, 60));
            int mx = 0, sy = 0;
            for (int i = 1; i <= 10; ++i) { auto r = rd.Feed(AGM(3000 + 30 * i, 3000, 60)); mx += r.dx; sy += r.scrollY; CHECK(r.buttons == 2); }
            CHECK(mx > 0 && sy == 0);
        }
        // Two-finger scroll with a contact near (but above) the strip is unchanged.
        {
            SynapticsGesture sc; for (int i = 0; i < 3; ++i) sc.Feed(P(3000, 2000, 70, 0));
            int t = 0; for (int i = 1; i <= 4; ++i) t += sc.Feed(P(3000, 2000 + 50 * i, 70, 0)).scrollY;
            CHECK(t == 2);
        }
        // Software buttons off (not a clickpad): position does not matter.
        {
            SynapticsGestureConfig off; off.softButtons = false; SynapticsGesture nb(off);
            CHECK(press(nb, 4800, 1600) == 1);
            nb.Feed(P(4800, 1600, 60, 6));
            nb.Feed(P(3000, 3000, 70, 0));
            CHECK(nb.Feed(P(3000, 3000, 70, 0, true)).buttons == 2);
        }
        // A resync reset forgets the last contact position.
        {
            SynapticsGesture rs; rs.Feed(P(4800, 1600, 60, 6)); rs.Reset();
            CHECK(rs.Feed(P(0, 0, 0, 0, true)).buttons == 1 && rs.clicksNoPos == 1);
        }
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
