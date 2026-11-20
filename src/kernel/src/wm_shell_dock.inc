// Side dock, the WM half (DESIGN.md "Side dock"; slice D1). The layout, hit
// test and display list builder are freestanding (chrome_shell.cpp); this
// part of window.cpp's translation unit owns the entry list, the docked
// window state, the work-area inset and persistence. Include only from
// window.cpp, after the launcher (it shares the config writer).
//
// D2 adds the caption button on native frames, either screen edge (one dock
// at a time) and a resizable width, both persisted.
//
// D1 scope: guests only (no residents or [Dock] resident list), the icon
// strip at the top of a left-edge sidebar, Super+S / Super+Shift+S, the pin,
// the edge tab. A docked window keeps running; while it is not the shown
// page it is hidden through Window::minimized, which every compositor, hit
// test and z-order path already skips. Explicit `docked` filters keep it out
// of the taskbar, launch feedback, Alt+Tab and automatic focus.

struct DockState {
    int entries[ChromeDockEntryLimit]; // window slots, in strip order
    uint32_t count;
    int selected;  // the page (entry index), remembered while closed; -1 none
    bool open;     // shown on request; a pinned dock is shown regardless
    bool pinned;
    bool reserved; // the work area last applied excludes the sidebar
    bool right;    // the sidebar is on the right edge
    int width;     // logical width; 0: ChromeDockMetrics::Width
    bool resizing; // the grip is being dragged
    int dragX, dragWidth; // pointer x and sidebar pixels when the drag began
    bool drop;     // a window being moved is over the drop target
};
static DockState g_dock = {{}, 0, -1, false, false, false, false, 0, false, 0, 0, false};

static ChromeDockPlacement DockPlacement() { return {g_dock.width, g_dock.right}; }

// Persistence: a kernel-owned [Dock] file beside launcher.ini (which belongs
// to the launcher model's serializer). Compositor writes queue a save; the
// launcher worker performs it.
static constexpr const char* DockStatePath = "/data/home/.config/brook/dock.ini";
static constexpr const char* DockStateName = "dock.ini";
static constexpr const char* DockStateTemp = "dock.ini.tmp";
static uint32_t g_dockSaveEpoch = 0, g_dockSavedEpoch = 0;
static uint64_t g_dockSaveRetry = 0;

static bool DockShown() { return g_dock.count && (g_dock.open || g_dock.pinned); }
static bool DockReserves() { return g_dock.pinned && g_dock.count; }

static uint32_t DockDesktopHeight(const DesktopMetrics& m, uint32_t screenH)
{
    return screenH > static_cast<uint32_t>(m.taskbar()) ? screenH - m.taskbar() : 0;
}

static WmRect WorkAreaFor(const DesktopMetrics& m, uint32_t screenW, uint32_t screenH)
{
    const uint32_t desktopH = DockDesktopHeight(m, screenH);
    WmRect area{0, 0, static_cast<int>(screenW), static_cast<int>(desktopH)};
    if (DockReserves()) {
        const DesktopRect side = ChromeDockSidebar(m, screenW, desktopH, DockPlacement());
        if (g_dock.right) area.w = side.x;
        else {
            area.x = side.x + side.w;
            area.w = static_cast<int>(screenW) - area.x;
        }
    }
    return area;
}

WmRect WmWorkArea(uint32_t screenW, uint32_t screenH) { return WorkAreaFor(WmDesktopMetrics(), screenW, screenH); }

static ChromeDockLayout DockLayoutFor(const DesktopMetrics& m, uint32_t screenW, uint32_t screenH)
{
    const bool shown = DockShown();
    ChromeDockLayout l = ChromeDockLayoutFor(m, screenW, DockDesktopHeight(m, screenH), static_cast<int>(g_dock.count),
                                             shown ? g_dock.selected : -1, shown, DockPlacement());
    if (g_dock.drop) {
        l.drop = true;
        if (!l.open) {
            // Shut (or empty): the tab grows into the "Send to dock" chip.
            const int w = m.pixels(ChromeDockMetrics::DropChipW), h = m.pixels(ChromeDockMetrics::DropChipH);
            if (w <= l.sidebar.w && h <= l.sidebar.h)
                l.tab = {g_dock.right ? l.sidebar.x + l.sidebar.w - w : l.sidebar.x,
                         l.sidebar.y + (l.sidebar.h - h) / 2, w, h};
        }
    }
    return l;
}

