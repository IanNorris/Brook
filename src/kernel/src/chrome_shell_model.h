#pragma once
// Owned presentation snapshots shared by the WM and the shell process.
// Coordinates are in screen pixels. No member borrows kernel storage.
#include "chrome_shell.h"
#include "chrome_launcher.h"

namespace brook {

enum class ChromeShellKind : uint8_t {
    Taskbar, Launcher, VolumePopup, Tooltip, BootSummary, TrayFlyout, PerformanceHud, Dock, None = 0xff,
};

struct ChromeShellConfig {
    BrookThemePalette palette;
    DesktopMetrics metrics;
    uint32_t materialOpacity;
    uint32_t glass;

    ChromeStyle Style(const ChromeFontSet& fonts, ChromeMaterialCache& material) const {
        if (glass)
            material.Ensure(palette.titleActive, palette.titleInactive, materialOpacity,
                            metrics.title(), metrics.pixels(8), metrics.border(), &palette, metrics.taskbar());
        return {palette, &metrics, &fonts, glass != 0, materialOpacity, glass ? &material : nullptr};
    }
};

struct ChromeShellIcon {
    uint32_t pixels[ChromeShellMetrics::LauncherIconSize * ChromeShellMetrics::LauncherIconSize];
    bool present;

    const uint32_t* View() const { return present ? pixels : nullptr; }
};

struct ChromeShellTelemetry {
    DesktopTelemetry stats;
    DesktopIntervals<> compositor, app;
    ThermalSample latest, points[ThermalGraphHistory];
    uint32_t tjmax, pointCount;
    bool thermal, rapl, hasCompositor, hasApp;

    ChromeTelemetryModel View() const {
        return {stats, hasCompositor ? &compositor : nullptr, hasApp ? &app : nullptr,
                thermal, rapl, tjmax, latest, points, pointCount};
    }
};

struct ChromeShellEntry {
    char label[64];
    bool active, minimized, launch, busy;
};

struct ChromeShellBoot {
    char total[32], totalNote[32], meta[3][96];
    uint64_t phaseMs[4];
    char phaseNames[4][16], phaseValues[4][16];

    ChromeBootSummaryModel View(DesktopRect rect) const {
        ChromeBootSummaryModel model{rect, Optional(total), Optional(totalNote),
            {Optional(meta[0]), Optional(meta[1]), Optional(meta[2])}};
        for (unsigned i = 0; i < 4; ++i) {
            model.phaseMs[i] = phaseMs[i];
            model.phaseNames[i] = Optional(phaseNames[i]);
            model.phaseValues[i] = Optional(phaseValues[i]);
        }
        return model;
    }

    static const char* Optional(const char* text) { return *text ? text : nullptr; }
};

struct ChromeShellModel {
    static constexpr uint32_t AppsIcon = ChromeTaskbarButtonLimit;
    static constexpr uint32_t TerminalIcon = AppsIcon + 1;
    static constexpr uint32_t EntryLimit = TerminalIcon + 1;
    static constexpr uint32_t IconLimit = LauncherLimits::Apps > EntryLimit ? LauncherLimits::Apps : EntryLimit;

