#pragma once
#include <stdint.h>
#include "window.h"
#include "../../shared/desktop_layers_abi.h"

namespace brook {
// Shell surfaces: the one list of desktop overlay surfaces (taskbar, the
// shared popup slot and the boot summary) that every compositor path draws,
// composes, blurs and captures: the CPU back buffer, the virgl overlay layer
// and the VKRENDER overlay capture. Add a new shell surface here, not in the
// paths. Material surfaces take the translucent shell material when glass is
// active; the others stay opaque.
enum class WmShellSurfaceKind : uint8_t { Taskbar, Launcher, VolumePopup, TrayTooltip, BootSummary };
struct WmShellSurface {
    WmShellSurfaceKind kind;
    int32_t x, y;
    uint32_t w, h;
    bool material;
};
// Taskbar (absent while WmTaskbarHidden), at most one of launcher / volume
// popup / tray tooltip, boot summary.
static constexpr uint32_t WM_SHELL_SURFACE_LIMIT = 3;

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
    // One popup slot: opening the launcher closes the volume popup and vice
    // versa, and the tray tooltip yields to either of them.
    int32_t x = 0, y = 0; uint32_t w = 0, h = 0;
    if (WmLauncherVisible()) {
        WmLauncherGetRect(screenW, screenH, &x, &y, &w, &h);
        out[count++] = {WmShellSurfaceKind::Launcher, x, y, w, h, true};
    } else if (WmVolumePopupVisible()) {
        WmVolumePopupGetRect(screenW, screenH, &x, &y, &w, &h);
        out[count++] = {WmShellSurfaceKind::VolumePopup, x, y, w, h, true};
    } else if (WmTrayTooltipGetRect(screenW, screenH, mx, my, &x, &y, &w, &h)) {
        out[count++] = {WmShellSurfaceKind::TrayTooltip, x, y, w, h, true};
    }
    if (WmBootSummaryRect(screenW, screenH, &x, &y, &w, &h))
        out[count++] = {WmShellSurfaceKind::BootSummary, x, y, w, h, false};
    return count;
}

// Content key of the surface as it would render now: equal keys mean
// identical pixels when rasterised onto a cleared buffer. Builds the
// surface's display list without executing it (window.cpp; compositor
// thread only). 0: the surface draws nothing.
uint64_t WmShellSurfaceKey(const WmShellSurface& surface, uint32_t screenW, uint32_t screenH,
                           uint64_t uptimeMs, int32_t mx, int32_t my);

// The surface's record (layers ABI v2): material, opacity, radii, shadow
// and the given content key (window.cpp).
BrookDesktopSurface WmShellSurfaceRecord(const WmShellSurface& surface, uint64_t key);

// Rasterise one surface with the WM's CPU painters into a full-screen buffer.
inline void WmRenderShellSurface(const WmShellSurface& surface, uint32_t* pixels, uint32_t stride,
                                 uint32_t screenW, uint32_t screenH, uint64_t uptimeMs,
                                 int32_t mx, int32_t my)
{
    switch (surface.kind) {
    case WmShellSurfaceKind::Taskbar: WmRenderTaskbar(pixels, stride, screenW, screenH, uptimeMs, mx, my); break;
    case WmShellSurfaceKind::Launcher: WmLauncherRender(pixels, stride, screenW, screenH, mx, my); break;
    case WmShellSurfaceKind::VolumePopup: WmVolumePopupRender(pixels, stride, screenW, screenH, mx, my); break;
    case WmShellSurfaceKind::TrayTooltip: WmTrayTooltipRender(pixels, stride, screenW, screenH, mx, my); break;
    case WmShellSurfaceKind::BootSummary: WmRenderBootSummary(pixels, stride, screenW, screenH); break;
    }
}
} // namespace brook
