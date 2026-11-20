#pragma once
// Shell-surface layouts and display list builders (compositor refactor stage
// 1.2): taskbar, launcher, tray flyouts, boot toast and performance HUD. The WM snapshots its state into the models below
// (wm_shell_*.inc); the builders read only the model and the style. These
// are the pieces a user-mode shell takes over at stage 3. Freestanding.
#include <stdint.h>
#include "chrome_style.h"
#include "desktop_telemetry.h"
#include "thermal_sampler.h"
#include "taskbar_volume.h"

#include "chrome_tray.h"

namespace brook {

// Logical sizes at 100% (window.h's WM_TASKBAR_* and the launcher's
// LAUNCHER_* are these values). Taskbar targets (stage 2.1, DESIGN.md
// "Sizes"): the full 48 px height and abutting, the launcher button 56 wide,
// window buttons 48 to 188 (sized to their label); each draws a visual pill
// inset 5 top and bottom and 2 each side (38 high), so a click anywhere in
// the column hits it. Task icons are 20, labels 12 px with 12 px padding.
struct ChromeShellMetrics {
    static constexpr uint32_t TaskbarButtonWidth = 48, TaskbarButtonHeight = 48, TaskbarPadding = 0;
    static constexpr uint32_t TaskbarMaxButtonWidth = 188, TaskbarLauncherWidth = 56, TaskbarPillInsetX = 2, TaskbarPillInsetY = 5;
    static constexpr uint32_t TaskbarIcon = 20, TaskbarButtonPad = 12, TaskbarLabelGap = 8, TaskbarLabelMin = 72;
    static constexpr uint32_t PanelMargin = 8; // shell panels: from the screen edge and the taskbar
    static constexpr uint32_t LauncherIconSize = 24; // icon sources (24x24 ARGB); the launcher is chrome_launcher.h
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
    DesktopRect apps, terminal, next, clock, network, volume, agent, build, hud;
    DesktopRect separators[2]; // after the launcher button; before the tray
    DesktopRect metrics[7];
    DesktopRect trayApps[TrayItemLimit];
    uint32_t trayAppCount;
    DesktopRect buttons[ChromeTaskbarButtonLimit];
    int indices[ChromeTaskbarButtonLimit];
    int count, nextIndex;
    char clockText[32], clockDate[24], buildText[32];
};

// ids: the taskbar entries in order (WM window indices, or -10 - launch
// slot); focused selects the page. agent: the agent indicator is shown.
// labels (optional, total entries): each entry's label; buttons are sized to
// them while every one fits, and share the space equally otherwise.
ChromeTaskbarLayout ChromeTaskbarLayoutFor(const ChromeStyle& s, uint32_t screenW, uint32_t screenH,
                                           const char* clockText, bool agent, uint32_t enabledMetrics,
                                           const int* ids, int total, int focused, const char* buildText = nullptr,
                                           bool compactHud = false, const char* const* labels = nullptr,
                                           uint32_t trayAppCount = 0);

struct ChromeTaskbarButton {
    const uint32_t* icon;   // borrowed; null: missing glyph
    const char* hoverTitle; // window label; launch state is held by the WM
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
    const ChromeTrayState* tray = nullptr;
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
    const char* detail = nullptr;
};
// The tooltip's padded box for its text (12 px labels).
inline int ChromeTooltipPadding(const ChromeStyle& s) { return static_cast<int>(s.M(8)); }
inline int ChromeTooltipWidth(const ChromeStyle& s, const char* text, const char* detail) {
    const int a = s.TextWidth(text, ChromeFont::Label), b = detail ? s.TextWidth(detail, ChromeFont::Label) : 0;
    return (a > b ? a : b) + 2 * ChromeTooltipPadding(s);
}
inline int ChromeTooltipHeight(const ChromeStyle& s, bool detail) {
    return s.Line(ChromeFont::Label) * (detail ? 2 : 1) + static_cast<int>(s.M(10));
}
// Width of the volume popup's "100%" readout.
inline int ChromeVolumeValueWidth(const ChromeStyle& s) { return s.TextWidth("100%", ChromeFont::Label); }
void ChromeBuildTooltip(const ChromeTooltipModel& model, const ChromeStyle& style, ChromeDisplayList& out);

// The launcher is chrome_launcher.h.

// ---------------------------------------------------------------------------
// Boot summary
// ---------------------------------------------------------------------------

struct ChromeBootSummaryModel {
    DesktopRect rect;
    const char* total;       // hero readout, e.g. "8.12 s"; null: unknown
    const char* totalNote;   // e.g. "from power-on"
    const char* meta[3];     // machine and composition lines, then the build label (monospace); null: skipped
    uint64_t phaseMs[4]{};   // firmware, menu, loader, kernel
    const char* phaseNames[4]{};  // null: no phase bar or legend
    const char* phaseValues[4]{};
};
// The toast's height for its content (it is 360 wide, DESIGN.md "Geometry").
int ChromeBootSummaryHeight(const ChromeStyle& style, bool total, bool phases, unsigned metaLines);
void ChromeBuildBootSummary(const ChromeBootSummaryModel& model, const ChromeStyle& style, ChromeDisplayList& out);

struct ChromeTrayFlyoutModel {
    DesktopRect rect;
    bool network, connected; // connected: the network has an address
    const char* readout;     // network: the address
    ChromeTelemetryModel telemetry;
};
// live: the network is connected, or a package temperature is available.
int ChromeTrayFlyoutHeight(const ChromeStyle& style, bool network, bool live);
void ChromeBuildTrayFlyout(const ChromeTrayFlyoutModel&, const ChromeStyle&, ChromeDisplayList&);

struct ChromePerformanceHudModel {
    DesktopRect rect;
    ChromeTelemetryModel telemetry;
    uint64_t tick;
};
int ChromePerformanceHudHeight(const ChromeStyle& style, bool thermal);
void ChromeBuildCompactHud(const ChromePerformanceHudModel&, const ChromeStyle&, ChromeDisplayList&);
void ChromeBuildPerformanceHud(const ChromePerformanceHudModel&, const ChromeStyle&, ChromeDisplayList&);

} // namespace brook
