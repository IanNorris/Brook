#pragma once
#include "desktop_metrics.h"

namespace brook {

enum class TaskbarPopup : uint8_t { None, Volume, Network, Temperature, Performance, AppMenu, Disk, CpuMemory, Compositor, App, Calendar, Build };

// The shared popup slot keeps at most one pin. App menus use the tray
// service's stable owner ID, so a replacement item never inherits a pin.
struct TaskbarPopupPin {
    bool Any() const { return Load() != 0; }

    bool Is(TaskbarPopup popup, uint32_t owner = 0) const {
        return popup != TaskbarPopup::None && Load() == Key(popup, owner);
    }

    void Toggle(TaskbarPopup popup, uint32_t owner = 0) {
        const auto key = Key(popup, owner);
        auto previous = Load();
        if (previous && previous != key) return;
        __atomic_compare_exchange_n(&selection, &previous, previous ? 0 : key, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }

    void RetainAppMenu(uint32_t owner) {
        auto previous = Load();
        if ((previous & 0xff) == uint8_t(TaskbarPopup::AppMenu) &&
            (!owner || previous != Key(TaskbarPopup::AppMenu, owner)))
            __atomic_compare_exchange_n(&selection, &previous, uint64_t{0}, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }

private:
    static uint64_t Key(TaskbarPopup popup, uint32_t owner) {
        return (uint64_t(owner) << 8) | uint8_t(popup);
    }

    uint64_t Load() const { return __atomic_load_n(&selection, __ATOMIC_RELAXED); }

    uint64_t selection = 0;
};

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
