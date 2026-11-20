#pragma once
#include "chrome_style.h"

namespace brook {

// Session service publishes only the visible subset. IDs belong to that
// service instance and survive reordering of the displayed items.
inline constexpr uint32_t TrayItemLimit = 8;
inline constexpr uint32_t TrayMenuLimit = 12;
inline constexpr uint32_t NoticeActionLimit = 3;
inline constexpr uint32_t TrayIconWords = 24 * 24;

struct ChromeTrayItem {
    uint32_t id;
    uint32_t icon[TrayIconWords];
    char title[64], tooltip[128];
    uint8_t hasIcon, attention;
};

struct ChromeTrayMenuRow {
    int32_t id;
    char label[96];
    uint8_t enabled, separator, submenu;
};

struct ChromeNotice {
    uint32_t id, actionCount;
    char app[64], title[128], body[224];
    char actions[NoticeActionLimit][48];
};

struct ChromeTrayState {
    uint32_t itemCount, menuOwner, menuCount;
    ChromeTrayItem items[TrayItemLimit];
    ChromeTrayMenuRow menu[TrayMenuLimit];
    ChromeNotice notice;
};

enum class TrayInputKind : uint32_t { Activate, Context, Scroll, MenuSelect, DismissMenu, NoticeAction, CloseNotice };
struct TrayInput {
    TrayInputKind kind;
    uint32_t owner;
    int32_t argument, x, y;
};

inline DesktopRect ChromeTrayMenuRect(const ChromeStyle& s, DesktopRect anchor, uint32_t count, int sw)
{
    if (!anchor.w || !count || sw <= 0) return {};
    const int margin = s.M(8), width = s.M(228), height = s.M(12) + int(count) * s.M(36);
    const int w = width < sw - 2 * margin ? width : sw - 2 * margin;
    if (w <= 0 || anchor.y < height + margin) return {};
    const int right = anchor.x + anchor.w < sw - margin ? anchor.x + anchor.w : sw - margin;
    const int left = right - w < margin ? margin : right - w;
    return {left, anchor.y - height - margin, w, height};
}

// The window menu: below and right of the press, flipped to stay on the
// desktop (above the taskbar), the tray menu's width and rows.
inline DesktopRect ChromeWindowMenuRect(const ChromeStyle& s, int x, int y, uint32_t count, int sw, int desktopH)
{
    if (!count || sw <= 0 || desktopH <= 0) return {};
    const int margin = s.M(8), width = s.M(228), height = s.M(12) + int(count) * s.M(36);
    const int w = width < sw - 2 * margin ? width : sw - 2 * margin;
    if (w <= 0 || height + 2 * margin > desktopH) return {};
    int left = x + w > sw - margin ? x - w : x;
    if (left + w > sw - margin) left = sw - margin - w;
    if (left < margin) left = margin;
    int top = y + height > desktopH - margin ? y - height : y;
    if (top + height > desktopH - margin) top = desktopH - margin - height;
    if (top < margin) top = margin;
    return {left, top, w, height};
}

inline DesktopRect ChromeNoticeRect(const ChromeStyle& s, int sw, int desktopH, uint32_t actions)
{
    const int margin = s.M(16), width = s.M(360);
    const int h = s.M(actions ? 128 : 88);
    const int w = width < sw - 2 * margin ? width : sw - 2 * margin;
    return w > 0 && desktopH >= h + 2 * margin ? DesktopRect{sw - margin - w, margin, w, h} : DesktopRect{};
}

inline DesktopRect ChromeTrayMenuRowRect(const ChromeStyle& s, const DesktopRect& panel, uint32_t row)
{
    const int pad = s.M(6), h = s.M(36);
    return {panel.x + pad, panel.y + pad + int(row) * h, panel.w - 2 * pad, h};
}

inline DesktopRect ChromeNoticeCloseRect(const ChromeStyle& s, const DesktopRect& rect)
{
    const int pad = s.M(8), button = s.M(28);
    return {rect.x + rect.w - pad - button, rect.y + pad, button, button};
}

inline DesktopRect ChromeNoticeActionRect(const ChromeStyle& s, const DesktopRect& rect, uint32_t count, uint32_t index)
{
    if (!count) return {};
    const int pad = s.M(12), gap = s.M(6), h = s.M(32);
    const int width = (rect.w - 2 * pad - int(count - 1) * gap) / int(count);
    return {rect.x + pad + int(index) * (width + gap), rect.y + rect.h - pad - h, width, h};
}

void ChromeBuildAppTrayMenu(const ChromeTrayState&, DesktopRect, int32_t mx, int32_t my,
                            const ChromeStyle&, ChromeDisplayList&);
void ChromeBuildNotice(const ChromeNotice&, DesktopRect, int32_t mx, int32_t my,
                       const ChromeStyle&, ChromeDisplayList&);

} // namespace brook
