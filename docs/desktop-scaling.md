# Desktop scaling work in progress

The remote checkpoint now wires 100%, 150% and 200% native desktop scaling
through Settings, boot configuration and frame-boundary application. Native
chrome, launcher and taskbar select matching immutable metrics and final-size
16/24/32px font atlases. Terminal text and application content retain their
existing scale. **This integration is build-tested, but has not yet passed live
VM acceptance.** The resize-ownership evidence below predates this integration.

Settings saves `[Desktop] scale=100`, `150` or `200` beside the theme in the
existing user settings file. A save failure restores the previous requested
appearance. Brook syscall `0xB04` takes operation 0 to query the requested scale,
1 to request a supported percent, and 2 to query the applied scale. Requests
apply after input processing and before rendering, deferred during drag/resize.
Normal client buffers retain their physical dimensions; maximized windows use
the new work area and existing terminal/Wayland resize paths. Restore geometry
is clamped when restored, and parent-relative CSD popups are not independently
repositioned. Toolkit output scale remains one.

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
failure status before this resize function commits dimensions. The maximize caller now rolls back on resize failure. Existing live
resize-drag preview still pre-stages geometry; its allocation-failure rollback
remains open.

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

## Remaining acceptance and integration

1. Exercise 100%/150%/200% transitions with normal, CSD, minimized and maximized
   windows, terminal grids, taskbar overflow, popup positioning, narrow screens,
   and saved settings through reboot on CPU and GPU compositors. Check caption
   hit testing and failure handling against actual frames.
2. Complete allocation-failure rollback for interactive resize previews.
3. Implement Wayland buffer/surface/screen conversions before changing advertised
   output scale. Currently `wl_surface.set_buffer_scale` is ignored, buffer pixels
   become window dimensions, and output scale is fixed at one. SHM and GPU
   buffers, pointer input, configure sizes, popups, subsurfaces and cursor hotspots
   need consistent handling. Start with 100%/200% app scaling; fractional app
   scaling requires additional protocol/resampling work.
4. Update user-facing screenshots after live acceptance. The Settings preview
   currently demonstrates the theme, not the selected desktop scale.
