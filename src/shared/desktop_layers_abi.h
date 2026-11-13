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
#define BROOK_DESKTOP_LAYER_LEGACY_OPAQUE 1u
// Omitted means 255; older renderers reject these new flag bits safely.
#define BROOK_DESKTOP_LAYER_HAS_OPACITY 2u
#define BROOK_DESKTOP_LAYER_OPACITY_SHIFT 8u
#define BROOK_DESKTOP_LAYER_OPACITY_MASK (255u<<BROOK_DESKTOP_LAYER_OPACITY_SHIFT)
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
#else
_Static_assert(sizeof(struct BrookDesktopLayer)==80 && sizeof(struct BrookDesktopLayers)==2576,"desktop layer ABI layout");
#endif
