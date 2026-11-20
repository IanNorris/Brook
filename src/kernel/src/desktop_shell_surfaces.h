#pragma once
#include <stdint.h>
#include "window.h"
#include "../../shared/desktop_layers_abi.h"

namespace brook {
// Shell surfaces: the one list of desktop overlay surfaces (taskbar, the
// shared popup slot, the side dock, the boot toast and performance HUD) that every compositor path draws,
// composes, blurs and captures: the CPU back buffer, the virgl overlay layer
// and the VKRENDER overlay capture. Add a new shell surface here, not in the
// paths. Material surfaces take the translucent shell material when glass is
// active; the others stay opaque.
enum class WmShellSurfaceKind : uint8_t { Taskbar, Launcher, VolumePopup, TrayTooltip, BootSummary, TrayFlyout, PerformanceHud, Dock };
struct WmShellSurface {
    WmShellSurfaceKind kind;
    int32_t x, y;
    uint32_t w, h;
    bool material;
};
// Taskbar, dock (strip or edge tab), popup slot, boot toast and optional
// expanded HUD.
static constexpr uint32_t WM_SHELL_SURFACE_LIMIT = 5;

// Back to front. (mx,my) is the pointer the caller renders hover state with.
inline uint32_t WmShellSurfaces(uint32_t screenW, uint32_t screenH, int32_t mx, int32_t my,
                                WmShellSurface (&out)[WM_SHELL_SURFACE_LIMIT])
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
    // One popup slot: opening the launcher closes the volume popup and vice
    // versa, and the tray tooltip yields to either of them.
    int32_t x = 0, y = 0; uint32_t w = 0, h = 0;
    if (WmLauncherVisible()) {
        WmLauncherGetRect(screenW, screenH, &x, &y, &w, &h);
        out[count++] = {WmShellSurfaceKind::Launcher, x, y, w, h, true};
    } else if (WmVolumePopupVisible()) {
        WmVolumePopupGetRect(screenW, screenH, &x, &y, &w, &h);
        out[count++] = {WmShellSurfaceKind::VolumePopup, x, y, w, h, true};
    } else if (WmTrayFlyoutRect(screenW, screenH, &x, &y, &w, &h)) {
        out[count++] = {WmShellSurfaceKind::TrayFlyout, x, y, w, h, true};
    } else if (WmTrayTooltipGetRect(screenW, screenH, mx, my, &x, &y, &w, &h)) {
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
    if (WmBootSummaryRect(screenW, screenH, &x, &y, &w, &h) && freeSpace())
        out[count++] = {WmShellSurfaceKind::BootSummary, x, y, w, h, true};
    if (WmPerformanceHudRect(screenW, screenH, &x, &y, &w, &h) && freeSpace())
        out[count++] = {WmShellSurfaceKind::PerformanceHud, x, y, w, h, true};
    return count;
}

// Material, opacity, radii and shadow for the published list's content key.
BrookDesktopSurface WmShellSurfaceRecord(const WmShellSurface& surface, uint64_t key);
} // namespace brook
