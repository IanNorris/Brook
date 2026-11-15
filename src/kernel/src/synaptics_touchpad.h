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
};
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
    return p;
}

struct SynapticsGestureConfig {
    int32_t touchZ = 25;          // pressure needed to count as a finger
    int32_t palmW = 12;           // widths at or above this are palms
    int32_t palmZ = 200;          // pressures at or above this are palms
    int32_t jumpUnits = 800;      // per-packet motion larger than this is a reposition
    int32_t scrollUnitsPerTick = 100; // ~2 mm of two-finger travel per wheel tick
    bool naturalScroll = false;   // false: fingers up scrolls up (wheel convention)
};

struct SynapticsOutput {
    int32_t dx = 0, dy = 0;       // pointer motion in PS/2-style counts; dy positive = down
    int32_t scrollY = 0;          // wheel ticks, positive = up
    int32_t scrollX = 0;          // wheel ticks, positive = right
    uint8_t buttons = 0;          // bit0 left, bit1 right
};

// One-finger motion, two-finger scroll, clickpad buttons (two-finger click is
// right click), palm rejection. Tap-to-click is deliberately not implemented.
class SynapticsGesture {
public:
    explicit SynapticsGesture(SynapticsGestureConfig c = {}) : cfg_(c) {}
    void SetConfig(const SynapticsGestureConfig& c) { cfg_ = c; }
    const SynapticsGestureConfig& Config() const { return cfg_; }
    unsigned fingerHistogram[4] = {0, 0, 0, 0}; // packets seen with 0,1,2,3+ fingers

    SynapticsOutput Feed(const SynapticsPacket& p) {
        SynapticsOutput o;
        if (p.w == 2) { o.buttons = buttons_; return o; } // extended (AGM) packet: ignored
        const int fingers = Fingers(p);
        ++fingerHistogram[fingers > 3 ? 3 : fingers];
        // Buttons: the physical click maps to right while two fingers rest.
        const bool click = p.left;
        if (click && !clickDown_) clickAsRight_ = (fingers == 2);
        clickDown_ = click;
        buttons_ = uint8_t((click ? (clickAsRight_ ? 2 : 1) : 0) | (p.right ? 2 : 0));
        o.buttons = buttons_;

        if (fingers != lastFingers_) {           // re-anchor on any count change
            haveAnchor_ = fingers != 0 && fingers < 3;
            ax_ = p.x; ay_ = p.y;
            fracX_ = fracY_ = 0; scrollAccX_ = scrollAccY_ = 0;
            lastFingers_ = fingers;
            return o;
        }
        if (!haveAnchor_) return o;
        int32_t dx = p.x - ax_, dy = p.y - ay_;   // pad: y up
        ax_ = p.x; ay_ = p.y;
        if (dx > cfg_.jumpUnits || dx < -cfg_.jumpUnits || dy > cfg_.jumpUnits || dy < -cfg_.jumpUnits) return o;
        if (fingers == 1) {
            // Speed-dependent gain: ~1/6 count per pad unit when slow, up to ~1/2 fast.
            const int32_t speed = Abs(dx) > Abs(dy) ? Abs(dx) : Abs(dy);
            const int32_t gainQ8 = 42 + (speed > 80 ? 80 : speed) * 11 / 10; // 42..130 (/256)
            o.dx = Scale(dx, gainQ8, fracX_);
            o.dy = Scale(-dy, gainQ8, fracY_);
        } else if (fingers == 2) {
            const int32_t sy = cfg_.naturalScroll ? -dy : dy;
            const int32_t sx = cfg_.naturalScroll ? -dx : dx;
            o.scrollY = Ticks(scrollAccY_, sy);
            o.scrollX = Ticks(scrollAccX_, sx);
        }
        return o;
    }

    int Fingers(const SynapticsPacket& p) const {
        if (p.z < cfg_.touchZ) return 0;
        if (p.w >= cfg_.palmW || p.z >= cfg_.palmZ) return 0; // palm: no motion
        if (p.w == 0) return 2;
        if (p.w == 1) return 3;
        return 1;
    }

private:
    static int32_t Abs(int32_t v) { return v < 0 ? -v : v; }
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
    SynapticsGestureConfig cfg_;
    int lastFingers_ = 0;
    bool haveAnchor_ = false;
    int32_t ax_ = 0, ay_ = 0, fracX_ = 0, fracY_ = 0, scrollAccX_ = 0, scrollAccY_ = 0;
    bool clickDown_ = false, clickAsRight_ = false;
    uint8_t buttons_ = 0;
};

} // namespace brook
