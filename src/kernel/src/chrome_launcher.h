#pragma once
// Clearwater launcher surface (DESIGN.md "Launcher organisation", "Geometry"):
// layout, hit targets and display list for a LauncherModel. The layout is
// shared by the builder and the pointer hit test, so what is drawn is what is
// hit. Freestanding: the kernel WM and the stage 3 shell process both build
// with it.
//
//   search field (44)
//   PINNED | RECENT tabs, five tiles a row (32 px icons)
//   ------------------------------------------------------------
//   rail (180: All apps, categories, Unsorted) | app list (48 rows)
//   ------------------------------------------------------------
//   footer (52): identity or status | Settings, Restart, Shut down
#include <stdint.h>
#include "chrome_style.h"
#include "launcher_model.h"

namespace brook {

struct ChromeLauncherMetrics {
    // Logical px at 100% (DESIGN.md "Geometry": launcher 680x648, search 44,
    // pinned tiles with 32 icons, rail 180 with 40 rows, app rows 48 with 28
    // icons, footer 52; menu 228 wide with 36 items).
    static constexpr int Width = 680, Height = 648, Pad = 12, Search = 44, Tab = 32, Tile = 76, TileIcon = 32;
    static constexpr int TilesPerRow = 5, Rail = 180, RailRow = 40, Row = 48, RowIcon = 28, Heading = 36;
    static constexpr int Chips = 44, Empty = 96, Footer = 52, Button = 44, ButtonFace = 36;
    static constexpr int Menu = 228, MenuItem = 36, MenuPad = 6;
};

// Everything besides the model: icons, identity, the power prompt (owned by
// the WM's LauncherPowerGuard) and the pointer.
struct ChromeLauncherChrome {
    const uint32_t* const* icons; // per catalog index, 24x24 straight-alpha ARGB; null: missing glyph
    uint32_t iconCount;
    const char* user;             // footer identity
    const char* build;            // footer build label (monospace); may be null
    const char* status;           // replaces the identity: launch errors, shutdown progress; may be null
    bool prompt, promptRestart, confirmFocused, shutdownActive;
    int32_t mouseX, mouseY;
};

struct ChromeLauncherHit {
    DesktopRect rect;
    LauncherTarget target;
};

struct ChromeLauncherFrame {
    static constexpr uint32_t TargetLimit = 160, LineLimit = 32;
    DesktopRect panel, search, top, mid, rail, list, footer, menu;
    LauncherViewport viewport;    // for LauncherModel::SetViewport
    // Visible list lines, from the model's scroll position.
    LauncherLine lines[LineLimit];
    DesktopRect lineRects[LineLimit];
    uint32_t lineCount, firstLine, totalLines;
    uint16_t tiles[LauncherLimits::Pins];
    uint32_t tileCount;
    ChromeLauncherHit targets[TargetLimit]; // menu first, then panel contents; the last is the panel
    uint32_t targetCount;
};

// The launcher's rectangle for a screen (also the shell surface rectangle).
DesktopRect ChromeLauncherRect(const ChromeStyle& s, uint32_t screenW, uint32_t screenH);
void ChromeLauncherFrameFor(const LauncherModel& model, const ChromeLauncherChrome& chrome, const ChromeStyle& s,
                          uint32_t screenW, uint32_t screenH, ChromeLauncherFrame& out);
// The target under (x, y): None outside the panel.
LauncherTarget ChromeLauncherHitTest(const ChromeLauncherFrame& frame, int32_t x, int32_t y);
void ChromeBuildLauncher(const LauncherModel& model, const ChromeLauncherChrome& chrome, const ChromeLauncherFrame& frame,
                         const ChromeStyle& s, ChromeDisplayList& dl);

// The category colours (DESIGN.md "Token sheet": the same in every theme).
uint32_t ChromeLauncherCategoryColor(LauncherColor color);

} // namespace brook
