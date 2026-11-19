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

class BdwCursorPlane {
    uint32_t base_ = 0, position_ = 0;
    uint32_t armFrame_ = 0;
    bool enabled_ = false, retiring_ = false;

public:
    bool enabled() const { return enabled_; }
    uint32_t base() const { return base_; }
    uint32_t position() const { return position_; }

    // After a base change, wait at least two vblank observations before
    // reusing the other image. Both allocations stay pinned for this boot.
    bool canUpload(uint32_t frame, uint32_t live) const {
        return frame != 0xffffffffu && live == (enabled_ ? base_ : 0) &&
            (!retiring_ || uint32_t(frame - armFrame_) >= 2);
    }

    template<class Io>
    bool disable(Io& io, uint32_t frame) {
        if (!enabled_)
            return true;
        BdwCursorDisable(io);
        enabled_ = false;
        retiring_ = true;
        armFrame_ = frame;
        return io.read32(BdwCursorRegs::Control) == 0;
    }

    template<class Io>
    bool update(Io& io, uint32_t base, uint32_t frame, int32_t x, int32_t y) {
        uint32_t pos = 0;
        if (!base || base % BdwCursorRegs::Alignment || frame == 0xffffffffu ||
            !BdwCursorPosition(x, y, pos))
            return false;
        const bool changesBase = !enabled_ || base != base_;
        if (changesBase && !canUpload(frame, io.read32(BdwCursorRegs::Live)))
            return false;
        if (!changesBase && pos == position_)
            return true;

        if (!enabled_) {
            if(io.read32(BdwCursorRegs::Control)!=0)
                return false;
            io.write32(BdwCursorRegs::Fbc, 0);
            io.write32(BdwCursorRegs::Control, BdwCursorRegs::Argb64);
        }
        io.write32(BdwCursorRegs::Position, pos);
        // i915 rearms with the same base on a position update. No surface
        // change, pixel upload, or composition is needed for motion.
        io.write32(BdwCursorRegs::Base, base);
        if (io.read32(BdwCursorRegs::Control) != BdwCursorRegs::Argb64 ||
            io.read32(BdwCursorRegs::Base) != base) {
            BdwCursorDisable(io);
            enabled_ = false;
            retiring_ = true;
            armFrame_ = frame;
            return false;
        }
        enabled_ = true;
        base_ = base;
        position_ = pos;
        if (changesBase) {
            armFrame_ = frame;
            retiring_ = true;
        }
        return true;
    }
};

} // namespace brook