// Drag to dock: the pointer of a window move is over the open sidebar, or
// the shut dock's screen edge.
static bool DockDropZoneLocked(const DesktopMetrics& m, int32_t mx, int32_t my, uint32_t sw, uint32_t sh)
{
    if (my < 0 || my >= static_cast<int32_t>(DockDesktopHeight(m, sh))) return false;
    if (DockShown()) return ChromeDockSidebar(m, sw, DockDesktopHeight(m, sh), DockPlacement()).contains(mx, my);
    const int edge = m.pixels(ChromeDockMetrics::DropEdge);
    return g_dock.right ? mx >= static_cast<int32_t>(sw) - edge : mx >= 0 && mx < edge;
}

// Floating windows cannot sit under a pinned sidebar: move the edge that
// overlaps it out (a window wider than the work area keeps its left edge).
static bool DockKeepOutLocked(Window& w)
{
    if (!DockReserves() || w.docked || !w.focusable || w.fullscreen || w.state != WindowState::Normal) return false;
    uint32_t sw = 0, sh = 0;
    CompositorGetPhysDims(&sw, &sh);
    const WmRect area = WorkAreaFor(w.desktopMetrics(), sw, sh);
    if (g_dock.right) {
        const int right = area.x + area.w, outer = static_cast<int>(w.outerWidth());
        if (w.x + outer <= right) return false;
        const int x = right - outer > area.x ? right - outer : area.x;
        if (x == w.x) return false;
        w.x = static_cast<int16_t>(x);
        return true;
    }
    if (w.x >= area.x) return false;
    w.x = static_cast<int16_t>(area.x);
    return true;
}

// The caption button (CaptionGeometry): native frames of windows the dock
// would take, showing the edge it docks into.
static uint8_t WindowDockButton(const Window& w)
{
    if (!w.proc || w.noChrome || w.docked || w.fullscreen || !w.focusable) return 0;
    return g_dock.right ? 2 : 1;
}

// Place a docked window as the page: the sidebar below the strip.
static bool DockPlaceLocked(int idx, const DesktopMetrics& m, uint32_t screenW, uint32_t screenH)
{
    Window& w = g_windows[idx];
    const ChromeDockLayout l = ChromeDockLayoutFor(m, screenW, DockDesktopHeight(m, screenH), 1, 0, true, DockPlacement());
    if (l.page.w < static_cast<int>(WM_MIN_WIDTH) || l.page.h < static_cast<int>(WM_MIN_HEIGHT)) return false;
    const int16_t x = w.x, y = w.y;
    w.x = static_cast<int16_t>(l.page.x);
    w.y = static_cast<int16_t>(l.page.y);
    if (w.clientW == l.page.w && w.clientH == l.page.h) return true;
    if (ResizeWindowLocked(idx, static_cast<uint16_t>(l.page.w), static_cast<uint16_t>(l.page.h))) return true;
    w.x = x; w.y = y;
    return false;
}

// Maximised windows follow the work area when the pin or the entries change
// it; floating ones are moved out from under a pinned sidebar.
static void DockReflowLocked()
{
    uint32_t sw = 0, sh = 0;
    CompositorGetPhysDims(&sw, &sh);
    if (!sw || !sh) return;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i) {
        Window& w = g_windows[i];
        if (!w.proc || w.docked) continue;
        const WmRect area = WorkAreaFor(w.desktopMetrics(), sw, sh);
        if (w.fullscreen) {
            // Leaving fullscreen restores a maximised window at the new area.
            if (w.fsState == WindowState::Maximized) {
                const int border = w.fsNoChrome ? 0 : w.metric(WM_BORDER_WIDTH);
                const int title = w.fsNoChrome ? 0 : w.metric(WM_TITLE_BAR_HEIGHT);
                if (area.w - 2 * border >= static_cast<int>(WM_MIN_WIDTH)) {
                    w.fsX = static_cast<int16_t>(area.x); w.fsY = static_cast<int16_t>(area.y);
                    w.fsW = static_cast<uint16_t>(area.w - 2 * border);
                    w.fsH = static_cast<uint16_t>(area.h - title - 2 * border);
                }
            }
            continue;
        }
        if (w.state == WindowState::Maximized) {
            const int border = w.noChrome ? 0 : w.metric(WM_BORDER_WIDTH);
            const int title = w.noChrome ? 0 : w.metric(WM_TITLE_BAR_HEIGHT);
            const int width = area.w - 2 * border, height = area.h - title - 2 * border;
            if (width < static_cast<int>(WM_MIN_WIDTH) || height < static_cast<int>(WM_MIN_HEIGHT) || width > 65535) continue;
            if (!ResizeWindowLocked(static_cast<int>(i), static_cast<uint16_t>(width), static_cast<uint16_t>(height))) continue;
            w.x = static_cast<int16_t>(area.x);
            w.y = static_cast<int16_t>(area.y);
        } else if (!DockKeepOutLocked(w)) continue;
        w.proc->fbDestX = w.clientX(); w.proc->fbDestY = w.clientY();
    }
    CompositorMarkDirty();
}

