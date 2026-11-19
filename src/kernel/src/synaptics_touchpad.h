#pragma once
// Synaptics PS/2 touchpad: protocol decoding and a small gesture state machine.
//
// Pure logic (no I/O, integer only) so it can run in the IRQ12 handler, which
// is compiled with -mgeneral-regs-only, and in host tests. The PS/2 command
// sequences live in mouse.cpp.
//
// Protocol notes follow Linux drivers/input/mouse/synaptics.c:
//  - "special" commands are sent as E6 then four E8 commands carrying the
//    8-bit argument two bits at a time (most significant first), followed by
//    E9 (status request, 3-byte reply) for queries or F3 0x14 for mode set;
//  - identify (query 0x00) reply byte 1 is 0x47 on a Synaptics pad;
//  - capabilities (query 0x02) form a 24-bit word whose middle byte is 0x47;
//  - absolute mode packets are 6 bytes: byte0 & 0xC8 == 0x80 and
//    byte3 & 0xC8 == 0xC0.
#include <stdint.h>

namespace brook {

constexpr uint8_t SynQueryIdentify     = 0x00;
constexpr uint8_t SynQueryModes        = 0x01;
constexpr uint8_t SynQueryCapabilities = 0x02;
constexpr uint8_t SynQueryExtCap       = 0x09;
constexpr uint8_t SynQueryExtCap0C     = 0x0c;
constexpr uint8_t SynQueryMaxCoords    = 0x0d;
constexpr uint8_t SynQueryMinCoords    = 0x0f;

constexpr uint8_t SynModeAbsolute = 0x80;
constexpr uint8_t SynModeHighRate = 0x40;  // 80 packets/s instead of 40
constexpr uint8_t SynModeDisableGesture = 0x04; // no firmware tap/gesture emulation
constexpr uint8_t SynModeW = 0x01;         // report W (finger count / width)

struct SynapticsIdentity {
    bool valid = false;
    uint8_t major = 0, minor = 0, model = 0;
};
inline SynapticsIdentity SynapticsDecodeIdentify(const uint8_t r[3]) {
    SynapticsIdentity id;
    id.valid = r[1] == 0x47;
    id.minor = r[0];
    id.major = r[2] & 0x0f;
    id.model = (r[2] >> 4) & 0x0f;
    return id;
}

struct SynapticsCaps {
    bool valid = false;
    uint32_t caps = 0, ext = 0, ext0c = 0;
    bool extended() const { return caps & (1u << 23); }
    unsigned extRequests() const { return (caps >> 20) & 0x7; }
    bool multiFinger() const { return caps & (1u << 1); }
    bool palmDetect() const { return caps & (1u << 0); }
    bool clickpad() const { return (ext0c & (1u << 20)) || (ext0c & (1u << 8)); }
    // Advanced gesture mode / image sensor: W=2 packets carry the second finger.
    bool agm() const { return (ext0c & (1u << 19)) || (ext0c & (1u << 11)); }
    // Linux synaptics_resolution(): query 0x0d (max) needs >= 5 extended
    // requests, query 0x0f (min) >= 7; both are flagged in the 0x0c word.
    bool maxCoords() const { return extRequests() >= 5 && (ext0c & (1u << 17)); }
    bool minCoords() const { return extRequests() >= 7 && (ext0c & (1u << 13)); }
};

// Reported coordinate range (pad units, y up). Defaults are Linux's nominal
// XMIN/XMAX/YMIN/YMAX_NOMINAL, used when the pad cannot report its own.
struct SynapticsRange {
    int32_t xMin = 1472, xMax = 5472, yMin = 1408, yMax = 4448;
    bool reportedMax = false, reportedMin = false;
};
// Query 0x0d / 0x0f reply: x = r0<<5 | (r1 & 0x0f)<<1, y = r2<<5 | (r1 & 0xf0)>>3.
inline void SynapticsDecodeCoords(const uint8_t r[3], int32_t& x, int32_t& y) {
    x = (int32_t(r[0]) << 5) | ((r[1] & 0x0f) << 1);
    y = (int32_t(r[2]) << 5) | ((r[1] & 0xf0) >> 3);
}
// Apply the replies that were read (null = not read); an implausible result
// (inverted, tiny or beyond the 13-bit packet range) keeps the nominal range.
inline SynapticsRange SynapticsDecodeRange(const uint8_t* maxReply, const uint8_t* minReply) {
    SynapticsRange r, n;
    if (maxReply) { SynapticsDecodeCoords(maxReply, r.xMax, r.yMax); r.reportedMax = true; }
    if (minReply) { SynapticsDecodeCoords(minReply, r.xMin, r.yMin); r.reportedMin = true; }
    if (r.xMax > 8191 || r.yMax > 8191 || r.xMax - r.xMin < 1000 || r.yMax - r.yMin < 1000) return n;
    return r;
}
inline uint32_t SynapticsWord(const uint8_t r[3]) {
    return (uint32_t(r[0]) << 16) | (uint32_t(r[1]) << 8) | r[2];
}
// Linux synaptics_capability(): firmware older than 7.5 has the submodel byte
// fixed at 0x47 and anything else means "not a Synaptics capability reply";
// newer firmware (e.g. the XPS 13 9343's 8.1) reports a real submodel there.
// Without capExtended (bit 23) the remaining flags must be ignored.
inline SynapticsCaps SynapticsDecodeCaps(const uint8_t r[3], const SynapticsIdentity& id) {
    SynapticsCaps c;
    const uint32_t raw = SynapticsWord(r);
    const unsigned full = (unsigned(id.major) << 8) | id.minor;
    c.valid = full >= 0x705 || ((raw >> 8) & 0xff) == 0x47;
    c.caps = (raw & (1u << 23)) ? raw : 0;
    return c;
}

// Mode byte Brook requests: absolute, 80 Hz, W reporting when the pad has
// extended capabilities, and no firmware gestures (taps would otherwise be
// converted into button presses behind our back).
inline uint8_t SynapticsModeByte(const SynapticsCaps& c) {
    uint8_t mode = SynModeAbsolute | SynModeHighRate | SynModeDisableGesture;
    if (c.extended()) mode |= SynModeW;
    return mode;
}

// The four 2-bit E8 arguments that encode an 8-bit special command.
inline void SynapticsSlice(uint8_t arg, uint8_t out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = (arg >> (6 - 2 * i)) & 3;
}

inline bool SynapticsByteValid(unsigned index, uint8_t b) {
    if (index == 0) return (b & 0xC8) == 0x80;
    if (index == 3) return (b & 0xC8) == 0xC0;
    return true;
}

struct SynapticsPacket {
    int32_t x = 0, y = 0;   // pad units, origin bottom-left
    int32_t z = 0;          // pressure, 0 = no finger
    int32_t w = 0;          // 0: two fingers, 1: three+, 2: extended packet, 4..15: width
    bool left = false, right = false;
    // W=2 AGM packet (type 1): second finger at half resolution, scaled up.
    bool agm = false;
    int32_t agmX = 0, agmY = 0, agmZ = 0;
};
inline SynapticsPacket SynapticsDecodePacket(const uint8_t b[6], bool clickpad) {
    SynapticsPacket p;
    p.w = ((b[0] & 0x30) >> 2) | ((b[0] & 0x04) >> 1) | ((b[3] & 0x04) >> 2);
    p.x = ((b[3] & 0x10) << 8) | ((b[1] & 0x0f) << 8) | b[4];
    p.y = ((b[3] & 0x20) << 7) | ((b[1] & 0xf0) << 4) | b[5];
    p.z = b[2];
    // Clickpads report the physical click as left XOR the mirror bit in byte 3.
    p.left = clickpad ? ((b[0] ^ b[3]) & 0x01) : (b[0] & 0x01);
    p.right = !clickpad && (b[0] & 0x02);
    if (p.w == 2 && ((b[5] & 0x30) >> 4) == 1) {   // Linux synaptics_parse_agm, type 1
        p.agm = true;
        p.agmX = (((b[4] & 0x0f) << 8) | b[1]) << 1;
        p.agmY = (((b[4] & 0xf0) << 4) | b[2]) << 1;
        p.agmZ = ((b[3] & 0x30) | (b[5] & 0x0f)) << 1;
    }
    return p;
}

struct SynapticsGestureConfig {
    int32_t touchZ = 25;          // pressure needed to count as a finger
    int32_t palmW = 12;           // widths at or above this are palms
    int32_t palmZ = 200;          // pressures at or above this are palms
    int32_t jumpUnits = 800;      // per-packet motion larger than this is a reposition
    int32_t scrollUnitsPerTick = 100; // ~2 mm of two-finger travel per wheel tick
    bool naturalScroll = false;   // false: fingers up scrolls up (wheel convention)
    int32_t speedPercent = 100;   // user pointer speed; 100 = default (25..300)
    int32_t dragNoiseUnits = 3;   // per-packet motion below this is a resting finger
    // Clickpad press jitter (Ian, gen 400: "clicking is also moving"):
    int32_t clickSuppressPackets = 10; // ~125 ms at 80 Hz after press and release: no pointer motion
    int32_t clickDeadzoneUnits = 40;   // while held, travel from the press point must exceed this
    int32_t pressureJumpZ = 30;        // a pressure step this large is a press shift, not motion
    // Phantom-scroll guards (Ian, gen 400: "scrolled without me near it"):
    int32_t scrollStablePackets = 3;   // consecutive two-finger packets before scrolling starts
    int32_t scrollJumpUnits = 250;     // per-packet two-finger motion above this is not a scroll
    // Clickpad software buttons (libinput button areas; Ian, gen 421: right
    // click was only reachable with a second finger resting). A physical
    // click whose contact is in the bottom strip (y below buttonTopY) is a
    // right click when x >= buttonSplitX, otherwise left. Pad units, y up;
    // set from the reported range by SynapticsSetButtonAreas. Defaults match
    // the nominal range.
    bool softButtons = true;
    int32_t buttonTopY = 1408 + (4448 - 1408) * 15 / 100;
    int32_t buttonSplitX = (1472 + 5472) / 2;
};
// Bottom 15% of the pad height (libinput without resolution), split in half.
inline void SynapticsSetButtonAreas(SynapticsGestureConfig& c, const SynapticsRange& r, bool clickpad) {
    c.softButtons = clickpad;
    c.buttonTopY = r.yMin + (r.yMax - r.yMin) * 15 / 100;
    c.buttonSplitX = (r.xMin + r.xMax) / 2;
}
constexpr int32_t SynapticsSpeedMin = 25, SynapticsSpeedMax = 300;
// Base gain multiplier: 80% of the original tuning (Ian: "a tiny bit fast").
constexpr int32_t SynapticsBaseGainPercent = 80;

struct SynapticsOutput {
    int32_t dx = 0, dy = 0;       // pointer motion in PS/2-style counts; dy positive = down
    int32_t scrollY = 0;          // wheel ticks, positive = up
    int32_t scrollX = 0;          // wheel ticks, positive = right
    int32_t scrollY120 = 0, scrollX120 = 0; // the same motion in 1/120 ticks (smooth scrolling)
    uint8_t buttons = 0;          // bit0 left, bit1 right
};

// One-finger motion, two-finger scroll, clickpad buttons (software button
// areas; two-finger click on the main surface is right click), palm rejection. Tap-to-click is a separate state machine
// (synaptics_tap.h) fed from the same packets.
class SynapticsGesture {
public:
    explicit SynapticsGesture(SynapticsGestureConfig c = {}) : cfg_(c) {}
    void SetConfig(const SynapticsGestureConfig& c) { cfg_ = c; }
    const SynapticsGestureConfig& Config() const { return cfg_; }
    unsigned fingerHistogram[4] = {0, 0, 0, 0}; // packets seen with 0,1,2,3+ fingers
    unsigned scrollTicksLowZ = 0;   // ticks emitted while either packet's Z was under 2x the touch threshold
    unsigned scrollRejected = 0;    // two-finger packets dropped by the stability/jump guards
    unsigned resets = 0;            // state resets (resync, lift-off)
    // Clickpad press decisions (each press counts once in total and once by reason).
    unsigned clicksLeft = 0, clicksRight = 0;
    unsigned clicksRightArea = 0;   // right: a contact in the bottom-right area
    unsigned clicksTwoFinger = 0;   // right: two fingers on the main surface
    unsigned clicksThumbLeft = 0;   // left: two contacts, one resting in the left area
    unsigned clicksStalePos = 0;    // press packet had no contact; used the last contact
    unsigned clicksNoPos = 0;       // press with no contact position known (left)

