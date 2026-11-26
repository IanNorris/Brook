#pragma once
// The CPU executor of chrome display lists, and the raster primitives it is
// built from (see chrome_raster.cpp). Freestanding: the caller supplies the
// font and the target, so the same sources build into the kernel, host tests
// and a user-mode shell.
#include <stdint.h>
#include "desktop_metrics.h"
#include "font_atlas.h"
#include "chrome_display_list.h"

namespace brook {

// 32-bit pixels. List coordinate (originX, originY) lands on pixel (0, 0);
// every write is clipped to width x height.
struct ChromeTarget {
    uint32_t* pixels;
    uint32_t stride, width, height;
    int originX = 0, originY = 0;
};

// One baked atlas per type role, at one desktop scale.
struct ChromeFontSet {
    const FontAtlas* font[static_cast<unsigned>(ChromeFont::Count)];
    const FontAtlas& operator[](ChromeFont role) const { return *font[static_cast<unsigned>(role)]; }
};
// The sets the build bakes (cmake/ChromeFonts.cmake).
extern const ChromeFontSet g_chromeFonts100, g_chromeFonts150, g_chromeFonts200;
const ChromeFontSet& ChromeFontsForScale(uint32_t percent);

// Map UTF-8 en and em dashes to their extra glyph slots. Existing text
// uses literal byte slots for the degree and middle dot.
inline uint32_t ChromeReadTextGlyph(const char*& text) {
    if (text[0] == '\xe2' && text[1] == '\x80' && (text[2] == '\x93' || text[2] == '\x94')) {
        const auto glyph = text[2] == '\x93' ? 0x81u : 0x82u;
        text += 3;
        return glyph;
    }
    const auto byte = static_cast<uint8_t>(*text++);
    return byte == 0x81 || byte == 0x82 ? 0 : byte;
}
inline int ChromeGlyphAdvance(const FontAtlas& font, uint32_t code) {
    return code >= font.firstChar && code - font.firstChar < font.glyphCount ?
        font.glyphs[code - font.firstChar].advance : 0;
}

// Advance width of text in the font.
int ChromeTextWidth(const FontAtlas& font, const char* text);
void ChromeFillRect(const ChromeTarget& t, int x0, int y0, int w, int h, uint32_t color);
// One glyph at the pen position; returns its advance. Pixels at or right of
// clipRight (when > 0) and outside clip (when given) are skipped.
int ChromeRenderGlyph(const ChromeTarget& t, const FontAtlas& font, int penX, int penY, int code,
                      uint32_t fg, int clipRight = 0, const DesktopRect* clip = nullptr);
// A string, stopped at maxWidth pixels from x when maxWidth > 0.
void ChromeRenderString(const ChromeTarget& t, const FontAtlas& font, int x, int y, const char* str,
                        uint32_t fg, int maxWidth = 0, const DesktopRect* clip = nullptr);
// A straight-alpha 24x24 (LAUNCHER_ICON_PX) icon scaled to size (bilinear).
void ChromeDrawIcon(const ChromeTarget& t, int x, int y, int size, const uint32_t* pixels, unsigned opacity = 255);
uint32_t ChromeBlendColor(uint32_t fg, uint32_t bg, uint32_t alpha);

// Rasterise every command of the list, in order.
void ChromeExecute(const ChromeDisplayList& list, const ChromeTarget& target, const ChromeFontSet& fonts);

} // namespace brook
