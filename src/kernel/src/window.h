#pragma once

#include <stdint.h>
#include "../../shared/brook_theme.h"

namespace brook {

struct Process;
struct AddressSpaceState;
struct MappingBacking;
struct CompositorVfbAllocation;
struct InputEvent;

// Window manager constants
static constexpr uint32_t WM_TITLE_BAR_HEIGHT = 36;
static constexpr uint32_t WM_BORDER_WIDTH     = 2;
static constexpr uint32_t WM_BUTTON_WIDTH     = 46;
static constexpr uint32_t WM_RESIZE_GRAB      = 16;  // corner/edge grab zone for resize
static constexpr uint32_t WM_RESIZE_EDGE      = 6;   // edge-only grab zone (bottom/right)
static constexpr uint32_t WM_MIN_WIDTH        = 200;
static constexpr uint32_t WM_MIN_HEIGHT       = 100;
static constexpr uint32_t WM_MAX_WINDOWS      = 32;
static constexpr uint32_t WM_TASKBAR_HEIGHT   = 44;   // bottom taskbar height

// Chrome layout constants
static constexpr uint32_t WM_TITLE_TEXT_PAD_X  = 12;   // left padding before title text
static constexpr uint32_t WM_BTN_ICON_SIZE     = 10;  // size of max/min button icons
static constexpr uint32_t WM_BTN_ICON_PAD_BOT  = 6;   // minimize icon bottom offset

using WmPalette = BrookThemePalette;
const WmPalette& WmCurrentPalette();
bool WmSetTheme(uint32_t theme);
uint32_t WmGetTheme();
static constexpr uint8_t WM_CHROME_TITLE_ALPHA = 255;
static constexpr int WM_BTN_RIGHT_PAD = 0;

// Taskbar layout constants
static constexpr uint32_t WM_TASKBAR_BTN_WIDTH  = 44; // icon button width
static constexpr uint32_t WM_TASKBAR_BTN_HEIGHT = 32;
static constexpr uint32_t WM_TASKBAR_PADDING    = 4;
static constexpr uint32_t WM_TASKBAR_TEXT_PAD_X = 6;   // text offset within button

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
    uint32_t    zOrder;         // higher = on top (0 = backmost)
    uint8_t     upscale;        // integer upscale factor (1 = 1:1, 2 = 2×, 4 = 4×)
    uint8_t     opacity;        // per-window opacity 0..255 (255 = opaque); GPU DRAW path
    uint8_t     blurRadius;     // backdrop blur behind the window/chrome (0 = off); GPU DRAW path
    WindowState state;
    bool        focused;
    bool        visible;
    bool        minimized;      // hidden from desktop, shown in taskbar
    bool        noChrome;       // CSD: client draws own chrome; WM skips title/border
    bool        focusable;      // popups/tooltips stay above but don't take keyboard focus
    char        title[64];
    char        appId[128];     // explicit xdg app_id, independent of document title

    // Per-window VFB (Phase A of wayland↔WM unification).  When non-null
    // the compositor blits from this buffer instead of `proc->fbVirtual`,
    // permitting multiple windows per process.  Allocated kernel-side and
    // mapped user-readable into `proc`'s address space at `vfbUser`.
    uint32_t*   vfb;            // kernel-virtual pointer (for blit reads)
    uint32_t    vfbStride;      // pixels per row (== clientW today)
    uint64_t    vfbBytes;       // total allocation size (page-aligned)
    void*       vfbUser;        // user-space address in vfbAddressSpace
    AddressSpaceState* vfbAddressSpace; // retained original mapping identity
    MappingBacking* vfbBacking; // retained page snapshot; aliases own independent refs
    CompositorVfbAllocation* vfbRetirement; // reserved before publication
    uint8_t     vfbDirty;       // set by syscall on each commit; cleared after blit
    uint16_t    wmId;           // (idx + 1); 0 reserved for "invalid"

