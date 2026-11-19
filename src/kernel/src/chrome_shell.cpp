// Shell-surface layouts and display list builders (compositor refactor stage
// 1.2). The primitives are the ones the taskbar, launcher, popup, tooltip and
// boot summary painters used to draw directly, in the same order, so the CPU
// executor reproduces their pixels. Freestanding.
#include "chrome_shell.h"

namespace brook {

using SM = ChromeShellMetrics;

static void CopyText(char* dst, const char* src, uint32_t capacity)
{
    uint32_t i = 0;
    while (src && src[i] && i + 1 < capacity) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

// ---------------------------------------------------------------------------
// Taskbar
// ---------------------------------------------------------------------------

ChromeTaskbarLayout ChromeTaskbarLayoutFor(const ChromeStyle& s, uint32_t screenW, uint32_t screenH,
                                           const char* clockText, bool agent, uint32_t enabledMetrics,
                                           const int* ids, int total, int focused)
{
    ChromeTaskbarLayout l = {};
    l.nextIndex = -1;
    l.screenW = static_cast<int>(screenW); l.screenH = static_cast<int>(screenH);
    const int sw = static_cast<int>(screenW), sh = static_cast<int>(screenH);
    l.y = sh > static_cast<int>(s.M(DesktopMetrics::TaskbarBase)) ? sh - s.M(DesktopMetrics::TaskbarBase) : 0;
    const int height = sh - l.y;
    // Targets span the whole taskbar height (Fitts: the screen's bottom edge
    // and the bottom-left corner hit them); pills are drawn inset.
    const int bh = height < static_cast<int>(s.M(SM::TaskbarButtonHeight)) ? height : s.M(SM::TaskbarButtonHeight);
    const int by = l.y + (height - bh) / 2;
    l.textY = by + (bh - s.Font().lineHeight) / 2;
    int x = s.M(SM::TaskbarPadding);
    const int appsW = s.M(SM::TaskbarLauncherWidth);
    l.apps = {x, by, appsW, bh}; x += appsW + s.M(SM::TaskbarPadding);
    l.terminal = {x, by, static_cast<int>(s.M(SM::TaskbarButtonWidth)), bh};
    x += s.M(SM::TaskbarButtonWidth) + s.M(SM::TaskbarPadding);
    CopyText(l.clockText, clockText, sizeof(l.clockText));
    int right = sw - s.M(8);
    // Reserve measured widgets from right to left. Keep one useful window
    // button on small displays; diagnostics yield before the clock does.
    auto reserve = [&](DesktopRect& r, int width) {
        if (right - width - static_cast<int>(s.M(12)) < x + static_cast<int>(s.M(80))) return;
        right -= width;
        r = {right, by, width, bh};
        right -= s.M(12);
    };
    reserve(l.clock, s.TextWidth(l.clockText));
    reserve(l.network, s.M(16));
    reserve(l.volume, s.M(16));
    // Leftmost fixed icon, so its appearance never moves the volume target.
    if (agent) reserve(l.agent, s.M(16));
    const int widths[7] = {64, 64, 112, 112, 240, 240, 150};
    for (int i = 6; i >= 0; --i) if (enabledMetrics & (1u << i)) {
        reserve(l.metrics[i], s.M(widths[i]));
        if (l.metrics[i].w) {
            // Two 19-pixel text lines do not fit the 32-pixel task buttons.
            // Use the full tray height, retaining a scaled top/bottom inset.
            l.metrics[i].y = l.y + s.M(2);
            l.metrics[i].h = height - 2 * s.M(2);
        }
    }
    if (total > static_cast<int>(ChromeTaskbarButtonLimit)) total = ChromeTaskbarButtonLimit;
    int available = right - x;
    if (!total || available <= 0) return l;
    int capacity = (available + s.M(SM::TaskbarPadding)) / (s.M(SM::TaskbarButtonWidth) + s.M(SM::TaskbarPadding));
    bool overflow = capacity < total;
    if (overflow)
    {
        // The arrow focuses the first window on the next page. The focused
        // window selects the page, so every hidden/minimized window remains
        // reachable through the existing focus/restore input path.
        int arrowW = available < static_cast<int>(s.M(32)) ? available : static_cast<int>(s.M(32));
        l.next = {right - arrowW, by, arrowW, bh};
        available -= arrowW + s.M(SM::TaskbarPadding);
        capacity = (available + s.M(SM::TaskbarPadding)) / (s.M(SM::TaskbarButtonWidth) + s.M(SM::TaskbarPadding));
        if (capacity < 1 && available > 0) capacity = 1;
        if (capacity < 1) { l.nextIndex = ids[(focused + 1) % total]; return l; }
    }
    if (capacity > total) capacity = total;
    const int first = (focused / capacity) * capacity;
    l.count = total - first < capacity ? total - first : capacity;
    int width = (available - (capacity - 1) * s.M(SM::TaskbarPadding)) / capacity;
    if (width > static_cast<int>(s.M(SM::TaskbarButtonWidth))) width = s.M(SM::TaskbarButtonWidth);
    for (int i = 0; i < l.count; ++i)
    {
        l.indices[i] = ids[first + i];
        l.buttons[i] = {x, by, width, bh};
        x += width + s.M(SM::TaskbarPadding);
    }
    if (overflow) l.nextIndex = ids[(first + capacity) % total];
    return l;
}

// Four ascending bars in the clock colour when connected; dimmed bars with a
// cross over the short (left) end when disconnected.
static void NetworkIndicator(ChromeDisplayList& dl, const ChromeStyle& s, const DesktopRect& r, bool connected)
{
    if (r.w <= 0) return;
    const uint32_t fg = s.P().secondaryText;
    const uint32_t bar = connected ? fg : ChromeBlendColor(fg, s.P().taskbar, 90);
    const int size = static_cast<int>(s.M(16));
    const int barW = static_cast<int>(s.M(3)), gap = static_cast<int>(s.M(1));
    const int x0 = r.x, top = r.y + (r.h - size) / 2, bottom = top + size;
    for (int i = 0; i < 4; ++i) {
        const int h = size * (i + 1) / 4;
        dl.Fill(x0 + i * (barW + gap), bottom - h, barW, h, bar);
    }
    if (connected) return;
    const int span = size * 5 / 8, t = static_cast<int>(s.M(2));
    dl.Squares(x0, top, t, t, 1, 1, span - t + 1, fg);
    dl.Squares(x0 + span - t, top, t, t, -1, 1, span - t + 1, fg);
}

// A small robot head: antenna, outlined face, two eyes. Connected draws it in
// the clock colour with green eyes; stale dims it, closes the eyes and adds
// an amber "!" badge at the lower right.
static void AgentIndicator(ChromeDisplayList& dl, const ChromeStyle& s, const DesktopRect& r, bool shown, bool stale)
{
    if (r.w <= 0 || !shown) return;
    const uint32_t fg = s.P().secondaryText;
    const uint32_t ink = stale ? ChromeBlendColor(fg, s.P().taskbar, 120) : fg;
    // Drawn on a 16-unit grid scaled to the 16-pixel metric (any UI scale).
    const int size = static_cast<int>(s.M(16));
    const int x0 = r.x + (r.w - size) / 2, y0 = r.y + (r.h - size) / 2;
    auto px = [&](int v) { return v * size / 16; };
    auto box = [&](int x, int y, int w, int h, uint32_t c) {
        dl.Fill(x0 + px(x), y0 + px(y), px(x + w) - px(x), px(y + h) - px(y), c);
    };
    box(6, 0, 4, 2, ink);           // antenna knob
    box(7, 2, 2, 2, ink);           // antenna stem
    box(1, 4, 14, 2, ink);          // face outline
    box(1, 14, 14, 2, ink);
    box(1, 6, 2, 8, ink);
    box(13, 6, 2, 8, ink);
    if (!stale) {
        const uint32_t eye = BROOK_AGENT_ICON_EYE;
        box(4, 8, 3, 3, eye);
        box(9, 8, 3, 3, eye);
        return;
    }
    box(4, 9, 3, 1, ink);           // closed eyes
    box(9, 9, 3, 1, ink);
    const uint32_t badge = BROOK_AGENT_ICON_BADGE, mark = BROOK_AGENT_ICON_MARK;
    box(10, 9, 6, 7, badge);
    box(12, 10, 2, 3, mark);
    box(12, 14, 2, 1, mark);
}

// Speaker body and cone, then one to three ")" waves for the level, on a
// 16x16 design grid scaled like the network bars. Muted draws a small cross
// where the waves go; no device dims the speaker and crosses it like the
// disconnected network bars.
void ChromeBuildVolumeGlyph(ChromeDisplayList& dl, const ChromeStyle& s, int x0, int y0, int size,
                            uint32_t control, bool available, uint32_t bg)
{
    const uint32_t fg = s.P().secondaryText;
    const uint32_t ink = available ? fg : ChromeBlendColor(fg, bg, 90);
    auto u = [&](int v) { return v * size / 16; };
    auto cell = [&](int x, int y, int w, int h) {
        dl.Fill(x0 + u(x), y0 + u(y), u(x + w) - u(x), u(y + h) - u(y), ink);
    };
    cell(0, 5, 3, 6);
    for (int c = 0; c < 3; ++c) cell(3 + c, 4 - c, 1, 8 + 2 * c);
    const TaskbarVolumeGlyph glyph = TaskbarVolumeGlyphFor(control);
    if (glyph == TaskbarVolumeGlyph::Muted) {
        const int span = u(6), t = u(2) > 0 ? u(2) : 1, left = x0 + u(9), top = y0 + u(5);
        dl.Squares(left, top, t, t, 1, 1, span - t + 1, ink);
        dl.Squares(left + span - t, top, t, t, -1, 1, span - t + 1, ink);
    } else {
        const int waves = glyph == TaskbarVolumeGlyph::Low ? 1 : glyph == TaskbarVolumeGlyph::Medium ? 2 : 3;
        for (int i = 0; i < waves; ++i) {
            const int x = 8 + 3 * i, half = 2 + 2 * i, cap = i ? 2 : 1;
            cell(x, 8 - half, 2, 2 * half);
            cell(x - 1, 8 - half - cap, 2, cap);
            cell(x - 1, 8 + half, 2, cap);
        }
    }
    if (available) return;
    const int span = size * 5 / 8, ct = static_cast<int>(s.M(2));
    dl.Squares(x0, y0, ct, ct, 1, 1, span - ct + 1, fg);
    dl.Squares(x0 + span - ct, y0, ct, ct, -1, 1, span - ct + 1, fg);
}

static void MetricGraph(ChromeDisplayList& dl, const ChromeStyle& s, int x, int y, int width, int height,
                        const DesktopIntervals<>& history, uint64_t now)
{
    if (width <= 0 || height <= 0) return;
    const unsigned n = history.count, shown = n < static_cast<unsigned>(width) ? n : width;
    for (unsigned i = 0; i < shown; ++i) {
        const auto& v = history.samples[(history.next + 128 - shown + i) % 128];
        if (now < v.tick || now - v.tick > 10000) continue;
        unsigned value = v.ms > 100 ? 100 : v.ms;
        int bar = static_cast<uint64_t>(value) * height / 100; if (!bar) bar = 1;
        dl.Fill(x + width - shown + i, y + height - bar, 1, bar, 0xff000000u | s.P().accent);
    }
}

static void DesktopMetric(ChromeDisplayList& dl, const ChromeStyle& s, const ChromeTelemetryModel& t,
                          const DesktopRect& r, unsigned kind, uint64_t now)
{
    if (r.w <= 0) return;
    const auto& p = s.P(); const int line = s.Font().lineHeight;
    const int top = r.y, bottom = r.y + r.h - line;
    auto text = [&](int x, int y, const char* v, uint32_t color) { dl.Text(x, y, v, color, r.x + r.w - x); };
    if (kind < 2) {
        unsigned percent = kind ? t.stats.memoryPercent : t.stats.cpuPercent; if (percent > 100) percent = 100;
        const uint32_t color = percent >= 90 ? p.hot : percent >= 70 ? p.warm : p.ok;
        text(r.x, top, kind ? "MEM" : "CPU", p.secondaryText);
        dl.Fill(r.x, bottom, r.w, line, s.MaterialColor(p.taskButton));
        dl.Fill(r.x, bottom, r.w * percent / 100, line, color | 0xff000000u);
        char value[12]; DesktopNumber(value, percent);
        unsigned n = 0; while (value[n]) ++n; value[n] = '%'; value[n + 1] = 0;
        text(r.x + s.M(3), bottom, value, p.taskText); return;
    }
    if (kind == 6) {
        // Package temperature graph (1 s points, coloured against TjMax) and
        // the current temperature / package power. Changes at most once a
        // second, so it never forces per-frame overlay recaptures.
        if (!t.thermal) { text(r.x, top, "TEMP", p.secondaryText); text(r.x, bottom, "n/a", p.mutedText); return; }
        const ThermalSample* points = t.points;
        const uint32_t n = t.pointCount;
        const ThermalSample& now1s = n ? points[n - 1] : t.latest;
        text(r.x, top, "TEMP", p.secondaryText);
        const int gx = r.x + s.M(44), gw = r.w - s.M(44), gh = line - 2;
        if (gw > 0) {
            const unsigned shown = n < unsigned(gw) ? n : unsigned(gw);
            const int floorC = 40, ceilC = int(t.tjmax ? t.tjmax : 100);
            for (unsigned i = 0; i < shown; ++i) {
                const auto& v = points[n - shown + i]; if (v.pkgC < 0) continue;
                int c = v.pkgC < floorC ? floorC : v.pkgC > ceilC ? ceilC : v.pkgC;
                int bar = (c - floorC) * gh / (ceilC - floorC); if (bar < 1) bar = 1;
                dl.Fill(gx + gw - int(shown) + int(i), top + gh - bar, 1, bar, ThermalColour(v.pkgC, t.tjmax) | 0xff000000u);
            }
        }
        char value[48]{}; unsigned used = 0;
        auto append = [&](const char* v) { while (*v && used + 1 < sizeof(value)) value[used++] = *v++; value[used] = 0; };
        char num[12]; DesktopNumber(num, unsigned(now1s.pkgC)); append(num); append("C");
        if (t.rapl) {
            append("  "); DesktopNumber(num, now1s.pkgMw / 1000); append(num); append(".");
            DesktopNumber(num, (now1s.pkgMw / 100) % 10); append(num); append("W");
        }
        text(r.x, bottom, value, ThermalColour(now1s.pkgC, t.tjmax)); return;
    }
    if (kind < 4) {
        char first[20], second[20], number[12]; const bool net = kind == 3;
        CopyText(first, net ? "D " : "R ", sizeof(first));
        DesktopRate(number, net ? t.stats.downBytesPerSecond : t.stats.readBytesPerSecond);
        CopyText(first + 2, number, sizeof(first) - 2);
        unsigned n = 0; while (first[n]) ++n; CopyText(first + n, " MB/s", sizeof(first) - n);
        CopyText(second, net ? "U " : "W ", sizeof(second));
        DesktopRate(number, net ? t.stats.upBytesPerSecond : t.stats.writeBytesPerSecond);
        CopyText(second + 2, number, sizeof(second) - 2);
        n = 0; while (second[n]) ++n; CopyText(second + n, " MB/s", sizeof(second) - n);
        text(r.x, top, first, p.secondaryText); text(r.x, bottom, second, p.secondaryText); return;
    }
    const DesktopIntervals<>* source = kind == 4 ? t.compositor : t.app;
    if (!source) return;
    const auto& history = *source;
    const auto summary = history.Read(now);
    text(r.x, top, kind == 4 ? "COMP" : "APP", p.secondaryText);
    MetricGraph(dl, s, r.x + s.M(48), top, r.w - s.M(48), line - 2, history, now);
    if (!summary.count || (kind == 5 && (!history.last || now - history.last > 2000))) {
        text(r.x, bottom, "idle / no frames", p.mutedText); return;
    }
    char value[64]{}; unsigned used = 0;
    auto append = [&](const char* v) { while (*v && used + 1 < sizeof(value)) value[used++] = *v++; value[used] = 0; };
    char n[12]; append("80:"); DesktopNumber(n, summary.p80); append(n); append(" 95:"); DesktopNumber(n, summary.p95);
    append(n); append(" max:"); DesktopNumber(n, summary.max); append(n); append("ms");
    text(r.x, bottom, value, p.secondaryText);
}

void ChromeBuildTaskbar(const ChromeTaskbarModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const ChromeTaskbarLayout& l = *m.layout;
    const auto& p = s.P();
    const int screenW = l.screenW, screenH = l.screenH;
    const int32_t mouseX = m.mouseX, mouseY = m.mouseY;
    dl.Fill(0, l.y, screenW, screenH - l.y, s.MaterialColor(p.taskbar));
    dl.Fill(0, l.y, screenW, 1, p.separator);
    // The visual pill of a full-height target (hover follows the target).
    auto pill = [&](const DesktopRect& r) {
        const int ix = r.w > static_cast<int>(4 * s.M(SM::TaskbarPillInsetX)) ? s.M(SM::TaskbarPillInsetX) : 0;
        const int iy = r.h > static_cast<int>(4 * s.M(SM::TaskbarPillInsetY)) ? s.M(SM::TaskbarPillInsetY) : 0;
        return DesktopRect{r.x + ix, r.y + iy, r.w - 2 * ix, r.h - 2 * iy};
    };
    auto button = [&](const DesktopRect& target, const char* title, bool active, bool minimized) {
        const DesktopRect r = pill(target);
        uint32_t bg = active ? p.taskActive : target.contains(mouseX, mouseY) ? p.taskHover : p.taskButton;
        ChromeMaterialRect(dl, s, r, bg);
        if (active || minimized)
            dl.Fill(r.x + s.M(6), r.y + r.h - s.M(2), r.w - s.M(12), s.M(2), active ? p.accent : p.minimized);
        if (r.w > static_cast<int>(s.M(12))) dl.Text(r.x + s.M(6), l.textY, title, p.taskText, r.w - s.M(12));
    };
    const int icon = static_cast<int>(s.M(24));
    button(l.apps, "", m.launcherOpen, false);
    button(l.terminal, "", false, false);
    ChromeIcon(dl, s, l.apps.x + (l.apps.w - icon) / 2, l.apps.y + (l.apps.h - icon) / 2, icon, m.appsIcon);
    ChromeIcon(dl, s, l.terminal.x + (l.terminal.w - icon) / 2, l.terminal.y + (l.terminal.h - icon) / 2, icon,
               m.terminalIcon);
    const char* hoverTitle = l.apps.contains(mouseX, mouseY) ? "Applications" :
        l.terminal.contains(mouseX, mouseY) ? "New terminal" : nullptr;
    for (int i = 0; i < l.count; ++i)
    {
        const DesktopRect& r = l.buttons[i];
        const ChromeTaskbarButton& b = m.buttons[i];
        if (b.launch) {
            button(r, "", false, false);
            if (r.w >= icon && r.h >= icon)
                ChromeIcon(dl, s, r.x + (r.w - icon) / 2, r.y + (r.h - icon) / 2, icon, b.icon);
            const DesktopRect pr = pill(r);
            if (b.busy) {
                // Eight positions form a bounded animated progress ring.
                static const int dx[8] = {0, 3, 4, 3, 0, -3, -4, -3};
                static const int dy[8] = {-4, -3, 0, 3, 4, 3, 0, -3};
                unsigned step = (m.uptimeMs / 100) % 8;
                for (unsigned dot = 0; dot < 8; ++dot)
                    dl.Fill(pr.x + pr.w - 8 + dx[dot], pr.y + pr.h - 8 + dy[dot], 2, 2,
                            dot == step ? p.taskText : p.minimized);
            } else {
                dl.Text(pr.x + pr.w - s.M(10), l.textY, "!", p.taskText, s.M(10));
            }
            if (r.contains(mouseX, mouseY)) hoverTitle = b.hoverTitle;
            continue;
        }
        button(r, "", b.active, b.minimized);
        if (r.w >= icon && r.h >= icon)
            ChromeIcon(dl, s, r.x + (r.w - icon) / 2, r.y + (r.h - icon) / 2, icon, b.icon);
        if (r.contains(mouseX, mouseY)) hoverTitle = b.hoverTitle;
    }
    if (l.next.w > 0) {
        button(l.next, ">", false, false);
        if (l.next.contains(mouseX, mouseY)) hoverTitle = "More windows";
    }
    // Use the already-reserved tray for the hover label. Geometry stays fixed
    // and no additional empty strip is reserved when nothing is hovered.
    int labelX = screenW;
    const DesktopRect trayRects[] = {l.metrics[0], l.metrics[1], l.metrics[2], l.metrics[3], l.metrics[4], l.metrics[5],
                                     l.metrics[6], l.agent, l.volume, l.network, l.clock};
    for (const auto& r : trayRects) if (r.w && r.x < labelX) labelX = r.x;
    if (hoverTitle && labelX + static_cast<int>(s.M(12)) < screenW)
    {
        dl.Text(labelX, l.textY, hoverTitle, p.text, screenW - labelX - s.M(8));
        return;
    }
    if (l.clock.w > 0) dl.Text(l.clock.x, l.textY, l.clockText, p.secondaryText, l.clock.w);
    NetworkIndicator(dl, s, l.network, m.networkConnected);
    if (l.volume.w > 0) {
        const int size = static_cast<int>(s.M(16));
        ChromeBuildVolumeGlyph(dl, s, l.volume.x, l.volume.y + (l.volume.h - size) / 2, size,
                               m.volumeControl, m.volumeAvailable, p.taskbar);
    }
    AgentIndicator(dl, s, l.agent, m.agentShown, m.agentStale);
    for (unsigned i = 0; i < 7; ++i) DesktopMetric(dl, s, m.telemetry, l.metrics[i], i, m.uptimeMs);
}

// ---------------------------------------------------------------------------
// Volume popup and tray tooltip
// ---------------------------------------------------------------------------

static void PanelFrame(ChromeDisplayList& dl, const ChromeStyle& s, const DesktopRect& p)
{
    const auto& pal = s.P();
    dl.Fill(p.x, p.y, p.w, p.h, s.MaterialColor(pal.launcher));
    dl.Fill(p.x, p.y, p.w, 1, pal.launcherBorder);
    dl.Fill(p.x, p.y + p.h - 1, p.w, 1, pal.launcherBorder);
    dl.Fill(p.x, p.y, 1, p.h, pal.launcherBorder);
    dl.Fill(p.x + p.w - 1, p.y, 1, p.h, pal.launcherBorder);
}

void ChromeBuildVolumePopup(const ChromeVolumePopupModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const TaskbarVolumePopupLayout& l = m.layout;
    const auto& pal = s.P();
    const DesktopRect& p = l.panel;
    if (p.w <= 2 || p.h <= 2) return;
    PanelFrame(dl, s, p);
    const bool muted = (m.control & BROOK_AUDIO_MUTED) != 0;
    const uint32_t buttonBg = l.mute.contains(m.mouseX, m.mouseY) ? pal.taskHover : pal.taskButton;
    ChromeMaterialRect(dl, s, l.mute, buttonBg);
    const int size = static_cast<int>(s.M(16));
    ChromeBuildVolumeGlyph(dl, s, l.mute.x + (l.mute.w - size) / 2, l.mute.y + (l.mute.h - size) / 2, size,
                           m.control, m.available, buttonBg);
    const uint32_t level = brook_audio_volume(m.control);
    const int barH = static_cast<int>(s.M(4)), barY = l.track.y + (l.track.h - barH) / 2;
    const int knobX = TaskbarVolumeSliderX(l.track, level);
    const uint32_t fill = muted || !m.available ? ChromeBlendColor(pal.secondaryText, pal.launcher, 110) : pal.accent;
    dl.Fill(l.track.x, barY, l.track.w, barH, pal.launcherBorder);
    dl.Fill(l.track.x, barY, knobX - l.track.x + 1, barH, fill);
    const int knob = static_cast<int>(s.M(12));
    ChromeMaterialRect(dl, s, {knobX - knob / 2, l.track.y + (l.track.h - knob) / 2, knob, knob},
                       m.dragging ? pal.accent : pal.text);
    char value[8]{};
    char* out = value;
    DesktopNumber(out, level);
    while (*out) ++out;
    *out++ = '%'; *out = 0;
    const int textY = l.value.y + (l.value.h - s.Font().lineHeight) / 2;
    ChromeMaterialString(dl, s, l.value.x + l.value.w - s.TextWidth(value), textY,
                         value, muted ? pal.secondaryText : pal.text, l.value.w);
}

void ChromeBuildTooltip(const ChromeTooltipModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const DesktopRect& p = m.box;
    if (p.w <= 0 || p.h <= 0 || !m.text) return;
    PanelFrame(dl, s, p);
    const int pad = ChromeTooltipPadding(s);
    ChromeMaterialString(dl, s, p.x + pad, p.y + (p.h - s.Font().lineHeight) / 2, m.text, s.P().text, p.w - pad);
}

// ---------------------------------------------------------------------------
// Launcher
// ---------------------------------------------------------------------------

static constexpr const char* LauncherPowerLabels[] = {"Restart", "Shut down", "Cancel"};
static constexpr const char* LauncherConfirmLabels[] = {"Restart now", "Shut down now"};

LauncherPowerMetrics ChromeLauncherPowerMetrics(const ChromeStyle& s)
{
    int label = 0, confirm = 0;
    for (const char* text : LauncherPowerLabels) { const int w = s.TextWidth(text); if (w > label) label = w; }
    for (const char* text : LauncherConfirmLabels) { const int w = s.TextWidth(text); if (w > confirm) confirm = w; }
    const int inner = 2 * static_cast<int>(s.M(14));
    return {static_cast<int>(s.M(SM::LauncherItemHeight)), static_cast<int>(s.M(SM::LauncherPadding)),
            static_cast<int>(s.M(6)), static_cast<int>(s.M(28)), static_cast<int>(s.M(12)),
            label + inner, confirm + inner};
}

ChromeLauncherLayout ChromeLauncherLayoutFor(const ChromeStyle& s, uint32_t screenW, uint32_t screenH, uint32_t total)
{
    const uint32_t desktopH = screenH > s.M(DesktopMetrics::TaskbarBase) ? screenH - s.M(DesktopMetrics::TaskbarBase) : 0;
    ChromeLauncherLayout l = {};
    l.x = screenW > s.M(8) ? s.M(4) : 0;
    const uint32_t availableW = screenW > s.M(8) ? screenW - s.M(8) : screenW;
    const uint32_t availableH = desktopH > s.M(4) ? desktopH - s.M(4) : desktopH;
    const LauncherPowerMetrics power = ChromeLauncherPowerMetrics(s);
    const uint32_t fixedH = s.M(SM::LauncherItemHeight) + 2 * s.M(SM::LauncherPadding) +
                            static_cast<uint32_t>(LauncherPowerFooterHeight(power));
    l.rows = availableH > fixedH ? (availableH - fixedH) / (s.M(SM::LauncherItemHeight) + s.M(2)) : 0;
    if (l.rows > SM::LauncherMaxRows) l.rows = SM::LauncherMaxRows;
    l.cols = l.rows ? (total + l.rows - 1) / l.rows : 1;
    if (!l.cols) l.cols = 1;
    if (l.cols > SM::LauncherMaxCols) l.cols = SM::LauncherMaxCols;
    uint32_t fitCols = availableW > 2 * s.M(SM::LauncherPadding) ?
        (availableW - 2 * s.M(SM::LauncherPadding)) / (s.M(SM::LauncherItemWidth) + s.M(4)) : 0;
    if (!fitCols) fitCols = 1;
    if (l.cols > fitCols) l.cols = fitCols;
    l.itemW = availableW > 2 * s.M(SM::LauncherPadding) + s.M(4) ?
        (availableW - 2 * s.M(SM::LauncherPadding)) / l.cols - s.M(4) : 0;
    if (l.itemW > s.M(SM::LauncherItemWidth)) l.itemW = s.M(SM::LauncherItemWidth);
    uint32_t neededRows = (total + l.cols - 1) / l.cols;
    if (l.rows > neededRows) l.rows = neededRows;
    l.capacity = l.rows * l.cols;
    l.w = l.cols * (l.itemW + s.M(4)) + 2 * s.M(SM::LauncherPadding);
    const uint32_t footerW = static_cast<uint32_t>(LauncherPowerMinimumWidth(power));
    if (l.w < footerW) l.w = footerW; // the power prompt never clips its confirm button
    if (l.w > availableW) l.w = availableW;
    l.h = fixedH + l.rows * (s.M(SM::LauncherItemHeight) + s.M(2));
    if (l.h > availableH) l.h = availableH;
    l.y = static_cast<int>(desktopH) - static_cast<int>(l.h) - s.M(2);
    if (l.y < 0) l.y = 0;
    return l;
}

DesktopRect ChromeLauncherItemRect(const ChromeStyle& s, const ChromeLauncherLayout& l, uint32_t slot)
{
    return {l.x + static_cast<int>(s.M(SM::LauncherPadding) + (slot % l.cols) * (l.itemW + s.M(4))),
            l.y + static_cast<int>(s.M(SM::LauncherPadding) + s.M(SM::LauncherItemHeight) +
                                   (slot / l.cols) * (s.M(SM::LauncherItemHeight) + s.M(2))),
            static_cast<int>(l.itemW), static_cast<int>(s.M(SM::LauncherItemHeight))};
}

static void PowerButton(ChromeDisplayList& dl, const ChromeStyle& s, const DesktopRect& r, const char* label,
                        uint32_t bg, uint32_t fg, bool focused)
{
    if (r.w <= 0 || r.h <= 0) return;
    const int inset = static_cast<int>(s.M(4));
    const DesktopRect face{r.x, r.y + inset, r.w, r.h - 2 * inset};
    if (focused) {
        const int ring = static_cast<int>(s.M(2));
        ChromeMaterialRect(dl, s, {face.x - ring, face.y - ring, face.w + 2 * ring, face.h + 2 * ring}, s.P().accent);
    }
    ChromeMaterialRect(dl, s, face, bg);
    const int textW = s.TextWidth(label);
    const int x = r.x + (textW < r.w ? (r.w - textW) / 2 : 0);
    ChromeMaterialString(dl, s, x, r.y + (r.h - s.Font().lineHeight) / 2, label, fg, r.w);
}

static void PowerFooter(ChromeDisplayList& dl, const ChromeStyle& s, const ChromeLauncherModel& m)
{
    const LauncherPowerLayout f = ChromeLauncherPowerLayout(s, m.layout);
    if (f.footer.w <= 0) return;
    const auto& pal = s.P();
    dl.Fill(f.separator.x, f.separator.y, f.separator.w, f.separator.h, pal.launcherBorder);
    const auto face = [&](const DesktopRect& r) { return r.contains(m.mouseX, m.mouseY) ? pal.taskHover : pal.taskButton; };
    if (m.shutdownActive) {
        PowerButton(dl, s, f.cluster, "Cancel shutdown", face(f.cluster), pal.text, false);
        return;
    }
    if (m.prompt) {
        // Destructive confirm: the close-button red, apart from the cluster.
        const uint32_t confirmBg = f.confirm.contains(m.mouseX, m.mouseY) ?
            ChromeBlendColor(pal.closeHover, pal.text, 200) : pal.closeHover;
        PowerButton(dl, s, f.confirm, LauncherConfirmLabels[m.promptRestart ? 0 : 1], confirmBg, pal.closeText,
                    m.confirmFocused);
        PowerButton(dl, s, f.dismiss, LauncherPowerLabels[2], face(f.dismiss), pal.text, !m.confirmFocused);
        return;
    }
    PowerButton(dl, s, f.restart, LauncherPowerLabels[0], face(f.restart), pal.text, false);
    PowerButton(dl, s, f.shutdown, LauncherPowerLabels[1], face(f.shutdown), pal.text, false);
}

void ChromeBuildLauncher(const ChromeLauncherModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const auto& pal = s.P();
    const ChromeLauncherLayout& layout = m.layout;
    const int32_t px = layout.x, py = layout.y;
    const uint32_t pw = layout.w, ph = layout.h;

    // Panel background and border
    dl.Fill(px, py, static_cast<int>(pw), static_cast<int>(ph), s.MaterialColor(pal.launcher));
    dl.Fill(px, py, static_cast<int>(pw), 1, pal.launcherBorder);
    dl.Fill(px, py + static_cast<int32_t>(ph) - 1, static_cast<int>(pw), 1, pal.launcherBorder);
    dl.Fill(px, py, 1, static_cast<int>(ph), pal.launcherBorder);
    dl.Fill(px + static_cast<int32_t>(pw) - 1, py, 1, static_cast<int>(ph), pal.launcherBorder);

    int textYOff = (static_cast<int>(s.M(SM::LauncherItemHeight)) - s.Font().lineHeight) / 2;

    // Extremely small surfaces cannot fit even the header.
    if (ph < s.M(SM::LauncherItemHeight) + 2 * s.M(SM::LauncherPadding) || pw < 2 * s.M(SM::LauncherPadding) + 1) return;

    // Header: status or prompt
    int32_t iy = py + static_cast<int32_t>(s.M(SM::LauncherPadding));
    if (pw > s.M(56) && m.header)
        ChromeMaterialString(dl, s, px + static_cast<int32_t>(s.M(SM::LauncherPadding)) + static_cast<int>(s.M(4)),
                             iy + static_cast<int32_t>(textYOff), m.header, pal.secondaryText,
                             static_cast<int>(pw) - s.M(56));
    iy += s.M(SM::LauncherItemHeight);

    // Separator line below header
    dl.Fill(px + static_cast<int32_t>(s.M(SM::LauncherPadding)), iy - 1,
            static_cast<int>(pw) - 2 * s.M(SM::LauncherPadding), 1, pal.launcherBorder);

    // Power controls: always available, in their own footer below a separator.
    PowerFooter(dl, s, m);

    for (uint32_t slot = 0; slot < m.count && slot < layout.capacity; ++slot)
    {
        const ChromeLauncherItem& entry = m.items[slot];
        const DesktopRect item = ChromeLauncherItemRect(s, layout, slot);
        const int itemX = item.x, itemY = item.y;
        bool hovered = item.contains(m.mouseX, m.mouseY);
        uint32_t itemBg = s.MaterialColor(hovered ? pal.itemHover : pal.launcher);

        dl.Fill(itemX, itemY, static_cast<int>(layout.itemW), static_cast<int>(s.M(SM::LauncherItemHeight)), itemBg);

        int32_t iconX = itemX + s.M(6);
        int32_t iconY = itemY + static_cast<int32_t>((s.M(SM::LauncherItemHeight) - s.M(SM::LauncherIconSize)) / 2);
        if (layout.itemW >= s.M(SM::LauncherIconSize) + s.M(12))
            ChromeIcon(dl, s, iconX, iconY, s.M(SM::LauncherIconSize), entry.icon);

        // Title text (offset to right of icon)
        int32_t textX = iconX + static_cast<int32_t>(s.M(SM::LauncherIconSize)) +
                        static_cast<int32_t>(s.M(SM::LauncherIconMargin));
        uint32_t textFg = hovered ? pal.text : pal.taskText;
        int maxTextW = static_cast<int>(layout.itemW) -
                       static_cast<int>(s.M(SM::LauncherIconSize) + s.M(SM::LauncherIconMargin) + s.M(12));
        if (maxTextW > 0) ChromeMaterialString(dl, s, textX, itemY + static_cast<int32_t>(textYOff),
                                               entry.title, textFg, maxTextW);
    }

    // Wheel-scroll hints live in the header, clear of application labels.
    if (pw >= s.M(48) && m.scrollUp)
        ChromeMaterialString(dl, s, px + static_cast<int32_t>(pw) - static_cast<int>(s.M(32)),
                             py + static_cast<int32_t>(s.M(SM::LauncherPadding)) + static_cast<int32_t>(textYOff),
                             "^", pal.secondaryText);
    if (pw >= s.M(48) && m.scrollDown)
        ChromeMaterialString(dl, s, px + static_cast<int32_t>(pw) - static_cast<int>(s.M(16)),
                             py + static_cast<int32_t>(s.M(SM::LauncherPadding)) + textYOff,
                             "v", pal.secondaryText);
}

// ---------------------------------------------------------------------------
// Boot summary
// ---------------------------------------------------------------------------

void ChromeBuildBootSummary(const ChromeBootSummaryModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const int x = m.rect.x, y = m.rect.y;
    const uint32_t w = static_cast<uint32_t>(m.rect.w), h = static_cast<uint32_t>(m.rect.h);
    if (!w || !h) return;
    const auto& p = s.P();
    dl.Fill(x, y, w, h, p.taskbar);
    dl.Fill(x, y, w, s.M(3), p.accent);
    int py = y + s.M(11); const int line = s.Font().lineHeight;
    const uint32_t colors[5] = {p.text, p.secondaryText, p.secondaryText, p.secondaryText, p.mutedText};
    for (int i = 0; i < 5; ++i) {
        if (m.rows[i]) dl.Text(x + s.M(12), py, m.rows[i], colors[i], w - s.M(24));
        py += line;
    }
}

} // namespace brook
