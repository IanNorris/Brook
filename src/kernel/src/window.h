#pragma once
#include "launcher_model.h"

#include <stdint.h>
#include "launch_history.h"
#include "desktop_metrics.h"
#include "../../shared/brook_theme.h"
#include "../../shared/brook_appearance.h"
#include "../../shared/brook_taskbar.h"
#include "../../shared/wm_window_state.h"

struct BrookDesktopSurface;

namespace brook {

struct Process;
struct AddressSpaceState;
struct MappingBacking;
struct CompositorVfbAllocation;
struct InputEvent;
struct FontAtlas;
struct ChromeMaterialCache;
const ChromeMaterialCache& WmChromeMaterial();
uint64_t WmMaterialCacheRebuilds();
const DesktopMetrics& WmDesktopMetrics();
uint32_t WmMetric(uint32_t base);
const FontAtlas& WmDesktopFont();
uint32_t WmGetRequestedScale();
uint32_t WmGetAppliedScale();
bool WmSetScale(uint32_t percent);
// Called by the compositor after old-frame input and before new-frame drawing.
bool WmApplyScalePending();

// Window manager constants. Frame and taskbar sizes come from DesktopMetrics.
static constexpr uint32_t WM_TITLE_BAR_HEIGHT = DesktopMetrics::TitleBase;
static constexpr uint32_t WM_BORDER_WIDTH     = DesktopMetrics::BorderBase;
static constexpr uint32_t WM_BUTTON_WIDTH     = DesktopMetrics::CaptionButtonBase;
// Resize zone sizes: ChromeResizeMetrics (chrome_layout.h).
static constexpr uint32_t WM_MIN_WIDTH        = 200;
static constexpr uint32_t WM_MIN_HEIGHT       = 100;
static constexpr uint32_t WM_MAX_WINDOWS      = 32;

// Retained per-window inputs for an eventual userspace desktop renderer. This
// is only the client-content part of the desktop; chrome, cursor and overlays
// are separate. Backing pins pages, not the lifetime of a kernel VFB pointer.
// Client pixels may change before the next dirty publication; callers must
// copy or otherwise synchronize them and validate before using the result.
struct WmDesktopLayer {
    MappingBacking* backing;
    uint64_t incarnation,generation,bytes;
    uint32_t window,width,height,stride,clientWidth,clientHeight;
    int32_t clientX,clientY;
    uint8_t upscale,legacyOpaque;
    uint8_t focused,noChrome,state,opacity,blurRadius;
    char title[64],appId[128];
    uint16_t ownerPid;uint64_t ownerIncarnation;
    uint32_t caretX,caretY,caretWidth,caretHeight;
};
struct WmDesktopLayers {
    uint32_t count,theme,appearance,scale;
    uint64_t launcherGeneration,settingsGeneration;
    WmDesktopLayer layers[WM_MAX_WINDOWS];
};
// Unsupported visible content fails the entire capture, so callers can fall
// back to the existing compositor without silently omitting a window.
bool WmAcquireDesktopLayers(WmDesktopLayers* out);
// Clone an already retained snapshot; the caller serializes its lifetime and
// releases the independent clone with WmReleaseDesktopLayers.
bool WmRetainDesktopLayers(const WmDesktopLayers& source,WmDesktopLayers* out);
bool WmDesktopLayersCurrent(const WmDesktopLayers& layers);
bool WmGpuSourceProviderMatches(uint32_t window,uint64_t incarnation,uint32_t pid,uint64_t providerIncarnation);
// Capture may tolerate changing decoration text/focus while retaining the
// same backing publication and geometry.
bool WmDesktopLayersCaptureCurrent(const WmDesktopLayers& layers);
// Run a short kernel presentation operation while WM geometry/order and
// published native-window generations remain locked. Legacy producers can
// still write or publish independently; this is not a pixel write barrier.
int64_t WmDesktopLayersUseIfCurrent(const WmDesktopLayers& layers,void* context,int64_t (*use)(void*));
// Presentation may be a frame behind: retain identity/backing safety without
// requiring newer content, geometry or appearance to stop changing.
int64_t WmDesktopLayersUseIfPresentable(const WmDesktopLayers& layers,void* context,int64_t (*use)(void*));
// Verify both the retained base and candidate under one WM lock, so a window
// cannot be replaced between the two identity/backing checks.
int64_t WmDesktopLayersUseIfBothPresentable(const WmDesktopLayers& base,const WmDesktopLayers& next,
                                            void* context,int64_t (*use)(void*));
struct WmDesktopLayerCopy { void* pixels; uint64_t capacity; };
// Copy into caller-owned kernel buffers. False means discard all copies;
// this does not prevent a client writing concurrently without publication.
bool WmCopyDesktopLayers(const WmDesktopLayers& layers,const WmDesktopLayerCopy* copies,uint32_t count);
void WmReleaseDesktopLayers(WmDesktopLayers* layers);
// Offscreen opaque-chrome prototype, one independent surface per window.
// It excludes glass effects and does not yet version theme, title or hover.
// key: WmChromeKey of the rendered pixels. interior: the transparent client
// rectangle relative to the surface (frames; empty for a caption block).
struct WmDesktopChrome {int32_t x,y;uint32_t width,height;uint64_t key;uint32_t interiorX,interiorY,interiorW,interiorH;};
// spansOnly: clear and render only the frame strips (outer minus interior);
// interior pixels of the buffer are left as they were.
int64_t WmDesktopChromeCapture(const WmDesktopLayers& layers,uint32_t index,
                               WmDesktopChrome* info,uint32_t* pixels,uint64_t capacity,
                               int32_t mouseX,int32_t mouseY,bool tolerateVisualChanges=false,bool spansOnly=false);
static constexpr uint32_t WM_TASKBAR_HEIGHT   = DesktopMetrics::TaskbarBase; // bottom taskbar height

// Chrome layout constants
static constexpr uint32_t WM_TITLE_TEXT_PAD_X  = 12;   // left padding before title text
static constexpr uint32_t WM_BTN_ICON_SIZE     = 10;  // size of max/min button icons
static constexpr uint32_t WM_BTN_ICON_PAD_BOT  = 6;   // minimize icon bottom offset

using WmPalette = BrookThemePalette;
// The painters' palette for the theme in use (selection resolved against the
// backend), and the hue's full token set (app content colours and more).
WmPalette WmCurrentPalette();
// 0xB02 op 1: a legacy palette id (0..2) chooses its hue.
bool WmSetTheme(uint32_t legacyPalette);
// A key that changes with the selection and with what it resolves to.
uint32_t WmGetTheme();
// Clearwater selection word (brook_theme.h: theme, mode, hue) and the theme
// in use (Glass, or its Opaque stand-in without backdrop blur).
uint64_t WmGetThemeSelection();
bool WmSetThemeSelection(uint64_t selection);
uint32_t WmResolvedTheme();
// One app content colour (BROOK_CONTENT_*) of the theme in use.
uint32_t WmContentColour(unsigned index);
uint32_t WmGetAppearance();
bool WmSetAppearance(uint32_t value);
uint32_t WmGetTaskbarMetrics();
bool WmSetTaskbarMetrics(uint32_t value);
void WmSetPerformanceMode(bool enabled);
// Stage 1.5: the compositor reports the active backend's backdrop-blur
// capability at each frame boundary; the theme resolves against it.
void WmSetBackendBlur(bool blur);
bool WmBackendBlur();
uint32_t WmResolvedAppearance();
void WmObserveDesktopTick(uint64_t now);
// Latest taskbar sample (CPU, memory, disk and network rates), for diagnostics.
struct DesktopTelemetry;
DesktopTelemetry WmDesktopTelemetryRead();
// The renderer composited a GPU-source frame for window identity/commit.
void WmNoteClientFrame(uint64_t identity,uint64_t commit);
// Surface records (layers ABI v2, chrome_surface.h): one per visible window
// with chrome, back to front, resolved against the active backend.
struct ChromeSurfaceEffects;
ChromeSurfaceEffects WmChromeSurfaceEffects();
uint32_t WmDesktopSurfaceRecords(BrookDesktopSurface* out,uint32_t max);
using WmRect = DesktopRect;
// Even nonzero values identify a stable badge style; odd means an update is in progress.
uint64_t WmBackdropStyleVersion();
bool WmBootSummaryRect(uint32_t,uint32_t,int32_t*,int32_t*,uint32_t*,uint32_t*);
static constexpr uint8_t WM_CHROME_TITLE_ALPHA = 255;
static constexpr int WM_BTN_RIGHT_PAD = 0;

// Taskbar layout constants
static constexpr uint32_t WM_TASKBAR_BTN_WIDTH  = 48; // icon button target width
static constexpr uint32_t WM_TASKBAR_BTN_HEIGHT = 48; // targets are the taskbar's full height
static constexpr uint32_t WM_TASKBAR_PADDING    = 0;  // targets abut; the pills are inset
static constexpr uint32_t WM_TASKBAR_TEXT_PAD_X = 6;   // text offset within button

enum class WindowState : uint8_t
{
    Normal,
    Maximized,
};
enum class WmClientOrigin : uint8_t { Unknown, NativeProcess, RegisteredUnixPeer };

struct Window
{
    const DesktopMetrics* metrics; // Immutable decoration geometry for this window.
    uint64_t    creationIdentity; // Monotonic identity across reused window slots.
    // Pointer-lock engagement (g_vfbLock). A capture revoked by the user or WM
    // (Super, Ctrl+Alt+Esc, focus/launcher/minimise) disengages the window; only
    // a physical primary press delivered to its client area re-engages it.
    uint64_t    pointerEngageSerial;
    bool        pointerDisengaged;
    Process*    proc;           // owning process (nullptr = unused slot)
    uint16_t    clientPid;      // Connection-origin TGID; never signal authority.
    uint64_t    clientIncarnation;
    uint64_t    clientLaunchId;
    WmClientOrigin clientOrigin;
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
    // CSD only: the WM still draws and owns its caption buttons over the
    // top-right of the client surface (Chromium's tab strip takes the rest).
    // Insets are physical pixels from the surface's right/top edges to the
    // client's window geometry (its transparent shadow margin).
    bool        captionOverlay;
    uint16_t    captionInsetRight,captionInsetTop;
    uint16_t    captionHeight;  // client's title-strip height (physical px); 0 = native title height
    uint32_t    captionStyle;   // DesktopCaptionStyle* packing; 0 = native title style
    bool        focusable;      // popups/tooltips stay above but don't take keyboard focus
    char        title[64];      // original title from the client
    char        webAppTitle[64]; // display override from the active launcher catalog
    char        appId[128];     // explicit xdg app_id, independent of document title

