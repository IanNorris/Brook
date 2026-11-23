#pragma once

// shell_output_layout.h — the desktop shell on several outputs (pure).
//
// Every output has its own taskbar along its bottom edge, with its own
// launcher button, tray and clock, and task buttons for all windows. Each
// output's work area excludes its own taskbar. The shell's code lays out
// one output at a time in that output's local coordinates (origin 0,0 and
// the output's size), exactly as it laid out the single screen before; this
// header maps desktop points to an output and back.
//
// Transient shell state (the launcher, taskbar popups and tray flyouts, the
// app menu) is shown on one output at a time: the shell output, which a
// press on an output's taskbar selects.

#include <stdint.h>
#include "display_output.h"

namespace brook {

// WmDesktopHeight for one output: the rows above its taskbar.
inline constexpr uint32_t ShellDesktopHeight(uint32_t outputH, uint32_t taskbarH)
{
    return outputH > taskbarH ? outputH - taskbarH : 0;
}

// WmTaskbarTop for one output: a fullscreen window on it hides its taskbar.
inline constexpr uint32_t ShellTaskbarTop(uint32_t outputH, uint32_t taskbarH, bool hidden)
{
    return hidden ? outputH : ShellDesktopHeight(outputH, taskbarH);
}

// The output's taskbar in desktop coordinates (the Taskbar shell surface).
inline OutputRect ShellTaskbarRect(const OutputRect& out, uint32_t taskbarH)
{
    if (out.empty()) return {};
    const uint32_t top = ShellDesktopHeight(out.h, taskbarH);
    return {out.x, int32_t(int64_t(out.y) + top), out.w, out.h - top};
}

// The output minus its own taskbar; the side dock (primary only) is taken
// out by the WM.
inline OutputRect ShellWorkAreaBase(const OutputRect& out, uint32_t taskbarH)
{
    if (out.empty()) return {};
    return {out.x, out.y, out.w, ShellDesktopHeight(out.h, taskbarH)};
}

// The output under a desktop point, or `fallback`.
inline uint32_t ShellOutputAt(const OutputLayout& layout, int64_t px, int64_t py, uint32_t fallback)
{
    const int found = OutputIndexForPoint(layout, px, py);
    return found >= 0 ? uint32_t(found) : fallback;
}

struct ShellOutputPoint {
    int output;        // -1: not on any taskbar
    int32_t x, y;      // output-local
};

// Which output's taskbar band holds the point. hiddenMask bit i: output i's
// taskbar is hidden by a fullscreen window. With a single output the band is
// every row at or below its taskbar top, as the single-screen test was
// (`my >= WmTaskbarTop(h)`), with no column check.
inline ShellOutputPoint ShellTaskbarAt(const OutputLayout& layout, uint32_t taskbarH, uint32_t hiddenMask,
                                       int32_t px, int32_t py)
{
    if (layout.count <= 1) {
        const OutputRect out = layout.rect(0);
        const int64_t top = int64_t(out.y) + ShellTaskbarTop(out.h, taskbarH, hiddenMask & 1u);
        if (py >= top) return {0, int32_t(int64_t(px) - out.x), int32_t(int64_t(py) - out.y)};
        return {-1, px, py};
    }
    for (uint32_t i = 0; i < layout.count; ++i) {
        if (!layout.enabled(i)) continue;
        const OutputRect out = layout.rect(i);
        if (!out.contains(px, py)) continue;
        const int64_t top = int64_t(out.y) + ShellTaskbarTop(out.h, taskbarH, hiddenMask >> i & 1u);
        if (py >= top) return {int(i), int32_t(int64_t(px) - out.x), int32_t(int64_t(py) - out.y)};
        return {-1, px, py};
    }
    return {-1, px, py};
}

} // namespace brook
