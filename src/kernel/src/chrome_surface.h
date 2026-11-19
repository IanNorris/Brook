#pragma once
// Surface records (compositor refactor stage 1.3): how each chrome and
// shell surface is composited, resolved once against the active backend,
// in the layers ABI v2 form (BrookDesktopSurface). The CPU executor needs
// none of it (it draws everything opaque); virgl and VKRENDER read the
// material, radii and shadow class from here instead of deriving their own
// geometry. The rules reproduce today's pixels: a window shadow and rounded
// glass corners exist only while glass is active, and an opaque frame or
// shell surface is opaque in every pixel. Freestanding.
#include <stdint.h>
#include "desktop_metrics.h"
#include "../../shared/desktop_layers_abi.h"

namespace brook {

// What the backend resolved (CompositorGlassActive and friends).
struct ChromeSurfaceEffects {
    bool glass;                 // translucent material with backdrop blur
    uint32_t blur;              // material blur radius under glass
    unsigned topRadius, bottomRadius; // glass frame corner radii (chrome material masks)
    uint32_t shadowSpread, shadowCorner; // WmMetric(16), WmMetric(24)
    uint32_t shellRadius;       // glass shell panel corners (ChromeMaterialRect: M(4))
};

struct ChromeFrameSurfaceModel {
    uint32_t window;
    uint64_t incarnation, key;
    bool clientDecorated;       // a caption block over the client's own title area
    bool maximised, focused;
    DesktopRect outer, client;  // desktop pixels; outer is the caption block when clientDecorated
    unsigned captionRadius;     // client-matched caption block corner (0: square)
    uint32_t opacity;
};

inline BrookDesktopSurface ChromeFrameSurface(const ChromeFrameSurfaceModel& m, const ChromeSurfaceEffects& e) {
    BrookDesktopSurface s{};
    s.window = m.window; s.incarnation = m.incarnation; s.key = m.key;
    s.x = m.outer.x; s.y = m.outer.y;
    s.width = m.outer.w > 0 ? uint32_t(m.outer.w) : 0; s.height = m.outer.h > 0 ? uint32_t(m.outer.h) : 0;
    s.opacity = m.opacity > 255 ? 255 : m.opacity;
    if (m.clientDecorated) {
        s.kind = BROOK_DESKTOP_SURFACE_CAPTION_BLOCK;
        const unsigned radius = m.maximised ? 0 : m.captionRadius;
        s.radii[1] = uint16_t(radius);
        // The client keeps its own shadow; the block is opaque except a
        // rounded top-right corner (chrome_alpha.h).
        s.material = e.glass ? BROOK_DESKTOP_MATERIAL_GLASS : BROOK_DESKTOP_MATERIAL_OPAQUE;
        s.level = e.glass ? e.blur : 0;
        if (!e.glass && !radius) s.flags |= BROOK_DESKTOP_SURFACE_OPAQUE;
        return s;
    }
    s.kind = BROOK_DESKTOP_SURFACE_FRAME;
    s.clientX = m.client.x; s.clientY = m.client.y;
    s.clientWidth = m.client.w > 0 ? uint32_t(m.client.w) : 0; s.clientHeight = m.client.h > 0 ? uint32_t(m.client.h) : 0;
    if (e.glass) {
        s.material = BROOK_DESKTOP_MATERIAL_GLASS; s.level = e.blur;
        if (!m.maximised) {
            s.radii[0] = s.radii[1] = uint16_t(e.topRadius);
            s.radii[2] = s.radii[3] = uint16_t(e.bottomRadius);
            s.shadow = BROOK_DESKTOP_SHADOW_WINDOW; s.shadowAlpha = m.focused ? 255 : 180;
            s.shadowSpread = e.shadowSpread; s.shadowCorner = e.shadowCorner;
        }
    } else {
        // Opaque material: frame = outer minus client, alpha 255 throughout.
        s.material = BROOK_DESKTOP_MATERIAL_OPAQUE;
        s.flags |= BROOK_DESKTOP_SURFACE_OPAQUE;
    }
    return s;
}

// kind: BROOK_DESKTOP_SURFACE_TASKBAR..BOOT_SUMMARY. material: the surface
// takes the translucent shell material when glass is active.
inline BrookDesktopSurface ChromeShellSurfaceRecord(uint32_t kind, const DesktopRect& r, bool material, uint64_t key,
                                                    uint32_t materialOpacity, const ChromeSurfaceEffects& e) {
    BrookDesktopSurface s{};
    s.kind = kind; s.key = key;
    s.x = r.x; s.y = r.y; s.width = r.w > 0 ? uint32_t(r.w) : 0; s.height = r.h > 0 ? uint32_t(r.h) : 0;
    const bool glass = material && e.glass;
    s.material = glass ? BROOK_DESKTOP_MATERIAL_GLASS : BROOK_DESKTOP_MATERIAL_OPAQUE;
    s.level = glass ? e.blur : 0;
    s.opacity = glass ? (materialOpacity > 255 ? 255 : materialOpacity) : 255;
    if (glass && kind != BROOK_DESKTOP_SURFACE_TASKBAR)
        for (auto& radius : s.radii) radius = uint16_t(e.shellRadius);
    if (!glass) s.flags |= BROOK_DESKTOP_SURFACE_OPAQUE;
    // DESIGN.md: the taskbar has no drop shadow; panel shadows are stage 2.
    s.shadow = BROOK_DESKTOP_SHADOW_NONE;
    return s;
}

} // namespace brook
