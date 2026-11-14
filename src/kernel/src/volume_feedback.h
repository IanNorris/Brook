#pragma once
#include <stdint.h>
#include "../../shared/brook_audio_control.h"

// Pure volume-change feedback policy shared by the window manager and host
// tests: decide when to play the short "pop" after a user volume change.
//  - Only user changes count (taskbar, Settings, shell `volume`), observed as
//    a change of AudioUserOutputControlSerial().
//  - Muting, or a level of 0, never pops and cancels a pending pop.
//  - At most one pop per DebounceMs. While a slider is dragged that gives a
//    pop every ~150 ms; the last level after release still pops once the
//    debounce allows, so a step or a release always ends with a pop at the
//    new level. The pop goes through the mixer, so it plays at that level.
namespace brook {
struct VolumeFeedback {
    static constexpr uint64_t DebounceMs = 150;
    bool pending = false, popped = false;
    uint64_t lastPopMs = 0;
    static bool Audible(uint32_t control) {
        return !(control & BROOK_AUDIO_MUTED) && brook_audio_volume(control) != 0;
    }
    void Change(uint32_t previous, uint32_t next) {
        previous &= 0x17fu; next &= 0x17fu;
        if (!Audible(next)) { pending = false; return; }
        if (next != previous) pending = true;
    }
    // Returns true when the pop should start now.
    bool Tick(uint64_t nowMs) {
        if (!pending) return false;
        if (popped && nowMs - lastPopMs < DebounceMs) return false;
        pending = false; popped = true; lastPopMs = nowMs;
        return true;
    }
};
}
