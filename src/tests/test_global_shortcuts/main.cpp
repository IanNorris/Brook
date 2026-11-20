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
    r=down(s,GlobalShortcuts::ScE,INPUT_MOD_SUPER);CHECK(!r.deliver && r.action==Action::OpenFiles);
    r=down(s,GlobalShortcuts::ScE,INPUT_MOD_SUPER);CHECK(!r.deliver && r.action==Action::None);
    CHECK(!up(s,GlobalShortcuts::ScE).deliver);
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
    r = down(s, GlobalShortcuts::ScP, INPUT_MOD_ALT);
    CHECK(!r.deliver && r.action == Action::CyclePerformanceHud);
    r = down(s, GlobalShortcuts::ScP, INPUT_MOD_ALT);
    CHECK(!r.deliver && r.action == Action::None);
    CHECK(!up(s, GlobalShortcuts::ScP).deliver);
    CHECK(down(s, GlobalShortcuts::ScP).deliver && up(s, GlobalShortcuts::ScP).deliver);
    // Alt+Tab repeats cycle focus, and its release is swallowed.
    r=down(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT);CHECK(!r.deliver && r.action==Action::CycleFocus);
    r=down(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT);CHECK(!r.deliver && r.action==Action::CycleFocus);
    CHECK(!up(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT).deliver);
    CHECK(down(s,GlobalShortcuts::ScTab).deliver);
}

static void escape() {
    GlobalShortcuts s;GlobalShortcutContext launcher{};launcher.launcherVisible=true;
    // The open launcher owns Escape (it unwinds menu, query, then closes).
    auto r=down(s,GlobalShortcuts::ScEscape,0,launcher);CHECK(!r.deliver && r.action==Action::LauncherKey && r.key==GlobalShortcuts::ScEscape);
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
    // Other keys go to the open launcher; Alt+Tab keeps its meaning.
    {auto r=down(s,0x10,0,prompt);CHECK(!r.deliver && r.action==Action::LauncherKey);CHECK(!up(s,0x10,0,prompt).deliver);}
    CHECK(down(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT,prompt).action==Action::CycleFocus);
    CHECK(!up(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT,prompt).deliver);
    // Without the prompt, Enter is the launcher's (open the selected app),
    // never a power action.
    GlobalShortcutContext launcher{};launcher.launcherVisible=true;
    auto r=down(s,GlobalShortcuts::ScEnter,0,launcher);CHECK(!r.deliver && r.action==Action::LauncherKey);
    CHECK(!up(s,GlobalShortcuts::ScEnter,0,launcher).deliver);
}

static void launcherTyping() {
    GlobalShortcuts s;GlobalShortcutContext launcher{};launcher.launcherVisible=true;
    // Type-anywhere: letters, Backspace and arrows reach the launcher, repeats included.
    for(uint8_t sc:{uint8_t(0x10),uint8_t(0x0E),SC_EXT_DOWN}) {
        auto r=down(s,sc,0,launcher);CHECK(!r.deliver && r.action==Action::LauncherKey && r.key==sc);
        r=down(s,sc,0,launcher);CHECK(!r.deliver && r.action==Action::LauncherKey);
        CHECK(!up(s,sc,0,launcher).deliver);
    }
    // Modifiers are state: Shift reaches the client untouched.
    CHECK(down(s,0x2A,0,launcher).deliver && up(s,0x2A,0,launcher).deliver);
    // Alt+Enter (the app menu) is the launcher's; Alt+Tab, Alt+F4, Alt+P and Super are not.
    CHECK(down(s,GlobalShortcuts::ScEnter,INPUT_MOD_ALT,launcher).action==Action::LauncherKey);
    CHECK(!up(s,GlobalShortcuts::ScEnter,INPUT_MOD_ALT,launcher).deliver);
    CHECK(down(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT,launcher).action==Action::CycleFocus);
    CHECK(!up(s,GlobalShortcuts::ScTab,INPUT_MOD_ALT,launcher).deliver);
    CHECK(down(s,GlobalShortcuts::ScP,INPUT_MOD_ALT,launcher).action==Action::CyclePerformanceHud);
    CHECK(!up(s,GlobalShortcuts::ScP,INPUT_MOD_ALT,launcher).deliver);
    // A key held from before the launcher opened releases to its client.
    GlobalShortcutContext closed{};
    CHECK(down(s,0x11,0,closed).deliver && up(s,0x11,0,launcher).deliver);
}