    // Per-window VFB (Phase A of wayland↔WM unification).  When non-null
    // the compositor blits from this buffer instead of `proc->fbVirtual`,
    // permitting multiple windows per process.  Allocated kernel-side and
    // mapped user-readable into `proc`'s address space at `vfbUser`.
    uint32_t*   vfb;            // kernel-virtual pointer (for blit reads)
    uint32_t    vfbStride;      // pixels per row (== clientW today)
    uint32_t    vfbWidth,vfbHeight; // Actual allocated extent, independent of requested geometry.
    uint64_t    vfbBytes;       // total allocation size (page-aligned)
    void*       vfbUser;        // user-space address in vfbAddressSpace
    AddressSpaceState* vfbAddressSpace; // retained original mapping identity
    MappingBacking* vfbBacking; // retained page snapshot; aliases own independent refs
    CompositorVfbAllocation* vfbRetirement; // reserved before publication
    uint8_t     vfbDirty;       // set by syscall on each commit; cleared after blit
    uint16_t    wmId;           // (idx + 1); 0 reserved for "invalid"

    // Only compositor-owned copies are published for hardware GL. No raw
    // source resource is retained here after WM_PRESENT_GRES returns.
    bool fineScroll;
    bool externalCaptured;
    uint32_t externalW,externalH;
    uint64_t publicationGeneration,publishedGeneration;
    uint64_t sourceCommitGeneration,sourceCommit,sourceWriteEpoch;
    bool sourceWriteActive;

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
        int16_t  x;          // client-local pixel, or vertical v120 for fine scroll
        int16_t  y;          // client-local pixel, or horizontal v120 for fine scroll
        uint32_t reserved;   // fine scroll pointer position, capture token or WM event flags
    };
    WmInputEvent inputQueue[WM_INPUT_QUEUE];
    volatile uint32_t inputHead;
    volatile uint32_t inputTail;
    uint32_t inputDropCount;

