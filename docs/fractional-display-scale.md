# Fractional display scale

Settings offers 100% through 200% in 5% steps. The existing Desktop
`scale` preference stores the percentage. The window manager uses one
applied scale across enabled outputs. It rounds chrome geometry and hit
targets from the same metrics and rasterizes text and icons at the nearest
physical pixel size. Cursor geometry uses the same percentage.

Wayland surfaces that request `wp_fractional_scale_v1` receive the applied
scale in units of 1/120. At 120% and 135% these values are 144 and 162.
The provider updates the event when Settings applies a new scale. Client
buffers can use native physical pixels and give their logical extent
through `wp_viewporter`. The CPU and GPU paths map the same rounded
destination edges. Input maps physical pointer coordinates back to the
surface's logical coordinates. Relative captured motion remains raw.

Chromium and GTK builds that use the fractional protocol can render
directly at the advertised physical pixel size. Clients that use only
`wl_output.scale` retain the nearest integer buffer scale. That fallback
reports 1× at 120% and 135%, so such clients can appear smaller than the
native chrome. Xwayland continues using its existing X11 pixel and DPI
contract. The Wayland provider's display enumeration remains unchanged.

The provider tests exercise live preferred-scale updates, exact viewport
pixels at both new test scales, and the existing 2×, cursor, resize and X11
paths. The headless input fixture requests fractional scale, checks that
the physical window matches its fractional buffer dimensions and captures
the desktop during real USB pointer input.
