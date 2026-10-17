# Desktop scaling work in progress

Native scale controls and live metric application remain under construction.
The current checkpoint prepares immutable 100%, 150% and 200% metrics and
final-size 16/24/32px font atlases. The larger atlases are not selected by the
running desktop yet. The terminal keeps its original font atlas.

`desktop_metrics.h` provides shared physical-pixel caption rectangles and
geometry conversion. Tests cover every pair of scale transitions, client-origin
and client-size preservation, client-side decorations, exact caption hit-test
boundaries, restore conversions, offscreen titles and oversized windows on
narrow displays. Invalid scale selections have no metrics instance.

`bake_font.py` accepts an optional C++ symbol name, allowing separate immutable
atlases without stretching a smaller bitmap. CMake generates the 150% and 200%
font sources as normal dependencies. This checkpoint uses the existing font
family; it does not change the desktop to Noto Sans.

## Resize ownership prerequisite

The previous `WmResizeWindow` used `TerminalFindByProcess`, whose deliberate
controlling-terminal semantics include descendants. Maximizing a GUI created by
a program launched from bash could therefore resize its parent terminal's
backing and character grid instead of notifying the GUI.

Resize now resolves an exact active terminal child by PID and incarnation under
the terminal-state guard. No terminal pointer escapes that guard. The window
mutation guard keeps the owner and window slot stable through resize. These
sleeping locks follow window → terminal order; terminal create/close/resize paths
do not acquire the window guard. Replacement allocation failure returns a
failure status before this resize function commits dimensions. Existing live
resize-drag preview and maximize callers still pre-stage some geometry; failure
rollback for those callers remains part of the live-scale integration.

This synchronous first version holds the window guard through terminal resize
allocation. Preparing resources outside that guard requires a separate owned
preparation and a birth/revision-validated commit; dropping the guard around a
raw Terminal or Process pointer would introduce stale-job and slot-reuse races.

## Acceptance evidence

All 26 hosted suites and the two-pass release kernel build pass. The new metrics
suite runs the actual shared geometry functions.

An isolated 8-core, 4 GiB CPU-compositor candidate used kernel SHA-256
`64c07027668d3a7af06e3aa532037134a232a21fc19f11433b075ce18ed4ced8`.
Session `2dbdc4c9f75f479ca698685877c77b81` verified native Terminal maximize to
1916×996 with a 52×191 character grid, then restore to 800×600 and 31×80.
Checked shutdown and three offline filesystem checks passed.

The Brook-only `wmterminalresizecheck.c` probe must run from a real Terminal,
so its GUI window owner is a descendant of the terminal's bash child. It
maximizes and restores its own window twelve times, checking that the parent
terminal character grid stays unchanged throughout.

- Candidate session `e06a606f962840dbbd9ed2cfc88e8e2f` displayed
  `WMTERMINALRESIZECHECK PASS rounds=12 parent=31x80`.
- Baseline session `b41d2009ef5a40998c52f24720a654d7` used the previous kernel
  `855b0d0d22af04b3658cdf04febd571e37eb1c0f1e495a1349fa44800ac73c65`
  and the same probe. It immediately displayed
  `WMTERMINALRESIZECHECK FAIL parent terminal changed 31x80 -> 52x191`, with the
  oversized terminal backing visible outside the unchanged window chrome.

Both comparison runs completed checked shutdown and all three offline filesystem
checks passed. Local captures, frozen artifacts, metadata and disks live under
`build/workloads/desktop-scale-20261017/` and the corresponding session registries.
The test is a bounded resize-ownership regression, not acceptance of live scaling.

## Remaining integration

1. Select one immutable metrics/font snapshot for native chrome, taskbar,
   launcher, spacing, icons and hit testing. Window geometry must retain its
   decoration metrics so client coordinates never silently use another scale.
2. Process input against the preceding frame before applying a requested scale,
   then update window and restore geometry under the window guard. Defer changes
   during drag/resize. Preserve normal client origins where possible; keep
   captions reachable when clamping is necessary. Recompute maximized work areas
   with allocation failure handling and matching terminal/Wayland resize events.
3. Add Settings selection, independent boot parsing and durable persistence beside
   the existing theme key. Define requested versus applied query semantics and
   rollback on save failure.
4. Implement Wayland buffer/surface/screen conversions before changing advertised
   output scale. Currently `wl_surface.set_buffer_scale` is ignored, buffer pixels
   become window dimensions, and output scale is fixed at one. SHM and GPU
   buffers, pointer input, configure sizes, popups, subsurfaces and cursor hotspots
   need consistent handling. Start with 100%/200% app scaling; fractional app
   scaling requires additional protocol/resampling work.
5. Exercise normal/CSD/minimized/maximized windows, terminal grids, taskbar
   overflow, narrow screens and persistence through reboot on CPU and GPU paths.
