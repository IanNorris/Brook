// Host tests for Synaptics tap-to-click (synaptics_tap.h), driven through the
// gesture state machine the same way mouse.cpp feeds IRQ12 packets.
#include "synaptics_tap.h"
#include "../../shared/brook_pointer.h"
#include <cstdio>
#include <vector>
using namespace brook;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// Encode an absolute clickpad packet (inverse of SynapticsDecodePacket).
static SynapticsPacket P(int x, int y, int z, int w, bool left = false) {
    uint8_t b[6];
    b[0] = 0x80 | ((w & 0x0c) << 2) | ((w & 0x02) << 1);
    b[1] = uint8_t(((y >> 8) & 0x0f) << 4 | ((x >> 8) & 0x0f));
    b[2] = uint8_t(z);
    b[3] = 0xC0 | ((x >> 12) & 1) << 4 | ((y >> 12) & 1) << 5 | ((w & 1) << 2);
    b[4] = uint8_t(x);
    b[5] = uint8_t(y);
    if (left) b[3] |= 0x01;
    return SynapticsDecodePacket(b, true);
}

// Mirrors SynapticsByte / MouseTouchpadTapPoll in mouse.cpp: tap steps are
// reported on top of the previous physical buttons, then the packet's own
// report carries physical | held tap buttons. `log` records every reported
// button-state change.
struct Pad {
    SynapticsGesture g;
    SynapticsTap t;
    uint8_t phys = 0, buttons = 0;
    uint64_t now = 1000;
    int motion = 0, scroll = 0;
    std::vector<int> log;
    explicit Pad(bool enabled = true, bool drag = true) {
        SynapticsTapConfig c; c.enabled = enabled; c.drag = drag;
        t.SetConfig(c);
    }
    void Report(uint8_t b) { if (b != buttons) { log.push_back(b); buttons = b; } }
    void Steps(const SynapticsTapOutput& s) { for (int i = 0; i < s.steps; ++i) Report(uint8_t(phys | s.mask[i])); }
    void Packet(int x, int y, int z, int w, bool left = false, int dtMs = 13) {
        Idle(dtMs);
        const SynapticsPacket p = P(x, y, z, w, left);
        const SynapticsOutput o = g.Feed(p);
        if (p.w != 2) {
            Steps(t.Feed(SynapticsTapInputFrom(p, g, now)));
            g.SetTapDrag(t.Dragging());
        }
        phys = o.buttons;
        motion += (o.dx < 0 ? -o.dx : o.dx) + (o.dy < 0 ? -o.dy : o.dy);
        scroll += (o.scrollY < 0 ? -o.scrollY : o.scrollY) + (o.scrollX < 0 ? -o.scrollX : o.scrollX);
        Report(uint8_t(o.buttons | t.Held()));
    }
    // Time passing without packets: the LAPIC timer poll.
    void Idle(int ms) {
        for (int i = 0; i < ms; ++i) {
            ++now;
            if (t.Deadline() && now >= t.Deadline()) Steps(t.Timeout(now));
        }
    }
    // One finger resting at (x,y) for n packets with sub-threshold jitter, then lift.
    void Tap(int n = 6, int x = 3000, int y = 2000, int z = 60) {
        for (int i = 0; i < n; ++i) Packet(x + (i & 1) * 6, y - (i & 1) * 4, z, 6);
        Packet(x, y, 0, 0);
    }
    bool Log(std::vector<int> want) const { return log == want; }
};