static void DockSyncWorkAreaLocked()
{
    const bool reserve = DockReserves();
    if (reserve == g_dock.reserved) return;
    g_dock.reserved = reserve;
    DockReflowLocked();
}

// Leaving the shown page, a focused window gives focus to the desktop.
static void DockUnfocusLocked(int idx)
{
    Window& w = g_windows[idx];
    if (g_pointerCaptureIndex == idx) PointerCaptureReleaseLocked();
    if (g_focusedIdx != idx) return;
    PointerActivationCancel();
    g_focusedIdx = -1;
    w.focused = false;
    WmPushWmEvent(&w, WM_EVT_FOCUS_LOST, 0, 0);
    const int next = NextFocusCandidateLocked(idx);
    if (next >= 0) FocusWindowLocked(next);
}

// Make every docked window's visibility match the dock: only the selected
// page of a shown dock is drawn. focusPage gives that page the keyboard.
static void DockApplyLocked(bool focusPage)
{
    if (!g_dock.count) { g_dock.selected = -1; g_dock.open = false; }
    else if (g_dock.selected < 0 || g_dock.selected >= static_cast<int>(g_dock.count)) g_dock.selected = 0;
    const bool shown = DockShown();
    for (uint32_t i = 0; i < g_dock.count; ++i) {
        const int idx = g_dock.entries[i];
        Window& w = g_windows[idx];
        const bool show = shown && static_cast<int>(i) == g_dock.selected;
        if (w.minimized == !show) continue;
        w.minimized = !show;
        if (!show) DockUnfocusLocked(idx);
    }
    if (focusPage && shown) FocusWindowLocked(g_dock.entries[g_dock.selected]);
    DockSyncWorkAreaLocked();
    CompositorMarkDirty();
    CompositorWake();
}

static int DockEntryOf(int idx)
{
    for (uint32_t i = 0; i < g_dock.count; ++i)
        if (g_dock.entries[i] == idx) return static_cast<int>(i);
    return -1;
}

// Drop an entry. The page moves to its neighbour; a returned or closed page
// closes an unpinned dock unless keepOpen.
static void DockRemoveEntryLocked(int entry, bool keepOpen)
{
    for (uint32_t i = static_cast<uint32_t>(entry); i + 1 < g_dock.count; ++i) g_dock.entries[i] = g_dock.entries[i + 1];
    --g_dock.count;
    if (entry < g_dock.selected) --g_dock.selected;
    else if (entry == g_dock.selected) {
        if (g_dock.selected >= static_cast<int>(g_dock.count)) g_dock.selected = static_cast<int>(g_dock.count) - 1;
        if (!keepOpen && !g_dock.pinned) g_dock.open = false;
    }
}

static bool DockDropEligible(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return false;
    const Window& w = g_windows[idx];
    return w.proc && w.visible && w.focusable && !w.docked && !w.fullscreen && g_dock.count < ChromeDockEntryLimit;
}