    // Pre-maximise geometry (for restore)
    int16_t     savedX, savedY;
    uint16_t    savedW, savedH;

    // Fullscreen (xdg_toplevel.set_fullscreen, EWMH _NET_WM_STATE_FULLSCREEN):
    // the client covers the whole output with no chrome, and while it is the
    // focused window the taskbar is hidden and the whole output is its input
    // area. state is Maximized meanwhile, so move/resize/rounding treat it as
    // fixed. The fs* fields hold what leaving fullscreen restores; decoration
    // requests made while fullscreen update them instead of the live window.
    bool        fullscreen;
    WindowState fsState;
    int16_t     fsX, fsY;
    uint16_t    fsW, fsH;
    bool        fsNoChrome, fsCaptionOverlay;
    uint16_t    fsCaptionInsetRight, fsCaptionInsetTop, fsCaptionHeight;
    uint32_t    fsCaptionStyle;

    // Side dock (wm_shell_dock.inc). A docked window keeps running but leaves
    // the taskbar, Alt+Tab and automatic focus. While docked it is placed as
    // the dock's page: the sidebar rectangle below the strip, no server-side
    // chrome, state Maximized (so the client is configured tiled and moves
    // are refused). It is drawn only while it is the selected page of a shown
    // dock; otherwise it is hidden through `minimized`, which every draw and
    // hit path already skips. The dk* fields hold what returning restores.
    bool        docked;
    WindowState dkState;
    int16_t     dkX, dkY;
    uint16_t    dkW, dkH;
    bool        dkNoChrome, dkCaptionOverlay;
    uint16_t    dkCaptionInsetRight, dkCaptionInsetTop, dkCaptionHeight;
    uint32_t    dkCaptionStyle;

    // Outer dimensions including chrome.  CSD (noChrome) windows have
    // no kernel chrome, so outer == client.
    const DesktopMetrics& desktopMetrics() const { return metrics ? *metrics : DesktopMetric100; }
    uint32_t metric(uint32_t base) const { return static_cast<uint32_t>(desktopMetrics().pixels(base)); }
    uint16_t outerWidth()  const { return noChrome ? clientW : (clientW + 2 * metric(WM_BORDER_WIDTH)); }
    uint16_t outerHeight() const { return noChrome ? clientH : (clientH + metric(WM_TITLE_BAR_HEIGHT) + 2 * metric(WM_BORDER_WIDTH)); }