int main() {
    // Default configuration: tapping is off.
    CHECK(!SynapticsTapConfig{}.enabled);

    // 1. One-finger tap = left click. With tap-and-drag the press is reported
    //    at lift and released when the drag window closes.
    {
        Pad pad;
        pad.Tap();
        CHECK(pad.Log({1}));
        CHECK(pad.t.Deadline() != 0);
        pad.Idle(240);
        CHECK(pad.Log({1}));
        pad.Idle(20);
        CHECK(pad.Log({1, 0}));
        CHECK(pad.t.Deadline() == 0 && pad.t.tapsLeft == 1);
        // Z=0 packets after the deadline also release (no timer needed).
        Pad z; z.Tap(); for (int i = 0; i < 25; ++i) z.Packet(3000, 2000, 0, 0);
        CHECK(z.Log({1, 0}));
        // Without tap-and-drag the click is immediate.
        Pad nd(true, false); nd.Tap();
        CHECK(nd.Log({1, 0}) && nd.t.Deadline() == 0);
        // A second tap after the window is a separate click.
        pad.Tap(); pad.Idle(300);
        CHECK(pad.Log({1, 0, 1, 0}) && pad.t.tapsLeft == 2);
    }

    // 2. Two-finger tap = right click (fingers landing and lifting a packet apart).
    {
        Pad pad;
        pad.Packet(3000, 2000, 60, 6);
        for (int i = 0; i < 5; ++i) pad.Packet(3000, 2000, 70, 0);
        pad.Packet(3000, 2000, 60, 6);
        pad.Packet(3000, 2000, 0, 0);
        CHECK(pad.Log({2, 0}) && pad.t.tapsRight == 1 && pad.t.Deadline() == 0);
        CHECK(pad.scroll == 0);
        // Two-finger scroll is not a tap.
        Pad s;
        for (int i = 0; i < 3; ++i) s.Packet(3000, 2000, 70, 0);
        for (int i = 1; i <= 8; ++i) s.Packet(3000, 2000 + 40 * i, 70, 0);
        s.Packet(3000, 2320, 0, 0);
        CHECK(s.log.empty() && s.scroll > 0);
        // A two-finger blip shorter than the stability guard is not a click of either kind.
        Pad b;
        b.Packet(3000, 2000, 60, 6); b.Packet(3000, 2000, 70, 0); b.Packet(3000, 2000, 70, 0);
        b.Packet(3000, 2000, 60, 6); b.Packet(3000, 2000, 0, 0);
        CHECK(b.log.empty() && b.t.rejectedPhantom == 1);
    }

    // 3. Moving is not a tap, even when quick.
    {
        Pad pad;
        for (int i = 0; i < 6; ++i) pad.Packet(3000 + 20 * i, 2000, 60, 6);
        pad.Packet(3100, 2000, 0, 0);
        CHECK(pad.log.empty() && pad.motion > 0 && pad.t.tapsLeft == 0);
        // Vertical travel likewise.
        Pad v;
        for (int i = 0; i < 4; ++i) v.Packet(3000, 2000 - 30 * i, 60, 6);
        v.Packet(3000, 1910, 0, 0);
        CHECK(v.log.empty());
    }

    // 4. A slow, still hold is not a tap.
    {
        Pad pad;
        pad.Tap(20);                       // 20 packets * 13 ms = 260 ms
        CHECK(pad.log.empty());
        Pad edge; edge.Tap(14);            // 182 ms: just over the limit
        CHECK(edge.log.empty());
        Pad ok; ok.Tap(13);                // 169 ms: a tap
        CHECK(ok.Log({1}));
    }

    // 5. Tap-and-drag: one press held from the tap through the drag, motion
    //    reported, released at lift. A quick second tap is a double click.
    {
        Pad pad;
        pad.Tap();
        pad.Idle(100);
        pad.Packet(3000, 2000, 60, 6);
        for (int i = 1; i <= 10; ++i) { pad.Packet(3000 + 30 * i, 2000, 60, 6); CHECK(pad.buttons == 1); }
        CHECK(pad.Log({1}) && pad.motion > 0 && pad.t.drags == 1);
        pad.Idle(1000);                    // drag survives a pause with the finger down (no packets)
        CHECK(pad.Log({1}));
        pad.Packet(3300, 2000, 0, 0);
        CHECK(pad.Log({1, 0}));
        // Holding still after the tap also drags (press stays until lift).
        Pad h; h.Tap(); h.Idle(50);
        for (int i = 0; i < 20; ++i) h.Packet(3000, 2000, 60, 6);
        CHECK(h.Log({1}) && h.t.drags == 1);
        h.Packet(3000, 2000, 0, 0);
        CHECK(h.Log({1, 0}));
        // Double tap.
        Pad d; d.Tap(); d.Idle(80); d.Tap();
        CHECK(d.Log({1, 0, 1, 0}) && d.t.doubleTaps == 1 && d.t.Deadline() == 0);
        // A second finger joining the drag keeps dragging: no scroll, button held.
        Pad two; two.Tap(); two.Idle(60);
        two.Packet(3000, 2000, 60, 6);
        for (int i = 1; i <= 5; ++i) two.Packet(3000 + 30 * i, 2000, 60, 6);
        for (int i = 1; i <= 10; ++i) two.Packet(3150, 2000 + 40 * i, 70, 0);
        CHECK(two.Log({1}) && two.scroll == 0);
        two.Packet(3150, 2400, 0, 0);
        CHECK(two.Log({1, 0}));
        // A touch after the drag window is ordinary motion.
        Pad late; late.Tap(); late.Idle(300);
        for (int i = 0; i < 10; ++i) late.Packet(3000 + 30 * i, 2000, 60, 6);
        late.Packet(3300, 2000, 0, 0);
        CHECK(late.Log({1, 0}) && late.t.drags == 0);
        // Tap followed by a two-finger tap: left click, then right click.
        Pad lr; lr.Tap(); lr.Idle(60);
        for (int i = 0; i < 5; ++i) lr.Packet(3000, 2000, 70, 0);
        lr.Packet(3000, 2000, 0, 0);
        CHECK(lr.Log({1, 0, 2, 0}));
    }

    // 6. Physical clicks: unchanged, and never also a tap.
    {
        Pad pad;
        pad.Packet(3000, 2000, 60, 6);
        pad.Packet(3000, 2000, 60, 6);
        pad.Packet(3000, 2000, 90, 6, true);    // press (pressure rises)
        pad.Packet(3000, 2000, 90, 6, true);
        pad.Packet(3000, 2000, 60, 6, false);   // release
        pad.Packet(3000, 2000, 0, 0);           // lift, well inside the tap time
        CHECK(pad.Log({1, 0}) && pad.t.tapsLeft == 0 && pad.t.rejectedPhysical == 1);
        pad.Idle(400);
        CHECK(pad.Log({1, 0}));
        // Same sequence with tapping off is identical.
        Pad off(false);
        off.Packet(3000, 2000, 60, 6); off.Packet(3000, 2000, 60, 6);
        off.Packet(3000, 2000, 90, 6, true); off.Packet(3000, 2000, 90, 6, true);
        off.Packet(3000, 2000, 60, 6, false); off.Packet(3000, 2000, 0, 0);
        CHECK(off.Log({1, 0}));
        // Two-finger physical click stays a right click, no tap.
        Pad r;
        r.Packet(3000, 2000, 70, 0); r.Packet(3000, 2000, 70, 0);
        r.Packet(3000, 2000, 90, 0, true); r.Packet(3000, 2000, 70, 0, false);
        r.Packet(3000, 2000, 0, 0);
        CHECK(r.Log({2, 0}) && r.t.tapsRight == 0);
        // A physical press during the tap's drag window: the tap button is
        // released first so the physical press is a distinct click.
        Pad w; w.Tap(); w.Idle(50);
        w.Packet(3000, 2000, 60, 6);
        w.Packet(3000, 2000, 90, 6, true);
        CHECK(w.Log({1, 0, 1}));
        w.Packet(3000, 2000, 60, 6, false);
        w.Packet(3000, 2000, 0, 0);
        w.Idle(400);
        CHECK(w.Log({1, 0, 1, 0}) && w.t.Deadline() == 0);
        // Physical click-drag still works with tapping on.
        Pad cd;
        cd.Packet(3000, 2000, 60, 6); cd.Packet(3000, 2000, 90, 6, true);
        for (int i = 1; i <= 20; ++i) cd.Packet(3000 + 20 * i, 2000, 90, 6, true);
        cd.Packet(3400, 2000, 60, 6, false); cd.Packet(3400, 2000, 0, 0);
        CHECK(cd.Log({1, 0}) && cd.motion > 0 && cd.t.drags == 0);
    }

    // 7. Setting off: taps do nothing; turning it off releases a held tap.
    {
        Pad off(false);
        off.Tap();
        off.Packet(3000, 2000, 60, 6);
        for (int i = 0; i < 5; ++i) off.Packet(3000, 2000, 70, 0);
        off.Packet(3000, 2000, 0, 0);
        off.Idle(400);
        CHECK(off.log.empty() && off.t.tapsLeft == 0 && off.t.tapsRight == 0);
        Pad held; held.Tap();
        CHECK(held.Log({1}));
        held.Steps(held.t.SetEnabled(false));
        CHECK(held.Log({1, 0}) && held.t.Deadline() == 0 && !held.t.Held());
        held.Tap(); held.Idle(400);
        CHECK(held.Log({1, 0}));
        // Turning it on with a finger already down: that contact cannot tap.
        Pad on(false);
        on.Packet(3000, 2000, 60, 6);
        on.Steps(on.t.SetEnabled(true));
        on.Packet(3000, 2000, 60, 6); on.Packet(3000, 2000, 0, 0);
        CHECK(on.log.empty());
        on.Tap(); CHECK(on.Log({1}));   // the next touch taps
        // Turning it on with nothing on the pad: the first touch taps.
        Pad first(false); first.Steps(first.t.SetEnabled(true)); first.Tap();
        CHECK(first.Log({1}));
    }

    // 8. Palms and phantom contacts never tap.
    {
        Pad pad;
        for (int i = 0; i < 5; ++i) pad.Packet(3000, 2000, 80, 14);      // wide: palm
        pad.Packet(3000, 2000, 0, 0);
        CHECK(pad.log.empty());
        Pad heavy;
        for (int i = 0; i < 5; ++i) heavy.Packet(3000, 2000, 220, 6);    // pressure: palm
        heavy.Packet(3000, 2000, 0, 0);
        CHECK(heavy.log.empty());
        // Starts like a finger, then the palm guard trips: no tap.
        Pad grow;
        grow.Packet(3000, 2000, 60, 6); grow.Packet(3000, 2000, 60, 8);
        grow.Packet(3000, 2000, 90, 13); grow.Packet(3000, 2000, 0, 0);
        CHECK(grow.log.empty() && grow.t.rejectedPalm == 1);
        // One-packet blip.
        Pad blip; blip.Packet(3000, 2000, 60, 6); blip.Packet(3000, 2000, 0, 0);
        CHECK(blip.log.empty() && blip.t.rejectedPhantom == 1);
        // Light contact just over the touch threshold (phantom range).
        Pad light; light.Tap(6, 3000, 2000, 30);
        CHECK(light.log.empty() && light.t.rejectedPhantom == 1);
        // Below the touch threshold: not even a contact.
        Pad none; none.Tap(6, 3000, 2000, 20);
        CHECK(none.log.empty() && none.t.rejectedPhantom == 0);
        // Three fingers: no tap.
        Pad three;
        for (int i = 0; i < 4; ++i) three.Packet(3000, 2000, 80, 1);
        three.Packet(3000, 2000, 0, 0);
        CHECK(three.log.empty());
    }

    // 9. Resync during a tap-drag releases the button and waits for lift.
    {
        Pad pad; pad.Tap(); pad.Idle(40);
        pad.Packet(3000, 2000, 60, 6);
        for (int i = 1; i <= 5; ++i) pad.Packet(3000 + 30 * i, 2000, 60, 6);
        CHECK(pad.Log({1}));
        pad.g.Reset(); pad.Steps(pad.t.Reset());
        CHECK(pad.Log({1, 0}));
        pad.Packet(3200, 2000, 60, 6); pad.Packet(3200, 2000, 0, 0);   // tail of the old contact
        CHECK(pad.Log({1, 0}));
        pad.Tap(); CHECK(pad.Log({1, 0, 1}));
    }

    // 10. Settings line parser.
    {
        CHECK(brook_touchpad_tap_setting(0, "touchpad_tap=1") == 1);
        CHECK(brook_touchpad_tap_setting(1, "touchpad_tap=0\r") == 0);
        CHECK(brook_touchpad_tap_setting(0, "touchpad_tap=2") == 0);
        CHECK(brook_touchpad_tap_setting(1, "touchpad_tap=") == 1);
        CHECK(brook_touchpad_tap_setting(0, "touchpad_tap=10") == 0);
        CHECK(brook_touchpad_tap_setting(0, "touchpad_taps=1") == 0);
        CHECK(brook_touchpad_tap_setting(1, "pointer_speed=100") == 1);
        CHECK(BROOK_TOUCHPAD_TAP_DEFAULT == 0);
    }

    if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
    std::printf("test_synaptics_tap: all passed\n");
    return 0;
}
