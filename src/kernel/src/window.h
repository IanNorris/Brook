#pragma once

#include <stdint.h>

namespace brook {

struct Process;

// Window manager constants
static constexpr uint32_t WM_TITLE_BAR_HEIGHT = 24;
static constexpr uint32_t WM_BORDER_WIDTH     = 2;
static constexpr uint32_t WM_BUTTON_WIDTH     = 24;
static constexpr uint32_t WM_RESIZE_GRAB      = 8;   // corner grab zone for resize
static constexpr uint32_t WM_MIN_WIDTH        = 200;
static constexpr uint32_t WM_MIN_HEIGHT       = 100;
static constexpr uint32_t WM_MAX_WINDOWS      = 32;
static constexpr uint32_t WM_TASKBAR_HEIGHT   = 32;   // bottom taskbar height

// Window chrome colours
static constexpr uint32_t WM_TITLE_BG_FOCUSED   = 0x002B4A7A; // blue-ish
static constexpr uint32_t WM_TITLE_BG_UNFOCUSED = 0x00404040; // dark grey
static constexpr uint32_t WM_TITLE_FG           = 0x00E0E0E0; // light grey text
static constexpr uint32_t WM_BORDER_FOCUSED     = 0x004080C0; // lighter blue
static constexpr uint32_t WM_BORDER_UNFOCUSED   = 0x00505050; // grey
static constexpr uint32_t WM_CLOSE_BTN_HOVER    = 0x00E04040; // red
static constexpr uint32_t WM_MAX_BTN_HOVER      = 0x00606060; // grey
static constexpr uint32_t WM_TASKBAR_BG         = 0x001E1E2E; // dark blue-grey
static constexpr uint32_t WM_TASKBAR_BTN_BG     = 0x002D2D3D; // slightly lighter
static constexpr uint32_t WM_TASKBAR_BTN_ACTIVE = 0x003B5998; // active/focused button
static constexpr uint32_t WM_TASKBAR_BTN_FG     = 0x00D0D0D0; // button text
static constexpr uint32_t WM_TASKBAR_CLOCK_FG   = 0x0090D0FF; // clock text (light blue)

enum class WindowState : uint8_t
{
    Normal,
    Maximized,
};

struct Window
{
    Process*    proc;           // owning process (nullptr = unused slot)
    int16_t     x, y;           // window position (outer top-left, including chrome)
    uint16_t    clientW;        // client area width (= VFB width * upscale)
    uint16_t    clientH;        // client area height (= VFB height * upscale)
    uint8_t     zOrder;         // higher = on top (0 = backmost)
    uint8_t     upscale;        // integer upscale factor (1 = 1:1, 2 = 2×, 4 = 4×)
    WindowState state;
    bool        focused;
    bool        visible;
    bool        minimized;      // hidden from desktop, shown in taskbar
    char        title[64];

    // Pre-maximise geometry (for restore)
    int16_t     savedX, savedY;
    uint16_t    savedW, savedH;

    // Outer dimensions including chrome
    uint16_t outerWidth()  const { return clientW + 2 * WM_BORDER_WIDTH; }
    uint16_t outerHeight() const { return clientH + WM_TITLE_BAR_HEIGHT + 2 * WM_BORDER_WIDTH; }