static bool DockWindowLocked(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return false;
    Window& w = g_windows[idx];
    if (!w.proc || !w.visible || !w.focusable || w.docked || g_dock.count >= ChromeDockEntryLimit) return false;
    uint32_t sw = 0, sh = 0;
    CompositorGetPhysDims(&sw, &sh);
    if (!sw || !sh) return false;
    if (w.fullscreen && !SetFullscreenLocked(idx, false)) return false;
    if (w.minimized) { w.minimized = false; }
    // What returning restores: floating (or maximised) geometry and chrome.
    w.dkState = w.state; w.dkX = w.x; w.dkY = w.y; w.dkW = w.clientW; w.dkH = w.clientH;
    w.dkNoChrome = w.noChrome; w.dkCaptionOverlay = w.captionOverlay;
    w.dkCaptionInsetRight = w.captionInsetRight; w.dkCaptionInsetTop = w.captionInsetTop;
    w.dkCaptionHeight = w.captionHeight; w.dkCaptionStyle = w.captionStyle;
    // The page: no server-side chrome, configured as maximised (tiled; a
    // client-decorated app drops its shadow margin), fixed in place.
    w.noChrome = true; w.captionOverlay = false;
    w.captionInsetRight = w.captionInsetTop = w.captionHeight = 0; w.captionStyle = 0;
    w.state = WindowState::Maximized;
    w.docked = true;
    if (!DockPlaceLocked(idx, w.desktopMetrics(), sw, sh)) {
        w.docked = false; w.state = w.dkState; w.x = w.dkX; w.y = w.dkY;
        w.noChrome = w.dkNoChrome; w.captionOverlay = w.dkCaptionOverlay;
        w.captionInsetRight = w.dkCaptionInsetRight; w.captionInsetTop = w.dkCaptionInsetTop;
        w.captionHeight = w.dkCaptionHeight; w.captionStyle = w.dkCaptionStyle;
        return false;
    }
    w.proc->fbDestX = w.clientX(); w.proc->fbDestY = w.clientY();
    g_dock.entries[g_dock.count++] = idx;
    // Docking opens the dock on the new entry, focused, so the window stays
    // in view: it moves into the sidebar rather than disappearing.
    w.minimized = true;
    g_dock.selected = static_cast<int>(g_dock.count) - 1;
    g_dock.open = true;
    DockApplyLocked(true);
    SerialPrintf("WM: docked window %d '%s' (%u entries)\n", idx, w.title, g_dock.count);
    return true;
}

static bool DockReturnLocked(int idx, bool focus)
{
    const int entry = DockEntryOf(idx);
    if (entry < 0) return false;
    Window& w = g_windows[idx];
    DockRemoveEntryLocked(entry, false);
    w.docked = false;
    w.minimized = false;
    w.noChrome = w.dkNoChrome; w.captionOverlay = w.dkCaptionOverlay;
    w.captionInsetRight = w.dkCaptionInsetRight; w.captionInsetTop = w.dkCaptionInsetTop;
    w.captionHeight = w.dkCaptionHeight; w.captionStyle = w.dkCaptionStyle;
    // The work area without this entry (it may have been the last one).
    DockSyncWorkAreaLocked();
    uint32_t sw = 0, sh = 0;
    CompositorGetPhysDims(&sw, &sh);
    uint16_t width = w.dkW, height = w.dkH;
    if (w.dkState == WindowState::Maximized) {
        // Maximised again at today's work area; savedX..H still hold the
        // geometry its restore button returns to.
        const WmRect area = WorkAreaFor(w.desktopMetrics(), sw, sh);
        const int border = w.noChrome ? 0 : w.metric(WM_BORDER_WIDTH);
        const int title = w.noChrome ? 0 : w.metric(WM_TITLE_BAR_HEIGHT);
        w.x = static_cast<int16_t>(area.x); w.y = static_cast<int16_t>(area.y);
        if (area.w - 2 * border >= static_cast<int>(WM_MIN_WIDTH) && area.h - title - 2 * border >= static_cast<int>(WM_MIN_HEIGHT)) {
            width = static_cast<uint16_t>(area.w - 2 * border);
            height = static_cast<uint16_t>(area.h - title - 2 * border);
        }
        w.state = WindowState::Maximized;
    } else {
        w.state = WindowState::Normal;
        w.x = w.dkX; w.y = w.dkY;
        DockKeepOutLocked(w);
    }
    if (!ResizeWindowLocked(idx, width, height))
        SerialPrintf("WM: return of window %d could not queue its configure\n", idx);
    w.proc->fbDestX = w.clientX(); w.proc->fbDestY = w.clientY();
    DockApplyLocked(false);
    if (focus) FocusWindowLocked(idx);
    SerialPrintf("WM: returned window %d '%s' to the desktop (%u entries)\n", idx, w.title, g_dock.count);
    return true;
}

// DestroyWindowLocked: the slot is already dead (proc cleared).
static void DockForgetLocked(int idx)
{
    Window& w = g_windows[idx];
    w.docked = false;
    const int entry = DockEntryOf(idx);
    if (entry < 0) return;
    const bool page = entry == g_dock.selected && DockShown();
    DockRemoveEntryLocked(entry, true);
    // A closed page hands the sidebar to its neighbour, focused if it was.
    DockApplyLocked(page && g_focusedIdx == idx);
}