    // Hardware GL (dmabuf) presentation: when a Wayland client shares a
    // hardware-rendered buffer via zwp_linux_dmabuf_v1, waylandd resolves it to
    // a host virgl resource id and records it here via WM_PRESENT_GRES. The
    // compositor then BLITs this resource into the window's content texture
    // instead of uploading the (unused) VFB. 0 = ordinary SHM/VFB window.
    uint32_t    externalGres;   // host virgl resource id of the latest GL frame
    uint32_t    externalW, externalH;  // its dimensions

    // Per-window input ring (Phase B).  When the WM routes an event
    // to this window (mouse over client area, or keyboard while focused),
    // the event is pushed here in addition to the legacy per-process queue.
    // Userspace drains via syscall WM_POP_INPUT.  Mouse coords are
    // *client-local* (0,0 = top-left of the window's client area).
    static constexpr uint32_t WM_INPUT_QUEUE = 256;
    struct WmInputEvent {
        uint8_t  type;       // InputEventType
        uint8_t  scanCode;
        uint8_t  ascii;
        uint8_t  modifiers;
        int16_t  x;          // client-local pixel (mouse events)
        int16_t  y;
        uint32_t reserved;
    };
    WmInputEvent inputQueue[WM_INPUT_QUEUE];
    volatile uint32_t inputHead;
    volatile uint32_t inputTail;
    uint32_t inputDropCount;

    // Pre-maximise geometry (for restore)
    int16_t     savedX, savedY;
    uint16_t    savedW, savedH;

    // Outer dimensions including chrome.  CSD (noChrome) windows have
    // no kernel chrome, so outer == client.
    uint16_t outerWidth()  const { return noChrome ? clientW : (clientW + 2 * WM_BORDER_WIDTH); }
    uint16_t outerHeight() const { return noChrome ? clientH : (clientH + WM_TITLE_BAR_HEIGHT + 2 * WM_BORDER_WIDTH); }

    // Client area origin relative to outer top-left
    int16_t clientX() const { return noChrome ? x : (x + static_cast<int16_t>(WM_BORDER_WIDTH)); }
    int16_t clientY() const { return noChrome ? y : (y + static_cast<int16_t>(WM_TITLE_BAR_HEIGHT + WM_BORDER_WIDTH)); }
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
    ResizeRight,    // right edge
    ResizeBottom,   // bottom edge
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
                   const char* title, uint8_t upscale = 1,
                   bool focusable = true);

// Remove a window (process exited or closed).
void WmDestroyWindow(int idx);
uint64_t WmMapWindowVfb(Process* caller,uint32_t wmId);
// Compositor-epoch readers only: captures one coherent native source tuple.
bool WmSnapshotVfb(const Window* window,uint32_t** pixels,uint32_t* stride,uint64_t* bytes);

// Remove the window owned by a specific process (safety net for ProcessDestroy).
void WmDestroyWindowForProcess(Process* proc);

// Hit-test: given mouse coordinates, determine which window/zone is under cursor.
WmHitResult WmHitTest(int32_t mx, int32_t my);

// Set focus to window at index. Unfocuses all others. Raises to top.
void WmSetFocus(int idx);

// Send a window to the back of the z-order (lowest z).
void WmSendToBack(int idx);

// Get the currently focused window index (-1 if none).
int WmGetFocusedWindow();

// Get window by index (nullptr if invalid/unused).
Window* WmGetWindow(int idx);

// Get the number of active windows.
uint32_t WmWindowCount();

// Toggle maximise/restore for a window.
void WmToggleMaximize(int idx);

// Set maximise state idempotently.
void WmSetMaximized(int idx, bool enable);

// Minimize a window (hide from desktop, show in taskbar).
void WmMinimizeWindow(int idx);

// Restore a minimized window.
void WmRestoreWindow(int idx);

// Toggle CSD (client-side decoration) mode for a window.  When enabled the
// kernel WM stops drawing chrome and treats the whole outer area as client.
// Used by waylandd to honour zxdg_toplevel_decoration_v1.set_mode.
void WmSetClientSideDecoration(int idx, bool enable);

// Per-window display-property bits for WmSetWindowProperties / syscall 0xB00.
static constexpr uint32_t WM_PROP_OPACITY = 1u << 0;  // apply opacity arg
static constexpr uint32_t WM_PROP_BLUR    = 1u << 1;  // apply blurRadius arg