    // Client area origin relative to outer top-left
    int16_t clientX() const { return x + static_cast<int16_t>(WM_BORDER_WIDTH); }
    int16_t clientY() const { return y + static_cast<int16_t>(WM_TITLE_BAR_HEIGHT + WM_BORDER_WIDTH); }
};

// Hit-test result for mouse clicks
enum class WmHitZone : uint8_t
{
    None,
    TitleBar,
    CloseButton,
    MaximizeButton,
    MinimizeButton,
    ClientArea,
    ResizeCorner,   // bottom-right corner
    Border,
    Taskbar,        // clicked on taskbar background
    TaskbarButton,  // clicked on a taskbar window button
};

struct WmHitResult
{
    int       windowIndex;  // -1 if no window hit
    WmHitZone zone;
};

// ---------------------------------------------------------------------------
// Window Manager API
// ---------------------------------------------------------------------------

// Initialise the window manager (called after CompositorInit).
void WmInit();

// Create a window for a process. Returns window index or -1 on failure.
// The process must already have a VFB set up via CompositorSetupProcess.
// upscale: integer scale factor (1=1:1, 2=2×, etc.). clientW/H are the
// displayed size (VFB size × upscale).
int WmCreateWindow(Process* proc, int16_t x, int16_t y,
                   uint16_t clientW, uint16_t clientH,
                   const char* title, uint8_t upscale = 1);

// Remove a window (process exited or closed).
void WmDestroyWindow(int idx);

// Remove the window owned by a specific process (safety net for ProcessDestroy).
void WmDestroyWindowForProcess(Process* proc);

// Hit-test: given mouse coordinates, determine which window/zone is under cursor.
WmHitResult WmHitTest(int32_t mx, int32_t my);

// Set focus to window at index. Unfocuses all others. Raises to top.
void WmSetFocus(int idx);

// Get the currently focused window index (-1 if none).
int WmGetFocusedWindow();

// Get window by index (nullptr if invalid/unused).
Window* WmGetWindow(int idx);

// Get the number of active windows.
uint32_t WmWindowCount();

// Toggle maximise/restore for a window.
void WmToggleMaximize(int idx);

// Minimize a window (hide from desktop, show in taskbar).
void WmMinimizeWindow(int idx);

// Restore a minimized window.
void WmRestoreWindow(int idx);

// Move a window to a new position.
void WmMoveWindow(int idx, int16_t newX, int16_t newY);

// Resize a window's client area.  For terminal windows, reallocates the VFB
// and sends SIGWINCH.  For non-terminal windows, updates dimensions only.
void WmResizeWindow(int idx, uint16_t newClientW, uint16_t newClientH);

// Render window chrome (title bar, border, buttons) for all windows into backbuffer.
// Called by compositor loop after blitting client areas.
void WmRenderChrome(uint32_t* backBuffer, uint32_t stride,
                    uint32_t screenW, uint32_t screenH);

// Render chrome for a single window (used when interleaving content + chrome per z-layer).
void WmRenderChromeForWindow(uint32_t* backBuffer, uint32_t stride,
                              uint32_t screenW, uint32_t screenH, int idx);

// Check if window manager mode is active.
bool WmIsActive();

// Enable window manager mode.
void WmSetActive(bool active);

// Get all windows sorted by z-order (back to front).
// Returns count; fills outIndices with window indices in z-order.
uint32_t WmGetZOrder(int* outIndices, uint32_t maxOut);

// Render the taskbar (bottom of screen) with window buttons and clock.
void WmRenderTaskbar(uint32_t* backBuffer, uint32_t stride,
                     uint32_t screenW, uint32_t screenH,
                     uint64_t uptimeMs);

// Hit-test the taskbar. Returns the window index if a button was clicked, -1 otherwise.
int WmTaskbarHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH);

// Get the usable desktop height (screen height minus taskbar).
uint32_t WmDesktopHeight(uint32_t screenH);

// Spawn a new terminal window (Ctrl+T handler).
void WmSpawnTerminal();

// ---------------------------------------------------------------------------
// App Launcher
// ---------------------------------------------------------------------------

static constexpr uint32_t WM_LAUNCHER_MAX_ITEMS = 16;

struct LauncherItem {
    char title[48];
    char scriptPath[128];  // e.g. "/boot/SHORTCUTS/QUAKE.RC"
    uint32_t iconColor;    // Icon background color (0 = auto from title)
    bool valid;
};

// Load shortcut files from /boot/SHORTCUTS/ directory.
void WmLauncherLoad();

// Toggle the launcher popup open/closed.
void WmLauncherToggle();

// Is the launcher popup currently visible?
bool WmLauncherVisible();

// Render the launcher popup over the desktop.
void WmLauncherRender(uint32_t* backBuffer, uint32_t stride,
                      uint32_t screenW, uint32_t screenH);

// Hit-test the launcher popup. Returns item index (0..N) or -1 if miss.
int WmLauncherHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH);

// Launch the item at the given index.
void WmLauncherExec(int itemIdx);

} // namespace brook
