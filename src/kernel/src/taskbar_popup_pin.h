#pragma once
#include "desktop_metrics.h"

namespace brook {

enum class TaskbarPopup : uint8_t { None, Volume, Network, Temperature, Performance, AppMenu, Disk, CpuMemory, Compositor, App, Calendar, Build };

// Built-in panels have independent pins. The tray service publishes only
// one app menu at a time, bound to the stable owner that requested its pin.
struct TaskbarPopupPin {
    bool Any() const { return __atomic_load_n(&mask, __ATOMIC_RELAXED) || AppOwner(); }

    bool Is(TaskbarPopup popup, uint32_t owner = 0) const {
        if (popup == TaskbarPopup::AppMenu) return owner && AppOwner() == owner;
        return (__atomic_load_n(&mask, __ATOMIC_RELAXED) & Bit(popup)) != 0;
    }

    void Toggle(TaskbarPopup popup, uint32_t owner = 0) {
        if (popup == TaskbarPopup::AppMenu) {
            if (owner) __atomic_store_n(&appOwner, AppOwner() == owner ? 0u : owner, __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_xor(&mask, Bit(popup), __ATOMIC_RELAXED);
        }
    }

    void RetainAppMenu(uint32_t owner) {
        auto previous = AppOwner();
        if (previous && previous != owner)
            __atomic_compare_exchange_n(&appOwner, &previous, 0u, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }

private:
    static uint32_t Bit(TaskbarPopup popup) {
        return popup != TaskbarPopup::None && popup <= TaskbarPopup::Build ? 1u << unsigned(popup) : 0;
    }

    uint32_t AppOwner() const { return __atomic_load_n(&appOwner, __ATOMIC_RELAXED); }

    uint32_t mask = 0, appOwner = 0;
};

// Place the requested panels right to left. The caller supplies the
// transient and most recently selected pin first. Overflow stays pinned;
// its taskbar chip can bring it forward or a wider output can reveal it.
inline void TaskbarPlacePopups(DesktopRect* panels, unsigned count, int left, int right, int gap) {
    for (unsigned i = 0; i < count; ++i) {
        auto& p = panels[i];
        if (p.w <= 0 || p.h <= 0 || p.w > right - left) { p = {}; continue; }
        // Keep a single panel adjacent to its chip whenever it fits there.
        if (count > 1 || p.x > right - p.w) p.x = right - p.w;
        if (p.x < left) p.x = left;
        right = p.x - gap;
    }
}

// Transient panels dismiss on an outside press. A pinned panel receives
// events only within its bounds, so other windows keep their normal input.
inline bool TaskbarPopupOwnsPointer(DesktopRect panel, bool pinned, int x, int y) {
    return panel.w > 0 && panel.h > 0 && (!pinned || panel.contains(x, y));
}

inline DesktopRect TaskbarPopupPinRect(const DesktopMetrics& metrics, DesktopRect panel, int inset, int header) {
    const int h = metrics.pixels(24), w = metrics.pixels(68);
    if (panel.w < w + 2 * inset || panel.h < inset + h) return {};
    return {panel.x + panel.w - inset - w, panel.y + inset + (header - h) / 2, w, h};
}

} // namespace brook