static void DockQueueSave() { ++g_dockSaveEpoch; }

// The placement changed (edge or width): every entry moves to the new page,
// the work area and the windows that follow it reflow, and the change is saved.
static void DockRelayoutLocked()
{
    uint32_t sw = 0, sh = 0;
    CompositorGetPhysDims(&sw, &sh);
    if (!sw || !sh) return;
    for (uint32_t i = 0; i < g_dock.count; ++i) {
        const int idx = g_dock.entries[i];
        Window& w = g_windows[idx];
        const ChromeDockLayout l = ChromeDockLayoutFor(w.desktopMetrics(), sw, DockDesktopHeight(w.desktopMetrics(), sh),
                                                       1, 0, true, DockPlacement());
        w.x = static_cast<int16_t>(l.page.x);
        w.y = static_cast<int16_t>(l.page.y);
        // Always configure: it also ends an interactive (grip) resize.
        if (l.page.w >= static_cast<int>(WM_MIN_WIDTH) && l.page.h >= static_cast<int>(WM_MIN_HEIGHT))
            ResizeWindowLocked(idx, static_cast<uint16_t>(l.page.w), static_cast<uint16_t>(l.page.h));
        if (w.proc) { w.proc->fbDestX = w.clientX(); w.proc->fbDestY = w.clientY(); }
    }
    if (DockReserves()) DockReflowLocked();
    DockQueueSave();
    CompositorMarkDirty();
    CompositorWake();
}

static void DockSetSideLocked(bool right)
{
    if (g_dock.right == right) return;
    g_dock.right = right;
    DockRelayoutLocked();
    SerialPrintf("WM: dock moved to the %s edge\n", right ? "right" : "left");
}

static void DockSetWidthLocked(int width)
{
    if (width < 0) width = 0;
    if (g_dock.width == width) return;
    g_dock.width = width;
    DockRelayoutLocked();
}

// Grip drag (compositor thread, under the guard): the page follows the
// pointer with interactive configures; the release settles the placement.
static void DockDragLocked(int32_t mx, bool down)
{
    if (!g_dock.resizing) return;
    uint32_t sw = 0, sh = 0;
    CompositorGetPhysDims(&sw, &sh);
    const DesktopMetrics m = WmDesktopMetrics();
    const int px = g_dock.dragWidth + (g_dock.right ? g_dock.dragX - mx : mx - g_dock.dragX);
    const int width = ChromeDockWidthFor(m, sw, px);
    if (width != g_dock.width) {
        g_dock.width = width;
        const int entry = g_dock.selected;
        if (DockShown() && entry >= 0 && entry < static_cast<int>(g_dock.count)) {
            const int idx = g_dock.entries[entry];
            Window& w = g_windows[idx];
            const ChromeDockLayout l = ChromeDockLayoutFor(m, sw, DockDesktopHeight(m, sh), 1, 0, true, DockPlacement());
            w.x = static_cast<int16_t>(l.page.x);
            w.y = static_cast<int16_t>(l.page.y);
            if (l.page.w >= static_cast<int>(WM_MIN_WIDTH))
                ResizeWindowLocked(idx, static_cast<uint16_t>(l.page.w), static_cast<uint16_t>(l.page.h), true);
            if (w.proc) { w.proc->fbDestX = w.clientX(); w.proc->fbDestY = w.clientY(); }
        }
        CompositorMarkDirty();
    }
    if (!down) {
        g_dock.resizing = false;
        DockRelayoutLocked();
        KLOG_INFO(Wm, "dock: width %d", g_dock.width);
    }
}

static void DockSetPinnedLocked(bool pinned)
{
    if (g_dock.pinned == pinned) return;
    g_dock.pinned = pinned;
    // Unpinning leaves it open until the next outside click or Escape.
    if (!pinned && g_dock.count) g_dock.open = true;
    DockApplyLocked(false);
    DockQueueSave();
    SerialPrintf("WM: dock %s\n", pinned ? "pinned" : "unpinned");
}

// ---------------------------------------------------------------------------
// Public API (compositor thread)
// ---------------------------------------------------------------------------

