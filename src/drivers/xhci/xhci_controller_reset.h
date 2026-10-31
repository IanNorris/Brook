#pragma once
#include <stdint.h>

namespace brook {
enum class XhciResetPhase { Initial, Halt, IntelQuiet, Reset, Ready, Complete };
enum class XhciResetError { None, Inaccessible, Timeout, ClockStopped, InitiallyNotReady };
struct XhciResetResult {
    XhciResetPhase phase;
    XhciResetError error;
    uint32_t command;
    uint32_t status;
};

// The IO adapter owns MMIO and a monotonic millisecond clock. Keep all waiting
// bounded even when early-boot interrupts or the clock are unavailable.
template<class Io>
XhciResetResult XhciResetController(Io& io, bool intel, uint64_t phaseTimeoutMs,
                                   uint64_t maxPolls) {
    constexpr uint32_t run = 1u, reset = 2u, interruptEnable = 4u;
    constexpr uint32_t halted = 1u, notReady = 1u << 11;
    XhciResetResult r{XhciResetPhase::Initial, XhciResetError::None, 0, 0};
    r.status = io.status();
    if (r.status == ~uint32_t{0}) {
        r.error = XhciResetError::Inaccessible; return r;
    }
    // Do not write operational registers while a firmware reset is active.
    if (r.status & notReady) { r.error = XhciResetError::InitiallyNotReady; return r; }
    r.command = io.command();
    if (r.command == ~uint32_t{0}) { r.error = XhciResetError::Inaccessible; return r; }
    if (r.command & reset) { r.error = XhciResetError::InitiallyNotReady; return r; }
    io.writeCommand(r.command & ~(run | interruptEnable));
    auto wait = [&](XhciResetPhase phase, bool checkCommand, uint32_t mask,
                    bool wantSet, uint64_t minimumMs) {
        r.phase = phase;
        const uint64_t start = io.now();
        for (uint64_t n = 0; n < maxPolls; ++n) {
            const uint64_t now = io.now();
            if (now < start) { r.error = XhciResetError::ClockStopped; return false; }
            if (minimumMs) {
                // No controller access in the Intel post-reset quiet interval.
                if (now - start >= minimumMs) return true;
            } else {
                uint32_t value = checkCommand ? io.command() : io.status();
                if (checkCommand) r.command = value; else r.status = value;
                if (value == ~uint32_t{0}) { r.error = XhciResetError::Inaccessible; return false; }
                if (((value & mask) != 0) == wantSet) return true;
            }
            if (now - start >= phaseTimeoutMs) { r.error = XhciResetError::Timeout; return false; }
            io.pause();
        }
        r.error = XhciResetError::ClockStopped; return false;
    };
    if (!wait(XhciResetPhase::Halt, false, halted, true, 0)) return r;
    io.writeCommand(reset);
    // Two tick boundaries guarantee at least one complete millisecond even
    // when this starts just before a coarse one-millisecond clock tick.
    if (intel && !wait(XhciResetPhase::IntelQuiet, false, 0, false, 2)) return r;
    if (!wait(XhciResetPhase::Reset, true, reset, false, 0)) return r;
    if (!wait(XhciResetPhase::Ready, false, notReady, false, 0)) return r;
    r.phase = XhciResetPhase::Complete;
    return r;
}
}