    // Client area origin relative to outer top-left
    int16_t clientX() const { return noChrome ? x : (x + static_cast<int16_t>(metric(WM_BORDER_WIDTH))); }
    int16_t clientY() const { return noChrome ? y : (y + static_cast<int16_t>(metric(WM_TITLE_BAR_HEIGHT) + metric(WM_BORDER_WIDTH))); }
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
    ResizeLeft,
    ResizeTop,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomLeft,
    DockButton,     // native caption: move the window into the side dock
};
// Resize zones as edge masks (bit 0 left, 1 top, 2 right, 3 bottom; the
// chrome_layout.h ChromeEdge* values). Zero for any other zone.
inline constexpr uint8_t WmHitZoneResizeEdges(WmHitZone zone)
{
    return zone == WmHitZone::ResizeLeft ? 1 : zone == WmHitZone::ResizeTop ? 2 :
           zone == WmHitZone::ResizeRight ? 4 : zone == WmHitZone::ResizeBottom ? 8 :
           zone == WmHitZone::ResizeTopLeft ? 3 : zone == WmHitZone::ResizeTopRight ? 6 :
           zone == WmHitZone::ResizeBottomLeft ? 9 : zone == WmHitZone::ResizeCorner ? 12 : 0;
}
inline constexpr WmHitZone WmHitZoneForResizeEdges(uint8_t edges)
{
    return edges == 1 ? WmHitZone::ResizeLeft : edges == 2 ? WmHitZone::ResizeTop :
           edges == 4 ? WmHitZone::ResizeRight : edges == 8 ? WmHitZone::ResizeBottom :
           edges == 3 ? WmHitZone::ResizeTopLeft : edges == 6 ? WmHitZone::ResizeTopRight :
           edges == 9 ? WmHitZone::ResizeBottomLeft : edges == 12 ? WmHitZone::ResizeCorner : WmHitZone::None;
}

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
                   bool focusable = true, bool focusOnCreate = true);

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

// Enter or leave fullscreen (see Window::fullscreen). Returns false if the
// output size is unavailable or the resize cannot be queued.
bool WmSetFullscreen(int idx, bool enable);

// True while the focused window is fullscreen and no launcher or volume popup
// is open: the taskbar is neither drawn nor hit-tested.
bool WmTaskbarHidden();

// Top row of the taskbar for input routing: the screen height while hidden,
// otherwise WmDesktopHeight. Layout (maximize, work area) uses WmDesktopHeight.
uint32_t WmTaskbarTop(uint32_t screenH);

// Set maximise state idempotently.
void WmSetMaximized(int idx, bool enable, bool acknowledge = false);

// Minimize a window (hide from desktop, show in taskbar).
void WmMinimizeWindow(int idx);

// Restore a minimized window.
void WmRestoreWindow(int idx);
// Same cooperative close path for chrome, keyboard and semantic controls.
bool WmRequestCloseWindow(int idx);

struct WmDebugWindow {
    uint32_t id,providerPid,clientPid;
    uint64_t incarnation,providerIncarnation,clientIncarnation,launchId;
    WmClientOrigin clientOrigin;
    int32_t x,y;uint32_t width,height,outerWidth,outerHeight,bufferWidth,bufferHeight;
    bool visible,minimized,maximized,focused,focusable,apiWindow,providerKernel;
    bool fullscreen;
    char title[64],rawTitle[64],appId[128];
};
enum class WmDebugAction : uint32_t { Focus,Minimize,Maximize,Restore,Close };
// Registry-backed owner reference; caller must ProcessUnref after cleanup.
Process* WmReferenceTerminatedOwner();
// Value-only snapshots; no raw Window/Process pointers escape this API.
uint32_t WmSnapshotDebugWindows(WmDebugWindow* out,uint32_t capacity,uint64_t* tick);
// Debug capture: copy one window's current CPU client buffer (packed rows of
// *width pixels; *scale is its on-screen upscale). out=nullptr only reports
// the size. -2 unknown window, -95 no CPU buffer (GPU-published or none),
// -11 the buffer outgrew capacityPixels since it was sized.
int WmCopyDebugWindowBuffer(uint32_t id,uint64_t incarnation,uint32_t* out,uint64_t capacityPixels,
                            uint32_t* width,uint32_t* height,uint32_t* scale);
// Compositor context only. restoreTarget: 0 previous, 1 normal, 2 maximized.
int WmApplyDebugAction(uint32_t id,uint64_t incarnation,WmDebugAction action,
                      uint32_t restoreTarget,WmDebugWindow* result,uint32_t* closeMethod);

// Toggle CSD (client-side decoration) mode for a window.  When enabled the
// kernel WM stops drawing chrome and treats the whole outer area as client.
// Used by waylandd to honour zxdg_toplevel_decoration_v1.set_mode.
// captionOverlay keeps the WM's caption buttons over the client's top-right,
// inset from the surface edges by insetRight/insetTop physical pixels.
void WmSetClientSideDecoration(int idx, bool enable, bool captionOverlay=false,
                               uint16_t insetRight=0, uint16_t insetTop=0,
                               uint16_t captionHeight=0, uint32_t captionStyle=0);
// Screen rectangle of a window's overlaid caption buttons (empty if none).
WmRect WmCaptionOverlayRect(const Window& w);