bool WmDockWindow(int idx) { VfbGuard guard; return DockWindowLocked(idx); }
bool WmDockReturnWindow(int idx) { VfbGuard guard; return DockReturnLocked(idx, true); }
bool WmDockPinned() { return g_dock.pinned; }
bool WmDockOpen() { return DockShown(); }
uint32_t WmDockEntryCount() { return g_dock.count; }
int WmDockEntryWindow(uint32_t entry) { return entry < g_dock.count ? g_dock.entries[entry] : -1; }
void WmDockSetPinned(bool pinned) { VfbGuard guard; DockSetPinnedLocked(pinned); }
bool WmDockDismissible() { return g_dock.count && g_dock.open && !g_dock.pinned; }
bool WmDockRight() { return g_dock.right; }
int WmDockWidth() { return g_dock.width; }
void WmDockSetRight(bool right) { VfbGuard guard; DockSetSideLocked(right); }
void WmDockSetWidth(int width) { VfbGuard guard; DockSetWidthLocked(width); }
bool WmDockDragActive() { return g_dock.resizing; }

void WmDockWindowDrag(int idx, int32_t mx, int32_t my)
{
    VfbGuard guard;
    uint32_t sw = 0, sh = 0;
    CompositorGetPhysDims(&sw, &sh);
    const bool drop = g_wmActive && !WmTaskbarHidden() && DockDropEligible(idx) &&
                      DockDropZoneLocked(WmDesktopMetrics(), mx, my, sw, sh);
    if (drop == g_dock.drop) return;
    g_dock.drop = drop;
    CompositorMarkDirty();
    CompositorWake();
}

void WmDockWindowDragCancel()
{
    VfbGuard guard;
    if (!g_dock.drop) return;
    g_dock.drop = false;
    CompositorMarkDirty();
}

bool WmDockWindowDrop(int idx, int32_t mx, int32_t my, int16_t startX, int16_t startY)
{
    VfbGuard guard;
    uint32_t sw = 0, sh = 0;
    CompositorGetPhysDims(&sw, &sh);
    const bool drop = g_dock.drop && DockDropEligible(idx) && DockDropZoneLocked(WmDesktopMetrics(), mx, my, sw, sh);
    g_dock.drop = false;
    CompositorMarkDirty();
    if (!drop || !DockWindowLocked(idx)) return false;
    // Returning puts it back where the drag began, not at the screen edge.
    Window& w = g_windows[idx];
    w.dkX = startX;
    w.dkY = startY;
    return true;
}
void WmDockDrag(int32_t mx, bool down) { VfbGuard guard; DockDragLocked(mx, down); }

bool WmDockSelect(int entry)
{
    VfbGuard guard;
    if (entry < 0 || entry >= static_cast<int>(g_dock.count)) return false;
    g_dock.selected = entry;
    g_dock.open = true;
    DockApplyLocked(true);
    return true;
}

static void DockOpenLocked()
{
    g_dock.open = true;
    DockApplyLocked(true);
}

void WmDockToggle()
{
    bool closeLauncher = false;
    {
        VfbGuard guard;
        if (!g_dock.count) return;
        if (DockShown() && !g_dock.pinned) { g_dock.open = false; DockApplyLocked(false); return; }
        // Opening (or Super+S on a pinned dock) puts focus on the page.
        closeLauncher = !g_dock.pinned;
        DockOpenLocked();
    }
    if (closeLauncher && WmLauncherVisible()) WmLauncherClose();
}

bool WmDockDismiss()
{
    VfbGuard guard;
    if (!WmDockDismissible()) return false;
    g_dock.open = false;
    DockApplyLocked(false);
    return true;
}

void WmDockFocusedWindow()
{
    VfbGuard guard;
    const int idx = g_focusedIdx;
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS) || !g_windows[idx].proc) return;
    if (g_windows[idx].docked) DockReturnLocked(idx, true);
    else DockWindowLocked(idx);
}

bool WmDockSurfaceRect(uint32_t screenW, uint32_t screenH, int32_t* x, int32_t* y, uint32_t* w, uint32_t* h,
                       bool* material)
{
    if (!g_wmActive || (!g_dock.count && !g_dock.drop)) return false;
    const ChromeDockLayout l = DockLayoutFor(WmDesktopMetrics(), screenW, screenH);
    const DesktopRect& r = l.open ? l.strip : l.tab;
    if (r.w <= 0 || r.h <= 0) return false;
    *x = r.x; *y = r.y; *w = static_cast<uint32_t>(r.w); *h = static_cast<uint32_t>(r.h);
    *material = l.open || l.drop; // the tab is a flat quad: no glass at rest
    return true;
}