    // Taskbar and launcher occupy separate surfaces and reuse these slots.
    ChromeShellIcon icons[IconLimit];
    ChromeShellEntry entries[EntryLimit];
    ChromeShellTelemetry telemetry, compactTelemetry;
    ChromeShellBoot boot;
    ChromeTaskbarLayout taskbar;
    ChromeDockLayout dock; // dock entries use icons[0..dock.count)
    LauncherModel launcherModel;
    ChromeLauncherFrame launcherFrame;
    char launcherBuild[64];
    ChromeVolumePopupModel volume;
    ChromeShellKind kind;
    DesktopRect rect;
    int32_t mouseX, mouseY;
    uint64_t uptimeMs, hudTick;
    uint32_t volumeControl;
    bool launcherOpen, networkConnected, volumeAvailable, agentShown, agentStale;
    bool shutdownActive, prompt, promptRestart, confirmFocused;
    bool network;
    bool dockPinned;
    char text[128], detail[64]; // launcher header, tooltip or flyout address
};

static_assert(__is_trivially_copyable(ChromeShellModel));
static_assert(ChromeShellModel::IconLimit >= ChromeDockEntryLimit, "dock entries use the icon slots");
static_assert(__is_trivially_copyable(ChromeShellConfig));

// Borrowed display-list assets remain valid while the model and style
// remain alive, through list execution or serialization.
inline void BuildShellModel(const ChromeShellModel& model, const ChromeStyle& style, ChromeDisplayList& list) {
    switch (model.kind) {
    case ChromeShellKind::None:
        break;
    case ChromeShellKind::Taskbar: {
        ChromeTaskbarButton buttons[ChromeTaskbarButtonLimit];
        for (int i = 0; i < model.taskbar.count; ++i) {
            const auto& entry = model.entries[i];
            buttons[i] = {model.icons[i].View(), entry.label, entry.active, entry.minimized, entry.launch, entry.busy};
        }
        ChromeBuildTaskbar({&model.taskbar, buttons, model.launcherOpen, model.icons[ChromeShellModel::AppsIcon].View(),
                           model.icons[ChromeShellModel::TerminalIcon].View(), model.networkConnected,
                           model.volumeControl, model.volumeAvailable, model.agentShown, model.agentStale,
                           model.telemetry.View(), model.uptimeMs, model.mouseX, model.mouseY}, style, list);
        if (model.taskbar.hud.w > 0)
            ChromeBuildCompactHud({model.taskbar.hud, model.compactTelemetry.View(), model.hudTick}, style, list);
        break;
    }
    case ChromeShellKind::Launcher: {
        const uint32_t count = model.launcherModel.AppCount();
        const uint32_t* icons[LauncherLimits::Apps];
        for (uint32_t i = 0; i < count; ++i) icons[i] = model.icons[i].View();
        const ChromeLauncherChrome chrome{icons, count, "brook", ChromeShellBoot::Optional(model.launcherBuild),
            ChromeShellBoot::Optional(model.text), model.prompt, model.promptRestart, model.confirmFocused,
            model.shutdownActive, model.mouseX, model.mouseY};
        ChromeBuildLauncher(model.launcherModel, chrome, model.launcherFrame, style, list);
        break;
    }
    case ChromeShellKind::VolumePopup:
        ChromeBuildVolumePopup(model.volume, style, list);
        break;
    case ChromeShellKind::Tooltip:
        ChromeBuildTooltip({model.rect, model.text, ChromeShellBoot::Optional(model.detail)}, style, list);
        break;
    case ChromeShellKind::BootSummary:
        ChromeBuildBootSummary(model.boot.View(model.rect), style, list);
        break;
    case ChromeShellKind::TrayFlyout:
        ChromeBuildTrayFlyout({model.rect, model.network, model.networkConnected, model.text, model.telemetry.View()},
                              style, list);
        break;
    case ChromeShellKind::PerformanceHud:
        ChromeBuildPerformanceHud({model.rect, model.telemetry.View(), model.hudTick}, style, list);
        break;
    case ChromeShellKind::Dock: {
        const uint32_t* icons[ChromeDockEntryLimit];
        const int count = model.dock.count < int(ChromeDockEntryLimit) ? model.dock.count : int(ChromeDockEntryLimit);
        for (int i = 0; i < count; ++i) icons[i] = model.icons[i].View();
        ChromeBuildDock({&model.dock, icons, model.text, model.dockPinned, model.mouseX, model.mouseY}, style, list);
        break;
    }
    }
}

struct WmShellSurface;
bool WmCaptureShellModel(const WmShellSurface&, uint32_t screenW, uint32_t screenH, uint64_t uptimeMs,
                         int32_t mouseX, int32_t mouseY, ChromeShellModel&);
ChromeShellConfig WmCaptureShellConfig();

} // namespace brook
