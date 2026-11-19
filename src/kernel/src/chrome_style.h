#pragma once
// Everything a chrome builder reads besides its surface model: palette,
// metrics, font and the material state the active backend resolved. The
// kernel fills it from the WM's settings (WmChromeStyle); host tests fill it
// per theme and scale. Freestanding.
#include <stdint.h>
#include "desktop_metrics.h"
#include "font_atlas.h"
#include "chrome_material.h"
#include "chrome_raster.h"
#include "chrome_display_list.h"

namespace brook {

struct ChromeStyle {
    const BrookThemePalette* palette;
    const DesktopMetrics* metrics;
    const ChromeFontSet* fonts;
    bool glass;                          // translucent material (virgl glass)
    uint32_t materialOpacity;            // material alpha under glass
    const ChromeMaterialCache* material; // glass title rows and rims; may be null otherwise

    const BrookThemePalette& P() const { return *palette; }
    const FontAtlas& Font(ChromeFont role = ChromeFont::Body) const { return (*fonts)[role]; }
    int Line(ChromeFont role = ChromeFont::Body) const { return Font(role).lineHeight; }
    // Scaled metric, typed like the WM's WmMetric.
    uint32_t M(uint32_t base) const { return static_cast<uint32_t>(metrics->pixels(static_cast<int>(base))); }
    int TextWidth(const char* text, ChromeFont role = ChromeFont::Body) const { return ChromeTextWidth(Font(role), text); }
    // A chrome colour with the material alpha (glass) or opaque.
    uint32_t MaterialColor(uint32_t color) const {
        return (color & 0xffffff) | (glass ? materialOpacity << 24 : 0xff000000u);
    }
};

// A filled rectangle in the material colour with rounded (r-sm) corners.
inline void ChromeMaterialRect(ChromeDisplayList& dl, const ChromeStyle& s, const DesktopRect& r, uint32_t color,
                               unsigned radius = 6) {
    dl.MaterialRect(r, s.MaterialColor(color), s.M(radius));
}

// A string with a halo that keeps pale text legible over thin glass.
inline void ChromeMaterialString(ChromeDisplayList& dl, const ChromeStyle& s, int x, int y, const char* text,
                                 uint32_t fg, int width = 0, ChromeFont font = ChromeFont::Body) {
    const unsigned brightness = (((fg >> 16) & 255) + ((fg >> 8) & 255) + (fg & 255)) / 3;
    if (s.glass && s.materialOpacity < 180 && brightness > 128) {
        const int offset = s.M(1);
        dl.Text(x - offset, y, text, s.P().halo, width, nullptr, font);
        dl.Text(x + offset, y, text, s.P().halo, width, nullptr, font);
        dl.Text(x, y - offset, text, s.P().halo, width, nullptr, font);
        dl.Text(x, y + offset, text, s.P().halo, width, nullptr, font);
    }
    dl.Text(x, y, text, fg, width, nullptr, font);
}

// A single line that ends in "..." when it does not fit width (the
// prototype's text-overflow: ellipsis), instead of a glyph cut in half.
inline void ChromeElidedText(ChromeDisplayList& dl, const ChromeStyle& s, ChromeFont font, int x, int y,
                             const char* text, uint32_t fg, int width) {
    if (width <= 0 || !text) return;
    if (s.TextWidth(text, font) <= width) { dl.Text(font, x, y, text, fg, width); return; }
    char shown[96];
    const FontAtlas& atlas = s.Font(font);
    const int dots = 3 * ChromeTextWidth(atlas, ".");
    unsigned n = 0;
    for (int used = 0; text[n] && n + 4 < sizeof(shown); ++n) {
        const char one[2] = {text[n], 0};
        const int advance = ChromeTextWidth(atlas, one);
        if (used + advance + dots > width) break;
        used += advance;
        shown[n] = text[n];
    }
    while (n && shown[n - 1] == ' ') --n;
    shown[n] = shown[n + 1] = shown[n + 2] = '.';
    shown[n + 3] = 0;
    dl.Text(font, x, y, shown, fg, width);
}

// An icon, or the neutral missing-icon glyph when pixels is null (never a
// misleading letter tile or a guessed application logo).
inline void ChromeIcon(ChromeDisplayList& dl, const ChromeStyle& s, int x, int y, int size, const uint32_t* pixels) {
    if (pixels) { dl.Icon(x, y, size, pixels); return; }
    dl.Fill(x + 2, y + 3, size - 4, size - 6, 0xff000000u | s.P().mutedText);
    dl.Fill(x + 3, y + 7, size - 6, size - 11, 0xff000000u | s.P().itemHover);
    dl.Fill(x + 5, y + 5, 2, 1, 0xff000000u | s.P().titleInactive);
}

} // namespace brook
