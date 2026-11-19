// Clearwater launcher surface: layout, hit targets and display list. See
// chrome_launcher.h. Everything is drawn once into the cached launcher
// surface when its content key changes (a model change, a hover target, a
// theme epoch); nothing runs per frame. Freestanding.
#include "chrome_launcher.h"

namespace brook {

namespace {

using LM = ChromeLauncherMetrics;
using CF = ChromeFont;
using TK = LauncherTargetKind;

int LPx(const ChromeStyle& s, int base) { return static_cast<int>(s.M(static_cast<uint32_t>(base))); }
int Right(const DesktopRect& r) { return r.x + r.w; }
int Bottom(const DesktopRect& r) { return r.y + r.h; }

void Upper(char* out, const char* in, uint32_t capacity) {
    uint32_t i = 0;
    for (; in && in[i] && i + 1 < capacity; ++i) out[i] = in[i] >= 'a' && in[i] <= 'z' ? char(in[i] - 32) : in[i];
    out[i] = 0;
}
void Number(char* out, unsigned v) {
    char rev[12]; unsigned n = 0;
    do { rev[n++] = char('0' + v % 10); v /= 10; } while (v && n < sizeof(rev));
    while (n) *out++ = rev[--n];
    *out = 0;
}
void Append(char* out, uint32_t capacity, const char* text) {
    uint32_t n = 0;
    while (out[n]) ++n;
    while (*text && n + 1 < capacity) out[n++] = *text++;
    out[n] = 0;
}
int FindI(const char* hay, const char* needle) {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; };
    if (!needle[0]) return -1;
    for (int i = 0; hay[i]; ++i) {
        int j = 0;
        while (needle[j] && hay[i + j] && lower(hay[i + j]) == lower(needle[j])) ++j;
        if (!needle[j]) return i;
    }
    return -1;
}

// DESIGN.md tokens that the palette does not carry.
struct Tokens {
    uint32_t panel, ink, ink2, ink3, hover, press, hairline, well, accent, onAccent, menu;
};
Tokens TokensFor(const ChromeStyle& s) {
    const auto& p = s.P();
    Tokens t{};
    t.panel = p.launcher;
    t.ink = p.text;
    t.ink2 = p.secondaryText;
    t.ink3 = p.mutedText;
    t.hover = p.itemHover;
    // Pressed / selected: ink at 17% (dark chrome) or 14% (light), over the panel.
    t.press = ChromeBlendColor(p.text, p.launcher, p.chromeDark ? 43 : 36);
    t.hairline = ChromeBlendColor(p.text, p.launcher, p.chromeDark ? 36 : 61);
    t.well = p.chromeDark ? ChromeBlendColor(0x000000, p.launcher, 56) : ChromeBlendColor(0xffffff, p.launcher, 128);
    t.accent = p.accent;
    const unsigned luma = (((p.accent >> 16) & 255) * 3 + ((p.accent >> 8) & 255) * 6 + (p.accent & 255)) / 10;
    t.onAccent = luma > 150 ? 0x101e2e : 0xffffff;
    t.menu = p.launcher;
    return t;
}

constexpr ChromeGlyph CategoryGlyphs[] = {
    ChromeGlyph::Folder, ChromeGlyph::Sliders, ChromeGlyph::Globe, ChromeGlyph::Play,
    ChromeGlyph::Doc, ChromeGlyph::Brush, ChromeGlyph::Pad, ChromeGlyph::Spark,
};
constexpr uint32_t CategoryInk = 0x0b1a2b;

const char* TopLabels[] = {"PINNED", "RECENT"};
const char* MenuPin = "Pin";
const char* MenuUnpin = "Unpin";
const char* MenuUninstall = "Uninstall";

struct Builder {
    const LauncherModel& m;
    const ChromeLauncherChrome& c;
    const ChromeLauncherFrame& f;
    const ChromeStyle& s;
    ChromeDisplayList& dl;
    Tokens t;

    uint32_t Mat(uint32_t color) const { return s.MaterialColor(color); }
    bool Hover(const DesktopRect& r) const { return r.contains(c.mouseX, c.mouseY); }
    void Rect(const DesktopRect& r, uint32_t color, int radius) { dl.MaterialRect(r, Mat(color), LPx(s, radius)); }
    void Outline(const DesktopRect& r, uint32_t color, int radius) { dl.MaterialOutline(r, Mat(color), LPx(s, radius)); }
    const uint32_t* IconOf(uint32_t app) const { return c.icons && app < c.iconCount ? c.icons[app] : nullptr; }
    int Center(const DesktopRect& r, CF font) const { return r.y + (r.h - s.Line(font)) / 2; }

    // Category swatch tile (22 px, radius 6) with its icon in the dark glyph ink.
    void CategoryTile(int x, int y, int category) {
        const int size = LPx(s, 22), glyph = LPx(s, 16);
        uint32_t fill = t.well, ink = t.ink2;
        ChromeGlyph g = category == LauncherViewAll ? ChromeGlyph::Grid : ChromeGlyph::Inbox;
        if (category >= 0) {
            fill = ChromeLauncherCategoryColor(m.CategoryColor(uint32_t(category)));
            ink = CategoryInk;
            g = CategoryGlyphs[uint32_t(m.CategoryIcon(uint32_t(category))) % 8];
        }
        Rect({x, y, size, size}, fill, 6);
        dl.Glyph(g, x + (size - glyph) / 2, y + (size - glyph) / 2, ink);
    }

