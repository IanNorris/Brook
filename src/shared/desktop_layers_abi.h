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
// Span-only chrome (stage 1.4): as TRACKED_CAPTURE_KEYED, but only the frame
// strips (outer minus client rectangle, as in BrookDesktopSurface) are
// written; the client interior of the caller's buffer is left untouched, so
// the caller must sample only the strips. A caption block is written whole.
// Older kernels return EINVAL; use TRACKED_CAPTURE_KEYED.
#define BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_SPANS 17
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
// "desktop present ..."; RAM only; boot default: app cadence on, direct
// scanout shadow). address = struct
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
// Layers ABI v2: surface records (compositor refactor stage 1.3). Every
// chrome and shell surface carries how it is composited, resolved by the
// kernel against the active backend before any backend sees it: material
// (opaque, or glass with a blur level), opacity, corner radii, shadow class
// and a content key. A backend draws what the record asks and nothing else;
// a backend that cannot do an effect is never handed it.
// Op 16: address = struct BrookDesktopSurfaces, size = its size, a = b = c =
// 0. version must be BROOK_DESKTOP_SURFACES_VERSION on input. Returns one
// FRAME or CAPTION_BLOCK record per visible window with chrome, matched to
// the layers by window and incarnation (not by order). Older kernels return
// EINVAL: the renderer then draws no shadows and treats every frame as
// translucent, exactly as before. Older renderers never call it.
#define BROOK_DESKTOP_SURFACES_QUERY 16
#define BROOK_DESKTOP_SURFACES_VERSION 2
#define BROOK_DESKTOP_SURFACE_FRAME 1          // title bar and border around a client
#define BROOK_DESKTOP_SURFACE_CAPTION_BLOCK 2  // overlaid caption buttons (client-decorated)
#define BROOK_DESKTOP_SURFACE_TASKBAR 3
#define BROOK_DESKTOP_SURFACE_LAUNCHER 4
#define BROOK_DESKTOP_SURFACE_VOLUME_POPUP 5
#define BROOK_DESKTOP_SURFACE_TOOLTIP 6
#define BROOK_DESKTOP_SURFACE_BOOT_SUMMARY 7
#define BROOK_DESKTOP_SURFACE_TRAY_FLYOUT 8
#define BROOK_DESKTOP_SURFACE_PERFORMANCE_HUD 9
#define BROOK_DESKTOP_SURFACE_DOCK 10          // side dock strip, or its edge tab
#define BROOK_DESKTOP_MATERIAL_OPAQUE 0
#define BROOK_DESKTOP_MATERIAL_GLASS 1         // needs backdrop blur; level = blur radius
// The 64 px nine-slice of brook_chrome_shadow.h, spread past each outer edge
// and drawn beneath the surface at shadowAlpha.
#define BROOK_DESKTOP_SHADOW_NONE 0
#define BROOK_DESKTOP_SHADOW_WINDOW 1          // legacy tile
#define BROOK_DESKTOP_SHADOW_WINDOW_ACTIVE 2
#define BROOK_DESKTOP_SHADOW_WINDOW_INACTIVE 3
#define BROOK_DESKTOP_SHADOW_PANEL 4
// Every pixel of the surface outside its client rectangle has alpha 255 (at
// opacity 255 it may replace what lies beneath).
#define BROOK_DESKTOP_SURFACE_OPAQUE 1u
#define BROOK_DESKTOP_SURFACE_LIGHT_SHADOW 2u
struct BrookDesktopSurface {
    uint32_t kind,window;           // window: the layer's window id (0: a shell surface)
    uint64_t incarnation;           // the layer's incarnation (0: a shell surface)
    uint64_t key;                   // content key: equal keys, identical pixels (0: unknown)
    int32_t x,y; uint32_t width,height;                          // outer rectangle, desktop pixels
    int32_t clientX,clientY; uint32_t clientWidth,clientHeight;  // transparent interior (frames)
    uint32_t material,level,opacity,flags;
    uint16_t radii[4];              // top-left, top-right, bottom-right, bottom-left
    uint32_t shadow,shadowAlpha;    // class; 0..255
    uint32_t shadowSpread,shadowCorner; // pixels past each edge; nine-slice corner, output pixels
};
struct BrookDesktopSurfaces {
    uint32_t version,count;
    uint64_t reserved;
    struct BrookDesktopSurface surfaces[BROOK_DESKTOP_LAYER_LIMIT];
};
// Overlay op 10 (BROOK_DESKTOP_OVERLAY_SURFACES, brook_debug_frame.h).
struct BrookDesktopOverlaySurfacesLegacy {
    uint32_t count,reserved;
    struct BrookDesktopSurface surfaces[5];
};
struct BrookDesktopOverlaySurfaces {
    uint32_t count,reserved;
    struct BrookDesktopSurface surfaces[14];
};
// Renderer capabilities (compositor refactor stage 1.5). Op 18: address =
// size = 0, a = BROOK_DESKTOP_CAPS_VERSION, b = the renderer's
// BROOK_DESKTOP_CAP_* bits, c = 0. Returns (>= 0) the bits this kernel knows
// and recorded; unknown bits are dropped, never an error. The record belongs
// to the calling process (its leader incarnation, the present-lease owner)
// and counts only while that owner holds the present lease. Detach and exit
// drop it, so report after every attach; a new report replaces the old one
// and b = 0 withdraws it. Older kernels return EINVAL: no capability is ever
// used for this renderer (Glass resolves to its Opaque stand-in), as before.
//
// BROOK_DESKTOP_CAP_BACKDROP_BLUR: the renderer blurs what lies beneath a
// surface record whose material is BROOK_DESKTOP_MATERIAL_GLASS (level: the
// blur radius in output pixels, 2/8/16 for Light/Moderate/Heavy, as virgl)
// and composites the surface over that blurred backdrop at its alpha. While
// the owner holds the lease and Glass is the selected theme, the kernel then
// resolves Glass instead of its Opaque stand-in: frame and shell records
// carry MATERIAL_GLASS with the level, glass radii and window shadows; chrome
// captures (ops 7/8/11/12/17) carry the glass alpha (title-bar material
// alpha, opaque text and accent stroke, rounded corners, exactly virgl's
// chrome layer); overlay captures give glass shell surfaces the material
// alpha, and their op 9 bands are not opaque. Without the bit, or with an
// older kernel, every record is opaque and nothing is translucent.
#define BROOK_DESKTOP_RENDERER_CAPS 18
#define BROOK_DESKTOP_CAPS_VERSION 1
#define BROOK_DESKTOP_CAP_BACKDROP_BLUR 1u
#define BROOK_DESKTOP_CAP_KNOWN BROOK_DESKTOP_CAP_BACKDROP_BLUR
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
static_assert(sizeof(BrookDesktopSurface)==96 && sizeof(BrookDesktopSurfaces)==16+96*BROOK_DESKTOP_LAYER_LIMIT &&
              sizeof(BrookDesktopOverlaySurfacesLegacy)==8+96*5 &&
              sizeof(BrookDesktopOverlaySurfaces)==8+96*14,"desktop surface ABI layout");
#else
_Static_assert(sizeof(struct BrookDesktopLayer)==80 && sizeof(struct BrookDesktopLayers)==2576,"desktop layer ABI layout");
_Static_assert(sizeof(struct BrookDesktopFocus)==48,"desktop focus ABI layout");
_Static_assert(sizeof(struct BrookDesktopSurface)==96 && sizeof(struct BrookDesktopSurfaces)==16+96*BROOK_DESKTOP_LAYER_LIMIT &&
               sizeof(struct BrookDesktopOverlaySurfacesLegacy)==8+96*5 &&
               sizeof(struct BrookDesktopOverlaySurfaces)==8+96*14,"desktop surface ABI layout");
#endif
