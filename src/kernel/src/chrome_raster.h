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
// A straight-alpha 24x24 (LAUNCHER_ICON_PX) icon scaled to size.
void ChromeDrawIcon(const ChromeTarget& t, int x, int y, int size, const uint32_t* pixels);
uint32_t ChromeBlendColor(uint32_t fg, uint32_t bg, uint32_t alpha);

// Rasterise every command of the list, in order.
void ChromeExecute(const ChromeDisplayList& list, const ChromeTarget& target, const FontAtlas& font);

} // namespace brook