// Per-window display-property bits for WmSetWindowProperties / syscall 0xB00.
static constexpr uint32_t WM_PROP_OPACITY = 1u << 0;  // apply opacity arg
static constexpr uint32_t WM_PROP_BLUR    = 1u << 1;  // apply blurRadius arg

// Frosted-glass preset applied by the Super+G window toggle.
static constexpr uint8_t  WM_GLASS_OPACITY = 200;     // translucent client (frost reads through)
static constexpr uint8_t  WM_GLASS_BLUR    = 4;       // backdrop blur radius

// Set per-window opacity and/or backdrop blur radius (selected by `mask`).
// Marks the compositor dirty. The GPU DRAW path reads these per window.
void WmToggleFocusedWindowGlass();
bool WmSetOwnedWindowProperties(Process* proc, uint32_t wmId, uint32_t mask,
                                uint8_t opacity, uint8_t blurRadius);

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
bool WmResizeWindow(int idx, uint16_t newClientW, uint16_t newClientH);

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

// Compositor thread: refresh the cached taskbar network state at most once per
// second from stack state (no device access). Returns true when it changed.
bool WmNetworkIndicatorRefresh(uint64_t now);

// Compositor thread: sample debug-bridge agent presence at most once per
// second into the taskbar cache. Returns true when the shown state changed.
bool WmAgentIndicatorRefresh(uint64_t now);

// Taskbar volume (compositor thread only). Refresh copies the master gain
// shared with Settings (BROOK_SYS_AUDIO_CONTROL) into the render cache and
// returns true when it changed. A left press opens/closes the popup or acts
// in it; the wheel steps 5%; a right click toggles mute. The input helpers
// return true when they consumed the event.
bool WmVolumeIndicatorRefresh();
// Plays the volume-change pop when a user change is observed (compositor loop).
void WmVolumeFeedbackTick(uint64_t nowMs);
bool WmTaskbarPopupPinned();
bool WmTaskbarSystemPopupModal();
bool WmTaskbarPopupDismissible();
void WmTaskbarPopupDismiss();
void WmTaskbarPopupForgetMenu();
bool WmTaskbarPopupPointerOwned(int32_t mx, int32_t my, uint32_t width, uint32_t height);
bool WmPerformanceHudPress(int32_t mx, int32_t my, uint32_t width, uint32_t height);
enum class DesktopStatsPanel : uint8_t;
DesktopStatsPanel WmDetailedStatsSelected();
bool WmDiskFlyoutSelected();
bool WmCpuFlyoutScroll(int32_t mx, int32_t my, int delta120, uint32_t width, uint32_t height);
bool WmDiskFlyoutScroll(int32_t mx, int32_t my, int delta120, uint32_t width, uint32_t height);
bool WmTrayFlyoutVisible();
bool WmTrayFlyoutPress(int32_t mx, int32_t my, uint32_t width, uint32_t height);
void WmTrayFlyoutClose();
bool WmTrayFlyoutRect(uint32_t width, uint32_t height, int32_t*, int32_t*, uint32_t*, uint32_t*);
void WmCyclePerformanceHud();
bool WmPerformanceHudRect(uint32_t width, uint32_t height, int32_t*, int32_t*, uint32_t*, uint32_t*);

bool WmVolumePopupVisible();
void WmVolumePopupClose();
void WmVolumePopupGetRect(uint32_t screenW, uint32_t screenH,
                          int32_t* outX, int32_t* outY, uint32_t* outW, uint32_t* outH);
bool WmVolumeLeftPress(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH);
bool WmVolumeDragActive();
void WmVolumeDrag(int32_t mx, bool held, uint32_t screenW, uint32_t screenH);
bool WmVolumeScroll(int32_t mx, int32_t my, int delta, uint32_t screenW, uint32_t screenH);
bool WmVolumeToggleMuteAt(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH);
void WmLauncherClose();
// Floating volume/network tooltip above the taskbar for pointer (mx,my);
// false when none is shown. Occupies the launcher/popup overlay slot.
bool WmTrayTooltipGetRect(uint32_t screenW, uint32_t screenH, int32_t mx, int32_t my,
                          int32_t* outX, int32_t* outY, uint32_t* outW, uint32_t* outH);

// Render the taskbar (bottom of screen) with window buttons and clock.

// Hit-test the taskbar. Returns the window index if a button was clicked, -1 otherwise.
void WmLaunchFeedbackPoll();
void WmTaskbarPendingClick(int target);
bool WmLaunchBusy();

int WmTaskbarHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH);

// Get the usable desktop height (screen height minus taskbar).
uint32_t WmDesktopHeight(uint32_t screenH);
// The work area: the desktop above the taskbar, less the side dock's width
// while it is pinned with entries. Maximise and tiling use it.
WmRect WmWorkArea(uint32_t screenW, uint32_t screenH);

// Side dock (wm_shell_dock.inc). Compositor thread.
// The dock's shell surface: the strip while shown, the edge tab while closed
// with entries. False when nothing of the dock is drawn.
bool WmDockSurfaceRect(uint32_t screenW, uint32_t screenH, int32_t* x, int32_t* y, uint32_t* w, uint32_t* h,
                       bool* material);
