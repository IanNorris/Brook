#pragma once
// Shell-surface layouts and display list builders (compositor refactor stage
// 1.2): taskbar with tray and telemetry, launcher, volume popup, tray tooltip
// and boot summary. The WM snapshots its state into the models below
// (wm_shell_*.inc); the builders read only the model and the style. These
// are the pieces a user-mode shell takes over at stage 3. Freestanding.
#include <stdint.h>
#include "chrome_style.h"
#include "desktop_telemetry.h"
#include "thermal_sampler.h"
#include "taskbar_volume.h"
#include "launcher_power.h"

namespace brook {

// Logical sizes at 100% (window.h's WM_TASKBAR_* and the launcher's
// LAUNCHER_* are these values). Taskbar targets (stage 2.1, DESIGN.md
// "Sizes"): the full 48 px height and abutting, the launcher button 56 wide,
// window buttons 48; each draws a visual pill inset 5 top and bottom and 2
// each side (38 high), so a click anywhere in the column hits it.
struct ChromeShellMetrics {
    static constexpr uint32_t TaskbarButtonWidth = 48, TaskbarButtonHeight = 48, TaskbarPadding = 0;
    static constexpr uint32_t TaskbarLauncherWidth = 56, TaskbarPillInsetX = 2, TaskbarPillInsetY = 5;
    static constexpr uint32_t LauncherIconSize = 24, LauncherIconMargin = 8, LauncherItemHeight = 40;
    static constexpr uint32_t LauncherItemWidth = 230, LauncherPadding = 6, LauncherMaxRows = 14, LauncherMaxCols = 3;
};

inline void DesktopNumber(char* out, unsigned value) {
    char rev[12]; unsigned n = 0;
    do { rev[n++] = '0' + value % 10; value /= 10; } while (value && n < sizeof(rev));
    while (n) *out++ = rev[--n];
    *out = 0;
}
// Decimal MB/s, one fractional digit. Never calls the potentially locking printf.
inline void DesktopRate(char* out, uint64_t bytes) {
    uint64_t tenths = bytes / 100000; if (tenths > 99999) tenths = 99999;
    DesktopNumber(out, static_cast<unsigned>(tenths / 10)); while (*out) ++out;
    *out++ = '.'; *out++ = '0' + tenths % 10; *out = 0;
}

// ---------------------------------------------------------------------------
// Taskbar
// ---------------------------------------------------------------------------

static constexpr uint32_t ChromeTaskbarButtonLimit = 40; // windows + launch feedback entries

struct ChromeTaskbarLayout {
    int y, textY, screenW, screenH;
    DesktopRect apps, terminal, next, clock, network, volume, agent;
    DesktopRect metrics[7];
    DesktopRect buttons[ChromeTaskbarButtonLimit];
    int indices[ChromeTaskbarButtonLimit];
    int count, nextIndex;
    char clockText[32];
};

// ids: the taskbar entries in order (WM window indices, or -10 - launch
// slot); focused selects the page. agent: the agent indicator is shown.
ChromeTaskbarLayout ChromeTaskbarLayoutFor(const ChromeStyle& s, uint32_t screenW, uint32_t screenH,
                                           const char* clockText, bool agent, uint32_t enabledMetrics,
                                           const int* ids, int total, int focused);

struct ChromeTaskbarButton {
    const uint32_t* icon;   // borrowed; null: missing glyph
    const char* hoverTitle; // shown in the tray while hovered
    bool active, minimized; // window entries
    bool launch, busy;      // launch feedback entries: progress ring, or "!"
};

struct ChromeTelemetryModel {
    DesktopTelemetry stats;
    const DesktopIntervals<>* compositor; // COMP cadence (borrowed)
    const DesktopIntervals<>* app;        // APP cadence (borrowed)
    bool thermal;                         // a package temperature is available
    bool rapl;                            // package power is sampled
    uint32_t tjmax;
    ThermalSample latest;
    const ThermalSample* points;          // 1 s graph points (borrowed)
    uint32_t pointCount;
};

struct ChromeTaskbarModel {
    const ChromeTaskbarLayout* layout;
    const ChromeTaskbarButton* buttons; // layout->count entries
    bool launcherOpen;
    const uint32_t* appsIcon;
    const uint32_t* terminalIcon;
    bool networkConnected;
    uint32_t volumeControl;
    bool volumeAvailable;
    bool agentShown, agentStale;
    ChromeTelemetryModel telemetry;
    uint64_t uptimeMs;
    int32_t mouseX, mouseY;
};

void ChromeBuildTaskbar(const ChromeTaskbarModel& model, const ChromeStyle& style, ChromeDisplayList& out);

// The speaker glyph shared by the tray icon and the popup's mute button.
void ChromeBuildVolumeGlyph(ChromeDisplayList& dl, const ChromeStyle& s, int x0, int y0, int size,
                            uint32_t control, bool available, uint32_t bg);

// ---------------------------------------------------------------------------
// Volume popup and tray tooltip
// ---------------------------------------------------------------------------

struct ChromeVolumePopupModel {
    TaskbarVolumePopupLayout layout;
    uint32_t control;
    bool available, dragging;
    int32_t mouseX, mouseY;
};
void ChromeBuildVolumePopup(const ChromeVolumePopupModel& model, const ChromeStyle& style, ChromeDisplayList& out);

struct ChromeTooltipModel {
    DesktopRect box;
    const char* text;
};
// The tooltip's padded box for its text.
inline int ChromeTooltipPadding(const ChromeStyle& s) { return static_cast<int>(s.M(8)); }
void ChromeBuildTooltip(const ChromeTooltipModel& model, const ChromeStyle& style, ChromeDisplayList& out);

// ---------------------------------------------------------------------------
// Launcher
// ---------------------------------------------------------------------------

struct ChromeLauncherLayout {
    int x, y;
    uint32_t w, h, cols, rows, itemW, capacity;
};
// total: valid launcher items.
ChromeLauncherLayout ChromeLauncherLayoutFor(const ChromeStyle& s, uint32_t screenW, uint32_t screenH, uint32_t total);
DesktopRect ChromeLauncherItemRect(const ChromeStyle& s, const ChromeLauncherLayout& l, uint32_t slot);
LauncherPowerMetrics ChromeLauncherPowerMetrics(const ChromeStyle& s);
inline LauncherPowerLayout ChromeLauncherPowerLayout(const ChromeStyle& s, const ChromeLauncherLayout& l) {
    return LauncherPowerFooter({l.x, l.y, static_cast<int>(l.w), static_cast<int>(l.h)}, ChromeLauncherPowerMetrics(s));
}

struct ChromeLauncherItem {
    const char* title;
    const uint32_t* icon; // borrowed; null: missing glyph
};
struct ChromeLauncherModel {
    ChromeLauncherLayout layout;
    const char* header;
    const ChromeLauncherItem* items; // the visible items, in slot order
    uint32_t count;
    bool scrollUp, scrollDown;       // hints: items above / below the page
    bool shutdownActive;             // the footer offers only "Cancel shutdown"
    bool prompt, promptRestart, confirmFocused;
    int32_t mouseX, mouseY;
};
void ChromeBuildLauncher(const ChromeLauncherModel& model, const ChromeStyle& style, ChromeDisplayList& out);

// ---------------------------------------------------------------------------
// Boot summary
// ---------------------------------------------------------------------------

struct ChromeBootSummaryModel {
    DesktopRect rect;
    const char* rows[5]; // headline, timing, machine, composition, build
};
void ChromeBuildBootSummary(const ChromeBootSummaryModel& model, const ChromeStyle& style, ChromeDisplayList& out);

} // namespace brook
