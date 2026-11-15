#pragma once
// Synaptics touchpad tap-to-click: a small pure state machine.
//
// Integer only and free of I/O so it runs in the IRQ12 handler (compiled with
// -mgeneral-regs-only), in the LAPIC timer poll and in host tests. The caller
// supplies a millisecond clock with every packet.
//
// Behaviour follows the libinput defaults:
//  - one-finger tap (touch to lift within tapMs, travel under moveUnits) is a
//    left click; a stable two-finger tap is a right click;
//  - tap-and-drag: after a one-finger tap the left button stays pressed for
//    dragMs; touching again within that window and moving (or holding past
//    tapMs) keeps it pressed until the finger lifts. A quick second tap is a
//    double click. If nothing follows, the button is released at the
//    deadline (Timeout(), driven by the timer when no packets arrive);
//  - contacts the gesture guards reject (palms, light/phantom touches) never
//    tap, and any physical button press cancels the tap and takes over.
#include <stdint.h>
#include "synaptics_touchpad.h"

namespace brook {

struct SynapticsTapConfig {
    bool enabled = false;          // user setting; default off
    bool drag = true;              // tap-and-drag (libinput default with tapping on)
    uint32_t tapMs = 180;          // touch-down to lift for a tap (libinput: 180 ms)
    uint32_t dragMs = 250;         // window after a tap for a second touch
    int32_t moveUnits = 80;        // travel from the touch point that makes it a move (~1.5-2 mm)
    uint32_t minPackets = 2;       // contact packets needed (one-packet blips are noise)
    uint32_t twoFingerPackets = 3; // two-finger packets needed for a right-click tap
    int32_t minPeakZ = 40;         // peak pressure needed (phantom contacts sit near touchZ)
};

// Per-packet input, derived from a decoded packet and the gesture guards.
struct SynapticsTapInput {
    uint64_t nowMs = 0;
    int fingers = 0;        // guarded finger count (0 for palms and light touches)
    bool contact = false;   // something presses the pad (z >= touchZ), even if rejected
    bool palm = false;      // contact rejected by the palm guards
    bool physical = false;  // a physical (clickpad) button is down
    int32_t x = 0, y = 0, z = 0;
};
inline SynapticsTapInput SynapticsTapInputFrom(const SynapticsPacket& p, const SynapticsGesture& g, uint64_t nowMs) {
    SynapticsTapInput in;
    in.nowMs = nowMs;
    in.fingers = g.Fingers(p);
    in.contact = p.z >= g.Config().touchZ;
    in.palm = in.contact && in.fingers == 0;
    in.physical = p.left || p.right;
    in.x = p.x; in.y = p.y; in.z = p.z;
    return in;
}

// Button masks to emit, in order, before the packet's own pointer report.
// Each entry is the complete tap mask (bit0 left, bit1 right) at that step.
struct SynapticsTapOutput {
    uint8_t steps = 0;
    uint8_t mask[3] = {0, 0, 0};
};

class SynapticsTap {
public:
    explicit SynapticsTap(SynapticsTapConfig c = {}) : cfg_(c) {}
    const SynapticsTapConfig& Config() const { return cfg_; }
    // Diagnostics.
    uint32_t tapsLeft = 0, tapsRight = 0, drags = 0, doubleTaps = 0;
    uint32_t rejectedPhysical = 0, rejectedPalm = 0, rejectedPhantom = 0;

    // Changing the configuration abandons any tap; a held tap button is released.
    SynapticsTapOutput SetConfig(const SynapticsTapConfig& c) {
        SynapticsTapOutput o;
        Release(o);
        cfg_ = c;
        state_ = lastContact_ ? WaitLift : Idle;   // a contact already down must lift first
        return o;
    }
    SynapticsTapOutput SetEnabled(bool on) { SynapticsTapConfig c = cfg_; c.enabled = on; return SetConfig(c); }

    // Contact state is no longer trustworthy (packet resync, renegotiation).
    SynapticsTapOutput Reset() {
        SynapticsTapOutput o;
        Release(o);
        state_ = lastContact_ ? WaitLift : Idle;
        return o;
    }

    uint8_t Held() const { return held_; }
    // Tap-and-drag in progress: the gesture should treat extra fingers as drag, not scroll.
    bool Dragging() const { return held_ && (state_ == Second || state_ == Drag); }
    // Absolute ms at which Timeout() has work to do; 0 when none.
    uint64_t Deadline() const { return state_ == Tapped ? deadline_ : 0; }

    SynapticsTapOutput Timeout(uint64_t nowMs) {
        SynapticsTapOutput o;
        if (state_ == Tapped && nowMs >= deadline_) { Release(o); state_ = Idle; }
        return o;
    }