bool WmDockPointerOwned(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    if (!g_wmActive || !g_dock.count || WmTaskbarHidden()) return false;
    if (g_dock.resizing) return true;
    // The drop chip is feedback for a window move, not a pointer target.
    if (g_dock.drop && !DockShown()) return false;
    const ChromeDockLayout l = DockLayoutFor(WmDesktopMetrics(), screenW, screenH);
    return (l.open ? l.strip : l.tab).contains(mx, my) || l.grip.contains(mx, my);
}

bool WmDockPress(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    if (!g_wmActive || !g_dock.count || WmTaskbarHidden()) return false;
    bool consumed = true;
    {
        VfbGuard guard;
        if (!g_dock.count) return false;
        const ChromeDockLayout l = DockLayoutFor(WmDesktopMetrics(), screenW, screenH);
        const ChromeDockHit hit = ChromeDockHitTest(l, mx, my);
        switch (hit.target) {
        case ChromeDockTarget::Tab:
            DockOpenLocked();
            break;
        case ChromeDockTarget::Entry:
            g_dock.selected = hit.index;
            DockOpenLocked();
            break;
        case ChromeDockTarget::Tile:
            // The page's title bar: hides the dock, or focuses a pinned page.
            if (g_dock.pinned) FocusWindowLocked(g_dock.entries[hit.index]);
            else { g_dock.open = false; DockApplyLocked(false); }
            break;
        case ChromeDockTarget::Return:
            DockReturnLocked(g_dock.entries[hit.index], true);
            break;
        case ChromeDockTarget::Close: {
            uint32_t method = 0;
            RequestCloseLocked(g_dock.entries[hit.index], &method);
            break;
        }
        case ChromeDockTarget::Pin:
            DockSetPinnedLocked(!g_dock.pinned);
            break;
        case ChromeDockTarget::Side:
            DockSetSideLocked(!g_dock.right);
            break;
        case ChromeDockTarget::Grip:
            g_dock.resizing = true;
            g_dock.dragX = mx;
            g_dock.dragWidth = l.sidebar.w;
            break;
        case ChromeDockTarget::Strip:
            break;
        case ChromeDockTarget::None:
            consumed = false;
            // A click outside an open, unpinned sidebar hides it and still
            // reaches what lies beneath.
            if (l.open && !g_dock.pinned && !l.sidebar.contains(mx, my)) {
                g_dock.open = false;
                DockApplyLocked(false);
            }
            break;
        }
    }
    return consumed;
}

int WmCycleFocus()
{
    VfbGuard guard;
    if (WmDockDismissible()) { g_dock.open = false; DockApplyLocked(false); }
    int all[WM_MAX_WINDOWS], order[WM_MAX_WINDOWS];
    const uint32_t total = WmGetZOrder(all, WM_MAX_WINDOWS);
    uint32_t count = 0;
    for (uint32_t i = 0; i < total; ++i)
        if (!g_windows[all[i]].docked) order[count++] = all[i];
    // From a pinned page, Alt+Tab goes to the frontmost desktop window.
    if (g_focusedIdx >= 0 && g_windows[g_focusedIdx].docked) {
        if (!count) return -1;
        FocusWindowLocked(order[count - 1]);
        return order[count - 1];
    }
    if (count < 2) return -1;
    SendToBackLocked(order[count - 1]);
    FocusWindowLocked(order[count - 2]);
    return order[count - 2];
}

// ---------------------------------------------------------------------------
// Shell snapshot
// ---------------------------------------------------------------------------

static void WmCaptureDock(uint32_t screenW, uint32_t screenH, int32_t mouseX, int32_t mouseY, ChromeShellModel& snapshot)
{
    if (!g_wmActive || (!g_dock.count && !g_dock.drop)) return;
    LauncherReadGuard icons;
    snapshot.kind = ChromeShellKind::Dock;
    snapshot.dock = DockLayoutFor(WmDesktopMetrics(), screenW, screenH);
    snapshot.dockPinned = g_dock.pinned;
    snapshot.dockResizing = g_dock.resizing;
    const auto& l = snapshot.dock;
    for (int i = 0; i < l.count; ++i) {
        const int idx = g_dock.entries[i];
        WmCaptureShellIcon(snapshot.icons[i], WindowIcon(g_windows[idx], icons.get()));
    }
    if (l.selected >= 0) WmStrCopy(snapshot.text, WindowDisplayTitle(g_windows[g_dock.entries[l.selected]]), sizeof(snapshot.text));
    const bool hovered = l.open && (l.strip.contains(mouseX, mouseY) || l.grip.contains(mouseX, mouseY));
    snapshot.mouseX = hovered ? mouseX : -1;
    snapshot.mouseY = hovered ? mouseY : -1;
}