    // Forget all contact state (used on packet resync): the next packets must
    // re-establish a stable touch before any motion or scroll.
    void Reset() {
        ++resets;
        lastFingers_ = 0; haveAnchor_ = false; haveAgm_ = false; stable_ = 0;
        fracX_ = fracY_ = scrollAccX_ = scrollAccY_ = 0; fine120X_ = fine120Y_ = 0; lastZ_ = 0;
        haveContact_ = false; contactRaw_ = 0;
    }

    SynapticsOutput Feed(const SynapticsPacket& p) {
        SynapticsOutput o;
        if (p.w == 2) {                          // extended packet: second finger only
            o.buttons = buttons_;
            if (!p.agm) return o;
            if (p.agmZ < cfg_.touchZ) { haveAgm_ = false; return o; }
            if (haveAgm_ && Dragging()) {
                const int32_t dx = p.agmX - agmX_, dy = p.agmY - agmY_;
                if (Abs(dx) <= cfg_.jumpUnits && Abs(dy) <= cfg_.jumpUnits) Pointer(dx, dy, o);
            }
            agmX_ = p.agmX; agmY_ = p.agmY; haveAgm_ = true;
            return o;
        }
        const int fingers = Fingers(p);
        ++fingerHistogram[fingers > 3 ? 3 : fingers];
        // Buttons: the press picks left or right from the contact positions
        // (ChooseClick) and keeps that button until release.
        const bool click = p.left;
        if (click && !clickDown_) clickButton_ = ChooseClick(p);
        if (p.z >= cfg_.touchZ) {               // raw contact, palms included
            haveContact_ = true; contactX_ = p.x; contactY_ = p.y; contactRaw_ = RawFingers(p);
        }
        if (click != clickDown_) {               // press or release: start jitter suppression
            suppress_ = cfg_.clickSuppressPackets;
            deadzone_ = click;
            pressX_ = p.x; pressY_ = p.y;
        } else if (suppress_ > 0) {
            --suppress_;
        }
        clickDown_ = click;
        buttons_ = uint8_t((click ? clickButton_ : 0) | (p.right ? 2 : 0));
        o.buttons = buttons_;
        if (fingers < 2) haveAgm_ = false;

        if (fingers == 0 && lastFingers_ != 0) ++resets;  // lift-off clears everything below
        if (fingers != lastFingers_) {           // re-anchor on any count change
            stable_ = 0;
            haveAnchor_ = fingers != 0 && fingers < 3;
            ax_ = p.x; ay_ = p.y; lastZ_ = p.z;
            fracX_ = fracY_ = 0; scrollAccX_ = scrollAccY_ = 0; fine120X_ = fine120Y_ = 0;
            lastFingers_ = fingers;
            return o;
        }
        if (!haveAnchor_) { lastZ_ = p.z; return o; }
        int32_t dx = p.x - ax_, dy = p.y - ay_;   // pad: y up
        ax_ = p.x; ay_ = p.y;
        const int32_t zStep = Abs(p.z - lastZ_);
        const int32_t prevZ = lastZ_;
        lastZ_ = p.z;
        if (stable_ < 1000) ++stable_;
        // Pointer-motion jitter filters (scrolling is unaffected): the window
        // after a button transition, a pressure step (the contact centroid
        // shifts as the finger presses), and the held-button dead zone.
        bool quiet = suppress_ > 0 || zStep >= cfg_.pressureJumpZ;
        if (!quiet && fingers == 1 && deadzone_ && clickDown_) {
            if (Abs(p.x - pressX_) < cfg_.clickDeadzoneUnits && Abs(p.y - pressY_) < cfg_.clickDeadzoneUnits) quiet = true;
            else deadzone_ = false;              // deliberate drag: motion resumes
        }
        if (Abs(dx) > cfg_.jumpUnits || Abs(dy) > cfg_.jumpUnits) return o;
        if (fingers == 1) {
            if (!quiet) Pointer(dx, dy, o);
        } else if (fingers == 2 && Dragging()) {
            // Click-drag with a second finger: one finger holds the button, the
            // other moves. Each finger's own deltas drive the pointer (the
            // primary here, the secondary from AGM packets); a resting finger
            // contributes nothing. Never scroll or right-click in this state.
            if (!quiet && (Abs(dx) >= cfg_.dragNoiseUnits || Abs(dy) >= cfg_.dragNoiseUnits)) Pointer(dx, dy, o);
        } else if (fingers == 2 && !clickDown_) {
            // Scroll only from a stable two-finger contact: both this and the
            // previous packet pressed, a few consecutive packets since touch
            // down, and plausible per-packet travel. Anything else is dropped
            // and the accumulators cleared so no stale fraction can fire later.
            if (prevZ < cfg_.touchZ || stable_ < cfg_.scrollStablePackets ||
                Abs(dx) > cfg_.scrollJumpUnits || Abs(dy) > cfg_.scrollJumpUnits) {
                if (stable_ >= cfg_.scrollStablePackets) ++scrollRejected;
                scrollAccX_ = scrollAccY_ = 0; fine120X_ = fine120Y_ = 0;
                return o;
            }
            const int32_t sy = cfg_.naturalScroll ? -dy : dy;
            const int32_t sx = cfg_.naturalScroll ? -dx : dx;
            o.scrollY = Ticks(scrollAccY_, sy);
            o.scrollX = Ticks(scrollAccX_, sx);
            o.scrollY120 = Fine120(fine120Y_, sy);
            o.scrollX120 = Fine120(fine120X_, sx);
            if ((o.scrollY || o.scrollX) && (p.z < 2 * cfg_.touchZ || prevZ < 2 * cfg_.touchZ))
                scrollTicksLowZ += unsigned(Abs(o.scrollY) + Abs(o.scrollX));
        }
        return o;
    }

