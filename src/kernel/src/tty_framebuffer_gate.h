#pragma once

#include <stdint.h>

namespace brook {

// One atomic word makes entering and closing mutually exclusive. Writers
// close admission before draining admitted users in process context.
// Framebuffer text is best-effort during a close or concurrent draw. Logging
// can hold the IRQ-disabling serial lock and panic can interrupt a drawer,
// so admission never waits for another writer. COM1 keeps the diagnostic log.
class TtyFramebufferGate {
public:
    enum class Access { Read, Draw, Legacy };

    bool TryEnter(Access access)
    {
        uint32_t state = __atomic_load_n(&state_, __ATOMIC_RELAXED);
        do {
            if ((state & Closed) || (state & Readers) == Readers) return false;
            if ((state & Managed) && access == Access::Legacy) return false;
            const bool exclusive = (state & Managed) && access == Access::Draw;
            if (exclusive && (state & Drawing)) return false;
            const uint32_t next = (state + 1) | (exclusive ? Drawing : 0);
            if (__atomic_compare_exchange_n(&state_, &state, next, true,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return true;
        } while (true);
    }

    void Leave(Access access)
    {
        // Managed cannot change while a user is admitted. Only the admitted
        // drawing operation can clear Drawing in managed mode.
        const bool exclusive = access == Access::Draw && IsManaged();
        __atomic_fetch_sub(&state_, 1 + (exclusive ? Drawing : 0), __ATOMIC_ACQ_REL);
    }

    bool TryClose()
    {
        uint32_t state = __atomic_load_n(&state_, __ATOMIC_RELAXED);
        do {
            if (state & Closed) return false;
            if (__atomic_compare_exchange_n(&state_, &state, state | Closed, true,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return true;
        } while (true);
    }

    bool Drained() const
    {
        return (__atomic_load_n(&state_, __ATOMIC_ACQUIRE) & (Closed | Readers)) == Closed;
    }

    void MakeManaged() { __atomic_fetch_or(&state_, Managed, __ATOMIC_RELAXED); }
    void Open() { __atomic_fetch_and(&state_, ~Closed, __ATOMIC_RELEASE); }

    bool IsManaged() const { return __atomic_load_n(&state_, __ATOMIC_RELAXED) & Managed; }

private:
    static constexpr uint32_t Closed = 1u << 31;
    static constexpr uint32_t Managed = 1u << 30;
    static constexpr uint32_t Drawing = 1u << 29;
    static constexpr uint32_t Readers = Drawing - 1;
    uint32_t state_ = 0;
};

struct TtyClippedRegion { uint32_t x, y, width, height; };

constexpr TtyClippedRegion TtyClipRegion(uint32_t screenWidth, uint32_t screenHeight,
                                       uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (x > screenWidth) x = screenWidth;
    if (y > screenHeight) y = screenHeight;
    const uint32_t remainingWidth = screenWidth - x;
    const uint32_t remainingHeight = screenHeight - y;
    if (!width || width > remainingWidth) width = remainingWidth;
    if (!height || height > remainingHeight) height = remainingHeight;
    return {x, y, width, height};
}

struct TtyCursorPosition { uint32_t x, y; };

constexpr TtyCursorPosition TtyClampCursor(uint32_t x, uint32_t y, uint32_t width,
                                           uint32_t height, uint32_t lineHeight)
{
    const uint32_t lastLine = height > lineHeight ? height - lineHeight : 0;
    return {x < width ? x : width, y < lastLine ? y : lastLine};
}

} // namespace brook