// ---------------------------------------------------------------------------
// Persistence: "[Dock]\npinned=0|1\nside=left|right\nwidth=N\n" (width in
// logical pixels, 0 the default). Residents join it in a later slice.
// ---------------------------------------------------------------------------

// The [Dock] section's settings; unknown keys and sections are ignored.
static void DockParseState(char* text)
{
    bool section = false;
    for (char* line = text; *line;) {
        char* end = line;
        while (*end && *end != '\n') ++end;
        const char saved = *end;
        *end = 0;
        if (end > line && end[-1] == '\r') end[-1] = 0;
        if (line[0] == '[') section = !strcmp(line, "[Dock]");
        else if (section && !strncmp(line, "pinned=", 7)) g_dock.pinned = line[7] == '1';
        else if (section && !strncmp(line, "side=", 5)) g_dock.right = !strcmp(line + 5, "right");
        else if (section && !strncmp(line, "width=", 6)) {
            int width = 0;
            for (const char* d = line + 6; *d >= '0' && *d <= '9' && width < 100000; ++d) width = width * 10 + (*d - '0');
            g_dock.width = width <= 4096 ? width : 0;
        }
        *end = saved;
        line = *end ? end + 1 : end;
    }
}

// Shell context (WmLauncherLoad), before the desktop draws.
static void DockLoadState()
{
    char text[256];
    const auto result = ConfigReadText(DockStatePath, text, sizeof(text));
    if (result.error) {
        if (strcmp(result.stage, "missing"))
            KLogCatConsole(LogCat::Wm, LogLevel::Warn, "dock: load failed stage=%s errno=%d", result.stage, -result.error);
        return;
    }
    DockParseState(text);
    g_dockSavedEpoch = g_dockSaveEpoch;
    KLOG_INFO(Wm, "dock: state restored pinned=%d side=%s width=%d", int(g_dock.pinned),
              g_dock.right ? "right" : "left", g_dock.width);
}

// Launcher worker: write a queued change (a failure retries in 5 s).
static void DockSaveState()
{
    const uint32_t epoch = __atomic_load_n(&g_dockSaveEpoch, __ATOMIC_ACQUIRE);
    if (epoch == g_dockSavedEpoch || ApicTickCount() < g_dockSaveRetry) return;
    char text[96];
    WmStrCopy(text, g_dock.pinned ? "[Dock]\npinned=1\n" : "[Dock]\npinned=0\n", sizeof(text));
    uint32_t bytes = static_cast<uint32_t>(strlen(text));
    WmStrCopy(text + bytes, g_dock.right ? "side=right\nwidth=" : "side=left\nwidth=", sizeof(text) - bytes);
    bytes = static_cast<uint32_t>(strlen(text));
    char digits[12];
    int n = 0;
    for (uint32_t v = static_cast<uint32_t>(g_dock.width > 0 ? g_dock.width : 0); n == 0 || v; v /= 10) digits[n++] = char('0' + v % 10);
    while (n && bytes + 2 < sizeof(text)) text[bytes++] = digits[--n];
    text[bytes++] = '\n';
    text[bytes] = 0;
    VfsPath root{}, directory{};
    LauncherPersistenceResult result;
    if (const int error = VfsPathRoot(&root)) result = {"root", error};
    else if (const int dirError = LauncherConfigDir(root, &directory)) result = {"directory", dirError};
    else result = LauncherWriteLayout(root, directory, text, bytes, DockStateTemp, DockStateName);
    VfsPathRelease(directory);
    VfsPathRelease(root);
    if (result.error) {
        g_dockSaveRetry = ApicTickCount() + 5000;
        KLogCatConsole(LogCat::Wm, LogLevel::Warn, "dock: save failed stage=%s errno=%d", result.stage, -result.error);
        return;
    }
    g_dockSavedEpoch = epoch;
}