    // A press other than a two-finger click is held: extra fingers move the
    // pointer, not scroll.
    bool Dragging() const { return (clickDown_ && !clickTwoFinger_) || tapDrag_; }
    // Tap-and-drag holds the left button without a physical press.
    void SetTapDrag(bool on) { tapDrag_ = on; }

    int Fingers(const SynapticsPacket& p) const {
        if (p.z < cfg_.touchZ) return 0;
        if (p.w >= cfg_.palmW || p.z >= cfg_.palmZ) return 0; // palm: no motion
        if (p.w == 0) return 2;
        if (p.w == 1) return 3;
        return 1;
    }

    // Contact count without the palm guards: a hard press can widen or
    // deepen a fingertip past palmW/palmZ, and it still chooses the button.
    int RawFingers(const SynapticsPacket& p) const {
        if (p.z < cfg_.touchZ) return 0;
        if (p.w == 0) return 2;
        if (p.w == 1) return 3;
        return 1;
    }

private:
    static int32_t Abs(int32_t v) { return v < 0 ? -v : v; }
    bool InStrip(int32_t y) const { return y < cfg_.buttonTopY; }
    bool InRight(int32_t x, int32_t y) const { return InStrip(y) && x >= cfg_.buttonSplitX; }

    // Button for a new physical press (bit0 left, bit1 right), libinput
    // software-button style:
    //  - any contact in the bottom-right area: right (whichever finger
    //    pressed; a thumb resting in the left area does not get in the way);
    //  - two contacts, one of them in the bottom strip (a resting thumb):
    //    left, the area it rests in;
    //  - two contacts on the main surface, or a second contact whose position
    //    is not known yet (no AGM packet): right, the two-finger click;
    //  - otherwise left.
    // A press packet without a contact (Z below touchZ) uses the last contact.
    uint8_t ChooseClick(const SynapticsPacket& p) {
        bool havePos = true;
        int32_t x = p.x, y = p.y;
        int raw = RawFingers(p);
        if (raw == 0) {
            if (haveContact_) { x = contactX_; y = contactY_; raw = contactRaw_; ++clicksStalePos; }
            else havePos = false;
        }
        clickTwoFinger_ = false;
        uint8_t b = 1;
        if (!havePos) {
            ++clicksNoPos;
        } else if (!cfg_.softButtons) {
            if (raw == 2) { b = 2; clickTwoFinger_ = true; ++clicksTwoFinger; }
        } else {
            const bool second = raw >= 2 && haveAgm_;
            if (InRight(x, y) || (second && InRight(agmX_, agmY_))) { b = 2; ++clicksRightArea; }
            else if (raw == 2) {
                if (second && (InStrip(y) || InStrip(agmY_))) ++clicksThumbLeft;
                else { b = 2; clickTwoFinger_ = true; ++clicksTwoFinger; }
            }
        }
        if (b == 2) ++clicksRight; else ++clicksLeft;
        return b;
    }

