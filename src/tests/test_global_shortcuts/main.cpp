#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "global_shortcuts.h"
using namespace brook;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}} while(0)
using Action=GlobalShortcutAction;
static const GlobalShortcutContext none{};
static GlobalShortcutResult down(GlobalShortcuts& s,uint8_t sc,uint8_t mods=0,const GlobalShortcutContext& c=none){return s.Key(true,sc,mods,c);}
static GlobalShortcutResult up(GlobalShortcuts& s,uint8_t sc,uint8_t mods=0,const GlobalShortcutContext& c=none){return s.Key(false,sc,mods,c);}

static void superTap() {
    GlobalShortcuts s;
    auto r=down(s,SC_LSUPER,INPUT_MOD_SUPER);
    CHECK(!r.deliver && r.releasePointer && r.action==Action::None); // release capture on key-down
    r=down(s,SC_LSUPER,INPUT_MOD_SUPER); // auto-repeat
    CHECK(!r.deliver && r.releasePointer && r.action==Action::None);
    r=up(s,SC_LSUPER);
    CHECK(!r.deliver && !r.releasePointer && r.action==Action::ToggleLauncher);
    r=down(s,SC_RSUPER,INPUT_MOD_SUPER);CHECK(!r.deliver && r.releasePointer);
    r=up(s,SC_RSUPER);CHECK(!r.deliver && r.action==Action::ToggleLauncher);
    // A stray Super release (press lost) neither delivers nor toggles.
    r=up(s,SC_LSUPER);CHECK(!r.deliver && r.action==Action::None);
}

static void superChord() {
    GlobalShortcuts s;
    CHECK(!down(s,SC_LSUPER,INPUT_MOD_SUPER).deliver);
    // An unbound Super chord reaches the client as press/release pair.
    auto r=down(s,0x1E,INPUT_MOD_SUPER);CHECK(r.deliver && r.action==Action::None);
    r=up(s,0x1E,INPUT_MOD_SUPER);CHECK(r.deliver);
    r=up(s,SC_LSUPER);CHECK(!r.deliver && r.action==Action::None); // not a tap
    // Bound chords are consumed together with their release.
    CHECK(!down(s,SC_LSUPER,INPUT_MOD_SUPER).deliver);
    r=down(s,GlobalShortcuts::ScEnter,INPUT_MOD_SUPER);CHECK(!r.deliver && r.action==Action::SpawnTerminal);
    r=up(s,GlobalShortcuts::ScEnter,INPUT_MOD_SUPER);CHECK(!r.deliver && r.action==Action::None);
    r=down(s,GlobalShortcuts::ScQ,INPUT_MOD_SUPER);CHECK(!r.deliver && r.action==Action::CloseFocusedWindow);
    r=up(s,GlobalShortcuts::ScQ,INPUT_MOD_SUPER);CHECK(!r.deliver);
    r=down(s,GlobalShortcuts::ScG,INPUT_MOD_SUPER);CHECK(!r.deliver && r.action==Action::ToggleGlass);
    // Super released before G: G's release is still swallowed exactly once.
    CHECK(up(s,SC_LSUPER).action==Action::None);
    r=up(s,GlobalShortcuts::ScG);CHECK(!r.deliver);
    r=down(s,GlobalShortcuts::ScG);CHECK(r.deliver && r.action==Action::None);
    CHECK(up(s,GlobalShortcuts::ScG).deliver);
    // Both Supers: the tap fires only when the last one is released.
    CHECK(!down(s,SC_LSUPER,INPUT_MOD_SUPER).deliver);
    CHECK(!down(s,SC_RSUPER,INPUT_MOD_SUPER).deliver);
    CHECK(up(s,SC_LSUPER,INPUT_MOD_SUPER).action==Action::None);
    CHECK(up(s,SC_RSUPER).action==Action::ToggleLauncher);
    // A click while Super is held is a chord.
    CHECK(!down(s,SC_LSUPER,INPUT_MOD_SUPER).deliver);
    s.PointerButton();
    CHECK(up(s,SC_LSUPER).action==Action::None);
    s.PointerButton(); // without Super: no effect on the next tap
    down(s,SC_LSUPER,INPUT_MOD_SUPER);
    CHECK(up(s,SC_LSUPER).action==Action::ToggleLauncher);
    // A chord in one hold does not leak into the next hold.
    down(s,SC_LSUPER,INPUT_MOD_SUPER);down(s,0x1E,INPUT_MOD_SUPER);up(s,0x1E,INPUT_MOD_SUPER);
    CHECK(up(s,SC_LSUPER).action==Action::None);
    down(s,SC_LSUPER,INPUT_MOD_SUPER);
    CHECK(up(s,SC_LSUPER).action==Action::ToggleLauncher);
}

