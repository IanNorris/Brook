#pragma once
#include <stdint.h>
// Experimental client-content export. It does not include wallpaper, chrome,
// cursor or overlays and grants no display ownership. On error discard every
// copied pixel; the client may write without publishing while a copy runs.
#define BROOK_SYS_DESKTOP_LAYERS 0xB19
// An immutable compositor-thread taskbar/launcher snapshot. Uses the same
// request/status/read/release protocol and info structure as debug frames,
// but is available to the experimental renderer and contains alpha.
#define BROOK_SYS_DESKTOP_OVERLAY 0xB1A
#define BROOK_DESKTOP_LAYERS_VERSION 1
#define BROOK_DESKTOP_LAYER_LIMIT 32
#define BROOK_DESKTOP_LAYERS_QUERY 0
#define BROOK_DESKTOP_LAYERS_CAPTURE 1
// Capture and bind the client-layer metadata to the attached GPU session.
// reserved must be zero on input and returns a nonzero scene token. It does
// not track desktop decorations or unannounced client writes.
#define BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED 2
// Opaque wallpaper and build badge plane, below windows and other overlays.
// QUERY fills layer 0 metadata. CAPTURE requires the returned metadata and a
// destination in pixels/capacity; on any error discard all copied pixels.
// TRACKED_CAPTURE additionally binds it to an idle tracked client session.
#define BROOK_DESKTOP_BACKDROP_QUERY 3
#define BROOK_DESKTOP_BACKDROP_CAPTURE 4
#define BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE 5
// One window's opaque chrome in a tightly packed ARGB surface. QUERY takes
// layers[0].window; CAPTURE takes returned metadata plus pixels/capacity.
// Offscreen only: theme, title, focus and hover are not yet tracked.
#define BROOK_DESKTOP_CHROME_QUERY 6
#define BROOK_DESKTOP_CHROME_CAPTURE 7
// As CAPTURE, and binds this window's chrome to the attached tracked scene.
#define BROOK_DESKTOP_CHROME_TRACKED_CAPTURE 8
// Like tracked capture, with argument a as a 32-bit mask of layers whose
// pixels are supplied separately. All input window/incarnation identities
// must still match the query; skipped layers require zero pixels/capacity.
#define BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_SELECTIVE 9
// Bind current display/backdrop identity for a renderer that draws a clear
// or a procedural backdrop. Returns the same metadata as QUERY.
#define BROOK_DESKTOP_BACKDROP_TRACKED_BIND 10
// Change-based chrome. TRACKED_CAPTURE_KEYED is TRACKED_CAPTURE that also
// returns, in BrookDesktopLayers.reserved, a nonzero content key of exactly
// the pixels it wrote (input reserved must be zero). TRACKED_KEEP takes the
// metadata a keyed capture returned plus that key in reserved and no pixels:
// if the window's chrome would render identically it binds the chrome to
// the tracked scene and refreshes the metadata (position may move) without
// rendering or copying; otherwise it fails with ESTALE and the caller
// recaptures into the same buffer.
#define BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_KEYED 11
#define BROOK_DESKTOP_CHROME_TRACKED_KEEP 12
// Change-based client layers. As CAPTURE_TRACKED_SELECTIVE (a = skip mask),
// plus b = a 32-bit mask of layers whose pixels the caller already holds.
// For a keep layer the caller sets generation/bytes/width/height/stride to
// the values of the capture that filled its buffer and still supplies that
// buffer in pixels/capacity. If the window's published content generation
// and geometry are unchanged (and no terminal caret is overlaid) the kernel
// copies nothing and sets BROOK_DESKTOP_LAYER_KEPT in the returned flags;
// otherwise it copies as usual. A client write the producer has not yet
// published is not reflected, exactly as with the CPU compositor.
#define BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_KEEP 13
// Renderer report for the taskbar APP cadence: it composited a GPU-source
// client frame. address=0, size=0, a=window, b=window incarnation (layer
// incarnation), c=client commit. Diagnostic only; the kernel counts each new
// commit once for the focused window.
#define BROOK_DESKTOP_CLIENT_FRAME 14
// Focused window and the app-driven present policy (kernel debug commands
// "desktop present ..."; RAM only, off at boot). address = struct
// BrookDesktopFocus, size = its size, a = b = c = 0. window 0: nothing
// focused. state holds BROOK_WINDOW_STATE_* bits (wm_window_state.h). Older
// kernels return EINVAL; the renderer then keeps its own cadence.
#define BROOK_DESKTOP_FOCUS_QUERY 15
#define BROOK_DESKTOP_FOCUS_VERSION 1
// A focused window covering more than 60% of the output drives presents.
#define BROOK_DESKTOP_POLICY_APP_CADENCE 1u
// Evaluate direct scanout eligibility and count it, without scanning out.
#define BROOK_DESKTOP_POLICY_DIRECT_SHADOW 2u
// Scan an eligible fullscreen client buffer out directly (needs a kernel
// with BROOK_GPU_PRESENT_CAP_CLIENT_SCANOUT; otherwise counted as a fallback).
#define BROOK_DESKTOP_POLICY_DIRECT_ON 4u
// Shell popups open (BrookDesktopFocus.shell, version 2). Tooltips, the
// boot summary and anything else the overlay draws show as overlay bands.
#define BROOK_DESKTOP_SHELL_LAUNCHER 1u
#define BROOK_DESKTOP_SHELL_VOLUME 2u
// Version 2 (48 bytes) adds shell and the pointer position; a kernel also
// accepts the 32-byte version 1 request and fills only its fields.
#define BROOK_DESKTOP_FOCUS_VERSION_POINTER 2
struct BrookDesktopFocus {
    uint32_t version,window;
    uint64_t incarnation;
    uint32_t state,policy;
    // Longest delay (us) for a present that carries only non-app damage
    // (pointer, overlay, other windows); 0 = the renderer default.
    uint32_t deferUs;
    uint32_t shell;          // BROOK_DESKTOP_SHELL_* (version 2; 0 in version 1)
    int32_t pointerX,pointerY; // desktop pixels (version 2)
    uint32_t reserved[2];
};
#define BROOK_DESKTOP_FOCUS_V1_SIZE 32
#define BROOK_DESKTOP_LAYER_LEGACY_OPAQUE 1u
// Omitted means 255; older renderers reject these new flag bits safely.
#define BROOK_DESKTOP_LAYER_HAS_OPACITY 2u
#define BROOK_DESKTOP_LAYER_OPACITY_SHIFT 8u
#define BROOK_DESKTOP_LAYER_OPACITY_MASK (255u<<BROOK_DESKTOP_LAYER_OPACITY_SHIFT)
// Output only (CAPTURE_TRACKED_KEEP): the caller's held pixels are current.
#define BROOK_DESKTOP_LAYER_KEPT (1u<<16)
struct BrookDesktopLayer {
    uint64_t pixels,capacity,incarnation,generation,bytes;
    uint32_t window,width,height,stride,clientWidth,clientHeight;
    int32_t clientX,clientY;
    uint32_t upscale,flags;
};
struct BrookDesktopLayers {
    uint32_t version,count;
    uint64_t reserved;
    struct BrookDesktopLayer layers[BROOK_DESKTOP_LAYER_LIMIT];
};
#ifdef __cplusplus
static_assert(sizeof(BrookDesktopLayer)==80 && sizeof(BrookDesktopLayers)==2576,"desktop layer ABI layout");
static_assert(sizeof(BrookDesktopFocus)==48,"desktop focus ABI layout");
#else
_Static_assert(sizeof(struct BrookDesktopLayer)==80 && sizeof(struct BrookDesktopLayers)==2576,"desktop layer ABI layout");
_Static_assert(sizeof(struct BrookDesktopFocus)==48,"desktop focus ABI layout");
#endif