// Frosted-glass preset applied by the Super+G window toggle.
static constexpr uint8_t  WM_GLASS_OPACITY = 200;     // translucent client (frost reads through)
static constexpr uint8_t  WM_GLASS_BLUR    = 4;       // backdrop blur radius

// Set per-window opacity and/or backdrop blur radius (selected by `mask`).
// Marks the compositor dirty. The GPU DRAW path reads these per window.
void WmSetWindowProperties(int idx, uint32_t mask, uint8_t opacity, uint8_t blurRadius);

// Move a window to a new position.
void WmMoveWindow(int idx, int16_t newX, int16_t newY);

// Move one WM-API window relative to another window's client-area origin.
// Used by waylandd for xdg_popup placement, where popup coordinates are
// relative to the parent xdg surface rather than screen coordinates.
bool WmMoveWindowRelativeToParent(Process* proc, uint32_t wmId,
                                  uint32_t parentWmId,
                                  int32_t relX, int32_t relY);

// Resize a window's client area.  For terminal windows, reallocates the VFB
// and sends SIGWINCH.  For non-terminal windows, updates dimensions only.
void WmResizeWindow(int idx, uint16_t newClientW, uint16_t newClientH);

// Return the window index owned by `proc`, or -1 if none.
int WmFindWindowForProcess(Process* proc);

// Render window chrome (title bar, border, buttons) for all windows into backbuffer.
// Called by compositor loop after blitting client areas.
void WmRenderChrome(uint32_t* backBuffer, uint32_t stride,
                    uint32_t screenW, uint32_t screenH);

// Render chrome for a single window (used when interleaving content + chrome per z-layer).
void WmRenderChromeForWindow(uint32_t* backBuffer, uint32_t stride,
                              uint32_t screenW, uint32_t screenH, int idx,
                              int32_t mouseX, int32_t mouseY);

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
                     uint64_t uptimeMs, int32_t mouseX, int32_t mouseY);

// Hit-test the taskbar. Returns the window index if a button was clicked, -1 otherwise.
int WmTaskbarHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH);

// Get the usable desktop height (screen height minus taskbar).
uint32_t WmDesktopHeight(uint32_t screenH);

// Spawn a new terminal window (Ctrl+T handler).
void WmSpawnTerminal();

// ---------------------------------------------------------------------------
// Per-window VFB API (Phase A of wayland↔WM unification).
//
// Lets a single process (e.g. waylandd) own multiple top-level windows,
// each backed by its own VFB.  The kernel allocates the VFB pages, maps
// them user-readable into `proc`, and registers the window with the WM.
// On destroy the pages are unmapped + freed.
//
// Returned wmId is `windowIndex + 1` so 0 can mean "invalid".
// ---------------------------------------------------------------------------

struct WmCreateWindowResult {
    uint32_t wmId;     // 0 on failure
    void*    vfbUser;  // user-virtual VFB pointer
    uint32_t vfbStride;
};

WmCreateWindowResult WmCreateWindowForProcess(Process* proc,
                                               uint16_t clientW,
                                               uint16_t clientH,
                                               const char* title,
                                               bool focusable = true);

void WmDestroyWindowById(Process* proc, uint32_t wmId);
void WmSignalDirtyById(Process* proc, uint32_t wmId);
// Record a hardware-GL (dmabuf) frame for a window: store the host virgl
// resource id the compositor should present, and mark the window dirty. Returns
// false if the window isn't found / not owned by proc.
bool WmPresentExternalById(Process* proc, uint32_t wmId, uint32_t gres,
                           uint32_t w, uint32_t h);
bool WmSetAppIdById(Process* proc, uint32_t wmId, const char* appId);

void WmSetTitleById(Process* proc, uint32_t wmId, const char* title);

