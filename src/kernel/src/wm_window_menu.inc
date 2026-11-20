// Window menu: the title bar's right-click menu, also opened for clients
// that draw their own title bar through xdg_toplevel.show_window_menu
// (syscall 0xB25). Part of window.cpp's translation unit, included before the
// taskbar shell code: the menu borrows the app tray menu's shell surface,
// geometry and row drawing (the two are exclusive), so it needs no surface of
// its own. Rows act on the press, as tray menus do; any press outside, or
// Escape, closes it.

enum class WindowMenuAction : int32_t { Dock = 1, Return, Minimise, Maximise, Restore, Close };

struct WindowMenuState {
    int window;         // slot, valid while identity matches
    uint64_t identity;  // creation identity at open
    int32_t x, y;       // anchor (screen pixels)
    uint32_t count;
    ChromeTrayMenuRow rows[5];
    bool open;
};
static WindowMenuState g_windowMenu{};

struct TaskbarContextMenuState {
    char key[LauncherLimits::KeyLength];
    int window; // -1 for an idle pin
    uint64_t identity;
    int32_t x, y;
    bool pinned, open, taskManager;
};
static TaskbarContextMenuState g_taskbarContextMenu;

static void TaskbarContextMenuCloseLocked()
{
    if (!g_taskbarContextMenu.open) return;
    g_taskbarContextMenu.open = false;
    CompositorMarkDirty();
}

static bool WindowMenuValidLocked()
{
    if (!g_windowMenu.open) return false;
    const int idx = g_windowMenu.window;
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return false;
    const Window& w = g_windows[idx];
    return w.proc && w.visible && w.creationIdentity == g_windowMenu.identity;
}

static void WindowMenuCloseLocked()
{
    if (!g_windowMenu.open) return;
    g_windowMenu.open = false;
    CompositorMarkDirty();
}

static void WindowMenuRow(const char* label, WindowMenuAction action, bool enabled)
{
    if (g_windowMenu.count >= sizeof(g_windowMenu.rows) / sizeof(g_windowMenu.rows[0])) return;
    ChromeTrayMenuRow& row = g_windowMenu.rows[g_windowMenu.count++];
    row = {};
    row.id = static_cast<int32_t>(action);
    WmStrCopy(row.label, label, sizeof(row.label));
    row.enabled = enabled;
}

// The menu's rows for the window: what docking would do, then the frame's
// own actions. A docked page has no minimise or maximise of its own.
static bool WindowMenuOpenLocked(int idx, int32_t x, int32_t y)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return false;
    const Window& w = g_windows[idx];
    if (!w.proc || !w.visible || !w.focusable || w.fullscreen) return false;
    g_windowMenu = {};
    g_windowMenu.window = idx;
    g_windowMenu.identity = w.creationIdentity;
    g_windowMenu.x = x;
    g_windowMenu.y = y;
    if (w.docked) WindowMenuRow("Return to desktop", WindowMenuAction::Return, true);
    else WindowMenuRow(WmDockRight() ? "Move to side dock (right)" : "Move to side dock",
                       WindowMenuAction::Dock, WmDockEntryCount() < ChromeDockEntryLimit);
    WindowMenuRow("Minimise", WindowMenuAction::Minimise, !w.docked);
    if (w.state == WindowState::Maximized)
        WindowMenuRow("Restore", WindowMenuAction::Restore, !w.docked);
    else WindowMenuRow("Maximise", WindowMenuAction::Maximise, true);
    WindowMenuRow("Close", WindowMenuAction::Close, true);
    g_windowMenu.open = true;
    CompositorMarkDirty();
    CompositorWake();
    return true;
}

static DesktopRect WindowMenuRectLocked(uint32_t sw, uint32_t sh)
{
    return ChromeWindowMenuRect(WmChromeStyle(), g_windowMenu.x, g_windowMenu.y, g_windowMenu.count,
                                static_cast<int>(sw), static_cast<int>(WmDesktopHeight(sh)));
}

static void WindowMenuRun(int idx, WindowMenuAction action)
{
    switch (action) {
    case WindowMenuAction::Dock: WmDockWindow(idx); break;
    case WindowMenuAction::Return: WmDockReturnWindow(idx); break;
    case WindowMenuAction::Minimise: WmMinimizeWindow(idx); break;
    case WindowMenuAction::Maximise: WmSetMaximized(idx, true); break;
    case WindowMenuAction::Restore: WmSetMaximized(idx, false); break;
    case WindowMenuAction::Close: WmRequestCloseWindow(idx); break;
    }
}

bool WmWindowMenuOpen() { return g_windowMenu.open || g_taskbarContextMenu.open; }

void WmWindowMenuClose() {
    VfbGuard guard;
    WindowMenuCloseLocked();
    TaskbarContextMenuCloseLocked();
}

bool WmShowWindowMenu(int idx, int32_t x, int32_t y)
{
    bool closeTray = false;
    {
        VfbGuard guard;
        if (!WindowMenuOpenLocked(idx, x, y)) return false;
        TaskbarContextMenuCloseLocked();
        closeTray = ShellServiceTrayState().menuOwner != 0 && !WmTaskbarPopupPinned();
    }
    // Exclusive with the other popups; the tray's menu shares the surface.
    if (closeTray) ShellServiceQueueTrayInput({TrayInputKind::DismissMenu, ShellServiceTrayState().menuOwner, 0, x, y});
    if (WmLauncherVisible()) WmLauncherClose();
    WmVolumePopupClose();
    WmTrayFlyoutClose();
    return true;
}

// Pointer events before any other desktop routing (from WmShellTrayPointer).
// Open, the menu owns every event; closed, a secondary press on a title bar
// opens it. True when the event was consumed.
static bool WindowMenuPointer(uint32_t sw, uint32_t sh, int32_t x, int32_t y, bool pressed, int button)
{
    if (!g_windowMenu.open) {
        if (!pressed || button != 1 || WmLauncherVisible() || y >= static_cast<int32_t>(WmTaskbarTop(sh))) return false;
        const WmHitResult hit = WmHitTest(x, y);
        if (hit.windowIndex < 0 || hit.zone != WmHitZone::TitleBar) return false;
        return WmShowWindowMenu(hit.windowIndex, x, y);
    }
    int idx = -1;
    WindowMenuAction action{};
    {
        VfbGuard guard;
        if (!WindowMenuValidLocked()) { WindowMenuCloseLocked(); return false; }
        if (!pressed) return true;
        const DesktopRect rect = WindowMenuRectLocked(sw, sh);
        const auto style = WmChromeStyle();
        if (rect.contains(x, y) && !button) {
            for (uint32_t i = 0; i < g_windowMenu.count; ++i) {
                const ChromeTrayMenuRow& row = g_windowMenu.rows[i];
                if (!ChromeTrayMenuRowRect(style, rect, i).contains(x, y)) continue;
                if (!row.enabled) return true; // a disabled row keeps the menu
                idx = g_windowMenu.window;
                action = static_cast<WindowMenuAction>(row.id);
                break;
            }
            if (idx < 0) return true; // the panel's padding
        }
        WindowMenuCloseLocked();
    }
    if (idx >= 0) WindowMenuRun(idx, action);
    return true;
}