// The pointer is over the strip or the tab: shell input, not a window's.
bool WmDockPointerOwned(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH);
// A primary press. True when the strip or tab consumed it. A press outside a
// shown, unpinned sidebar closes the dock and returns false (it still reaches
// what lies beneath).
bool WmDockPress(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH);
// Super+S: open on the last page (or the first entry), or close.
void WmDockToggle();
// Super+Shift+S: dock the focused window, or return the focused page.
void WmDockFocusedWindow();
// Escape and the launcher: hide an open, unpinned dock. True if it closed.
bool WmDockDismiss();
// Open and unpinned: Escape belongs to the dock.
bool WmDockDismissible();
bool WmDockWindow(int idx);
bool WmDockReturnWindow(int idx);
bool WmDockSelect(int entry);
void WmDockSetPinned(bool pinned);
bool WmDockPinned();
bool WmDockOpen();
uint32_t WmDockEntryCount();
int WmDockEntryWindow(uint32_t entry);
// Placement: the right edge (else the left) and the logical width (0 default).
bool WmDockRight();
int WmDockWidth();
void WmDockSetRight(bool right);
void WmDockSetWidth(int width);
// The sidebar's inner-edge grip, pressed in WmDockPress: motion while the
// button is held resizes the page; the release settles and saves the width.
bool WmDockDragActive();
// The open sidebar's resize grip is under the point (the resize cursor).
bool WmDockGripAt(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH);
// Drag to dock, from the compositor's window move: each motion updates the
// drop target (feedback in the dock surface); the release docks the window
// if it is over the target, its return position being where the move began.
void WmDockWindowDrag(int idx, int32_t mx, int32_t my);
void WmDockWindowDragCancel();
bool WmDockWindowDrop(int idx, int32_t mx, int32_t my, int16_t startX, int16_t startY);
void WmDockDrag(int32_t mx, bool down);
// Alt+Tab: send the frontmost desktop window back and focus the next one.
// Docked windows take no part; an open, unpinned dock closes first. Returns
// the newly focused window, or -1.
int WmCycleFocus();

// Spawn a new terminal window (Super+Enter handler).
void WmSpawnTerminal();
void WmOpenFiles();

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
                                               bool focusable = true,
                                               bool nativeClient = false,
                                               bool fineScroll = false);

void WmDestroyWindowById(Process* proc, uint32_t wmId);
void WmSignalDirtyById(Process* proc, uint32_t wmId);
int64_t WmPublishSourceCommit(Process* proc,uint32_t wmId,uint64_t incarnation,uint64_t commit);
int64_t WmQuerySourceCommit(uint32_t wmId,uint64_t incarnation,uint64_t generation,uint64_t* commit);
int64_t WmBeginSourceWrite(Process* proc,uint32_t wmId,uint64_t incarnation);
// Reserve an incarnation/generation identity for publication after GPU completion.
struct WmCaptureToken {
    uint32_t wmId,w,h;
    uint64_t incarnation,generation;
};
struct WmContentSnapshot {
    bool live,externalCaptured;
    uint64_t incarnation,generation;
    uint32_t w,h;
};
// All publication checks hold the same lock as SHM, resize and destruction.
// Reserving a newer request does not invalidate the last published frame.
bool WmReserveCapture(Process*,uint32_t wmId,uint32_t w,uint32_t h,WmCaptureToken*);
bool WmGetCaptureBinding(Process*,uint32_t wmId,WmCaptureToken*);
// Scene capability already checked by the scene service. Bind to the original
// incarnation; fd transfer must never grant access to a reused window slot.
bool WmReserveBoundCapture(const WmCaptureToken& binding,uint32_t w,uint32_t h,WmCaptureToken*);
bool WmCaptureCurrent(const WmCaptureToken&);
bool WmCompleteCapture(const WmCaptureToken&);
WmContentSnapshot WmSnapshotContent(uint32_t slot);
void WmAcknowledgeContent(uint32_t slot,const WmContentSnapshot& uploaded);
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
// Coherent owned-window geometry; output is unchanged on error.
// Focused window for the desktop renderer's app-driven present cadence:
// its identity and BROOK_WINDOW_STATE_* flags (VISIBLE, FOCUSED, MINIMIZED,
// MAXIMIZED, FULLSCREEN, CLIENT_CHROME). False when nothing is focused.
bool WmSnapshotFocus(uint32_t* window, uint64_t* incarnation, uint32_t* flags);
bool WmClientScanoutWindowMatches(uint32_t window,uint64_t incarnation,uint32_t width,uint32_t height);
int WmSnapshotOwnedWindowState(Process* proc, uint32_t wmId,
                               uint64_t expectedIncarnation, BrookWindowState* out);
int WmPlaceOwnedWindow(Process* proc, uint32_t wmId, uint64_t incarnation,
                       int32_t clientX, int32_t clientY);

