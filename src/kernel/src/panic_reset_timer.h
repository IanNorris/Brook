#pragma once
#include <stdint.h>

namespace brook {
// Panic-only PIT channel 2 delay. The caller must halt the other CPUs, own the
// PIT, and decide that a reset is permitted before invoking this function.
// It does not authorize reset during storage repair or capture crash data.
// Each completed interval is about 50 ms (59659 / 1193182 seconds). A failed
// timer never causes an early reset. A returned reset write is not proof that
// the machine restarted, and it is never retried here.
enum class PanicResetResult { Invalid, TimerFailed, ResetReturned };

template<class Io>
PanicResetResult PanicResetAfterIntervals(Io& io, uint32_t intervals, uint32_t pollLimit) {
    constexpr uint16_t ticks = 59659;
    if (!intervals || intervals > 12000 || !pollLimit || pollLimit > 100000000)
        return PanicResetResult::Invalid;
    const uint8_t saved = io.readGate();
    const uint8_t stopped = uint8_t(saved & ~3u); // gate low, speaker off
    io.writeGate(stopped);
    for (uint32_t n = 0; n < intervals; ++n) {
        io.writeControl(0xb0); // channel 2, lsb/msb, one-shot mode 0
        io.writeCounter(uint8_t(ticks));
        io.writeCounter(uint8_t(ticks >> 8));
        bool low = false;
        for (uint32_t i = 0; i < pollLimit; ++i) {
            if (!(io.readGate() & 0x20)) { low = true; break; }
            io.pause();
        }
        if (!low) { io.writeGate(saved); return PanicResetResult::TimerFailed; }
        io.writeGate(uint8_t(stopped | 1u));
        bool high = false;
        for (uint32_t i = 0; i < pollLimit; ++i) {
            if (io.readGate() & 0x20) { high = true; break; }
            io.pause();
        }
        if (!high) { io.writeGate(saved); return PanicResetResult::TimerFailed; }
        io.writeGate(stopped);
    }
    io.issueReset();
    io.writeGate(saved);
    return PanicResetResult::ResetReturned;
}

// ACPI PM timer fallback: 24 low bits tick at 3,579,545 Hz. Each interval is
// at least 50 ms; splitting it avoids ambiguity across the 24-bit wrap.
// A stalled timer returns without resetting. The caller owns reset authority.
template<class Io>
PanicResetResult PanicResetAfterPmIntervals(Io& io,uint32_t intervals,uint32_t pollLimit) {
    constexpr uint32_t mask=0xffffffu, ticks=178978;
    if(!intervals || intervals>12000 || !pollLimit || pollLimit>100000000)return PanicResetResult::Invalid;
    for(uint32_t n=0;n<intervals;++n) {
        const uint32_t start=io.readTimer()&mask;bool elapsed=false;
        for(uint32_t i=0;i<pollLimit;++i) {
            const uint32_t now=io.readTimer()&mask;
            if(((now-start)&mask)>=ticks){elapsed=true;break;}
            io.pause();
        }
        if(!elapsed)return PanicResetResult::TimerFailed;
    }
    io.issueReset();return PanicResetResult::ResetReturned;
}
}