    SynapticsTapOutput Feed(const SynapticsTapInput& in) {
        SynapticsTapOutput o;
        lastContact_ = in.contact;
        if (!cfg_.enabled) return o;
        // A physical click always wins: finish any tap button first so the
        // press is reported as a fresh transition, then ignore this contact.
        if (in.physical) {
            if (state_ == Touch || state_ == Second) ++rejectedPhysical;
            Release(o);
            state_ = WaitLift;
            return o;
        }
        if (in.palm) {
            if (state_ == Touch || state_ == Second) ++rejectedPalm;
            Release(o);
            state_ = WaitLift;
            return o;
        }
        const int f = in.fingers;
        switch (state_) {
        case WaitLift:
            if (f == 0) state_ = Idle;
            break;
        case Idle:
            if (f > 0) Begin(in);
            break;
        case Touch:
            if (f == 0) { Lift(in, o); break; }
            if (!Track(in)) state_ = WaitLift;   // moved, slow or 3+ fingers: not a tap
            break;
        case Tapped:
            if (f == 0) { if (in.nowMs >= deadline_) { Release(o); state_ = Idle; } break; }
            if (f == 1) { Begin(in); state_ = Second; break; }
            // Two or more fingers after a tap: the tap was a plain click and
            // this is a fresh touch (e.g. a two-finger tap or scroll).
            Release(o);
            Begin(in);
            break;
        case Second:
            if (f == 0) {
                // Quick second tap: double click (release, press, release).
                Push(o, 0); Push(o, 1); Push(o, 0);
                held_ = 0;
                ++doubleTaps;
                state_ = Idle;
                break;
            }
            if (f >= 2) {
                // More fingers before the drag started: the first tap was a
                // plain click and this is a fresh (e.g. two-finger) touch.
                Release(o);
                Begin(in);
                break;
            }
            if (!Track(in)) { state_ = Drag; ++drags; }
            break;
        case Drag:
            if (f == 0) { Release(o); state_ = Idle; }
            break;
        }
        return o;
    }

private:
    enum State : uint8_t { Idle, Touch, Tapped, Second, Drag, WaitLift };
    static int32_t Abs(int32_t v) { return v < 0 ? -v : v; }
    static void Push(SynapticsTapOutput& o, uint8_t mask) { if (o.steps < 3) o.mask[o.steps++] = mask; }
    void Release(SynapticsTapOutput& o) { if (held_) { held_ = 0; Push(o, 0); } }

    void Begin(const SynapticsTapInput& in) {
        state_ = in.fingers >= 3 ? WaitLift : Touch;
        startMs_ = in.nowMs;
        fingers_ = in.fingers;
        ax_ = in.x; ay_ = in.y;
        packets_ = 1;
        twoCount_ = in.fingers == 2 ? 1 : 0;
        peakZ_ = in.z;
    }
    // Returns false once the contact can no longer be a tap.
    bool Track(const SynapticsTapInput& in) {
        if (in.fingers >= 3) return false;
        if (in.nowMs - startMs_ > cfg_.tapMs) return false;
        ++packets_;
        if (in.z > peakZ_) peakZ_ = in.z;
        if (in.fingers == 2) ++twoCount_;
        if (in.fingers != fingers_) {           // reported position jumps with the count
            fingers_ = in.fingers; ax_ = in.x; ay_ = in.y;
            return true;
        }
        return Abs(in.x - ax_) < cfg_.moveUnits && Abs(in.y - ay_) < cfg_.moveUnits;
    }
    void Lift(const SynapticsTapInput& in, SynapticsTapOutput& o) {
        state_ = Idle;
        if (in.nowMs - startMs_ > cfg_.tapMs) return;          // slow hold
        if (packets_ < cfg_.minPackets || peakZ_ < cfg_.minPeakZ) { ++rejectedPhantom; return; }
        if (twoCount_ >= cfg_.twoFingerPackets) {               // right-click tap
            Push(o, 2); Push(o, 0);
            ++tapsRight;
            return;
        }
        if (twoCount_ != 0) { ++rejectedPhantom; return; }     // two-finger blip: ambiguous
        ++tapsLeft;
        if (!cfg_.drag) { Push(o, 1); Push(o, 0); return; }
        held_ = 1;                                              // held until the drag window closes
        Push(o, 1);
        deadline_ = in.nowMs + cfg_.dragMs;
        state_ = Tapped;
    }

    SynapticsTapConfig cfg_;
    State state_ = Idle;
    uint8_t held_ = 0;
    bool lastContact_ = false;
    int fingers_ = 0;
    int32_t ax_ = 0, ay_ = 0, peakZ_ = 0;
    uint32_t packets_ = 0, twoCount_ = 0;
    uint64_t startMs_ = 0, deadline_ = 0;
};

} // namespace brook