static void altF4() {
    GlobalShortcuts s;
    auto r=down(s,0x38,INPUT_MOD_ALT);CHECK(r.deliver); // Alt itself is ordinary
    r=down(s,GlobalShortcuts::ScF4,INPUT_MOD_ALT);
    CHECK(!r.deliver && !r.releasePointer && r.action==Action::CloseFocusedWindow);
    r=down(s,GlobalShortcuts::ScF4,INPUT_MOD_ALT); // repeat: no second close
    CHECK(!r.deliver && r.action==Action::None);
    r=up(s,0x38);CHECK(r.deliver);
    r=up(s,GlobalShortcuts::ScF4);CHECK(!r.deliver); // swallowed after Alt released
    r=down(s,GlobalShortcuts::ScF4);CHECK(r.deliver && r.action==Action::None); // plain F4
    CHECK(up(s,GlobalShortcuts::ScF4).deliver);
    // Alt+Tab repeats cycle focus, and its release is swallowed.
    r=down(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT);CHECK(!r.deliver && r.action==Action::CycleFocus);
    r=down(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT);CHECK(!r.deliver && r.action==Action::CycleFocus);
    CHECK(!up(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT).deliver);
    CHECK(down(s,GlobalShortcuts::ScTab).deliver);
}

static void escape() {
    GlobalShortcuts s;GlobalShortcutContext launcher{};launcher.launcherVisible=true;
    auto r=down(s,GlobalShortcuts::ScEscape,0,launcher);CHECK(!r.deliver && r.action==Action::CloseLauncher);
    r=up(s,GlobalShortcuts::ScEscape);CHECK(!r.deliver);
    GlobalShortcutContext volume{};volume.volumePopupVisible=true;volume.launcherVisible=true;
    r=down(s,GlobalShortcuts::ScEscape,0,volume);CHECK(!r.deliver && r.action==Action::CloseVolumePopup);
    CHECK(!up(s,GlobalShortcuts::ScEscape).deliver);
    r=down(s,GlobalShortcuts::ScEscape);CHECK(r.deliver && r.action==Action::None);
    CHECK(up(s,GlobalShortcuts::ScEscape).deliver);
    // Ordinary typing is untouched.
    CHECK(down(s,0x10).deliver && up(s,0x10).deliver);
}

static void launcherPrompt() {
    GlobalShortcuts s;GlobalShortcutContext prompt{};prompt.launcherVisible=true;prompt.launcherPrompt=true;
    // The prompt owns its keys; Escape dismisses it instead of closing the launcher.
    for(uint8_t sc:{GlobalShortcuts::ScEscape,GlobalShortcuts::ScEnter,GlobalShortcuts::ScTab,SC_EXT_LEFT,SC_EXT_RIGHT}) {
        auto r=down(s,sc,0,prompt);CHECK(!r.deliver && r.action==Action::LauncherPromptKey && r.key==sc);
        r=down(s,sc,0,prompt);CHECK(!r.deliver && r.action==Action::None); // auto-repeat does not re-fire
        CHECK(!up(s,sc,0,prompt).deliver);
    }
    // Other keys, and Alt+Tab, keep their meaning.
    CHECK(down(s,0x10,0,prompt).deliver && up(s,0x10,0,prompt).deliver);
    CHECK(down(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT,prompt).action==Action::CycleFocus);
    CHECK(!up(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT,prompt).deliver);
    // Without the prompt, Enter reaches the client: there is no keyboard path to power.
    GlobalShortcutContext launcher{};launcher.launcherVisible=true;
    auto r=down(s,GlobalShortcuts::ScEnter,0,launcher);CHECK(r.deliver && r.action==Action::None);
    CHECK(up(s,GlobalShortcuts::ScEnter,0,launcher).deliver);
}

int main() {
    superTap();superChord();altF4();escape();launcherPrompt();
    std::puts("global shortcuts: ok");
    return 0;
}