    void Panel() {
        const DesktopRect& p = f.panel;
        dl.Fill(p.x, p.y, p.w, p.h, Mat(t.panel));
        const uint32_t border = s.P().launcherBorder;
        dl.Fill(p.x, p.y, p.w, 1, border);
        dl.Fill(p.x, p.y + p.h - 1, p.w, 1, border);
        dl.Fill(p.x, p.y, 1, p.h, border);
        dl.Fill(p.x + p.w - 1, p.y, 1, p.h, border);
    }

    void Search() {
        const DesktopRect& r = f.search;
        if (r.w <= 0) return;
        Rect(r, t.well, 8);
        // Typing anywhere searches, so the field always has the focus ring.
        dl.MaterialOutline(r, 0xff000000u | t.accent, LPx(s, 8));
        const int glyph = LPx(s, 16), x = r.x + LPx(s, 12), tx = x + glyph + LPx(s, 10);
        dl.Glyph(ChromeGlyph::Search, x, r.y + (r.h - glyph) / 2, t.ink3);
        const int kbdW = s.TextWidth("Esc", CF::Small) + LPx(s, 10), kbdH = s.Line(CF::Small) + LPx(s, 2);
        const DesktopRect kbd{Right(r) - LPx(s, 12) - kbdW, r.y + (r.h - kbdH) / 2, kbdW, kbdH};
        Outline(kbd, t.hairline, 4);
        dl.Text(CF::Small, kbd.x + LPx(s, 5), kbd.y + LPx(s, 1), "Esc", t.ink3);
        const int textW = kbd.x - LPx(s, 10) - tx, y = Center(r, CF::Body);
        const char* q = m.Query();
        if (!q[0]) {
            ChromeElidedText(dl, s, CF::Body, tx, y, "Type to search apps and categories", t.ink3, textW);
            dl.Fill(tx, y, LPx(s, 1) ? LPx(s, 1) : 1, s.Line(CF::Body), 0xff000000u | t.accent);
            return;
        }
        // Keep the end of a long query (and the caret) in view.
        const char* shown = q;
        while (*shown && s.TextWidth(shown, CF::Body) + LPx(s, 2) > textW) ++shown;
        dl.Text(CF::Body, tx, y, shown, t.ink, textW);
        const int caret = tx + s.TextWidth(shown, CF::Body) + LPx(s, 1);
        if (caret < tx + textW) dl.Fill(caret, y, LPx(s, 1) ? LPx(s, 1) : 1, s.Line(CF::Body), 0xff000000u | t.accent);
    }

    void TopTab(const ChromeLauncherHit& h) {
        const bool on = uint32_t(m.Top()) == h.target.index, hover = Hover(h.rect);
        const DesktopRect face{h.rect.x, h.rect.y, h.rect.w, LPx(s, LM::Tab)};
        if (on) Rect(face, hover ? t.hover : t.press, 6);
        else if (hover) Rect(face, t.hover, 6);
        dl.Text(CF::Small, face.x + LPx(s, 10), Center(face, CF::Small), TopLabels[h.target.index & 1],
                on || hover ? t.ink : t.ink3);
    }

    void Tile(const ChromeLauncherHit& h) {
        const DesktopRect& r = h.rect;
        if (h.target.index >= f.tileCount) return;
        const uint32_t app = f.tiles[h.target.index];
        const bool kb = m.KeyboardActive() && m.Section() == LauncherSection::Tiles && m.SelectedTile() == int(h.target.index);
        const bool hover = Hover(r) && !m.MenuOpen();
        if (kb) { Rect(r, t.press, 8); Outline(r, t.hairline, 8); }
        else if (hover || (m.MenuOpen() && m.MenuApp() == int(app))) Rect(r, t.hover, 8);
        const int icon = LPx(s, LM::TileIcon);
        ChromeIcon(dl, s, r.x + (r.w - icon) / 2, r.y + LPx(s, 10), icon, IconOf(app));
        const char* name = m.App(app).name;
        const int maxW = r.w - LPx(s, 8), w = s.TextWidth(name, CF::Label);
        const int x = w < maxW ? r.x + (r.w - w) / 2 : r.x + LPx(s, 4);
        ChromeElidedText(dl, s, CF::Label, x, r.y + LPx(s, 10) + icon + LPx(s, 6), name, kb || hover ? t.ink : t.ink2, maxW);
    }

    void TilesEmpty() {
        if (f.top.w <= 0 || f.tileCount || m.Query()[0]) return;
        const DesktopRect r{f.top.x, f.top.y + LPx(s, LM::Tab) + LPx(s, 4), f.top.w, LPx(s, LM::Tile)};
        Outline(r, t.hairline, 8);
        const char* text = m.Top() == LauncherTop::Pinned ?
            "Nothing pinned yet. Right-click an app and choose Pin." : "Apps you open will appear here.";
        const int w = s.TextWidth(text, CF::Label);
        ChromeElidedText(dl, s, CF::Label, w < r.w ? r.x + (r.w - w) / 2 : r.x + LPx(s, 8), Center(r, CF::Label), text,
                         t.ink3, r.w - LPx(s, 16));
    }