// Push an input event into a window's per-window queue.  Coords should be
// client-local for mouse events.  Drops on overflow.
bool WmInputPush(Window* win, const InputEvent& ev, int16_t localX, int16_t localY,
                 uint32_t captureToken = 0);
// WM_POINTER_CAPTURE: query=0, acquire=1, release=2, engage serial=3,
// activation pending=4 (1 when acquire may skip the hover requirement).
// Positive leases never identify a later incarnation of the same window slot.
// Acquire also requires the pointer to be over the window's own client area
// (never the taskbar, launcher, volume popup, chrome or another window) and,
// after an involuntary revoke, re-engagement: a fresh physical click in that
// client area, or a deliberate activation (Alt+Tab, taskbar button). An
// activation also waives the hover requirement once: the grant warps the
// pointer into the window's content. The engage serial is per window and
// advances only on those two gestures.
int64_t WmPointerCapture(Process* provider,uint32_t wmId,uint32_t operation,uint32_t token);
bool WmPointerCaptureActive();
uint64_t WmPointerCaptureRevision();
uint32_t WmPointerSuppressedButtons();
void WmPointerCaptureRefresh();
// User-initiated revoke (the Super key): same as the provider losing its lease,
// so waylandd unlocks and stays inhibited until a fresh click in the window.
bool WmPointerCaptureReleaseByUser();
bool WmRouteCapturedPointer(const InputEvent& event);
// WM routing publishes where pointer input is currently delivered: the client
// area of window `idx`, or -1 for shell surfaces (taskbar, launcher, popups),
// chrome and desktop. Acquire is refused unless it names the requester.
void WmPointerSetHover(int idx);
// Creation identity of the window currently hovered (as published by
// WmPointerSetHover), or 0 over shell surfaces, chrome and the desktop.
uint64_t WmPointerHoverIdentity();
// The hovered identity only when `proc` owns the hovered window, else 0.
uint64_t WmPointerHoverIdentityOf(Process* proc);
// Creation identity of `proc`'s window `wmId`, or 0 when it owns no such window.
uint64_t WmWindowCreationIdentity(Process* proc, uint32_t wmId);
// A physical primary press was delivered to window `idx`'s client area.
void WmPointerEngage(int idx);
// Window `idx` was just deliberately activated (Alt+Tab, taskbar button) and
// is focused. Call after the focus change, never for click-to-focus.
void WmPointerActivate(int idx);
bool WmInputPushRelative(Window* window,const InputEvent& event);

// Drain up to `max` events from the window's queue into `out`.  Returns
// number of events actually written.  Non-blocking.
uint32_t WmInputPop(Window* win, Window::WmInputEvent* out, uint32_t max);
int WmInputPopOwned(Process* proc, uint32_t wmId, uint64_t incarnation,
                    Window::WmInputEvent* out, uint32_t max);
// Same, for the legacy id-only syscall: finds and drains the caller's window
// under the window lock into a kernel buffer. Returns -ENOENT (-2) if absent.
int WmInputPopById(Process* proc, uint32_t wmId, Window::WmInputEvent* out, uint32_t max);

// Phase C — synthetic "WM event" types.  These share the same ring as
// input events; clients distinguish via the `type` byte.  Values >= 0x80
// are reserved for WM events to avoid colliding with InputEventType.
static constexpr uint8_t WM_EVT_CLOSE_REQUESTED = 0x80;
static constexpr uint8_t WM_EVT_FOCUS_GAINED    = 0x81;
static constexpr uint8_t WM_EVT_FOCUS_LOST      = 0x82;
static constexpr uint8_t WM_EVT_RESIZED         = 0x83; // x=newW, y=newH; reserved=WM_RESIZE_*
// Pointer input stopped going to this window's client area (it moved to the
// desktop, a shell surface, the WM's caption buttons or another window). The
// provider sends wl_pointer.leave; the next delivered event re-enters.
// Withheld while a button is held over nothing (implicit grab).
static constexpr uint8_t WM_EVT_POINTER_LEFT    = 0x84;
static constexpr uint32_t WM_RESIZE_MAXIMIZED = 1;
static constexpr uint32_t WM_RESIZE_RESIZING = 2;
static constexpr uint32_t WM_RESIZE_FULLSCREEN = 4; // never with MAXIMIZED

// Push a synthetic WM event into the per-window queue.  Drops on overflow.
bool WmPushWmEvent(Window* win, uint8_t type, int16_t x = 0, int16_t y = 0, uint32_t resizeFlags = 0);
// Kernel-authenticated socket origin, associated by the provider. Immutable
// after the first registration, and only for a window owned by that provider.
bool WmRegisterClientOrigin(Process* provider,uint32_t wmId,uint16_t clientPid,
                            uint64_t incarnation,uint64_t launchId);

// ---------------------------------------------------------------------------
// App Launcher
// ---------------------------------------------------------------------------

static constexpr uint32_t WM_LAUNCHER_MAX_ITEMS = 128;

static constexpr uint32_t LAUNCHER_ICON_PX = 24;  // Icon dimensions (24x24 pixels)
static constexpr uint32_t LAUNCHER_ICON_BYTES = LAUNCHER_ICON_PX * LAUNCHER_ICON_PX * 4;

