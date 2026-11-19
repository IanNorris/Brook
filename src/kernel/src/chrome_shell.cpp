// Shell-surface layouts and display list builders: taskbar, launcher, tray
// flyouts, boot toast and performance HUD, in the Clearwater type scale and
// geometry (DESIGN.md "Token sheet"; compositor refactor stages 1.2 and 2).
// Everything here is drawn once into a cached surface when its content key
// changes; nothing runs per frame. Freestanding.
#include "chrome_shell.h"

namespace brook {

using SM = ChromeShellMetrics;
using CF = ChromeFont;

static void CopyText(char* dst, const char* src, uint32_t capacity)
{
    uint32_t i = 0;
    while (src && src[i] && i + 1 < capacity) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

static int Px(const ChromeStyle& s, int base) { return static_cast<int>(s.M(static_cast<uint32_t>(base))); }

// Text whose right edge is at `right`.
static void TextRight(ChromeDisplayList& dl, const ChromeStyle& s, CF font, int right, int y, const char* text,
                      uint32_t color)
{
    dl.Text(font, right - s.TextWidth(text, font), y, text, color);
}

// The recessed fill behind search fields, graphs and status pills
// (DESIGN.md "Well": black 22% on dark chrome, white 50% on light).
static uint32_t WellColor(const ChromeStyle& s, uint32_t surface)
{
    return s.MaterialColor(s.P().chromeDark ? ChromeBlendColor(0x000000, surface, 56)
                                            : ChromeBlendColor(0xffffff, surface, 128));
}
// A hairline on a material surface (the prototype's outer rim colour).
static uint32_t HairlineColor(const ChromeStyle& s, uint32_t surface)
{
    return s.MaterialColor(ChromeBlendColor(s.P().text, surface, s.P().chromeDark ? 36 : 61));
}

static void IconAt(ChromeDisplayList& dl, const ChromeStyle& s, int x, int y, int size, const uint32_t* pixels,
                   unsigned opacity = 255)
{
    if (pixels && opacity < 255) dl.Icon(x, y, size, pixels, opacity);
    else ChromeIcon(dl, s, x, y, size, pixels);
}

// ---------------------------------------------------------------------------
// Taskbar
// ---------------------------------------------------------------------------

// Compact HUD: 8 px padding, then for APP and COMP a caption label, a 16-bar
// graph and a three-digit value (6 px gaps, 12 px between the two), then "ms".
static constexpr int CompactHudBars = 16;
static int CompactHudWidth(const ChromeStyle& s)
{
    const int series = Px(s, 6) + CompactHudBars * Px(s, 3) + Px(s, 6) + s.TextWidth("000", CF::Label);
    return 2 * Px(s, 8) + s.TextWidth("APP", CF::Small) + s.TextWidth("COMP", CF::Small) + 2 * series + Px(s, 12) +
           Px(s, 4) + s.TextWidth("ms", CF::Small);
}

ChromeTaskbarLayout ChromeTaskbarLayoutFor(const ChromeStyle& s, uint32_t screenW, uint32_t screenH,
                                           const char* clockText, bool agent, uint32_t enabledMetrics,
                                           const int* ids, int total, int focused, const char* buildText,
                                           bool compactHud, const char* const* labels)
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
    l.textY = by + (bh - s.Line(CF::Label)) / 2;
    // A 1 px, 20 px high separator with 4 px either side.
    const int sepGap = Px(s, 4), sepH = Px(s, 20) < bh ? Px(s, 20) : bh;
    int x = s.M(SM::TaskbarPadding);
    const int appsW = s.M(SM::TaskbarLauncherWidth);
    l.apps = {x, by, appsW, bh}; x += appsW + s.M(SM::TaskbarPadding);
    l.separators[0] = {x + sepGap, by + (bh - sepH) / 2, 1, sepH};
    x += 2 * sepGap + 1;
    l.terminal = {x, by, static_cast<int>(s.M(SM::TaskbarButtonWidth)), bh};
    x += s.M(SM::TaskbarButtonWidth) + s.M(SM::TaskbarPadding);
    CopyText(l.clockText, clockText, sizeof(l.clockText));
    for (unsigned i = 0; l.clockText[i] && l.clockText[i + 1]; ++i) {
        if (l.clockText[i] != ' ') continue;
        const char* date = l.clockText + i + 1;
        while (*date == ' ') ++date;
        CopyText(l.clockDate, date, sizeof(l.clockDate));
        // The tray keeps the day and abbreviated month on the second line.
        for (unsigned j = 0, spaces = 0; l.clockDate[j]; ++j) {
            if (l.clockDate[j] == ' ' && ++spaces == 2) { l.clockDate[j] = 0; break; }
        }
        l.clockText[i] = 0;
        break;
    }
    int right = sw - Px(s, 4);
    // Reserve measured widgets from right to left. Keep one useful window
    // button on small displays; diagnostics yield before the clock does.
    auto reserve = [&](DesktopRect& r, int width, int gap = 0) {
        if (right - width - gap < x + Px(s, 80)) return false;
        right -= width;
        r = {right, by, width, bh};
        right -= gap;
        return true;
    };
    // Clock: 12 px left and 8 px right of the wider of its two lines.
    if (l.clockText[0]) {
        const int time = s.TextWidth(l.clockText, CF::Label), date = s.TextWidth(l.clockDate, CF::Small);
        reserve(l.clock, Px(s, 12) + (time > date ? time : date) + Px(s, 8));
    }
    // Tray, as the prototype orders it: network, volume, temperature.
    const int trayTarget = Px(s, 44);
    if (enabledMetrics & (1u << 6))
        reserve(l.metrics[6], 2 * Px(s, 12) + Px(s, 16) + Px(s, 6) + s.TextWidth("100" BROOK_CHROME_DEGREE, CF::Label));
    reserve(l.volume, trayTarget);
    reserve(l.network, trayTarget);
    // Leftmost fixed icon, so its appearance never moves the tray targets.
    if (agent) reserve(l.agent, Px(s, 16) + Px(s, 12));
    {
        DesktopRect separator{};
        if (reserve(separator, 2 * sepGap + 1))
            l.separators[1] = {separator.x + sepGap, by + (bh - sepH) / 2, 1, sepH};
    }
    if (buildText && *buildText) {
        // Choose the complete build ID before adding the product name. A
        // dirty build can fall back to its complete base hash on tight trays.
        const char prefix[] = "Brook ";
        unsigned name = 0;
        while (prefix[name] && buildText[name] == prefix[name]) ++name;
        if (prefix[name]) name = 0;
        const char* id = buildText + name;
        unsigned length = 0, base = 0;
        while (id[length] && (!name || id[length] != ' ')) {
            if (id[length] == '+' && !base) base = length;
            ++length;
        }
        auto reserveBuild = [&](unsigned start, unsigned count) {
            if (!count || count >= sizeof(l.buildText)) return false;
            char text[sizeof(l.buildText)];
            for (unsigned i = 0; i < count; ++i) text[i] = buildText[start + i];
            text[count] = 0;
            const int width = s.TextWidth(text, CF::Mono) + 2 * Px(s, 8);
            if (width > Px(s, 188) || !reserve(l.build, width)) return false;
            CopyText(l.buildText, text, sizeof(l.buildText));
            return true;
        };
        if (!reserveBuild(0, name + length) && !reserveBuild(name, length) && base) {
            if (!reserveBuild(0, name + base)) reserveBuild(name, base);
        }
    }
    if (compactHud) reserve(l.hud, CompactHudWidth(s));
    // CPU and MEM are one-line chips; the rates and cadence keep two lines.
    const int widths[] = {92, 92, 84, 84, 176, 176};
    for (int i = 5; i >= 0; --i) if (enabledMetrics & (1u << i)) {
        // 12 px clear of whatever is to the right.
        const int before = right;
        right -= Px(s, 12);
        if (!reserve(l.metrics[i], Px(s, widths[i]))) { right = before; continue; }
        l.metrics[i].y = l.y + Px(s, 2);
        l.metrics[i].h = height - 2 * Px(s, 2);
    }
    if (total > static_cast<int>(ChromeTaskbarButtonLimit)) total = ChromeTaskbarButtonLimit;
    int available = right - x;
    if (!total || available <= 0) return l;
    // Sized to their labels while every entry fits (48 to 188 wide).
    if (labels) {
        int natural[ChromeTaskbarButtonLimit], sum = 0;
        for (int i = 0; i < total; ++i) {
            const int label = labels[i] && labels[i][0] ? s.TextWidth(labels[i], CF::Label) : 0;
            int width = 2 * Px(s, SM::TaskbarButtonPad) + Px(s, SM::TaskbarIcon) + (label ? Px(s, SM::TaskbarLabelGap) + label : 0);
            if (width < Px(s, SM::TaskbarButtonWidth)) width = Px(s, SM::TaskbarButtonWidth);
            if (width > Px(s, SM::TaskbarMaxButtonWidth)) width = Px(s, SM::TaskbarMaxButtonWidth);
            natural[i] = width; sum += width;
        }
        if (sum <= available) {
            l.count = total;
            for (int i = 0; i < total; ++i) {
                l.indices[i] = ids[i];
                l.buttons[i] = {x, by, natural[i], bh};
                x += natural[i];
            }
            return l;
        }
    }
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
    if (width > static_cast<int>(s.M(SM::TaskbarMaxButtonWidth))) width = s.M(SM::TaskbarMaxButtonWidth);
    for (int i = 0; i < l.count; ++i)
    {
        l.indices[i] = ids[first + i];
        l.buttons[i] = {x, by, width, bh};
        x += width + s.M(SM::TaskbarPadding);
    }
    if (overflow) l.nextIndex = ids[(first + capacity) % total];
    return l;
}

// The network line icon in the tray colour; disconnected dims the arrows and
// adds a cross (one mask, ChromeGlyph::NetworkOff).
static void NetworkIndicator(ChromeDisplayList& dl, const ChromeStyle& s, const DesktopRect& r, bool connected)
{
    if (r.w <= 0) return;
    const int size = Px(s, 16);
    dl.Glyph(connected ? ChromeGlyph::Network : ChromeGlyph::NetworkOff, r.x + (r.w - size) / 2,
             r.y + (r.h - size) / 2, s.P().secondaryText);
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


// The speaker line icon for the level, centred in a size x size cell: two
// waves when loud, one when quiet, a cross when muted. No device draws the
// muted icon dimmed toward bg.
void ChromeBuildVolumeGlyph(ChromeDisplayList& dl, const ChromeStyle& s, int x0, int y0, int size,
                            uint32_t control, bool available, uint32_t bg)
{
    const uint32_t fg = s.P().secondaryText;
    const TaskbarVolumeGlyph level = TaskbarVolumeGlyphFor(control);
    const ChromeGlyph glyph = !available || level == TaskbarVolumeGlyph::Muted ? ChromeGlyph::VolumeMuted :
                              level == TaskbarVolumeGlyph::High ? ChromeGlyph::VolumeHigh : ChromeGlyph::VolumeLow;
    const int icon = Px(s, 16);
    dl.Glyph(glyph, x0 + (size - icon) / 2, y0 + (size - icon) / 2, available ? fg : ChromeBlendColor(fg, bg, 110));
}

// Colour band of a package temperature (ThermalColour's margins to TjMax).
static uint32_t ThermalInk(const ChromeStyle& s, int32_t tempC, uint32_t tjmax, uint32_t cool)
{
    const int32_t margin = int32_t(tjmax ? tjmax : 100) - tempC;
    return margin <= 10 ? s.P().hot : margin <= 25 ? s.P().warm : cool;
}
static const char* ThermalWord(int32_t tempC, uint32_t tjmax)
{
    const int32_t margin = int32_t(tjmax ? tjmax : 100) - tempC;
    return margin <= 10 ? "Hot" : margin <= 25 ? "Warm" : "Cool";
}
// "57<degree>" or "57 <degree>C".
static void ThermalText(char* out, int32_t tempC, bool unit)
{
    DesktopNumber(out, static_cast<unsigned>(tempC < 0 ? 0 : tempC));
    while (*out) ++out;
    CopyText(out, unit ? " " BROOK_CHROME_DEGREE "C" : BROOK_CHROME_DEGREE, 8);
}
// "6.2 W".
static void PowerText(char* out, uint32_t milliwatts)
{
    DesktopNumber(out, milliwatts / 1000); while (*out) ++out;
    *out++ = '.'; *out++ = static_cast<char>('0' + (milliwatts / 100) % 10);
    CopyText(out, " W", 4);
}

// Bars of a cadence history, newest at the right: `pitch` apart, one per
// sample from the last 10 s, scaled to `fullMs`; over-budget frames are hot.
static void CadenceBars(ChromeDisplayList& dl, const ChromeStyle& s, const DesktopRect& r,
                        const DesktopIntervals<>& history, uint64_t tick, unsigned fullMs, uint32_t ink)
{
    const int pitch = Px(s, 3), bar = pitch > 1 ? pitch - Px(s, 1) : 1;
    if (r.w < pitch || r.h <= 0) return;
    const unsigned capacity = static_cast<unsigned>(r.w / pitch);
    const unsigned shown = history.count < capacity ? history.count : capacity;
    for (unsigned i = 0; i < shown; ++i) {
        const auto& v = history.samples[(history.next + 128 - shown + i) % 128];
        if (tick < v.tick || tick - v.tick > 10000) continue;
        const unsigned ms = v.ms < fullMs ? v.ms : fullMs;
        const int height = 1 + (r.h - 1) * static_cast<int>(ms) / static_cast<int>(fullMs);
        dl.Fill(r.x + r.w - static_cast<int>(shown - i) * pitch, r.y + r.h - height, bar, height,
                0xff000000u | (v.ms >= 17 ? s.P().hot : ink));
    }
}
// The newest sample's interval in ms, or false when none is recent.
static bool CadenceLatest(const DesktopIntervals<>& history, uint64_t tick, char* out)
{
    if (!history.count) return false;
    const auto& v = history.samples[(history.next + 127) % 128];
    if (tick < v.tick || tick - v.tick > 10000) return false;
    DesktopNumber(out, v.ms > 999 ? 999 : v.ms);
    return true;
}

// Taskbar diagnostics (Settings > Taskbar): CPU and MEM as one-line chips
// (caption label, 6 px meter, value), disk and network rates as two caption
// lines, and the cadence histories as label, graph and percentiles.
static void DesktopMetric(ChromeDisplayList& dl, const ChromeStyle& s, const ChromeTelemetryModel& t,
                          const DesktopRect& r, unsigned kind, uint64_t now)
{
    if (r.w <= 0) return;
    const auto& p = s.P(); const int line = s.Line(CF::Small);
    const int top = r.y + (r.h - 2 * line) / 2, bottom = top + line;
    auto text = [&](int x, int y, const char* v, uint32_t color) { dl.Text(CF::Small, x, y, v, color, r.x + r.w - x); };
    if (kind < 2) {
        unsigned percent = kind ? t.stats.memoryPercent : t.stats.cpuPercent; if (percent > 100) percent = 100;
        const uint32_t color = percent >= 90 ? p.hot : percent >= 70 ? p.warm : p.ok;
        const char* label = kind ? "MEM" : "CPU";
        const int labelW = s.TextWidth("MEM", CF::Small);
        dl.Text(CF::Small, r.x, r.y + (r.h - line) / 2, label, p.mutedText);
        const int meterX = r.x + labelW + Px(s, 6), meterW = Px(s, 28), meterH = Px(s, 6);
        const int meterY = r.y + (r.h - meterH) / 2;
        dl.MaterialRect({meterX, meterY, meterW, meterH}, WellColor(s, p.taskbar), Px(s, 3));
        const int fill = meterW * static_cast<int>(percent) / 100;
        if (fill > 0) dl.MaterialRect({meterX, meterY, fill < meterH ? meterH : fill, meterH}, color | 0xff000000u, Px(s, 3));
        char value[12]; DesktopNumber(value, percent);
        unsigned n = 0; while (value[n]) ++n; value[n] = '%'; value[n + 1] = 0;
        TextRight(dl, s, CF::Label, r.x + r.w, r.y + (r.h - s.Line(CF::Label)) / 2, value, p.taskText);
        return;
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
    if (kind > 5) return;
    const DesktopIntervals<>* source = kind == 4 ? t.compositor : t.app;
    if (!source) return;
    const auto& history = *source;
    const auto summary = history.Read(now);
    text(r.x, top, kind == 4 ? "COMP" : "APP", p.mutedText);
    const int graphX = r.x + s.TextWidth("COMP", CF::Small) + Px(s, 6);
    CadenceBars(dl, s, {graphX, top + Px(s, 1), r.x + r.w - graphX, line - Px(s, 2)}, history, now, 100, p.accent);
    if (!summary.count || (kind == 5 && (!history.last || now - history.last > 2000))) {
        text(r.x, bottom, "idle / no frames", p.mutedText); return;
    }
    char value[64]{}; unsigned used = 0;
    auto append = [&](const char* v) { while (*v && used + 1 < sizeof(value)) value[used++] = *v++; value[used] = 0; };
    char n[12]; append("p80 "); DesktopNumber(n, summary.p80); append(n); append("  p95 "); DesktopNumber(n, summary.p95);
    append(n); append("  max "); DesktopNumber(n, summary.max); append(n); append(" ms");
    text(r.x, bottom, value, p.secondaryText);
}

void ChromeBuildTaskbar(const ChromeTaskbarModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const ChromeTaskbarLayout& l = *m.layout;
    const auto& p = s.P();
    const int screenW = l.screenW, screenH = l.screenH;
    const int32_t mouseX = m.mouseX, mouseY = m.mouseY;
    const int barHeight = screenH - l.y;
    if (s.glass && s.material && s.material->taskbarHeight >= static_cast<unsigned>(barHeight))
        dl.Rows(0, l.y, screenW, barHeight, s.material->taskbarRows);
    else dl.Fill(0, l.y, screenW, barHeight, s.MaterialColor(p.taskbar));
    if (!s.glass) dl.Fill(0, l.y, screenW, 1, 0xff000000u | p.launcherBorder);
    auto pill = [&](const DesktopRect& r) {
        const int ix = r.w > static_cast<int>(4 * s.M(SM::TaskbarPillInsetX)) ? s.M(SM::TaskbarPillInsetX) : 0;
        const int iy = r.h > static_cast<int>(4 * s.M(SM::TaskbarPillInsetY)) ? s.M(SM::TaskbarPillInsetY) : 0;
        return DesktopRect{r.x + ix, r.y + iy, r.w - 2 * ix, r.h - 2 * iy};
    };
    // The visual pill appears only for the focused window, an open launcher
    // or the pointer; idle targets are bare (DESIGN.md "Taskbar").
    auto target = [&](const DesktopRect& r, bool on) {
        const bool hovered = r.contains(mouseX, mouseY);
        if (on || hovered) dl.MaterialRect(pill(r), s.MaterialColor(on ? p.taskActive : p.taskHover), s.M(6));
        return hovered;
    };
    // State pill, 7 px above the screen edge: wide accent for the focused
    // window, short for running, outlined for minimised.
    auto indicator = [&](const DesktopRect& r, bool active, bool minimized) {
        const int width = Px(s, active ? 20 : 6), height = Px(s, 3);
        const DesktopRect mark{r.x + (r.w - width) / 2, r.y + r.h - Px(s, 7) - height, width, height};
        if (minimized) dl.MaterialOutline(mark, 0xff000000u | p.mutedText, Px(s, 1));
        else dl.MaterialRect(mark, 0xff000000u | (active ? p.accent : p.mutedText), Px(s, 1));
    };
    for (const DesktopRect& separator : l.separators)
        if (separator.w > 0) dl.Fill(separator.x, separator.y, separator.w, separator.h, HairlineColor(s, p.taskbar));
    const int icon = Px(s, SM::TaskbarIcon), mark = Px(s, 24);
    auto centeredIcon = [&](const DesktopRect& r, const uint32_t* pixels, int size) {
        if (r.w >= size && r.h >= size) ChromeIcon(dl, s, r.x + (r.w - size) / 2, r.y + (r.h - size) / 2, size, pixels);
    };
    target(l.apps, m.launcherOpen);
    target(l.terminal, false);
    centeredIcon(l.apps, m.appsIcon, mark);
    centeredIcon(l.terminal, m.terminalIcon, icon);
    const int labelY = l.textY;
    for (int i = 0; i < l.count; ++i) {
        const DesktopRect& r = l.buttons[i];
        const ChromeTaskbarButton& b = m.buttons[i];
        const bool hovered = target(r, b.active);
        if (b.launch) {
            centeredIcon(r, b.icon, icon);
            const DesktopRect pr = pill(r);
            if (b.busy) {
                static const int dx[8] = {0, 3, 4, 3, 0, -3, -4, -3};
                static const int dy[8] = {-4, -3, 0, 3, 4, 3, 0, -3};
                unsigned step = (m.uptimeMs / 100) % 8;
                for (unsigned dot = 0; dot < 8; ++dot)
                    dl.Fill(pr.x + pr.w - 8 + dx[dot], pr.y + pr.h - 8 + dy[dot], 2, 2,
                            0xff000000u | (dot == step ? p.taskText : p.minimized));
            } else if (pr.w > Px(s, 10)) {
                dl.Text(CF::Label, pr.x + pr.w - Px(s, 10), labelY, "!", p.taskText, Px(s, 10));
            }
            continue;
        }
        indicator(r, b.active, b.minimized);
        const unsigned opacity = b.minimized ? 153 : 255;
        if (r.w >= Px(s, SM::TaskbarLabelMin) && r.h >= icon && b.hoverTitle && b.hoverTitle[0]) {
            const int ix = r.x + Px(s, SM::TaskbarButtonPad), tx = ix + icon + Px(s, SM::TaskbarLabelGap);
            IconAt(dl, s, ix, r.y + (r.h - icon) / 2, icon, b.icon, opacity);
            dl.Text(CF::Label, tx, labelY, b.hoverTitle,
                    b.active || hovered ? p.text : b.minimized ? p.mutedText : p.secondaryText,
                    r.x + r.w - tx - Px(s, SM::TaskbarButtonPad));
        } else if (r.w >= icon && r.h >= icon)
            IconAt(dl, s, r.x + (r.w - icon) / 2, r.y + (r.h - icon) / 2, icon, b.icon, opacity);
    }
    if (l.next.w > 0) {
        target(l.next, false);
        const int size = Px(s, 16);
        if (size <= l.next.w)
            dl.Glyph(ChromeGlyph::ChevronRight, l.next.x + (l.next.w - size) / 2, l.next.y + (l.next.h - size) / 2, p.taskText);
    }
    if (l.build.w > 0)
        dl.Text(CF::Mono, l.build.x + Px(s, 8), l.build.y + (l.build.h - s.Line(CF::Mono)) / 2, l.buildText,
                p.badgeText, l.build.w - Px(s, 8));
    if (l.clock.w > 0) {
        // Time over date, right-aligned 8 px from the target's edge.
        const int timeH = s.Line(CF::Label), dateH = s.Line(CF::Small), right = l.clock.x + l.clock.w - Px(s, 8);
        const bool twoLines = l.clockDate[0] && l.clock.h >= timeH + dateH;
        const int top = l.clock.y + (l.clock.h - (twoLines ? timeH + dateH : timeH)) / 2;
        TextRight(dl, s, CF::Label, right, top, l.clockText, p.text);
        if (twoLines) TextRight(dl, s, CF::Small, right, top + timeH, l.clockDate, p.secondaryText);
    }
    target(l.network, false);
    target(l.volume, false);
    if (l.metrics[6].w > 0) {
        // Thermometer and package temperature, coloured only when warm or hot.
        const DesktopRect& r = l.metrics[6];
        target(r, false);
        const auto& t = m.telemetry;
        const bool live = t.thermal && t.latest.pkgC >= 0;
        char value[16] = "n/a";
        if (live) ThermalText(value, t.latest.pkgC, false);
        const uint32_t ink = live ? ThermalInk(s, t.latest.pkgC, t.tjmax, p.secondaryText) : p.mutedText;
        const int glyph = Px(s, 16), width = glyph + Px(s, 6) + s.TextWidth(value, CF::Label);
        const int x = r.x + (r.w - width) / 2;
        dl.Glyph(ChromeGlyph::Temperature, x, r.y + (r.h - glyph) / 2, ink);
        dl.Text(CF::Label, x + glyph + Px(s, 6), labelY, value, ink, r.x + r.w - x - glyph - Px(s, 6));
    }
    NetworkIndicator(dl, s, l.network, m.networkConnected);
    if (l.volume.w > 0) {
        const int size = Px(s, 16);
        ChromeBuildVolumeGlyph(dl, s, l.volume.x + (l.volume.w - size) / 2,
                               l.volume.y + (l.volume.h - size) / 2, size,
                               m.volumeControl, m.volumeAvailable, p.taskbar);
    }
    AgentIndicator(dl, s, l.agent, m.agentShown, m.agentStale);
    for (unsigned i = 0; i < 6; ++i) DesktopMetric(dl, s, m.telemetry, l.metrics[i], i, m.uptimeMs);
}

// ---------------------------------------------------------------------------
// Shell panels: shared pieces
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

static int PanelHeaderHeight(const ChromeStyle& s)
{
    const int line = s.Line(CF::Lead), pill = Px(s, 20);
    return line > pill ? line : pill;
}
// Flyout header: 16 px icon, 15 px bold title and, at the right, a status
// pill (a 6 px dot and an 11 px word on the well).
static void PanelHeader(ChromeDisplayList& dl, const ChromeStyle& s, int x, int y, int w, ChromeGlyph icon,
                        const char* title, const char* status, uint32_t dot)
{
    const auto& pal = s.P();
    const int h = PanelHeaderHeight(s), glyph = Px(s, 16);
    dl.Glyph(icon, x, y + (h - glyph) / 2, pal.text);
    dl.Text(CF::Lead, x + glyph + Px(s, 8), y + (h - s.Line(CF::Lead)) / 2, title, pal.text, w - glyph - Px(s, 8));
    if (!status) return;
    const int pillH = Px(s, 20), dotSize = Px(s, 6), pad = Px(s, 8);
    const int pillW = 2 * pad + dotSize + Px(s, 6) + s.TextWidth(status, CF::Small);
    const DesktopRect pill{x + w - pillW, y + (h - pillH) / 2, pillW, pillH};
    dl.MaterialRect(pill, WellColor(s, pal.launcher), pillH / 2);
    dl.MaterialRect({pill.x + pad, pill.y + (pillH - dotSize) / 2, dotSize, dotSize}, 0xff000000u | dot, dotSize / 2);
    dl.Text(CF::Small, pill.x + pad + dotSize + Px(s, 6), pill.y + (pillH - s.Line(CF::Small)) / 2, status,
            pal.secondaryText);
}
// A key on the left, its value right-aligned (12 px); returns the next row's y.
static int PanelRow(ChromeDisplayList& dl, const ChromeStyle& s, int x, int y, int w, const char* key, const char* value)
{
    dl.Text(CF::Label, x, y, key, s.P().mutedText, w);
    TextRight(dl, s, CF::Label, x + w, y, value, s.P().text);
    return y + s.Line(CF::Label) + Px(s, 5);
}
// The recessed graph box (6 px radius, 6 px padding); returns its plot area.
static DesktopRect GraphBox(ChromeDisplayList& dl, const ChromeStyle& s, const DesktopRect& box)
{
    dl.MaterialRect(box, WellColor(s, s.P().launcher), Px(s, 6));
    const int pad = Px(s, 6);
    return {box.x + pad, box.y + pad, box.w - 2 * pad, box.h - pad - Px(s, 4)};
}
// Package temperature, one bar per second, 40 degrees to TjMax.
static void ThermalBars(ChromeDisplayList& dl, const ChromeStyle& s, const DesktopRect& r, const ChromeTelemetryModel& t)
{
    const int pitch = Px(s, 3), bar = pitch > 1 ? pitch - Px(s, 1) : 1;
    if (r.w < pitch || r.h <= 0 || !t.points) return;
    const unsigned capacity = static_cast<unsigned>(r.w / pitch);
    const unsigned shown = t.pointCount < capacity ? t.pointCount : capacity;
    const int floorC = 40, ceilC = int(t.tjmax ? t.tjmax : 100);
    for (unsigned i = 0; i < shown; ++i) {
        const auto& v = t.points[t.pointCount - shown + i];
        if (v.pkgC < 0) continue;
        const int c = v.pkgC < floorC ? floorC : v.pkgC > ceilC ? ceilC : v.pkgC;
        const int height = 1 + (r.h - 1) * (c - floorC) / (ceilC > floorC ? ceilC - floorC : 1);
        dl.Fill(r.x + r.w - static_cast<int>(shown - i) * pitch, r.y + r.h - height, bar, height,
                0xff000000u | ThermalInk(s, v.pkgC, t.tjmax, s.P().warm));
    }
}

// ---------------------------------------------------------------------------
// Volume popup and tray tooltip
// ---------------------------------------------------------------------------

void ChromeBuildVolumePopup(const ChromeVolumePopupModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const TaskbarVolumePopupLayout& l = m.layout;
    const auto& pal = s.P();
    const DesktopRect& p = l.panel;
    if (p.w <= 2 || p.h <= 2) return;
    PanelFrame(dl, s, p);
    const bool muted = (m.control & BROOK_AUDIO_MUTED) != 0;
    const int glyph = Px(s, 16);
    ChromeBuildVolumeGlyph(dl, s, l.title.x, l.title.y + (l.title.h - glyph) / 2, glyph, m.control, m.available,
                           pal.launcher);
    ChromeMaterialString(dl, s, l.title.x + glyph + Px(s, 8), l.title.y + (l.title.h - s.Line(CF::Lead)) / 2,
                         "Volume", pal.text, l.title.w - glyph - Px(s, 8), CF::Lead);
    // Mute: a 36 px icon button, filled only under the pointer.
    const bool muteHover = l.mute.contains(m.mouseX, m.mouseY);
    if (muteHover) ChromeMaterialRect(dl, s, l.mute, pal.taskHover);
    ChromeBuildVolumeGlyph(dl, s, l.mute.x + (l.mute.w - glyph) / 2, l.mute.y + (l.mute.h - glyph) / 2, glyph,
                           m.control, m.available, muteHover ? pal.taskHover : pal.launcher);
    const uint32_t level = brook_audio_volume(m.control);
    const int barH = Px(s, 4), barY = l.track.y + (l.track.h - barH) / 2;
    const int knobX = TaskbarVolumeSliderX(l.track, level);
    const uint32_t fill = muted || !m.available ? ChromeBlendColor(pal.secondaryText, pal.launcher, 110) : pal.accent;
    dl.MaterialRect({l.track.x, barY, l.track.w, barH}, WellColor(s, pal.launcher), barH / 2);
    if (knobX > l.track.x)
        dl.MaterialRect({l.track.x, barY, knobX - l.track.x + 1, barH}, 0xff000000u | fill, barH / 2);
    // Thumb: a 20 px disc with a 2 px ring in the fill colour.
    const int knob = l.thumbSize, ring = Px(s, 2);
    const DesktopRect disc{knobX - knob / 2, l.track.y + (l.track.h - knob) / 2, knob, knob};
    dl.MaterialRect(disc, 0xff000000u | fill, knob / 2);
    if (!m.dragging)
        dl.MaterialRect({disc.x + ring, disc.y + ring, knob - 2 * ring, knob - 2 * ring}, 0xffffffffu,
                        (knob - 2 * ring) / 2);
    char value[8]{};
    char* out = value;
    DesktopNumber(out, level);
    while (*out) ++out;
    *out++ = '%'; *out = 0;
    const int textY = l.value.y + (l.value.h - s.Line(CF::Label)) / 2;
    ChromeMaterialString(dl, s, l.value.x + l.value.w - s.TextWidth(value, CF::Label), textY,
                         value, muted ? pal.mutedText : pal.secondaryText, l.value.w, CF::Label);
}

void ChromeBuildTooltip(const ChromeTooltipModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const DesktopRect& p = m.box;
    if (p.w <= 0 || p.h <= 0 || !m.text) return;
    PanelFrame(dl, s, p);
    const int pad = ChromeTooltipPadding(s);
    const int line = s.Line(CF::Label);
    const int y = p.y + (p.h - line * (m.detail ? 2 : 1)) / 2;
    const int width = p.w - 2 * pad;
    if (width <= 0) return;
    ChromeMaterialString(dl, s, p.x + pad, y, m.text, s.P().text, width, CF::Label);
    if (m.detail)
        ChromeMaterialString(dl, s, p.x + pad, y + line, m.detail, s.P().secondaryText, width, CF::Label);
}

// ---------------------------------------------------------------------------
// Launcher
// ---------------------------------------------------------------------------

static constexpr const char* LauncherPowerLabels[] = {"Restart", "Shut down", "Cancel"};
static constexpr const char* LauncherConfirmLabels[] = {"Restart now", "Shut down now"};

LauncherPowerMetrics ChromeLauncherPowerMetrics(const ChromeStyle& s)
{
    int label = 0, confirm = 0;
    for (const char* text : LauncherPowerLabels) { const int w = s.TextWidth(text, CF::Label); if (w > label) label = w; }
    for (const char* text : LauncherConfirmLabels) { const int w = s.TextWidth(text, CF::Label); if (w > confirm) confirm = w; }
    // 12 px labels with 14 px padding; the idle buttons lead with a 16 px icon.
    const int inner = 2 * Px(s, 14), icon = Px(s, 16) + Px(s, 6);
    return {Px(s, SM::LauncherItemHeight), Px(s, SM::LauncherPadding), Px(s, 6), Px(s, 28), Px(s, 12),
            label + icon + inner, confirm + inner};
}

ChromeLauncherLayout ChromeLauncherLayoutFor(const ChromeStyle& s, uint32_t screenW, uint32_t screenH, uint32_t total)
{
    const uint32_t desktopH = screenH > s.M(DesktopMetrics::TaskbarBase) ? screenH - s.M(DesktopMetrics::TaskbarBase) : 0;
    ChromeLauncherLayout l = {};
    // 8 px from the screen edge and from the taskbar (DESIGN.md "Geometry").
    const uint32_t margin = s.M(SM::PanelMargin);
    l.x = screenW > 2 * margin ? margin : 0;
    const uint32_t availableW = screenW > 2 * margin ? screenW - 2 * margin : screenW;
    const uint32_t availableH = desktopH > 2 * margin ? desktopH - 2 * margin : desktopH;
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
    l.y = static_cast<int>(desktopH) - static_cast<int>(l.h) - static_cast<int>(desktopH > 2 * margin ? margin : 0);
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
                        uint32_t bg, uint32_t fg, bool focused, bool icon = false, ChromeGlyph glyph = ChromeGlyph::Power)
{
    if (r.w <= 0 || r.h <= 0) return;
    const int inset = Px(s, 4);
    const DesktopRect face{r.x, r.y + inset, r.w, r.h - 2 * inset};
    if (focused) {
        const int ring = Px(s, 2);
        dl.MaterialRect({face.x - ring, face.y - ring, face.w + 2 * ring, face.h + 2 * ring},
                        s.MaterialColor(s.P().accent), Px(s, 6) + ring);
    }
    ChromeMaterialRect(dl, s, face, bg);
    const int size = Px(s, 16), lead = icon ? size + Px(s, 6) : 0;
    const int textW = s.TextWidth(label, CF::Label) + lead;
    const int x = r.x + (textW < r.w ? (r.w - textW) / 2 : 0);
    if (icon && r.w >= textW) dl.Glyph(glyph, x, r.y + (r.h - size) / 2, fg);
    ChromeMaterialString(dl, s, x + (r.w >= textW ? lead : 0), r.y + (r.h - s.Line(CF::Label)) / 2, label, fg, r.w,
                         CF::Label);
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
    PowerButton(dl, s, f.restart, LauncherPowerLabels[0], face(f.restart), pal.text, false, true, ChromeGlyph::Restart);
    PowerButton(dl, s, f.shutdown, LauncherPowerLabels[1], face(f.shutdown), pal.text, false, true, ChromeGlyph::Power);
}

void ChromeBuildLauncher(const ChromeLauncherModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const auto& pal = s.P();
    const ChromeLauncherLayout& layout = m.layout;
    const int32_t px = layout.x, py = layout.y;
    const uint32_t pw = layout.w, ph = layout.h;

    PanelFrame(dl, s, {px, py, static_cast<int>(pw), static_cast<int>(ph)});

    const int rowH = Px(s, SM::LauncherItemHeight), pad = Px(s, SM::LauncherPadding);

    // Extremely small surfaces cannot fit even the header.
    if (static_cast<int>(ph) < rowH + 2 * pad || static_cast<int>(pw) < 2 * pad + 1) return;

    // Header: status or prompt
    int32_t iy = py + pad;
    if (pw > s.M(56) && m.header)
        ChromeMaterialString(dl, s, px + pad + Px(s, 8), iy + (rowH - s.Line(CF::Label)) / 2, m.header,
                             pal.secondaryText, static_cast<int>(pw) - Px(s, 56), CF::Label);
    iy += rowH;

    // Separator line below header
    dl.Fill(px + pad, iy - 1, static_cast<int>(pw) - 2 * pad, 1, pal.launcherBorder);

    // Power controls: always available, in their own footer below a separator.
    PowerFooter(dl, s, m);

    const int icon = Px(s, SM::LauncherIconSize);
    for (uint32_t slot = 0; slot < m.count && slot < layout.capacity; ++slot)
    {
        const ChromeLauncherItem& entry = m.items[slot];
        const DesktopRect item = ChromeLauncherItemRect(s, layout, slot);
        const bool hovered = item.contains(m.mouseX, m.mouseY);
        if (hovered) ChromeMaterialRect(dl, s, item, pal.itemHover);

        const int iconX = item.x + Px(s, 8);
        if (static_cast<int>(layout.itemW) >= icon + Px(s, 12))
            ChromeIcon(dl, s, iconX, item.y + (rowH - icon) / 2, icon, entry.icon);

        // Title text (offset to right of icon)
        const int textX = iconX + icon + Px(s, 10);
        const int maxTextW = item.x + static_cast<int>(layout.itemW) - textX - Px(s, 8);
        if (maxTextW > 0) ChromeMaterialString(dl, s, textX, item.y + (rowH - s.Line()) / 2, entry.title,
                                               hovered ? pal.text : pal.taskText, maxTextW);
    }

    // Wheel-scroll hints live in the header, clear of application labels.
    const int hint = Px(s, 16), hintY = py + pad + (rowH - hint) / 2;
    if (pw >= s.M(64) && m.scrollUp)
        dl.Glyph(ChromeGlyph::ChevronUp, px + static_cast<int>(pw) - pad - 2 * hint - Px(s, 8), hintY, pal.secondaryText);
    if (pw >= s.M(64) && m.scrollDown)
        dl.Glyph(ChromeGlyph::ChevronDown, px + static_cast<int>(pw) - pad - hint - Px(s, 4), hintY, pal.secondaryText);
}

// ---------------------------------------------------------------------------
// Boot toast
// ---------------------------------------------------------------------------

// DESIGN.md "Token sheet": boot segments, the same in every theme.
static constexpr uint32_t BootPhaseInk[4] = {0x7db8ff, 0xb9a6f5, 0x5fd6c8, 0xf0b35a};

int ChromeBootSummaryHeight(const ChromeStyle& s, bool total, bool phases, unsigned metaLines)
{
    int h = Px(s, 12) + s.Line(CF::Strong) + Px(s, 2);
    if (total) h += s.Line(CF::Hero) + Px(s, 8);
    if (phases) h += Px(s, 6) + Px(s, 8) + s.Line(CF::Small) + s.Line(CF::Label) + Px(s, 8);
    h += static_cast<int>(metaLines) * s.Line(CF::Small);
    return h + Px(s, 12);
}

// Icon column, bold title, then the body: the total as the hero number, a
// proportional Firmware / Menu / Loader / Kernel bar with its legend, and the
// machine, composition and build lines (DESIGN.md "Toasts").
void ChromeBuildBootSummary(const ChromeBootSummaryModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    if (m.rect.w <= 0 || m.rect.h <= 0) return;
    const auto& p = s.P();
    PanelFrame(dl, s, m.rect);
    const int glyph = Px(s, 16);
    const int x = m.rect.x + Px(s, 16) + Px(s, 20) + Px(s, 12), right = m.rect.x + m.rect.w - Px(s, 12);
    const int w = right - x;
    if (w <= 0) return;
    int y = m.rect.y + Px(s, 12);
    dl.Glyph(ChromeGlyph::Bolt, m.rect.x + Px(s, 16) + (Px(s, 20) - glyph) / 2, y + (s.Line(CF::Strong) - glyph) / 2,
             p.accent);
    dl.Text(CF::Strong, x, y, "Desktop ready", p.text, w);
    const int titleW = s.TextWidth("Desktop ready", CF::Strong) + Px(s, 6);
    dl.Text(CF::Small, x + titleW, y + s.Font(CF::Strong).ascent - s.Font(CF::Small).ascent, "boot", p.mutedText,
            w - titleW);
    y += s.Line(CF::Strong) + Px(s, 2);
    if (m.total) {
        dl.Text(CF::Hero, x, y, m.total, p.text, w);
        if (m.totalNote) {
            const int lead = s.TextWidth(m.total, CF::Hero) + Px(s, 6);
            dl.Text(CF::Label, x + lead, y + s.Font(CF::Hero).ascent - s.Font(CF::Label).ascent, m.totalNote,
                    p.secondaryText, w - lead);
        }
        y += s.Line(CF::Hero) + Px(s, 8);
    }
    if (m.phaseNames[0]) {
        uint64_t total = 0;
        for (uint64_t ms : m.phaseMs) total += ms;
        const int gap = Px(s, 2), barH = Px(s, 6);
        if (total) {
            int last = 0;
            uint64_t elapsed = 0;
            for (unsigned phase = 0; phase < 4; ++phase) {
                elapsed += m.phaseMs[phase];
                const int edge = static_cast<int>(elapsed * static_cast<uint64_t>(w) / total);
                const int width = edge - last - (phase < 3 ? gap : 0);
                if (width > 0) dl.MaterialRect({x + last, y, width, barH}, 0xff000000u | BootPhaseInk[phase], Px(s, 2));
                last = edge;
            }
        } else dl.MaterialRect({x, y, w, barH}, WellColor(s, p.launcher), Px(s, 2));
        y += barH + Px(s, 8);
        // Legend: four columns, a 6 px swatch and the phase name over its time.
        const int swatch = Px(s, 6), lead = swatch + Px(s, 5);
        for (unsigned phase = 0; phase < 4; ++phase) {
            if (!m.phaseNames[phase]) continue;
            const int column = x + static_cast<int>(phase) * w / 4, columnW = w / 4 - lead;
            dl.MaterialRect({column, y + (s.Line(CF::Small) - swatch) / 2, swatch, swatch},
                            0xff000000u | BootPhaseInk[phase], Px(s, 2));
            dl.Text(CF::Small, column + lead, y, m.phaseNames[phase], p.secondaryText, columnW);
            if (m.phaseValues[phase])
                dl.Text(CF::Label, column + lead, y + s.Line(CF::Small), m.phaseValues[phase], p.text, columnW);
        }
        y += s.Line(CF::Small) + s.Line(CF::Label) + Px(s, 8);
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!m.meta[i]) continue;
        dl.Text(i == 2 ? CF::Mono : CF::Small, x, y, m.meta[i], p.mutedText, w);
        y += s.Line(CF::Small);
    }
}

// ---------------------------------------------------------------------------
// Tray flyouts
// ---------------------------------------------------------------------------

static constexpr int FlyoutPad = 16, FlyoutGap = 12, FlyoutGraph = 48;

int ChromeTrayFlyoutHeight(const ChromeStyle& s, bool network, bool live)
{
    const int row = s.Line(CF::Label), rowGap = Px(s, 5);
    int h = 2 * Px(s, FlyoutPad) + PanelHeaderHeight(s) + Px(s, FlyoutGap);
    if (!live) return h + row;
    if (network) return h + 3 * row + 2 * rowGap;
    return h + s.Line(CF::Hero) + Px(s, FlyoutGap) + Px(s, FlyoutGraph) + Px(s, FlyoutGap) + 2 * row + rowGap;
}

void ChromeBuildTrayFlyout(const ChromeTrayFlyoutModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const DesktopRect& p = m.rect;
    if (p.w <= 0 || p.h <= 0) return;
    const auto& pal = s.P();
    PanelFrame(dl, s, p);
    const int pad = Px(s, FlyoutPad), x = p.x + pad, w = p.w - 2 * pad;
    int y = p.y + pad;
    const auto& t = m.telemetry;
    if (m.network) {
        PanelHeader(dl, s, x, y, w, m.connected ? ChromeGlyph::Network : ChromeGlyph::NetworkOff, "Network",
                    m.connected ? "Connected" : "Disconnected", m.connected ? pal.ok : pal.mutedText);
        y += PanelHeaderHeight(s) + Px(s, FlyoutGap);
        if (!m.connected) { dl.Text(CF::Label, x, y, "No network connection.", pal.secondaryText, w); return; }
        char rate[24];
        y = PanelRow(dl, s, x, y, w, "Address", m.readout ? m.readout : "");
        DesktopRate(rate, t.stats.downBytesPerSecond);
        unsigned n = 0; while (rate[n]) ++n; CopyText(rate + n, " MB/s", sizeof(rate) - n);
        y = PanelRow(dl, s, x, y, w, "Download", rate);
        DesktopRate(rate, t.stats.upBytesPerSecond);
        n = 0; while (rate[n]) ++n; CopyText(rate + n, " MB/s", sizeof(rate) - n);
        PanelRow(dl, s, x, y, w, "Upload", rate);
        return;
    }
    const bool live = t.thermal && t.latest.pkgC >= 0;
    const ThermalSample& now = t.pointCount && t.points ? t.points[t.pointCount - 1] : t.latest;
    PanelHeader(dl, s, x, y, w, ChromeGlyph::Temperature, "Temperature",
                live ? ThermalWord(now.pkgC, t.tjmax) : "Unavailable",
                live ? ThermalInk(s, now.pkgC, t.tjmax, pal.ok) : pal.mutedText);
    y += PanelHeaderHeight(s) + Px(s, FlyoutGap);
    if (!live) { dl.Text(CF::Label, x, y, "No temperature sensor reported.", pal.secondaryText, w); return; }
    char value[24];
    ThermalText(value, now.pkgC, true);
    dl.Text(CF::Hero, x, y, value, pal.text, w);
    if (t.rapl) {
        char power[24];
        PowerText(power, now.pkgMw);
        dl.Text(CF::Body, x + s.TextWidth(value, CF::Hero) + Px(s, 8), y + s.Font(CF::Hero).ascent - s.Font(CF::Body).ascent,
                power, pal.secondaryText);
    }
    y += s.Line(CF::Hero) + Px(s, FlyoutGap);
    ThermalBars(dl, s, GraphBox(dl, s, {x, y, w, Px(s, FlyoutGraph)}), t);
    y += Px(s, FlyoutGraph) + Px(s, FlyoutGap);
    y = PanelRow(dl, s, x, y, w, "Sensor", "CPU package");
    char limit[24];
    ThermalText(limit, static_cast<int32_t>(t.tjmax ? t.tjmax : 100), true);
    PanelRow(dl, s, x, y, w, "Throttles at", limit);
}

// ---------------------------------------------------------------------------
// Performance HUD
// ---------------------------------------------------------------------------

static constexpr int HudPad = 12, HudGap = 8, HudGraph = 44;

// Compact: APP and COMP in the taskbar, each a caption label, a 16-bar graph
// whose top edge is the 16.7 ms budget, and the newest interval.
void ChromeBuildCompactHud(const ChromePerformanceHudModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const auto& p = s.P();
    if (m.rect.w <= 0) return;
    const int small = s.Line(CF::Small), label = s.Line(CF::Label), graphH = Px(s, 16), graphW = CompactHudBars * Px(s, 3);
    const int labelY = m.rect.y + (m.rect.h - small) / 2, valueY = m.rect.y + (m.rect.h - label) / 2;
    const int valueW = s.TextWidth("000", CF::Label);
    int x = m.rect.x + Px(s, 8);
    for (unsigned series = 0; series < 2; ++series) {
        const char* name = series ? "COMP" : "APP";
        const auto* h = series ? m.telemetry.compositor : m.telemetry.app;
        dl.Text(CF::Small, x, labelY, name, p.mutedText);
        x += s.TextWidth(name, CF::Small) + Px(s, 6);
        if (h) CadenceBars(dl, s, {x, m.rect.y + (m.rect.h - graphH) / 2, graphW, graphH}, *h, m.tick, 17,
                           series ? p.ok : p.accent);
        x += graphW + Px(s, 6);
        char value[12] = "-";
        if (h) CadenceLatest(*h, m.tick, value);
        TextRight(dl, s, CF::Label, x + valueW, valueY, value, p.text);
        x += valueW + (series ? Px(s, 4) : Px(s, 12));
    }
    dl.Text(CF::Small, x, labelY, "ms", p.mutedText);
}

int ChromePerformanceHudHeight(const ChromeStyle& s, bool thermal)
{
    const int group = s.Line(CF::Body) + Px(s, 4) + Px(s, HudGraph);
    return 2 * Px(s, HudPad) + s.Line(CF::Strong) + 3 * Px(s, HudGap) + 2 * group +
           (thermal ? group : s.Line(CF::Body));
}

// One HUD graph: caption label, the newest value, percentiles at the right,
// then the graph in its well.
static int HudGroup(ChromeDisplayList& dl, const ChromeStyle& s, int x, int y, int w, const char* label,
                    const char* value, const char* detail, bool graph, DesktopRect* plot)
{
    const auto& p = s.P();
    const int row = s.Line(CF::Body), ascent = s.Font(CF::Body).ascent;
    const int labelW = s.TextWidth("COMP", CF::Small) + Px(s, 8);
    dl.Text(CF::Small, x + Px(s, 2), y + ascent - s.Font(CF::Small).ascent, label, p.secondaryText);
    dl.Text(CF::Body, x + Px(s, 2) + labelW, y, value, p.text);
    if (detail) TextRight(dl, s, CF::Small, x + w - Px(s, 2), y + ascent - s.Font(CF::Small).ascent, detail, p.mutedText);
    y += row;
    if (!graph) return y;
    y += Px(s, 4);
    *plot = GraphBox(dl, s, {x, y, w, Px(s, HudGraph)});
    return y + Px(s, HudGraph);
}

static void HudCadence(ChromeDisplayList& dl, const ChromeStyle& s, int x, int& y, int w,
                       const DesktopIntervals<>* history, uint64_t tick, const char* label, uint32_t ink)
{
    char value[24] = "-", detail[48]{};
    if (history) {
        char number[12];
        if (CadenceLatest(*history, tick, number)) { CopyText(value, number, sizeof(value)); unsigned n = 0; while (value[n]) ++n; CopyText(value + n, " ms", sizeof(value) - n); }
        const auto summary = history->Read(tick);
        if (summary.count) {
            unsigned used = 0;
            auto append = [&](const char* v) { while (*v && used + 1 < sizeof(detail)) detail[used++] = *v++; detail[used] = 0; };
            append("p80 "); DesktopNumber(number, summary.p80); append(number);
            append(" " BROOK_CHROME_DOT " p95 "); DesktopNumber(number, summary.p95); append(number);
            append(" " BROOK_CHROME_DOT " max "); DesktopNumber(number, summary.max); append(number);
        } else CopyText(detail, "no recent frames", sizeof(detail));
    }
    DesktopRect plot{};
    y = HudGroup(dl, s, x, y, w, label, value, detail, true, &plot);
    // The 16.7 ms budget on a 24 ms scale.
    dl.Fill(plot.x, plot.y + plot.h - plot.h * 167 / 240, plot.w, 1, HairlineColor(s, s.P().launcher));
    if (history) CadenceBars(dl, s, plot, *history, tick, 24, ink);
}

void ChromeBuildPerformanceHud(const ChromePerformanceHudModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    if (m.rect.w <= 0 || m.rect.h <= 0) return;
    const auto& p = s.P();
    const int pad = Px(s, HudPad), x = m.rect.x + pad, w = m.rect.w - 2 * pad;
    int y = m.rect.y + pad;
    PanelFrame(dl, s, m.rect);
    dl.Text(CF::Strong, x + Px(s, 4), y, "Performance", p.text, w);
    TextRight(dl, s, CF::Small, x + w - Px(s, 2), y + s.Font(CF::Strong).ascent - s.Font(CF::Small).ascent,
              "16.7 ms budget", p.mutedText);
    y += s.Line(CF::Strong) + Px(s, HudGap);
    const auto& t = m.telemetry;
    HudCadence(dl, s, x, y, w, t.app, m.tick, "APP", p.accent);
    y += Px(s, HudGap);
    HudCadence(dl, s, x, y, w, t.compositor, m.tick, "COMP", p.ok);
    y += Px(s, HudGap);
    const bool live = t.thermal && t.latest.pkgC >= 0;
    char value[24] = "n/a", power[32]{};
    DesktopRect plot{};
    if (live) {
        const ThermalSample& now = t.pointCount && t.points ? t.points[t.pointCount - 1] : t.latest;
        ThermalText(value, now.pkgC, true);
        if (t.rapl) { PowerText(power, now.pkgMw); unsigned n = 0; while (power[n]) ++n; CopyText(power + n, " package", sizeof(power) - n); }
    }
    HudGroup(dl, s, x, y, w, "TEMP", value, power[0] ? power : nullptr, live, &plot);
    if (live) ThermalBars(dl, s, plot, t);
}

} // namespace brook
