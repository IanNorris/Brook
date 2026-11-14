#pragma once
#include <stdint.h>
#include "input.h"

// Desktop-wide keyboard shortcuts, decided before any focused client, raw
// grabber or pointer-capturing game sees a key. Pure state machine: the
// compositor feeds every key event and performs the returned action.
//
// Invariants:
//  - Super (either side) never reaches a client, press or release.
//  - Super key-down always requests pointer-capture release, immediately.
//  - A Super tap (press then release with no other key or click in between)
//    toggles the launcher; Super used as a modifier does not.
//  - A consumed press also consumes its matching release, so a client never
//    sees an orphaned release for a key it never saw pressed. A release is
//    never swallowed for a press the client did receive.
namespace brook {

enum class GlobalShortcutAction : uint8_t {
    None,
    ToggleLauncher,
    CloseLauncher,
    CloseVolumePopup,
    CloseFocusedWindow,
    CycleFocus,
    SpawnTerminal,
    ToggleGlass,
};

struct GlobalShortcutResult {
    bool deliver = true;          // route the event to clients as usual
    bool releasePointer = false;  // revoke pointer capture and show the cursor
    GlobalShortcutAction action = GlobalShortcutAction::None;
};

struct GlobalShortcutContext {
    bool launcherVisible = false;
    bool volumePopupVisible = false;
};

class GlobalShortcuts {
public:
    static constexpr uint8_t ScEscape = 0x01, ScTab = 0x0F, ScQ = 0x10,
        ScEnter = 0x1C, ScG = 0x22, ScF4 = 0x3E;

    static bool IsSuper(uint8_t scan) { return scan == SC_LSUPER || scan == SC_RSUPER; }

    GlobalShortcutResult Key(bool press, uint8_t scan, uint8_t mods,
                             const GlobalShortcutContext& context) {
        GlobalShortcutResult r{};
        if (IsSuper(scan)) {
            const uint8_t bit = scan == SC_LSUPER ? 1 : 2;
            r.deliver = false;
            if (press) {
                // Auto-repeat re-sends the press; only a fresh hold starts a tap.
                if (!m_super) m_superChord = false;
                m_super |= bit;
                r.releasePointer = true;
            } else {
                const bool held = (m_super & bit) != 0;
                m_super &= static_cast<uint8_t>(~bit);
                if (held && !m_super && !m_superChord)
                    r.action = GlobalShortcutAction::ToggleLauncher;
            }
            return r;
        }
        if (!press) {
            if (Swallowed(scan)) { SetSwallowed(scan, false); r.deliver = false; }
            return r;
        }
        if (m_super) m_superChord = true;
        const bool super = m_super || (mods & INPUT_MOD_SUPER);
        const bool alt = (mods & INPUT_MOD_ALT) != 0;
        if (scan == ScEscape && context.volumePopupVisible) r.action = GlobalShortcutAction::CloseVolumePopup;
        else if (scan == ScEscape && context.launcherVisible) r.action = GlobalShortcutAction::CloseLauncher;
        else if (alt && scan == ScF4) r.action = GlobalShortcutAction::CloseFocusedWindow;
        else if (alt && scan == ScTab) r.action = GlobalShortcutAction::CycleFocus;
        else if (super && scan == ScEnter) r.action = GlobalShortcutAction::SpawnTerminal;
        else if (super && scan == ScQ) r.action = GlobalShortcutAction::CloseFocusedWindow;
        else if (super && scan == ScG) r.action = GlobalShortcutAction::ToggleGlass;
        if (r.action != GlobalShortcutAction::None || Swallowed(scan)) {
            // A repeat of a consumed key stays consumed (and does not re-fire
            // window actions); the release is swallowed once.
            if (Swallowed(scan) && r.action != GlobalShortcutAction::CycleFocus)
                r.action = GlobalShortcutAction::None;
            SetSwallowed(scan, true);
            r.deliver = false;
        }
        return r;
    }

    // A pointer button while Super is held makes it a chord, not a tap.
    void PointerButton() { if (m_super) m_superChord = true; }
    bool SuperHeld() const { return m_super != 0; }

private:
    bool Swallowed(uint8_t scan) const { return (m_swallowed[scan >> 5] >> (scan & 31)) & 1u; }
    void SetSwallowed(uint8_t scan, bool on) {
        const uint32_t bit = 1u << (scan & 31);
        if (on) m_swallowed[scan >> 5] |= bit; else m_swallowed[scan >> 5] &= ~bit;
    }
    uint32_t m_swallowed[8] = {};
    uint8_t m_super = 0;
    bool m_superChord = false;
};

} // namespace brook