    void RailRow(const ChromeLauncherHit& h) {
        const DesktopRect& r = h.rect;
        const uint32_t row = h.target.index;
        const int view = m.RailView(row);
        const bool on = !m.Query()[0] && m.View() == view, hover = Hover(r) && !m.MenuOpen();
        const bool kb = m.KeyboardActive() && m.Section() == LauncherSection::Rail && m.SelectedRail() == int(row);
        if (on) Rect(r, t.press, 6);
        else if (hover) Rect(r, t.hover, 6);
        if (kb) Outline(r, t.accent, 6);
        const int tile = LPx(s, 22), x = r.x + LPx(s, 8);
        CategoryTile(x, r.y + (r.h - tile) / 2, view);
        const char* name = view == LauncherViewAll ? "All apps" : view == LauncherViewUnsorted ? "Unsorted" : m.CategoryName(uint32_t(view));
        const uint32_t count = view == LauncherViewAll ? m.AppCount() : view == LauncherViewUnsorted ? m.UnsortedCount()
                                                                                                    : m.CategoryAppCount(uint32_t(view));
        char number[12];
        Number(number, count);
        const int numberW = s.TextWidth(number, CF::Small);
        int right = Right(r) - LPx(s, 10);
        if (view == LauncherViewUnsorted && count) {
            // Unsorted shows a count badge while it has something in it.
            const int bw = numberW + LPx(s, 14), bh = s.Line(CF::Small) + LPx(s, 2);
            const DesktopRect badge{right - bw + LPx(s, 4), r.y + (r.h - bh) / 2, bw, bh};
            dl.MaterialRect(badge, 0xff000000u | t.accent, bh / 2);
            dl.Text(CF::Small, badge.x + LPx(s, 7), badge.y + LPx(s, 1), number, t.onAccent);
            right = badge.x - LPx(s, 6);
        } else if (view != LauncherViewUnsorted) {
            dl.Text(CF::Small, right - numberW, Center(r, CF::Small), number, t.ink3);
            right -= numberW + LPx(s, 6);
        }
        const int tx = x + tile + LPx(s, 10);
        ChromeElidedText(dl, s, CF::Body, tx, Center(r, CF::Body), name, on || hover ? t.ink : t.ink2, right - tx);
    }

    // A chip: the category's dot and name on the well (search results).
    void Chip(int right, int cy, int category) {
        const char* name = category >= 0 ? m.CategoryName(uint32_t(category)) : "Unsorted";
        const int dot = LPx(s, 6), h = s.Line(CF::Small) + LPx(s, 4), pad = LPx(s, 8);
        const int w = pad + dot + LPx(s, 5) + s.TextWidth(name, CF::Small) + pad;
        const DesktopRect r{right - w, cy - h / 2, w, h};
        Rect(r, t.well, 9);
        dl.MaterialRect({r.x + pad, cy - dot / 2, dot, dot}, 0xff000000u |
                        (category >= 0 ? ChromeLauncherCategoryColor(m.CategoryColor(uint32_t(category))) : t.ink3), LPx(s, 2));
        dl.Text(CF::Small, r.x + pad + dot + LPx(s, 5), r.y + LPx(s, 2), name, t.ink2);
    }

    // The name, with the matched part in bold accent while searching.
    void Name(int x, int y, const char* name, uint32_t ink, int width) {
        const char* q = m.Query();
        const int at = q[0] ? FindI(name, q) : -1;
        if (at < 0 || s.TextWidth(name, CF::Strong) > width) { ChromeElidedText(dl, s, CF::Body, x, y, name, ink, width); return; }
        char part[LauncherLimits::NameLength];
        int i = 0;
        for (; i < at && i + 1 < int(sizeof(part)); ++i) part[i] = name[i];
        part[i] = 0;
        dl.Text(CF::Body, x, y, part, ink);
        x += s.TextWidth(part, CF::Body);
        int n = 0;
        while (q[n]) ++n;
        for (i = 0; i < n && name[at + i]; ++i) part[i] = name[at + i];
        part[i] = 0;
        dl.Text(CF::Strong, x, y, part, t.accent);
        x += s.TextWidth(part, CF::Strong);
        dl.Text(CF::Body, x, y, name + at + i, ink);
    }

