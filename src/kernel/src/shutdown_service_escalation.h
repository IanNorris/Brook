#pragma once
#include <stdint.h>

namespace brook {
// Bounded stop of system services during a checked shutdown/restart.
// Services get SIGTERM on entry, a second SIGTERM, then SIGKILL; after that
// the phase only waits for teardown (process cleanup is the kernel's job).
// A service that ignores or cannot act on SIGTERM (e.g. a renderer parked on
// an unknown present result) must never block the power transition forever.
struct ServiceStopPolicy {
    static constexpr uint64_t ResendTermMs = 3000;
    static constexpr uint64_t KillMs = 8000;
    enum class Action : uint8_t { None, Term, Kill };
    uint64_t enteredMs = 0;
    bool started = false, resent = false, killed = false;

    // Call on entry to the Services phase (the initial SIGTERM is sent there).
    void Begin(uint64_t nowMs) { enteredMs = nowMs; started = true; resent = false; killed = false; }
    // Called on each worker poll while services remain; returns at most one
    // action per escalation step.
    Action Poll(uint64_t nowMs) {
        if (!started) return Action::None;
        const uint64_t elapsed = nowMs - enteredMs;
        if (!killed && elapsed >= KillMs) { killed = true; resent = true; return Action::Kill; }
        if (!resent && elapsed >= ResendTermMs) { resent = true; return Action::Term; }
        return Action::None;
    }
    // Milliseconds until the forced stop, 0 once it has been issued.
    uint64_t MsUntilKill(uint64_t nowMs) const {
        if (!started || killed) return 0;
        const uint64_t elapsed = nowMs - enteredMs;
        return elapsed >= KillMs ? 0 : KillMs - elapsed;
    }
};
}
