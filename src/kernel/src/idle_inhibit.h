#pragma once
#include <stdint.h>

// Idle inhibitors (Wayland zwp_idle_inhibit_manager_v1, relayed by waylandd
// through syscall 0xB27 as a per-window count). The kernel decides
// visibility: per the protocol an inhibitor only applies while its surface is
// visible, so a window counts only while it is live, shown and not minimised
// (an unselected side-dock page is also hidden through `minimized`).
// Occlusion by other windows is not considered. Inhibitors hold the display
// idle (backlight blanking) only; thermal and CPU idle policy ignore them.
namespace brook {
inline constexpr uint32_t IdleInhibitMaxPerWindow = 1024;
inline constexpr uint32_t IdleInhibitReportLimit = 8;
inline constexpr uint32_t IdleInhibitTextSize = 48;

struct IdleInhibitClient {
    uint16_t wmId = 0, pid = 0, count = 0;
    bool visible = false;
    char appId[IdleInhibitTextSize]{};
    char title[IdleInhibitTextSize]{};
};
struct IdleInhibitSnapshot {
    // Windows/inhibitors that currently hold the display on, and those
    // ignored because their window is hidden or minimised.
    uint32_t activeWindows = 0, activeInhibitors = 0;
    uint32_t hiddenWindows = 0, hiddenInhibitors = 0;
    uint32_t reported = 0; // entries in clients[]; active windows first
    IdleInhibitClient clients[IdleInhibitReportLimit]{};
};

// window.cpp, under the WM lock.
void WmIdleInhibitRead(IdleInhibitSnapshot* out);

template<typename W> inline bool IdleInhibitApplies(const W& w) {
    return w.proc && w.idleInhibitors && w.visible && !w.minimized;
}
template<typename W> inline bool IdleInhibitAny(const W* windows, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (IdleInhibitApplies(windows[i])) return true;
    return false;
}
inline void IdleInhibitCopyText(char* out, const char* in) {
    uint32_t i = 0;
    for (; in && in[i] && i + 1 < IdleInhibitTextSize; ++i) out[i] = in[i];
    out[i] = 0;
}
template<typename W> inline void IdleInhibitCollect(const W* windows, uint32_t n, IdleInhibitSnapshot& out) {
    out = {};
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < n; ++i) {
            const W& w = windows[i];
            if (!w.proc || !w.idleInhibitors) continue;
            const bool visible = IdleInhibitApplies(w);
            if (visible != (pass == 0)) continue;
            if (visible) { ++out.activeWindows; out.activeInhibitors += w.idleInhibitors; }
            else { ++out.hiddenWindows; out.hiddenInhibitors += w.idleInhibitors; }
            if (out.reported >= IdleInhibitReportLimit) continue;
            auto& c = out.clients[out.reported++];
            c.wmId = w.wmId; c.pid = w.clientPid; c.count = w.idleInhibitors; c.visible = visible;
            IdleInhibitCopyText(c.appId, w.appId); IdleInhibitCopyText(c.title, w.title);
        }
    }
}
}