struct LauncherItem {
    char catalogId[33], specification[33];
    char title[48];
    char appId[128];       // desktop basename without .desktop (optional manifest field)
    char executable[64];  // executable basename for native/older manifest matching
    char scriptPath[128];  // e.g. "/boot/SHORTCUTS/QUAKE.RC" or exec command
    char description[64];  // shortcut "# desc:" (launcher row subtitle); may be empty
    char category[16];     // declared default launcher category ID ("# category:", or from Categories=)
    uint32_t iconColor;    // Icon background color (0 = auto from title)
    uint32_t* iconPixels;  // Raw ARGB icon data (24x24), or nullptr for generic application icon
    bool valid;
    bool isDesktopEntry;   // launch a program instead of a shortcut script
    bool isUserDesktopEntry;
    bool isWebApp;
};

struct WmDebugApp {
    char id[33],title[48],desktopAppId[128],executable[64];
    bool desktop,userDesktop,eligible;
};
uint32_t WmSnapshotDebugApps(WmDebugApp* out,uint32_t capacity,
                             uint32_t* generation,uint64_t* builtTick);

// Load shortcut files from /boot/SHORTCUTS/ directory.
void WmLauncherLoad();

struct LauncherPersistenceResult {
    const char* stage = "unattempted";
    int error = 0;
};

struct LauncherPersistenceStatus {
    LauncherPersistenceResult load, save;
    uint32_t layoutEpoch = 0;
    uint32_t savedEpoch = 0;
    bool pending = false;
    bool saving = false;
    const char* worker = nullptr;
    const char* path = nullptr;
    uint32_t pinCount = 0;
    char pins[LauncherLimits::Pins][LauncherLimits::KeyLength]{};
};

void WmLauncherPersistenceStatus(LauncherPersistenceStatus& out);

struct LauncherUserManifestStatus {
    LauncherPersistenceResult load;
    uint32_t entries = 0;
    uint32_t rejected = 0;
};
LauncherUserManifestStatus WmLauncherUserManifestStatus();
void WmRequestUserLauncherRefresh();

// Notification is nonblocking; a worker rebuilds and the compositor publishes.
void WmRequestLauncherRefresh();
// Compositor-only: apply after input, before geometry/render for this frame.
bool WmLauncherApplyPending();

// Toggle the launcher popup open/closed.
void WmLauncherToggle();

// Is the launcher popup currently visible?
struct WmShellTrayGeometry;
void WmGetShellTrayGeometry(uint32_t sw, uint32_t sh, int32_t x, int32_t y, WmShellTrayGeometry&);
bool WmShellTrayPointer(uint32_t sw, uint32_t sh, int32_t x, int32_t y,
                       bool pressed, int button, int scroll);
// The window menu (title bar right-click, xdg show_window_menu). x/y: the
// anchor in screen pixels. Routed through WmShellTrayPointer while open.
bool WmShowWindowMenu(int idx, int32_t x, int32_t y);
bool WmWindowMenuOpen();
void WmWindowMenuClose();
bool WmLauncherVisible();

// Scroll the launcher by delta (negative = up, positive = down).
// delta120: 1/120 wheel ticks, up positive.
void WmLauncherScroll(int delta120, uint32_t screenW, uint32_t screenH);

// Render the launcher popup over the desktop.

// Hit-test the launcher popup. Returns item index (0..N) or -1 if miss.
int WmLauncherHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH);
// Pointer press (secondary: right button) while the launcher is open; false
// when outside the panel. Keys while it is open (scan, translated ASCII, mods).
bool WmLauncherGesture(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH, bool released);
bool WmLauncherPress(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH, bool secondary);
void WmLauncherKey(uint8_t scan, char ascii, uint8_t modifiers);

// Get the launcher popup panel geometry (screen-space rect). Lets the GPU
// compositor mark its pixels opaque / compose it as a translucent overlay.
void WmLauncherGetRect(uint32_t screenW, uint32_t screenH,
                       int32_t* outX, int32_t* outY,
                       uint32_t* outW, uint32_t* outH);

// Semantic admission is compositor-only and uses the same worker as a click.
int WmLauncherActivate(const char* appId,uint32_t generation,uint64_t* token,bool* coalesced);
bool WmSnapshotLaunch(uint64_t token,LaunchRecord* out);
uint32_t WmSnapshotLaunches(LaunchRecord* out,uint32_t capacity);
void WmCancelBridgeQueuedLaunches(uint32_t* cancelled,uint32_t* startedOrShared);

// Launch the item at the given index. Hit-test results at or above
// WM_LAUNCHER_MAX_ITEMS are footer power targets: a power button only arms a
// confirmation prompt; the separate confirm button performs it.
void WmLauncherExec(int itemIdx);
// The launcher power confirmation is showing; it owns Enter, Escape, Tab and
// Left/Right until dismissed.
bool WmLauncherPowerPrompt();
void WmLauncherPowerKey(uint8_t scan);
void WmSetLaunchInhibited(bool inhibited);
bool WmLauncherDispatching();

} // namespace brook