    void Pointer(int32_t dx, int32_t dy, SynapticsOutput& o) {
        // Speed-dependent gain: ~1/6 count per pad unit when slow, up to ~1/2
        // fast, scaled by the base tuning and the user's speed setting.
        const int32_t speed = Abs(dx) > Abs(dy) ? Abs(dx) : Abs(dy);
        int32_t gainQ8 = 42 + (speed > 80 ? 80 : speed) * 11 / 10;
        int32_t user = cfg_.speedPercent;
        if (user < SynapticsSpeedMin) user = SynapticsSpeedMin;
        if (user > SynapticsSpeedMax) user = SynapticsSpeedMax;
        gainQ8 = gainQ8 * SynapticsBaseGainPercent / 100 * user / 100;
        if (gainQ8 < 1) gainQ8 = 1;
        o.dx += Scale(dx, gainQ8, fracX_);
        o.dy += Scale(-dy, gainQ8, fracY_);
    }
    static int32_t Scale(int32_t d, int32_t gainQ8, int32_t& frac) {
        const int32_t total = d * gainQ8 + frac;
        const int32_t out = total / 256;           // truncates toward zero
        frac = total - out * 256;
        return out;
    }
    int32_t Ticks(int32_t& acc, int32_t d) {
        acc += d;
        int32_t t = acc / cfg_.scrollUnitsPerTick;
        if (t > 8) t = 8;
        if (t < -8) t = -8;
        acc -= t * cfg_.scrollUnitsPerTick;
        return t;
    }
    // Scroll units to 1/120 ticks, carrying the remainder.
    int32_t Fine120(int32_t& acc, int32_t d) {
        if (cfg_.scrollUnitsPerTick <= 0) return 0;
        acc += d * 120;
        const int32_t out = acc / cfg_.scrollUnitsPerTick;
        acc -= out * cfg_.scrollUnitsPerTick;
        return out;
    }
    SynapticsGestureConfig cfg_;
    int lastFingers_ = 0;
    bool haveAnchor_ = false;
    int32_t ax_ = 0, ay_ = 0, fracX_ = 0, fracY_ = 0, scrollAccX_ = 0, scrollAccY_ = 0;
    int32_t fine120X_ = 0, fine120Y_ = 0;
    bool clickDown_ = false, clickTwoFinger_ = false;
    uint8_t clickButton_ = 1;
    bool haveContact_ = false;      // last raw contact position (any packet with Z >= touchZ)
    int32_t contactX_ = 0, contactY_ = 0;
    int contactRaw_ = 0;
    bool haveAgm_ = false;
    int32_t agmX_ = 0, agmY_ = 0;
    int32_t suppress_ = 0, pressX_ = 0, pressY_ = 0, lastZ_ = 0, stable_ = 0;
    bool deadzone_ = false;
    bool tapDrag_ = false;
    uint8_t buttons_ = 0;
};

} // namespace brook