    void Row(const ChromeLauncherHit& h) {
        const DesktopRect& r = h.rect;
        const uint32_t app = h.target.index;
        const bool kb = m.KeyboardActive() && m.Section() == LauncherSection::List && m.Selected() == int(app);
        const bool hover = Hover(r) && Hover(ChromeLauncherListClip(f)) && !m.MenuOpen();
        if (kb) { Rect(r, t.press, 6); Outline(r, t.hairline, 6); }
        else if (hover || (m.MenuOpen() && m.MenuApp() == int(app))) Rect(r, t.hover, 6);
        const int icon = LPx(s, LM::RowIcon), x = r.x + LPx(s, 8);
        ChromeIcon(dl, s, x, r.y + (r.h - icon) / 2, icon, IconOf(app));
        const LauncherApp& a = m.App(app);
        int right = Right(r) - LPx(s, 8);
        if (m.Query()[0]) {
            const int category = m.CategoryOf(app);
            const char* label = category >= 0 ? m.CategoryName(uint32_t(category)) : "Unsorted";
            Chip(right, r.y + r.h / 2, category);
            right -= LPx(s, 16) + LPx(s, 6) + LPx(s, 5) + s.TextWidth(label, CF::Small) + LPx(s, 8);
        }
        const int tx = x + icon + LPx(s, 10), width = right - tx;
        if (a.desc[0]) {
            const int nameH = s.Line(CF::Body), descH = s.Line(CF::Small), top = r.y + (r.h - nameH - descH) / 2;
            Name(tx, top, a.name, t.ink, width);
            ChromeElidedText(dl, s, CF::Small, tx, top + nameH, a.desc, t.ink3, width);
        } else Name(tx, Center(r, CF::Body), a.name, t.ink, width);
    }

    void Heading(const LauncherLine& line, const DesktopRect& r) {
        int x = r.x + LPx(s, 8);
        const int dot = LPx(s, 8);
        char text[40];
        if (line.category >= 0 || line.category == LauncherViewUnsorted) {
            const uint32_t color = line.category >= 0 ? ChromeLauncherCategoryColor(m.CategoryColor(uint32_t(line.category))) : t.ink3;
            dl.MaterialRect({x, r.y + (r.h - dot) / 2, dot, dot}, 0xff000000u | color, LPx(s, 3));
            x += dot + LPx(s, 8);
            Upper(text, line.category >= 0 ? m.CategoryName(uint32_t(line.category)) : "Unsorted", sizeof(text));
        } else if (line.category == -4) Upper(text, "Categories", sizeof(text));
        else {
            Number(text, line.count);
            Append(text, sizeof(text), line.count == 1 ? " APP" : " APPS");
        }
        const int y = Center(r, CF::Small);
        dl.Text(CF::Small, x, y, text, t.ink3, Right(r) - x);
        if (line.category >= -2) {
            char number[12];
            Number(number, line.count);
            dl.Text(CF::Small, x + s.TextWidth(text, CF::Small) + LPx(s, 8), y, number, t.ink3);
        }
    }

    void ChipButton(const ChromeLauncherHit& h) {
        const DesktopRect& r = h.rect;
        const bool hover = Hover(r) && Hover(ChromeLauncherListClip(f)) && !m.MenuOpen();
        if (hover) Rect(r, t.hover, 6);
        Outline(r, t.hairline, 6);
        const int tile = LPx(s, 22), x = r.x + LPx(s, 8);
        CategoryTile(x, r.y + (r.h - tile) / 2, h.target.index);
        const char* name = m.CategoryName(h.target.index);
        const int tx = x + tile + LPx(s, 8);
        dl.Text(CF::Label, tx, Center(r, CF::Label), name, t.ink);
        char number[12];
        Number(number, m.CategoryAppCount(h.target.index));
        dl.Text(CF::Small, tx + s.TextWidth(name, CF::Label) + LPx(s, 6), Center(r, CF::Small), number, t.ink3);
    }

    void EmptyLine(const LauncherLine& line, const DesktopRect& r) {
        char first[96] = {}, second[96] = {};
        if (line.category == -3) {
            Append(first, sizeof(first), "Nothing matches \"");
            Append(first, sizeof(first), m.Query());
            Append(first, sizeof(first), "\"");
            Append(second, sizeof(second), "Search covers app names, descriptions and your category names.");
        } else if (line.category == LauncherViewUnsorted) {
            Append(first, sizeof(first), "Nothing to sort.");
            Append(second, sizeof(second), "Newly installed apps land here until you file them.");
        } else if (line.category >= 0) {
            Append(first, sizeof(first), "No apps in ");
            Append(first, sizeof(first), m.CategoryName(uint32_t(line.category)));
            Append(first, sizeof(first), " yet.");
            Append(second, sizeof(second), "Select an app and press Alt+Up or Alt+Down to move it here.");
        } else {
            Append(first, sizeof(first), "No apps installed.");
            Append(second, sizeof(second), "Apps from App Gallery appear here.");
        }
        const int a = s.Line(CF::Body), b = s.Line(CF::Small), top = r.y + (r.h - a - b - LPx(s, 6)) / 2;
        auto centered = [&](CF font, int y, const char* text, uint32_t ink) {
            const int w = s.TextWidth(text, font);
            ChromeElidedText(dl, s, font, w < r.w ? r.x + (r.w - w) / 2 : r.x, y, text, ink, r.w);
        };
        centered(CF::Body, top, first, t.ink2);
        centered(CF::Small, top + a + LPx(s, 6), second, t.ink3);
    }

