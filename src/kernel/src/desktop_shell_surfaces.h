#pragma once
#include <stdint.h>
#include "window.h"
#include "chrome_layout.h"
#include "taskbar_popup_pin.h"
#include "../../shared/desktop_layers_abi.h"

namespace brook {
// Shell surfaces: the one list of desktop overlay surfaces (taskbar, the
// shared popup slot, the side dock, the boot toast and performance HUD) that every compositor path draws,
// composes, blurs and captures: the CPU back buffer, the virgl overlay layer
// and the VKRENDER overlay capture. Add a new shell surface here, not in the
// paths. Material surfaces take the translucent shell material when glass is
// active; the others stay opaque.
enum class WmShellSurfaceKind : uint8_t {
    Taskbar, Launcher, VolumePopup, TrayTooltip, BootSummary, TrayFlyout, PerformanceHud,
    AppTrayMenu, AppTrayTooltip, Notification, Dock,
    TemperatureFlyout, DiskFlyout, CpuMemoryFlyout, CompositorFlyout, AppFlyout, CalendarFlyout, BuildFlyout,
    Count
};
inline constexpr WmShellSurfaceKind TaskbarPopupSurfaceKinds[] = {
    WmShellSurfaceKind::Count, WmShellSurfaceKind::VolumePopup, WmShellSurfaceKind::TrayFlyout,
    WmShellSurfaceKind::TemperatureFlyout, WmShellSurfaceKind::PerformanceHud, WmShellSurfaceKind::AppTrayMenu,
    WmShellSurfaceKind::DiskFlyout, WmShellSurfaceKind::CpuMemoryFlyout, WmShellSurfaceKind::CompositorFlyout,
    WmShellSurfaceKind::AppFlyout, WmShellSurfaceKind::CalendarFlyout, WmShellSurfaceKind::BuildFlyout,
};
static_assert(sizeof(TaskbarPopupSurfaceKinds) / sizeof(*TaskbarPopupSurfaceKinds) == unsigned(TaskbarPopup::Build) + 1);
inline TaskbarPopup WmTaskbarPopupForSurface(WmShellSurfaceKind kind) {
    for (unsigned i = 1; i <= unsigned(TaskbarPopup::Build); ++i)
        if (TaskbarPopupSurfaceKinds[i] == kind) return TaskbarPopup(i);
    return TaskbarPopup::None;
}
struct WmShellSurface {
    WmShellSurfaceKind kind;
    int32_t x, y;
    uint32_t w, h;
    bool material;
    int32_t popupShiftX = 0; // placed x minus the coordinates of its model
};
struct WmShellTrayGeometry {
    DesktopRect popup, notice;
    bool menu, windowMenu;
};
// Taskbar, dock, every independent popup, and a notice/boot toast.
static constexpr uint32_t WM_SHELL_SURFACE_LIMIT = 3 + unsigned(TaskbarPopup::Build);
uint32_t WmCollectTaskbarPopupSurfaces(uint32_t sw, uint32_t sh, WmShellSurface* out);

// Back to front. (mx,my) is the pointer the caller renders hover state with.
inline uint32_t WmShellSurfaces(uint32_t screenW, uint32_t screenH, int32_t mx, int32_t my,
                                WmShellSurface (&out)[WM_SHELL_SURFACE_LIMIT],
                                DesktopRect trayPopup = {}, bool trayMenu = false, DesktopRect notice = {}, bool windowMenu = false)
{
    uint32_t count = 0;
    // A focused fullscreen window owns the whole output (WmTaskbarHidden).
    if (!WmTaskbarHidden()) {
        const uint32_t tbY = WmDesktopHeight(screenH);
        out[count++] = {WmShellSurfaceKind::Taskbar, 0, static_cast<int32_t>(tbY), screenW, screenH - tbY, true};
    }
    // The side dock: its glass strip while shown, else a flat edge tab while
    // it has entries; nothing otherwise. Fullscreen covers it.
    {
        int32_t x = 0, y = 0; uint32_t w = 0, h = 0; bool material = false;
        if (!WmTaskbarHidden() && WmDockSurfaceRect(screenW, screenH, &x, &y, &w, &h, &material))
            out[count++] = {WmShellSurfaceKind::Dock, x, y, w, h, material};
    }
    // The launcher and window menu temporarily cover taskbar popups.
    int32_t x = 0, y = 0; uint32_t w = 0, h = 0;
    if (WmLauncherVisible()) {
        WmLauncherGetRect(screenW, screenH, &x, &y, &w, &h);
        out[count++] = {WmShellSurfaceKind::Launcher, x, y, w, h, true};
    } else if (windowMenu) {
        out[count++] = {WmShellSurfaceKind::AppTrayMenu, trayPopup.x, trayPopup.y,
                        uint32_t(trayPopup.w), uint32_t(trayPopup.h), true};
    } else if (!WmTaskbarHidden()) {
        const uint32_t popups = WmCollectTaskbarPopupSurfaces(screenW, screenH, out + count);
        count += popups;
        if (!popups && !trayMenu && trayPopup.w > 0 && trayPopup.h > 0)
            out[count++] = {WmShellSurfaceKind::AppTrayTooltip, trayPopup.x, trayPopup.y,
                            uint32_t(trayPopup.w), uint32_t(trayPopup.h), true};
        else if (!popups && WmTrayTooltipGetRect(screenW, screenH, mx, my, &x, &y, &w, &h))
            out[count++] = {WmShellSurfaceKind::TrayTooltip, x, y, w, h, true};
    }
    // One cached pixel layer serves every shell surface. Keep its semantic
    // rectangles disjoint so the GPU never samples the same pixels twice.
    auto freeSpace = [&] {
        for (uint32_t i = 0; i < count; ++i) {
            const auto& s = out[i];
            if (int64_t(x) < int64_t(s.x) + s.w && int64_t(s.x) < int64_t(x) + w &&
                int64_t(y) < int64_t(s.y) + s.h && int64_t(s.y) < int64_t(y) + h)
                return false;
        }
        return true;
    };
    if (notice.w > 0 && notice.h > 0) {
        x = notice.x; y = notice.y; w = notice.w; h = notice.h;
        if (freeSpace()) out[count++] = {WmShellSurfaceKind::Notification, x, y, w, h, true};
    } else if (WmBootSummaryRect(screenW, screenH, &x, &y, &w, &h) && freeSpace())
        out[count++] = {WmShellSurfaceKind::BootSummary, x, y, w, h, true};
    return count;
}

// Material, opacity, radii and shadow for the published list's content key.
BrookDesktopSurface WmShellSurfaceRecord(const WmShellSurface& surface, uint64_t key);
} // namespace brook
