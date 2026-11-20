// Shell-surface layouts and display list builders: taskbar, launcher, tray
// flyouts, boot toast and performance HUD, in the Clearwater type scale and
// geometry (DESIGN.md "Token sheet"; compositor refactor stages 1.2 and 2).
// Everything here is drawn once into a cached surface when its content key
// changes; nothing runs per frame. Freestanding.
#include "chrome_shell.h"
#include "chrome_launcher.h"

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

// The CPU and MEM bar: wide enough for "100%" inside it, at every scale.
static int ChromeLoadBarWidth(const ChromeStyle& s)
{
    return s.TextWidth("100%", CF::Small) + 2 * Px(s, ChromeTaskbarSpacing::Pad);
}

ChromeTaskbarLayout ChromeTaskbarLayoutFor(const ChromeStyle& s, uint32_t screenW, uint32_t screenH,
                                           const char* clockText, bool agent, uint32_t enabledMetrics,
                                           const int* ids, int total, int focused, const char* buildText,
                                           bool compactHud, const char* const* labels, uint32_t trayAppCount)
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
    using SP = ChromeTaskbarSpacing;
    const int gap = Px(s, SP::Gap), pad = Px(s, SP::Pad), trayTarget = Px(s, 44);
    // Clock: the wider of its two lines with the pad either side (a 44 px
    // target at least).
    if (l.clockText[0]) {
        const int time = s.TextWidth(l.clockText, CF::Label), date = s.TextWidth(l.clockDate, CF::Small);
        const int width = 2 * pad + (time > date ? time : date);
        reserve(l.clock, width > trayTarget ? width : trayTarget);
    }
    // Tray, as the prototype orders it: network, volume, temperature.
    if (enabledMetrics & (1u << 6)) {
        const int width = 2 * pad + Px(s, 16) + Px(s, 4) + s.TextWidth("100" BROOK_CHROME_DEGREE, CF::Label);
        reserve(l.metrics[6], width > trayTarget ? width : trayTarget);
    }
    reserve(l.volume, trayTarget);
    reserve(l.network, trayTarget);
    // Leftmost fixed icon, so its appearance never moves the tray targets.
    if (agent) reserve(l.agent, Px(s, 16) + Px(s, 12));
    while (l.trayAppCount < trayAppCount && l.trayAppCount < TrayItemLimit &&
           reserve(l.trayApps[l.trayAppCount], Px(s, 36)))
        ++l.trayAppCount;
    {
        DesktopRect separator{};
        if (reserve(separator, 2 * sepGap + 1))
            l.separators[1] = {separator.x + sepGap, by + (bh - sepH) / 2, 1, sepH};
    }
    if (buildText && *buildText) {
        // The build or payload name (e.g. "dell-int51-20261119"), in the
        // small monospace face; on a tight tray without its trailing
        // "-YYYYMMDD" date, then without a "+dirty" suffix. The tooltip has
        // the full detail.
        unsigned length = 0;
        while (buildText[length] && length + 1 < sizeof(l.buildText)) ++length;
        auto reserveBuild = [&](unsigned count) {
            if (!count) return false;
            char text[sizeof(l.buildText)];
            for (unsigned i = 0; i < count; ++i) text[i] = buildText[i];
            text[count] = 0;
            const int width = s.TextWidth(text, CF::Mono) + 2 * pad;
            if (width > Px(s, 188) || !reserve(l.build, width, gap)) return false;
            CopyText(l.buildText, text, sizeof(l.buildText));
            return true;
        };
        unsigned dated = length, plus = 0;
        if (length > 9 && buildText[length - 9] == '-') {
            bool digits = true;
            for (unsigned i = length - 8; i < length; ++i) digits &= buildText[i] >= '0' && buildText[i] <= '9';
            if (digits) dated = length - 9;
        }
        for (unsigned i = 0; i < length && !plus; ++i) if (buildText[i] == '+') plus = i;
        if (!reserveBuild(length) && !(dated != length && reserveBuild(dated))) reserveBuild(plus);
    }
    if (compactHud) reserve(l.hud, CompactHudWidth(s));
    // Diagnostics, right to left: APP and COMP (label and graph over p95),
    // disk R/W, then CPU over MEM in one chip. Each is sized to its widest
    // value; network rates live in the network flyout, not here.
    const int row = s.Line(CF::Small) + Px(s, SP::RowGap), pairTop = by + (bh - 2 * row) / 2;
    const int cadence = [&] {
        const int top = s.TextWidth("COMP", CF::Small) + pad + SP::GraphBars * Px(s, 3);
        const int bottom = s.TextWidth("p95 999 ms", CF::Small);
        return top > bottom ? top : bottom;
    }();
    const int disk = s.TextWidth("W 999.9M", CF::Small);
    const int load = s.TextWidth("MEM", CF::Small) + pad + ChromeLoadBarWidth(s);
    auto metric = [&](int index, int width) {
        const int before = right;
        right -= gap;
        if (!reserve(l.metrics[index], width)) { right = before; return false; }
        l.metrics[index].y = l.y + Px(s, 2);
        l.metrics[index].h = height - 2 * Px(s, 2);
        return true;
    };
    if (enabledMetrics & (1u << 5)) metric(5, cadence);
    if (enabledMetrics & (1u << 4)) metric(4, cadence);
    if (enabledMetrics & (1u << 2)) metric(2, disk);
    const bool cpu = enabledMetrics & 1u, mem = enabledMetrics & 2u;
    if ((cpu || mem) && metric(cpu ? 0 : 1, load) && cpu && mem) {
        // Stacked: CPU's row over MEM's, one chip wide.
        DesktopRect& top = l.metrics[0];
        l.metrics[1] = {top.x, pairTop + row, top.w, row};
        top = {top.x, pairTop, top.w, row};
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

// Taskbar diagnostics (Settings > Taskbar), condensed (Ian, 19 November):
// CPU over MEM as label and a bar with the percentage inside it, disk R/W as
// two short rates, and the cadence histories as label and graph over p95.
// Network rates are in the network flyout.
static void DesktopMetric(ChromeDisplayList& dl, const ChromeStyle& s, const ChromeTelemetryModel& t,
                          const DesktopRect& r, unsigned kind, uint64_t now)
{
    if (r.w <= 0) return;
    using SP = ChromeTaskbarSpacing;
    const auto& p = s.P(); const int line = s.Line(CF::Small);
    const int row = line + Px(s, SP::RowGap);
    const int top = r.y + (r.h - 2 * row) / 2 + Px(s, SP::RowGap) / 2, bottom = top + row;
    auto text = [&](int x, int y, const char* v, uint32_t color) { dl.Text(CF::Small, x, y, v, color, r.x + r.w - x); };
    if (kind < 2) {
        unsigned percent = kind ? t.stats.memoryPercent : t.stats.cpuPercent; if (percent > 100) percent = 100;
        const uint32_t color = percent >= 90 ? p.hot : percent >= 70 ? p.warm : p.ok;
        const int y = r.y + (r.h - line) / 2;
        dl.Text(CF::Small, r.x, y, kind ? "MEM" : "CPU", p.mutedText);
        // The bar, its fill and the value inside it: an ink that contrasts
        // with the fill over the fill, the tray ink over the well, each clipped to its part so the digits
        // read over both in every theme.
        const int barW = ChromeLoadBarWidth(s), barH = line, barX = r.x + r.w - barW, barY = y;
        dl.MaterialRect({barX, barY, barW, barH}, WellColor(s, p.taskbar), Px(s, 3));
        const int fill = barW * static_cast<int>(percent) / 100;
        if (fill > 0) dl.MaterialRect({barX, barY, fill < Px(s, 6) ? Px(s, 6) : fill, barH}, color | 0xff000000u, Px(s, 3));
        char value[12]; DesktopNumber(value, percent);
        unsigned n = 0; while (value[n]) ++n; value[n] = '%'; value[n + 1] = 0;
        const int vx = barX + (barW - s.TextWidth(value, CF::Small)) / 2;
        const DesktopRect filled{barX, barY, fill, barH}, empty{barX + fill, barY, barW - fill, barH};
        // Light theme levels are dark fills, dark theme levels light ones.
        const unsigned luma = (((color >> 16) & 255) * 3 + ((color >> 8) & 255) * 6 + (color & 255)) / 10;
        if (filled.w > 0) dl.Text(vx, barY, value, luma > 128 ? 0x0b1a2bu : 0xffffffu, 0, &filled, CF::Small);
        if (empty.w > 0) dl.Text(vx, barY, value, p.taskText, 0, &empty, CF::Small);
        return;
    }
    if (kind == 2) {
        char first[20], second[20], number[12];
        CopyText(first, "R ", sizeof(first));
        DesktopRate(number, t.stats.readBytesPerSecond);
        CopyText(first + 2, number, sizeof(first) - 2);
        unsigned n = 0; while (first[n]) ++n; CopyText(first + n, "M", sizeof(first) - n);
        CopyText(second, "W ", sizeof(second));
        DesktopRate(number, t.stats.writeBytesPerSecond);
        CopyText(second + 2, number, sizeof(second) - 2);
        n = 0; while (second[n]) ++n; CopyText(second + n, "M", sizeof(second) - n);
        text(r.x, top, first, p.secondaryText); text(r.x, bottom, second, p.secondaryText); return;
    }
    if (kind != 4 && kind != 5) return;
    const DesktopIntervals<>* source = kind == 4 ? t.compositor : t.app;
    if (!source) return;
    const auto& history = *source;
    const auto summary = history.Read(now);
    text(r.x, top, kind == 4 ? "COMP" : "APP", p.mutedText);
    const int graphW = SP::GraphBars * Px(s, 3), graphX = r.x + r.w - graphW;
    CadenceBars(dl, s, {graphX, top + Px(s, 1), graphW, line - Px(s, 2)}, history, now, 100, p.accent);
    if (!summary.count || (kind == 5 && (!history.last || now - history.last > 2000))) {
        text(r.x, bottom, "idle", p.mutedText); return;
    }
    char value[32]{}, number[12];
    CopyText(value, "p95 ", sizeof(value));
    DesktopNumber(number, summary.p95 > 999 ? 999 : summary.p95);
    unsigned n = 0; while (value[n]) ++n; CopyText(value + n, number, sizeof(value) - n);
    n = 0; while (value[n]) ++n; CopyText(value + n, " ms", sizeof(value) - n);
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
    if (m.tray) for (uint32_t i = 0; i < l.trayAppCount; ++i) {
        const auto& item = m.tray->items[i];
        const auto& r = l.trayApps[i];
        target(r, item.attention);
        if (item.hasIcon) centeredIcon(r, item.icon, Px(s, 20));
        else {
            char initial[2] = {item.title[0] ? item.title[0] : '?', 0};
            const int x = r.x + (r.w - s.TextWidth(initial, CF::Strong)) / 2;
            dl.Text(CF::Strong, x, r.y + (r.h - s.Line(CF::Strong)) / 2, initial,
                    p.taskText, r.w);
        }
    }
    const int labelY = l.textY;
    for (int i = 0; i < l.count; ++i) {
        const DesktopRect& r = l.buttons[i];
        const ChromeTaskbarButton& b = m.buttons[i];
        const bool hovered = target(r, b.active);
        const auto appIcon = [&](int x, unsigned opacity) {
            const int y = r.y + (r.h - icon) / 2;
            if (!b.icon && b.initial) {
                const char initial[] = {b.initial, 0};
                ChromeLauncherInitial(dl, s, x, y, icon, b.color, initial);
            } else IconAt(dl, s, x, y, icon, b.icon, opacity);
        };
        if (b.launch) {
            if (r.w >= icon && r.h >= icon) appIcon(r.x + (r.w - icon) / 2, 255);
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
            appIcon(ix, opacity);
            ChromeElidedText(dl, s, CF::Label, tx, labelY, b.hoverTitle,
                             b.active || hovered ? p.text : b.minimized ? p.mutedText : p.secondaryText,
                             r.x + r.w - tx - Px(s, SM::TaskbarButtonPad));
        } else if (r.w >= icon && r.h >= icon)
            appIcon(r.x + (r.w - icon) / 2, opacity);
    }
    if (l.next.w > 0) {
        target(l.next, false);
        const int size = Px(s, 16);
        if (size <= l.next.w)
            dl.Glyph(ChromeGlyph::ChevronRight, l.next.x + (l.next.w - size) / 2, l.next.y + (l.next.h - size) / 2, p.taskText);
    }
    if (l.build.w > 0)
        dl.Text(CF::Mono, l.build.x + Px(s, ChromeTaskbarSpacing::Pad), l.build.y + (l.build.h - s.Line(CF::Mono)) / 2,
                l.buildText, p.badgeText, l.build.w - Px(s, ChromeTaskbarSpacing::Pad));
    if (l.clock.w > 0) {
        // Time over date, right-aligned a pad from the target's edge.
        const int timeH = s.Line(CF::Label), dateH = s.Line(CF::Small);
        const int right = l.clock.x + l.clock.w - Px(s, ChromeTaskbarSpacing::Pad);
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
        const int glyph = Px(s, 16), space = Px(s, 4), width = glyph + space + s.TextWidth(value, CF::Label);
        const int x = r.x + (r.w - width) / 2;
        dl.Glyph(ChromeGlyph::Temperature, x, r.y + (r.h - glyph) / 2, ink);
        dl.Text(CF::Label, x + glyph + space, labelY, value, ink, r.x + r.w - x - glyph - space);
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
// Side dock
// ---------------------------------------------------------------------------

using DM = ChromeDockMetrics;

static int DockClampPixels(const DesktopMetrics& m, uint32_t screenW, int width)
{
    const int minimum = m.pixels(DM::MinWidth), half = static_cast<int>(screenW / 2);
    const int maximum = m.pixels(DM::MaxWidth) < half ? m.pixels(DM::MaxWidth) : half;
    if (width < minimum) width = minimum;
    if (width > maximum) width = maximum;
    return width;
}

DesktopRect ChromeDockSidebar(const DesktopMetrics& m, uint32_t screenW, uint32_t desktopH,
                              const ChromeDockPlacement& place)
{
    const int width = DockClampPixels(m, screenW, m.pixels(place.width > 0 ? place.width : DM::Width));
    return {place.right ? static_cast<int>(screenW) - width : 0, 0, width, static_cast<int>(desktopH)};
}

int ChromeDockWidthFor(const DesktopMetrics& m, uint32_t screenW, int px)
{
    const int width = DockClampPixels(m, screenW, px);
    const int percent = m.percent ? static_cast<int>(m.percent) : 100;
    return (width * 100 + percent / 2) / percent;
}

ChromeDockLayout ChromeDockLayoutFor(const DesktopMetrics& m, uint32_t screenW, uint32_t desktopH,
                                     int entries, int selected, bool open, const ChromeDockPlacement& place)
{
    ChromeDockLayout l{};
    l.selected = -1;
    l.right = place.right;
    l.sidebar = ChromeDockSidebar(m, screenW, desktopH, place);
    if (entries <= 0 || l.sidebar.w <= 0 || l.sidebar.h <= 0) return l;
    if (entries > static_cast<int>(ChromeDockEntryLimit)) entries = ChromeDockEntryLimit;
    const int stripH = m.pixels(DM::Strip) < l.sidebar.h ? m.pixels(DM::Strip) : l.sidebar.h;
    if (!open) {
        // The 5 px tab, vertically centred on the sidebar's screen edge.
        const int h = m.pixels(DM::TabHeight) < l.sidebar.h ? m.pixels(DM::TabHeight) : l.sidebar.h;
        const int w = m.pixels(DM::Tab);
        l.tab = {place.right ? l.sidebar.x + l.sidebar.w - w : l.sidebar.x, l.sidebar.y + (l.sidebar.h - h) / 2, w, h};
        l.count = entries;
        return l;
    }
    l.open = true;
    l.strip = {l.sidebar.x, l.sidebar.y, l.sidebar.w, stripH};
    l.page = {l.sidebar.x, l.sidebar.y + stripH, l.sidebar.w, l.sidebar.h - stripH};
    // The resize grip straddles the inner edge, the full height of the sidebar.
    const int grip = m.pixels(DM::Grip), inner = place.right ? l.sidebar.x : l.sidebar.x + l.sidebar.w;
    l.grip = {inner - grip / 2, l.sidebar.y, grip, l.sidebar.h};
    const int pinW = m.pixels(DM::Pin), cell = m.pixels(DM::Cell), button = m.pixels(DM::TileButton);
    l.pin = {l.strip.x + l.strip.w - pinW, l.strip.y, pinW, stripH};
    l.side = {l.pin.x - pinW, l.strip.y, pinW, stripH};
    if (selected < 0 || selected >= entries) selected = -1;
    // The selected tile keeps an icon cell and both buttons. Entries that
    // leave it less are not laid out (overflow is a later slice).
    const int room = l.strip.w - 2 * pinW, tileMin = selected >= 0 ? cell + 2 * button : 0;
    int fit = selected >= 0 ? 1 + (room - tileMin) / cell : room / cell;
    if (fit < 0) fit = 0;
    l.count = entries < fit ? entries : fit;
    if (selected >= l.count) selected = l.count - 1;
    l.selected = selected;
    const int tileW = room - (selected >= 0 ? (l.count - 1) * cell : 0);
    int x = l.strip.x;
    for (int i = 0; i < l.count; ++i) {
        const int w = i == selected ? tileW : cell;
        l.cells[i] = {x, l.strip.y, w, stripH};
        x += w;
    }
    if (selected >= 0) {
        l.tile = l.cells[selected];
        l.close = {l.tile.x + l.tile.w - button, l.tile.y, button, stripH};
        l.back = {l.close.x - button, l.tile.y, button, stripH};
        const int titleX = l.tile.x + cell;
        l.title = {titleX, l.tile.y, l.back.x - titleX, stripH};
    }
    return l;
}

ChromeDockHit ChromeDockHitTest(const ChromeDockLayout& l, int32_t x, int32_t y)
{
    if (!l.open) return {l.tab.contains(x, y) ? ChromeDockTarget::Tab : ChromeDockTarget::None, -1};
    if (l.grip.contains(x, y)) return {ChromeDockTarget::Grip, -1};
    if (!l.strip.contains(x, y)) return {ChromeDockTarget::None, -1};
    if (l.pin.contains(x, y)) return {ChromeDockTarget::Pin, -1};
    if (l.side.contains(x, y)) return {ChromeDockTarget::Side, -1};
    if (l.selected >= 0) {
        if (l.close.contains(x, y)) return {ChromeDockTarget::Close, l.selected};
        if (l.back.contains(x, y)) return {ChromeDockTarget::Return, l.selected};
        if (l.tile.contains(x, y)) return {ChromeDockTarget::Tile, l.selected};
    }
    for (int i = 0; i < l.count; ++i)
        if (l.cells[i].contains(x, y)) return {ChromeDockTarget::Entry, i};
    return {ChromeDockTarget::Strip, -1};
}

// "Return to desktop": a bar at the sidebar's edge and an arrow leaving it
// (mirrored for a right-edge sidebar).
static void DockReturnGlyph(ChromeDisplayList& dl, const ChromeStyle& s, int x0, int y0, int size, uint32_t ink,
                            bool mirror)
{
    auto px = [&](int v) { return v * size / 16; };
    auto box = [&](int x, int y, int w, int h) {
        if (mirror) x = 16 - x - w;
        dl.Fill(x0 + px(x), y0 + px(y), px(x + w) - px(x), px(y + h) - px(y), 0xff000000u | ink);
    };
    box(2, 3, 2, 10);  // the edge being left
    box(6, 7, 8, 2);   // shaft
    for (int k = 0; k < 4; ++k) { // head: two diagonals meeting at the tip
        box(9 + k, 3 + k, 2, 1);
        box(9 + k, 12 - k, 2, 1);
    }
    (void)s;
}

void ChromeBuildDock(const ChromeDockModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const ChromeDockLayout& l = *m.layout;
    const auto& p = s.P();
    if (!l.open) {
        // At rest: a flat quad with a short grip, no glass.
        const DesktopRect& t = l.tab;
        if (t.w <= 0 || t.h <= 0) return;
        dl.Fill(t.x, t.y, t.w, t.h, 0xff000000u | p.taskbar);
        const int grip = Px(s, 2) < t.w ? Px(s, 2) : t.w, gripH = Px(s, 32) < t.h ? Px(s, 32) : t.h;
        dl.Fill(t.x + (t.w - grip) / 2, t.y + (t.h - gripH) / 2, grip, gripH, 0xff000000u | p.mutedText);
        return;
    }
    const DesktopRect& r = l.strip;
    if (r.w <= 0 || r.h <= 0) return;
    // The taskbar's material: the same glass gradient at the same height.
    if (s.glass && s.material && s.material->taskbarHeight >= static_cast<unsigned>(r.h))
        dl.Rows(r.x, r.y, r.w, r.h, s.material->taskbarRows);
    else dl.Fill(r.x, r.y, r.w, r.h, s.MaterialColor(p.taskbar));
    const uint32_t edge = s.glass ? HairlineColor(s, p.taskbar) : 0xff000000u | p.launcherBorder;
    dl.Fill(r.x, r.y + r.h - 1, r.w, 1, edge);
    // The inner edge: a hairline, the accent while the grip is hovered or dragged.
    const int innerX = l.right ? r.x : r.x + r.w - 1;
    if (m.resizing || l.grip.contains(m.mouseX, m.mouseY))
        dl.Fill(l.right ? r.x : r.x + r.w - Px(s, 2), r.y, Px(s, 2), r.h, 0xff000000u | p.accent);
    else dl.Fill(innerX, r.y, 1, r.h - 1, edge);
    auto pill = [&](const DesktopRect& c) {
        const int ix = c.w > 4 * Px(s, SM::TaskbarPillInsetX) ? Px(s, SM::TaskbarPillInsetX) : 0;
        const int iy = c.h > 4 * Px(s, SM::TaskbarPillInsetY) ? Px(s, SM::TaskbarPillInsetY) : 0;
        return DesktopRect{c.x + ix, c.y + iy, c.w - 2 * ix, c.h - 2 * iy};
    };
    auto hovered = [&](const DesktopRect& c) { return c.contains(m.mouseX, m.mouseY); };
    const int icon = Px(s, DM::Icon), cell = Px(s, DM::Cell), glyph = Px(s, 16);
    for (int i = 0; i < l.count; ++i) {
        const DesktopRect& c = l.cells[i];
        const uint32_t* pixels = m.icons ? m.icons[i] : nullptr;
        if (i == l.selected) continue;
        if (hovered(c)) dl.MaterialRect(pill(c), s.MaterialColor(p.taskHover), Px(s, 6));
        if (c.w >= icon && c.h >= icon)
            ChromeIcon(dl, s, c.x + (c.w - icon) / 2, c.y + (c.h - icon) / 2, icon, pixels);
        // Docked and running: a 4x3 dot under the icon.
        const int dotW = Px(s, 4), dotH = Px(s, 3);
        dl.MaterialRect({c.x + (c.w - dotW) / 2, c.y + c.h - Px(s, 7) - dotH, dotW, dotH},
                        0xff000000u | p.mutedText, Px(s, 1));
    }
    if (l.selected >= 0) {
        // The selected tile: the lighter fill is the only selection mark.
        const DesktopRect& t = l.tile;
        dl.MaterialRect(pill(t), s.MaterialColor(p.taskActive), Px(s, 6));
        const uint32_t* pixels = m.icons ? m.icons[l.selected] : nullptr;
        const int iconCell = cell < t.w ? cell : t.w;
        if (iconCell >= icon) ChromeIcon(dl, s, t.x + (iconCell - icon) / 2, t.y + (t.h - icon) / 2, icon, pixels);
        if (m.title && m.title[0] && l.title.w > Px(s, 8))
            ChromeElidedText(dl, s, CF::Label, l.title.x, t.y + (t.h - s.Line(CF::Label)) / 2, m.title, p.text,
                             l.title.w - Px(s, 4));
        const DesktopRect* buttons[2] = {&l.back, &l.close};
        for (const DesktopRect* b : buttons) {
            if (b->w <= 0) continue;
            const bool hover = hovered(*b);
            if (hover) {
                const int inset = Px(s, 8);
                dl.MaterialRect({b->x + Px(s, 2), b->y + inset, b->w - Px(s, 4), b->h - 2 * inset},
                                s.MaterialColor(p.taskHover), Px(s, 4));
            }
            const uint32_t ink = hover ? p.text : p.secondaryText;
            const int gx = b->x + (b->w - glyph) / 2, gy = b->y + (b->h - glyph) / 2;
            if (b == &l.close) dl.Glyph(ChromeGlyph::Close, gx, gy, ink);
            else DockReturnGlyph(dl, s, gx, gy, glyph, ink, l.right);
        }
    }
    // Pin: the lighter fill while pinned, a hover pill otherwise.
    if (m.pinned || hovered(l.pin))
        dl.MaterialRect(pill(l.pin), s.MaterialColor(m.pinned ? p.taskActive : p.taskHover), Px(s, 6));
    dl.Glyph(ChromeGlyph::Pin, l.pin.x + (l.pin.w - glyph) / 2, l.pin.y + (l.pin.h - glyph) / 2,
             m.pinned || hovered(l.pin) ? p.text : p.secondaryText);
    // Side: move the sidebar to the other screen edge.
    if (l.side.w > 0) {
        if (hovered(l.side)) dl.MaterialRect(pill(l.side), s.MaterialColor(p.taskHover), Px(s, 6));
        dl.Glyph(ChromeGlyph::Swap, l.side.x + (l.side.w - glyph) / 2, l.side.y + (l.side.h - glyph) / 2,
                 hovered(l.side) ? p.text : p.secondaryText);
    }
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

void ChromeBuildAppTrayMenu(const ChromeTrayState& state, DesktopRect rect, int32_t mx, int32_t my,
                            const ChromeStyle& s, ChromeDisplayList& dl)
{
    PanelFrame(dl, s, rect);
    for (uint32_t i = 0; i < state.menuCount; ++i) {
        const auto& row = state.menu[i];
        const auto r = ChromeTrayMenuRowRect(s, rect, i);
        const int pad = Px(s, 8);
        if (row.separator) {
            dl.Fill(r.x + pad, r.y + r.h / 2, r.w - 2 * pad, 1, HairlineColor(s, s.P().launcher));
            continue;
        }
        if (row.enabled && r.contains(mx, my))
            dl.MaterialRect(r, s.MaterialColor(s.P().taskHover), Px(s, 6));
        ChromeElidedText(dl, s, CF::Label, r.x + pad, r.y + (r.h - s.Line(CF::Label)) / 2, row.label,
                         row.enabled ? s.P().text : s.P().mutedText, r.w - 2 * pad - Px(s, 16));
        if (row.submenu)
            dl.Glyph(ChromeGlyph::ChevronRight, r.x + r.w - Px(s, 20), r.y + (r.h - Px(s, 16)) / 2, s.P().taskText);
    }
}

void ChromeBuildNotice(const ChromeNotice& notice, DesktopRect rect, int32_t mx, int32_t my,
                       const ChromeStyle& s, ChromeDisplayList& dl)
{
    PanelFrame(dl, s, rect);
    const int x = rect.x + Px(s, 16), y = rect.y + Px(s, 12);
    const auto close = ChromeNoticeCloseRect(s, rect);
    const auto& p = s.P();
    if (close.contains(mx, my)) dl.MaterialRect(close, s.MaterialColor(p.taskHover), Px(s, 6));
    dl.Glyph(ChromeGlyph::Close, close.x + (close.w - Px(s, 16)) / 2,
             close.y + (close.h - Px(s, 16)) / 2, p.taskText);
    ChromeElidedText(dl, s, CF::Small, x, y, notice.app, p.mutedText, close.x - x);
    ChromeElidedText(dl, s, CF::Strong, x, y + s.Line(CF::Small) + Px(s, 3), notice.title, p.text, close.x - x);
    ChromeElidedText(dl, s, CF::Label, x, y + s.Line(CF::Small) + s.Line(CF::Strong) + Px(s, 8),
                     notice.body, p.secondaryText, rect.w - Px(s, 32));
    for (uint32_t i = 0; i < notice.actionCount; ++i) {
        const auto r = ChromeNoticeActionRect(s, rect, notice.actionCount, i);
        if (r.w <= 0) continue;
        dl.MaterialRect(r, s.MaterialColor(r.contains(mx, my) ? p.taskHover : p.taskActive), Px(s, 6));
        ChromeElidedText(dl, s, CF::Label, r.x + Px(s, 8), r.y + (r.h - s.Line(CF::Label)) / 2,
                         notice.actions[i], p.text, r.w - Px(s, 16));
    }
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