    void Scrollbar() {
        const uint32_t max = m.MaxScroll(), total = m.ContentHeight();
        if (!max || f.list.w <= 0 || !total) return;
        const DesktopRect& l = f.list;
        const int track = l.h, w = LPx(s, 3);
        int thumb = int(int64_t(track) * (total - max) / total);
        if (thumb < LPx(s, 24)) thumb = LPx(s, 24);
        const uint32_t at = f.scroll < max ? f.scroll : max;
        const int y = l.y + int(int64_t(track - thumb) * at / max);
        dl.MaterialRect({Right(l) + LPx(s, 3), y, w, thumb}, Mat(t.ink3), w / 2);
    }

    void Footer() {
        const DesktopRect& r = f.footer;
        if (r.w <= 0) return;
        dl.Fill(r.x, r.y, r.w, 1, Mat(t.hairline));
        int left = r.x + LPx(s, LM::Pad);
        int right = r.x + r.w;
        for (uint32_t i = 0; i < f.targetCount; ++i) {
            const auto& h = f.targets[i];
            if (h.target.kind >= TK::Settings && h.target.kind <= TK::CancelShutdown && h.rect.x < right) right = h.rect.x;
        }
        const char* status = c.prompt && !c.shutdownActive ?
            (c.promptRestart ? "Restart? Open apps will be asked to close" : "Shut down? Open apps will be asked to close") :
            c.status;
        const int width = right - LPx(s, 8) - left;
        if (status && status[0]) {
            ChromeElidedText(dl, s, CF::Label, left, Center(r, CF::Label), status, c.prompt ? t.ink : t.ink2, width);
        } else if (c.user) {
            const int av = LPx(s, 28), ay = r.y + (r.h - av) / 2;
            dl.MaterialRect({left, ay, av, av}, Mat(t.well), av / 2);
            const char initial[2] = {c.user[0], 0};
            dl.Text(CF::Strong, left + (av - s.TextWidth(initial, CF::Strong)) / 2, ay + (av - s.Line(CF::Strong)) / 2,
                    initial, t.ink2);
            const int tx = left + av + LPx(s, 10), lines = s.Line(CF::Body) + (c.build ? s.Line(CF::Mono) : 0);
            const int top = r.y + (r.h - lines) / 2;
            ChromeElidedText(dl, s, CF::Body, tx, top, c.user, t.ink, right - tx);
            if (c.build) ChromeElidedText(dl, s, CF::Mono, tx, top + s.Line(CF::Body), c.build, t.ink3, right - tx);
        }
        for (uint32_t i = 0; i < f.targetCount; ++i) {
            const auto& h = f.targets[i];
            switch (h.target.kind) {
            case TK::Settings: IconButton(h.rect, ChromeGlyph::Sliders); break;
            case TK::Restart: IconButton(h.rect, ChromeGlyph::Restart); break;
            case TK::Shutdown: IconButton(h.rect, ChromeGlyph::Power); break;
            case TK::Confirm: TextButton(h.rect, c.promptRestart ? "Restart now" : "Shut down now", true, c.confirmFocused); break;
            case TK::Dismiss: TextButton(h.rect, "Cancel", false, !c.confirmFocused); break;
            case TK::CancelShutdown: TextButton(h.rect, "Cancel shutdown", false, false); break;
            default: break;
            }
        }
    }

    DesktopRect Face(const DesktopRect& r) const {
        const int inset = (r.h - LPx(s, LM::ButtonFace)) / 2;
        return {r.x + (r.w > r.h ? 0 : inset), r.y + inset, r.w > r.h ? r.w : r.w - 2 * inset, r.h - 2 * inset};
    }
    void IconButton(const DesktopRect& r, ChromeGlyph glyph) {
        const bool hover = Hover(r) && !m.MenuOpen();
        if (hover) Rect(Face(r), t.hover, 6);
        const int g = LPx(s, 16);
        dl.Glyph(glyph, r.x + (r.w - g) / 2, r.y + (r.h - g) / 2, hover ? t.ink : t.ink2);
    }
    void TextButton(const DesktopRect& r, const char* label, bool danger, bool focused) {
        const DesktopRect face = Face(r);
        const bool hover = Hover(r);
        const auto& p = s.P();
        if (focused) {
            const int ring = LPx(s, 2);
            dl.MaterialRect({face.x - ring, face.y - ring, face.w + 2 * ring, face.h + 2 * ring}, 0xff000000u | t.accent,
                            LPx(s, 6) + ring);
        }
        if (danger) dl.MaterialRect(face, 0xff000000u | (hover ? ChromeBlendColor(p.closeHover, p.text, 200) : p.closeHover), LPx(s, 6));
        else { Rect(face, hover ? t.press : t.hover, 6); }
        const int w = s.TextWidth(label, CF::Label);
        dl.Text(CF::Label, face.x + (face.w - w) / 2, Center(face, CF::Label), label, danger ? p.closeText : t.ink);
    }

