#pragma once

#include <stdint.h>

namespace brook {

// Broadwell pipe A. See Linux i915 display/intel_cursor_regs.h and
// i9xx_cursor_update_arm() in display/intel_cursor.c.
struct BdwCursorRegs {
    static constexpr uint32_t Control = 0x70080;
    static constexpr uint32_t Base = 0x70084;
    static constexpr uint32_t Position = 0x70088;
    static constexpr uint32_t Fbc = 0x700a0;
    static constexpr uint32_t Live = 0x700ac;
    static constexpr uint32_t Argb64 = 0x27;
    static constexpr uint32_t Alignment = 4096;
    static constexpr uint32_t Side = 64;
    static constexpr uint32_t AddressMask = 0xfffff000u;
};

inline bool BdwCursorPosition(int32_t x, int32_t y, uint32_t& encoded)
{
    if (x < -32767 || x > 32767 || y < -32767 || y > 32767)
        return false;
    const uint32_t horizontal = x < 0 ? uint32_t(-x) | 0x8000 : uint32_t(x);
    const uint32_t vertical = y < 0 ? uint32_t(-y) | 0x8000 : uint32_t(y);
    encoded = horizontal | (vertical << 16);
    return true;
}

template<class Io>
void BdwCursorDisable(Io& io)
{
    io.write32(BdwCursorRegs::Control, 0);
    io.write32(BdwCursorRegs::Base, 0);
    (void)io.read32(BdwCursorRegs::Control);
}

inline bool BdwCursorHasAlpha(const uint32_t* pixels, uint32_t width, uint32_t height)
{
    for (uint32_t row = 0; row < height; ++row)
        for (uint32_t col = 0; col < width; ++col)
            if (pixels[row * BdwCursorRegs::Side + col] >> 24)
                return true;
    return false;
}

// Prove the plane can latch a transparent image while software still draws
// the cursor. Only then request a cursor-free primary frame. Make the real
// cursor visible once that primary frame has itself latched.
class BdwCursorHandoff {
    enum class State : uint8_t { Idle, Probing, Cleaning, Arming, Owned };
    State state_ = State::Idle;
    uint64_t epoch_ = 0, started_ = 0, offeredAfter_ = 0, firstClean_ = 0;
    static constexpr uint64_t TimeoutMs = 250;

public:
    bool prepare(uint64_t now) {
        if (state_ != State::Idle)
            return false;
        started_ = now;
        state_ = State::Probing;
        return true;
    }
    bool inProgress() const { return state_ != State::Idle; }
    bool probing() const { return state_ == State::Probing; }
    bool owned() const { return state_ == State::Owned; }
    bool expired(uint64_t now) const {
        return inProgress() && !owned() && now >= started_ && now - started_ >= TimeoutMs;
    }
    uint64_t offer(uint64_t sequence, uint64_t now, bool probeLatched) {
        if (probing() && probeLatched && epoch_ != (uint64_t{1} << 63) - 1) {
            ++epoch_;
            started_ = now;
            offeredAfter_ = sequence;
            state_ = State::Cleaning;
        }
        return epoch();
    }
    void cancel() { state_ = State::Idle; firstClean_ = 0; }
    uint64_t epoch() const { return inProgress() && !probing() ? epoch_ : 0; }
    bool suppress(bool hardwareMayBeVisible) const { return epoch() || hardwareMayBeVisible; }
    bool presented(uint64_t epoch, uint64_t sequence) {
        if (!awaitingPresent() || !epoch || epoch != this->epoch() || sequence <= offeredAfter_)
            return false;
        firstClean_ = sequence;
        return true;
    }
    bool awaitingPresent() const { return state_ == State::Cleaning && !firstClean_; }
    bool mayEnable(uint64_t front) const { return state_ == State::Cleaning && firstClean_ && front >= firstClean_; }
    void arm(uint64_t now) { state_ = State::Arming; started_ = now; }
    void latched() { if (state_ == State::Arming) state_ = State::Owned; }
};

class BdwCursorPlane {
    enum class State : uint8_t { Off, Arming, On, Disarming };
    State state_ = State::Off;
    uint32_t base_ = 0, position_ = 0, armFrame_ = 0;
    bool latched_ = false;

public:
    // True from the first arm request through the final disable latch.
    bool blocksSoftware() const { return state_ != State::Off; }
    bool enabled() const { return state_ == State::On || state_ == State::Arming; }
    bool active() const { return enabled() && latched_; }
    bool ready() const { return state_ == State::Off || state_ == State::On; }
    uint32_t position() const { return position_; }

    template<class Io>
    void disable(Io& io, uint32_t frame) {
        if (!enabled())
            return;
        BdwCursorDisable(io);
        state_ = State::Disarming;
        armFrame_ = frame;
        latched_ = false;
    }

    template<class Io>
    bool poll(Io& io, uint32_t frame) {
        if (state_ == State::Off || frame == 0xffffffffu)
            return true;
        if (state_ == State::Disarming) {
            if (armFrame_ == 0xffffffffu)
                armFrame_ = frame;
            if (uint32_t(frame - armFrame_) >= 2) {
                // CURSURFLIVE can still report the last address when the
                // plane is off. Check the programmed disable and two vblanks.
                if (!io.read32(BdwCursorRegs::Control) && !io.read32(BdwCursorRegs::Base))
                    state_ = State::Off;
                else {
                    BdwCursorDisable(io);
                    armFrame_ = frame;
                    return false;
                }
            }
            return true;
        }
        const uint32_t live = io.read32(BdwCursorRegs::Live);
        if (live != 0xffffffffu && (live & BdwCursorRegs::AddressMask) == base_) {
            if (state_ == State::Arming && frame != armFrame_) {
                latched_ = true;
                state_ = State::On;
            }
            return true;
        }
        return state_ == State::Arming && uint32_t(frame - armFrame_) < 4;
    }

    template<class Io>
    bool update(Io& io, uint32_t base, uint32_t frame, int32_t x, int32_t y) {
        uint32_t pos = 0;
        if (!ready() || !base || base % BdwCursorRegs::Alignment || frame == 0xffffffffu ||
            !BdwCursorPosition(x, y, pos))
            return false;
        const bool enabling = state_ == State::Off;
        const bool changesBase = enabling || base != base_;
        if (!changesBase && pos == position_)
            return true;
        if (enabling && io.read32(BdwCursorRegs::Control))
            return false;

        if (changesBase) {
            io.write32(BdwCursorRegs::Fbc, 0);
            io.write32(BdwCursorRegs::Control, BdwCursorRegs::Argb64);
        }
        io.write32(BdwCursorRegs::Position, pos);
        // i915 rearms with the same base on a position update. A new image
        // waits for its own latch before either cursor buffer is reused.
        io.write32(BdwCursorRegs::Base, base);
        if (changesBase) {
            state_ = State::Arming;
            armFrame_ = frame;
        }
        base_ = base;
        position_ = pos;
        return io.read32(BdwCursorRegs::Control) == BdwCursorRegs::Argb64 &&
               (io.read32(BdwCursorRegs::Base) & BdwCursorRegs::AddressMask) == base;
    }
};

} // namespace brook