// Resize an existing WM-API window's VFB to (newW × newH).  Allocates fresh
// kernel pages, zeros them, unmaps the old user-side mapping, and maps the
// new pages at the same userBase if it fits, else at a fresh user range.
// Used by waylandd after sending xdg_toplevel.configure and the client
// commits a buffer at the new size.  Returns 0 on success, -errno on
// failure; on success populates *outResult with the new vfbUser pointer
// and stride (which may differ from the old stride).  The kernel-side
// w.clientW/H are also updated to the new dimensions.
int WmResizeVfbForProcess(Process* proc, uint32_t wmId,
                          uint16_t newW, uint16_t newH,
                          WmCreateWindowResult* outResult);

// Look up a Window* given (proc, wmId).  Returns nullptr if mismatch.
Window* WmFindWindowById(Process* proc, uint32_t wmId);

// Push an input event into a window's per-window queue.  Coords should be
// client-local for mouse events.  Drops on overflow.
void WmInputPush(Window* win, const InputEvent& ev, int16_t localX, int16_t localY);

// Drain up to `max` events from the window's queue into `out`.  Returns
// number of events actually written.  Non-blocking.
uint32_t WmInputPop(Window* win, Window::WmInputEvent* out, uint32_t max);

// Phase C — synthetic "WM event" types.  These share the same ring as
// input events; clients distinguish via the `type` byte.  Values >= 0x80
// are reserved for WM events to avoid colliding with InputEventType.
static constexpr uint8_t WM_EVT_CLOSE_REQUESTED = 0x80;
static constexpr uint8_t WM_EVT_FOCUS_GAINED    = 0x81;
static constexpr uint8_t WM_EVT_FOCUS_LOST      = 0x82;
static constexpr uint8_t WM_EVT_RESIZED         = 0x83; // x=newW, y=newH

// Push a synthetic WM event into the per-window queue.  Drops on overflow.
void WmPushWmEvent(Window* win, uint8_t type, int16_t x = 0, int16_t y = 0);

// ---------------------------------------------------------------------------
// App Launcher
// ---------------------------------------------------------------------------

static constexpr uint32_t WM_LAUNCHER_MAX_ITEMS = 128;

static constexpr uint32_t LAUNCHER_ICON_PX = 24;  // Icon dimensions (24x24 pixels)
static constexpr uint32_t LAUNCHER_ICON_BYTES = LAUNCHER_ICON_PX * LAUNCHER_ICON_PX * 4;

struct LauncherItem {
    char title[48];
    char appId[128];       // desktop basename without .desktop (optional manifest field)
    char executable[64];  // executable basename for native/older manifest matching
    char scriptPath[128];  // e.g. "/boot/SHORTCUTS/QUAKE.RC" or exec command
    uint32_t iconColor;    // Icon background color (0 = auto from title)
    uint32_t* iconPixels;  // Raw ARGB icon data (24x24), or nullptr for generic application icon
    bool valid;
    bool isDesktopEntry;   // true if sourced from .desktop file (exec directly)
};

// Load shortcut files from /boot/SHORTCUTS/ directory.
void WmLauncherLoad();
// Notification is nonblocking; a worker rebuilds and the compositor publishes.
void WmRequestLauncherRefresh();
// Compositor-only: apply after input, before geometry/render for this frame.
bool WmLauncherApplyPending();

// Toggle the launcher popup open/closed.
void WmLauncherToggle();

// Is the launcher popup currently visible?
bool WmLauncherVisible();

// Scroll the launcher by delta (negative = up, positive = down).
void WmLauncherScroll(int delta, uint32_t screenW, uint32_t screenH);

// Render the launcher popup over the desktop.
void WmLauncherRender(uint32_t* backBuffer, uint32_t stride,
                      uint32_t screenW, uint32_t screenH,
                      int32_t mouseX, int32_t mouseY);

// Hit-test the launcher popup. Returns item index (0..N) or -1 if miss.
int WmLauncherHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH);

// Get the launcher popup panel geometry (screen-space rect). Lets the GPU
// compositor mark its pixels opaque / compose it as a translucent overlay.
void WmLauncherGetRect(uint32_t screenW, uint32_t screenH,
                       int32_t* outX, int32_t* outY,
                       uint32_t* outW, uint32_t* outH);

// Launch the item at the given index.
void WmLauncherExec(int itemIdx);

} // namespace brook