    void Menu() {
        if (!m.MenuOpen() || f.menu.w <= 0) return;
        const DesktopRect& r = f.menu;
        // A flat fill at 98%: a menu over the launcher costs no second backdrop.
        const uint32_t alpha = s.glass ? (s.materialOpacity > 250 ? s.materialOpacity : 250) : 255;
        // Shadow: two soft steps below and to the side (0 10 24 at 45%, approximated).
        // Under glass the surface alpha is the material's, so a darker band
        // would read as a hole; the hairline carries the edge there.
        if (!s.glass) {
            const uint32_t shadow = s.P().chromeDark ? 0x000000 : 0x14223a;
            dl.MaterialRect({r.x - LPx(s, 1), r.y + LPx(s, 3), r.w + LPx(s, 2), r.h + LPx(s, 4)},
                            0xff000000u | ChromeBlendColor(shadow, t.panel, 70), LPx(s, 10));
        }
        dl.MaterialRect(r, (alpha << 24) | t.menu, LPx(s, 8));
        dl.MaterialOutline(r, (alpha << 24) | t.hairline, LPx(s, 8));
        const LauncherApp& a = m.App(uint32_t(m.MenuApp()));
        ChromeElidedText(dl, s, CF::Strong, r.x + LPx(s, LM::MenuPad) + LPx(s, 10), r.y + LPx(s, LM::MenuPad) + LPx(s, 6), a.name,
                         t.ink, r.w - 2 * LPx(s, LM::MenuPad) - LPx(s, 20));
        for (uint32_t i = 0; i < f.targetCount; ++i) {
            const auto& h = f.targets[i];
            if (h.target.kind != TK::MenuItem) continue;
            const bool enabled = h.target.index == 0;
            const bool hover = enabled && Hover(h.rect);
            const bool kb = enabled && m.KeyboardActive() && m.MenuItem() == int(h.target.index);
            if (hover || kb) dl.MaterialRect(h.rect, (alpha << 24) | t.hover, LPx(s, 6));
            const uint32_t ink = enabled ? t.ink : t.ink3;
            const int g = LPx(s, 16), x = h.rect.x + LPx(s, 10);
            dl.Glyph(h.target.index == 0 ? ChromeGlyph::Pin : ChromeGlyph::Trash, x, h.rect.y + (h.rect.h - g) / 2, ink);
            const char* label = h.target.index == 0 ? (m.Pinned(uint32_t(m.MenuApp())) ? MenuUnpin : MenuPin) : MenuUninstall;
            dl.Text(CF::Body, x + g + LPx(s, 10), Center(h.rect, CF::Body), label, ink);
        }
    }

    void Build() {
        Panel();
        Search();
        if (f.top.w > 0) TilesEmpty();
        dl.Fill(f.panel.x, f.mid.y, f.panel.w, 1, Mat(t.hairline));
        if (f.rail.w > 0) dl.Fill(Right(f.rail) - 1, f.mid.y, 1, f.mid.h, Mat(t.hairline));
        // The scrolled list draws inside its rectangle only.
        dl.Clip(ChromeLauncherListClip(f));
        for (uint32_t i = 0; i < f.lineCount; ++i) {
            const LauncherLine& line = f.lines[i];
            if (line.kind == LauncherLineKind::Heading) Heading(line, f.lineRects[i]);
            else if (line.kind == LauncherLineKind::Empty) EmptyLine(line, f.lineRects[i]);
        }
        for (uint32_t i = 0; i < f.targetCount; ++i) {
            const auto& h = f.targets[i];
            switch (h.target.kind) {
            case TK::Row: Row(h); break;
            case TK::Chip: ChipButton(h); break;
            default: break;
            }
        }
        dl.Unclip();
        for (uint32_t i = 0; i < f.targetCount; ++i) {
            const auto& h = f.targets[i];
            switch (h.target.kind) {
            case TK::TopTab: TopTab(h); break;
            case TK::Tile: Tile(h); break;
            case TK::Rail: RailRow(h); break;
            default: break;
            }
        }
        Scrollbar();
        Footer();
        Menu();
    }
};

} // namespace

uint32_t ChromeLauncherCategoryColor(LauncherColor color) {
    static constexpr uint32_t colors[] = {0x6aaeff, 0x5fd6c8, 0x6fd08c, 0xf0b35a, 0xff8a73, 0xee8fc0, 0xb9a6f5, 0xa9b8cc};
    return colors[uint32_t(color) % 8];
}

DesktopRect ChromeLauncherRect(const ChromeStyle& s, uint32_t screenW, uint32_t screenH) {
    const int sw = int(screenW), sh = int(screenH);
    const int bar = LPx(s, DesktopMetrics::TaskbarBase), margin = LPx(s, 8);
    const int desktopH = sh > bar ? sh - bar : 0;
    int w = LPx(s, LM::Width), h = LPx(s, LM::Height);
    if (w > sw - 2 * margin) w = sw - 2 * margin;
    if (h > desktopH - 2 * margin) h = desktopH - 2 * margin;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    return {sw > 2 * margin ? margin : 0, desktopH - margin - h > 0 ? desktopH - margin - h : 0, w, h};
}