// Side dock: Super+S toggles it, Super+Shift+S docks or returns the focused
// window, Escape hides an open unpinned dock (and is then not delivered).
static void dockKeys() {
    GlobalShortcuts s;
    using G=GlobalShortcuts;
    CHECK(!down(s,SC_LSUPER,INPUT_MOD_SUPER).deliver);
    auto r=down(s,G::ScS,INPUT_MOD_SUPER);CHECK(!r.deliver && r.action==Action::ToggleDock);
    r=down(s,G::ScS,INPUT_MOD_SUPER);CHECK(!r.deliver && r.action==Action::None); // auto-repeat: once
    CHECK(!up(s,G::ScS,INPUT_MOD_SUPER).deliver);
    r=down(s,G::ScS,INPUT_MOD_SUPER|INPUT_MOD_LSHIFT);CHECK(!r.deliver && r.action==Action::DockFocusedWindow);
    CHECK(!up(s,G::ScS,INPUT_MOD_SUPER|INPUT_MOD_LSHIFT).deliver);
    r=down(s,G::ScS,INPUT_MOD_SUPER|INPUT_MOD_RSHIFT);CHECK(r.action==Action::DockFocusedWindow);
    up(s,G::ScS);
    CHECK(up(s,SC_LSUPER).action==Action::None); // a chord, not a launcher tap
    // Plain S reaches the client.
    r=down(s,G::ScS);CHECK(r.deliver && r.action==Action::None);CHECK(up(s,G::ScS).deliver);
    // Escape belongs to an open, unpinned dock; otherwise to the client.
    GlobalShortcutContext dock{};dock.dockDismissible=true;
    r=down(s,G::ScEscape,0,dock);CHECK(!r.deliver && r.action==Action::CloseDock);
    CHECK(!up(s,G::ScEscape,0,dock).deliver);
    r=down(s,G::ScEscape);CHECK(r.deliver && r.action==Action::None);CHECK(up(s,G::ScEscape).deliver);
    // The volume popup's Escape comes first; the launcher owns its keys.
    GlobalShortcutContext both=dock;both.volumePopupVisible=true;
    CHECK(down(s,G::ScEscape,0,both).action==Action::CloseVolumePopup);up(s,G::ScEscape);
    // A window menu's Escape comes before both.
    GlobalShortcutContext menu=both;menu.windowMenuOpen=true;
    r=down(s,G::ScEscape,0,menu);CHECK(!r.deliver && r.action==Action::CloseWindowMenu);
    CHECK(!up(s,G::ScEscape,0,menu).deliver);
    GlobalShortcutContext launcher{};launcher.launcherVisible=true;
    CHECK(down(s,G::ScS,0,launcher).action==Action::LauncherKey);up(s,G::ScS);
    // Super+S still toggles the dock over the launcher.
    CHECK(down(s,SC_LSUPER,INPUT_MOD_SUPER,launcher).releasePointer);
    CHECK(down(s,G::ScS,INPUT_MOD_SUPER,launcher).action==Action::ToggleDock);
    up(s,G::ScS,INPUT_MOD_SUPER,launcher);up(s,SC_LSUPER,0,launcher);
}

static void taskManager()
{
    GlobalShortcuts shortcuts;
    GlobalShortcutContext overlays{};
    overlays.launcherVisible = overlays.launcherPrompt = overlays.volumePopupVisible =
        overlays.windowMenuOpen = overlays.dockDismissible = true;
    for (const auto& context : {none, overlays}) {
        for (uint8_t shift : {INPUT_MOD_LSHIFT, INPUT_MOD_RSHIFT}) {
            const uint8_t chord = INPUT_MOD_CTRL | shift;
            auto result = down(shortcuts, GlobalShortcuts::ScEscape, chord, context);
            CHECK(!result.deliver && result.releasePointer && result.action == Action::OpenTaskManager);
            result = down(shortcuts, GlobalShortcuts::ScEscape, chord, context);
            CHECK(!result.deliver && !result.releasePointer && result.action == Action::None);
            result = down(shortcuts, GlobalShortcuts::ScEscape, 0, context);
            CHECK(!result.deliver && result.action == Action::None); // modifiers released while Escape repeats
            CHECK(!up(shortcuts, GlobalShortcuts::ScEscape, 0, context).deliver);
        }
    }
    for (uint8_t mods : {uint8_t(0), INPUT_MOD_CTRL, INPUT_MOD_LSHIFT,
                         uint8_t(INPUT_MOD_ALT | INPUT_MOD_CTRL | INPUT_MOD_LSHIFT),
                         uint8_t(INPUT_MOD_SUPER | INPUT_MOD_CTRL | INPUT_MOD_LSHIFT)}) {
        CHECK(down(shortcuts, GlobalShortcuts::ScEscape, mods).deliver);
        CHECK(up(shortcuts, GlobalShortcuts::ScEscape).deliver);
    }
    CHECK(down(shortcuts, GlobalShortcuts::ScEscape).deliver);
    CHECK(down(shortcuts, GlobalShortcuts::ScEscape, INPUT_MOD_CTRL | INPUT_MOD_LSHIFT).deliver);
    CHECK(up(shortcuts, GlobalShortcuts::ScEscape).deliver);
    CHECK(down(shortcuts, GlobalShortcuts::ScEscape, 0, overlays).action == Action::LauncherPromptKey);
}

int main() {
    superTap();superChord();altF4();escape();launcherPrompt();launcherTyping();dockKeys();taskManager();
    std::puts("global shortcuts: ok");
    return 0;
}
