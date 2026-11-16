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

// Bounded stop of windowless user processes while closing applications.
// Advisory window close cannot reach a process with no window (a daemon or
// helper started over the bridge, a lingering launcher child, an app that
// closed its window but hangs on exit). Once the close requests are out and
// no client window is open, such processes get GraceMs to exit, then
// SIGTERM, then SIGKILL after KillMs (repeated every RekillMs for late
// descendants). While any client window is open nothing is escalated, so an
// unsaved-document prompt stays the user's decision; a window that appears
// again sends the policy back to Idle and the full grace period restarts.
struct WindowlessStopPolicy {
    static constexpr uint64_t GraceMs = 10000;
    static constexpr uint64_t KillMs = 5000;
    static constexpr uint64_t RekillMs = 2000;
    enum class Stage : uint8_t { Idle, Grace, Terminated, Killed };
    enum class Action : uint8_t { None, Term, Kill };
    Stage stage = Stage::Idle;
    uint64_t since = 0, lastKill = 0;

    void Reset() { stage = Stage::Idle; since = lastKill = 0; }
    // eligible: every initial window received its close request, no client
    // window is open, no launch is being dispatched, and at least one
    // windowless user process remains.
    Action Poll(uint64_t nowMs, bool eligible) {
        if (!eligible) { stage = Stage::Idle; return Action::None; }
        switch (stage) {
        case Stage::Idle: stage = Stage::Grace; since = nowMs; return Action::None;
        case Stage::Grace:
            if (nowMs - since < GraceMs) return Action::None;
            stage = Stage::Terminated; since = nowMs; return Action::Term;
        case Stage::Terminated:
            if (nowMs - since < KillMs) return Action::None;
            stage = Stage::Killed; since = lastKill = nowMs; return Action::Kill;
        case Stage::Killed:
            if (nowMs - lastKill < RekillMs) return Action::None;
            lastKill = nowMs; return Action::Kill;
        }
        return Action::None;
    }
    // Milliseconds until the next escalation step (0 when none is pending).
    uint64_t MsUntilNext(uint64_t nowMs) const {
        const uint64_t elapsed = nowMs - since;
        const uint64_t bound = stage == Stage::Grace ? GraceMs : stage == Stage::Terminated ? KillMs : 0;
        return bound && elapsed < bound ? bound - elapsed : 0;
    }
    const char* StageName() const {
        switch (stage) {
        case Stage::Idle: return "waiting";
        case Stage::Grace: return "grace";
        case Stage::Terminated: return "sigterm";
        case Stage::Killed: return "sigkill";
        }
        return "waiting";
    }
};
}