void ChromeLauncherFrameFor(const LauncherModel& m, const ChromeLauncherChrome& c, const ChromeStyle& s,
                          uint32_t screenW, uint32_t screenH, ChromeLauncherFrame& f) {
    f.panel = ChromeLauncherRect(s, screenW, screenH);
    f.search = f.top = f.mid = f.rail = f.list = f.footer = f.menu = {};
    f.lineCount = f.firstLine = f.totalLines = f.scroll = f.tileCount = f.targetCount = 0;
    const DesktopRect& p = f.panel;
    const int pad = LPx(s, LM::Pad);
    f.viewport = {LPx(s, LM::Heading), LPx(s, LM::Row), LPx(s, LM::Chips), LPx(s, LM::Empty), 0, LPx(s, LM::ScrollStep)};
    if (p.w < 4 * pad || p.h < LPx(s, LM::Search) + LPx(s, LM::Footer) + 2 * pad) {
        f.targets[f.targetCount++] = {p, {TK::Panel, 0}};
        return;
    }
    ChromeLauncherHit body[ChromeLauncherFrame::TargetLimit];
    uint32_t n = 0;
    auto add = [&](const DesktopRect& r, TK kind, uint32_t index) {
        if (n < ChromeLauncherFrame::TargetLimit - 8 && r.w > 0 && r.h > 0) body[n++] = {r, {kind, uint16_t(index)}};
    };

    f.search = {p.x + pad, p.y + pad, p.w - 2 * pad, LPx(s, LM::Search)};
    add(f.search, TK::Search, 0);
    int y = Bottom(f.search);
    const bool searching = m.Query()[0] != 0;
    if (!searching) {
        f.tileCount = m.TopTiles(f.tiles, LauncherLimits::Pins);
        const int rows = f.tileCount ? int((f.tileCount + LM::TilesPerRow - 1) / LM::TilesPerRow) : 1;
        const int tileH = LPx(s, LM::Tile), gap = LPx(s, 4);
        f.top = {p.x + pad, y + LPx(s, 8), p.w - 2 * pad, LPx(s, LM::Tab) + gap + rows * tileH + (rows - 1) * gap};
        int tx = f.top.x;
        for (uint32_t i = 0; i < 2; ++i) {
            const int w = s.TextWidth(TopLabels[i], CF::Small) + LPx(s, 20);
            add({tx, f.top.y, w, LPx(s, LM::Tab)}, TK::TopTab, i);
            tx += w + LPx(s, 2);
        }
        const int tilesY = f.top.y + LPx(s, LM::Tab) + gap;
        const int tileW = (f.top.w - (LM::TilesPerRow - 1) * gap) / LM::TilesPerRow;
        for (uint32_t i = 0; i < f.tileCount; ++i)
            add({f.top.x + int(i % LM::TilesPerRow) * (tileW + gap), tilesY + int(i / LM::TilesPerRow) * (tileH + gap), tileW, tileH},
                TK::Tile, i);
        y = Bottom(f.top) + LPx(s, 8);
    } else y += pad;

    f.footer = {p.x, Bottom(p) - LPx(s, LM::Footer), p.w, LPx(s, LM::Footer)};
    if (y > f.footer.y) y = f.footer.y;
    f.mid = {p.x, y, p.w, f.footer.y - y};
    const int railW = LPx(s, LM::Rail) < p.w / 3 ? LPx(s, LM::Rail) : p.w / 3;
    f.rail = {p.x, f.mid.y, railW, f.mid.h};
    const int inner = LPx(s, 8);
    f.list = {Right(f.rail) + inner, f.mid.y + inner, Right(p) - Right(f.rail) - 2 * inner - LPx(s, 4), f.mid.h - 2 * inner};
    f.viewport.height = f.list.h > 0 ? f.list.h : 0;

    // Rail rows while they fit.
    const int railRow = LPx(s, LM::RailRow);
    for (uint32_t row = 0; row < m.RailRows(); ++row) {
        const DesktopRect r{f.rail.x + inner, f.rail.y + inner + int(row) * railRow, f.rail.w - 2 * inner, railRow};
        if (Bottom(r) > Bottom(f.rail) - inner) break;
        add(r, TK::Rail, row);
    }

    // List lines that meet the list rectangle at the scroll offset; the
    // builder clips the partly visible ones to it.
    LauncherLine all[LauncherLimits::Lines];
    f.totalLines = m.Lines(all, LauncherLimits::Lines);
    f.scroll = m.Scroll();
    f.firstLine = f.totalLines;
    int ly = f.list.y - int(f.scroll);
    const int colGap = LPx(s, 4);
    for (uint32_t i = 0; i < f.totalLines && f.lineCount < ChromeLauncherFrame::LineLimit; ++i) {
        const LauncherLine& line = all[i];
        const int h = line.kind == LauncherLineKind::Heading ? f.viewport.heading : line.kind == LauncherLineKind::Apps ? f.viewport.row
                    : line.kind == LauncherLineKind::Chips ? f.viewport.chips : f.viewport.empty;
        const DesktopRect shown = ChromeLauncherListClip(f);
        if (ly >= Bottom(shown)) break;
        if (ly + h <= shown.y) { ly += h; continue; }
        if (f.firstLine == f.totalLines) f.firstLine = i;
        const DesktopRect r{f.list.x, ly, f.list.w, h};
        f.lines[f.lineCount] = line;
        f.lineRects[f.lineCount++] = r;
        if (line.kind == LauncherLineKind::Apps) {
            const int colW = line.columns == 2 ? (r.w - colGap) / 2 : r.w;
            for (uint32_t j = 0; j < line.count; ++j) add({r.x + int(j) * (colW + colGap), r.y, colW, r.h}, TK::Row, line.apps[j]);
        } else if (line.kind == LauncherLineKind::Chips) {
            uint16_t cats[LauncherLimits::Categories];
            const uint32_t count = m.SearchCategories(cats, LauncherLimits::Categories);
            int cx = r.x + LPx(s, 8);
            const int ch = LPx(s, 36);
            for (uint32_t j = 0; j < count; ++j) {
                char number[12];
                Number(number, m.CategoryAppCount(cats[j]));
                const int w = LPx(s, 8) + LPx(s, 22) + LPx(s, 8) + s.TextWidth(m.CategoryName(cats[j]), CF::Label) + LPx(s, 6) +
                              s.TextWidth(number, CF::Small) + LPx(s, 12);
                if (cx + w > Right(r)) break;
                add({cx, r.y + (r.h - ch) / 2, w, ch}, TK::Chip, cats[j]);
                cx += w + LPx(s, 6);
            }
        }
        ly += h;
    }

    // Footer: Settings, Restart, Shut down as 44 px icon targets; the
    // prompt replaces them with its confirm and Cancel buttons.
    const int button = LPx(s, LM::Button), by = f.footer.y + (f.footer.h - button) / 2;
    int right = Right(f.footer) - LPx(s, 8);
    auto textButton = [&](TK kind, const char* label) {
        const int w = s.TextWidth(label, CF::Label) + 2 * LPx(s, 14);
        right -= w;
        add({right, by, w, button}, kind, 0);
        right -= LPx(s, 8);
    };
    if (c.shutdownActive) textButton(TK::CancelShutdown, "Cancel shutdown");
    else if (c.prompt) {
        textButton(TK::Dismiss, "Cancel");
        textButton(TK::Confirm, c.promptRestart ? "Restart now" : "Shut down now");
    } else {
        static constexpr TK kinds[] = {TK::Shutdown, TK::Restart, TK::Settings};
        for (TK kind : kinds) {
            right -= button;
            add({right, by, button, button}, kind, 0);
        }
    }
    add(f.footer, TK::Footer, 0);

    // The menu, anchored at the pointer or, from the keyboard, on the app.
    uint32_t total = 0;
    if (m.MenuOpen()) {
        const int w = LPx(s, LM::Menu), header = LPx(s, 28), item = LPx(s, LM::MenuItem), mp = LPx(s, LM::MenuPad);
        const int h = 2 * mp + header + 2 * item;
        int ax = m.MenuX(), ay = m.MenuY();
        if (ax < 0) {
            ax = p.x + p.w / 2; ay = p.y + p.h / 2;
            for (uint32_t i = 0; i < n; ++i) {
                const auto& t = body[i];
                const bool tile = t.target.kind == TK::Tile && t.target.index < f.tileCount && f.tiles[t.target.index] == m.MenuApp();
                if ((t.target.kind == TK::Row && t.target.index == m.MenuApp()) || tile) {
                    ax = t.rect.x + t.rect.w * 45 / 100; ay = t.rect.y + t.rect.h * 6 / 10;
                    break;
                }
            }
        }
        int mx = ax, my = ay;
        if (mx + w > Right(p) - inner) mx = Right(p) - inner - w;
        if (my + h > Bottom(p) - inner) my = ay - h;
        if (mx < p.x + inner) mx = p.x + inner;
        if (my < p.y + inner) my = p.y + inner;
        f.menu = {mx, my, w, h};
        for (uint32_t i = 0; i < 2; ++i)
            f.targets[total++] = {{mx + mp, my + mp + header + int(i) * item, w - 2 * mp, item}, {TK::MenuItem, uint16_t(i)}};
        f.targets[total++] = {f.menu, {TK::Menu, 0}};
    }
    for (uint32_t i = 0; i < n && total < ChromeLauncherFrame::TargetLimit - 1; ++i) f.targets[total++] = body[i];
    f.targets[total++] = {p, {TK::Panel, 0}};
    f.targetCount = total;
}

LauncherTarget ChromeLauncherHitTest(const ChromeLauncherFrame& f, int32_t x, int32_t y) {
    for (uint32_t i = 0; i < f.targetCount; ++i) {
        const ChromeLauncherHit& h = f.targets[i];
        // List targets scroll: only their part inside the list is live.
        if ((h.target.kind == TK::Row || h.target.kind == TK::Chip) && !ChromeLauncherListClip(f).contains(x, y)) continue;
        if (h.rect.contains(x, y)) return h.target;
    }
    return {TK::None, 0};
}

void ChromeBuildLauncher(const LauncherModel& model, const ChromeLauncherChrome& chrome, const ChromeLauncherFrame& frame,
                         const ChromeStyle& s, ChromeDisplayList& dl) {
    if (frame.panel.w <= 0 || frame.panel.h <= 0) return;
    Builder b{model, chrome, frame, s, dl, TokensFor(s)};
    if (frame.search.w <= 0) { b.Panel(); return; }
    b.Build();
}

} // namespace brook
