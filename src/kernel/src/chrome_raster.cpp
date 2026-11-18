// CPU raster primitives for desktop chrome, and the display-list executor
// built from them (compositor refactor stages 0.3 and 1.2). The primitives
// are the painters' historical ones, unchanged apart from taking the font
// and target explicitly, so lists reproduce the old pixels exactly.
// Freestanding: no kernel dependencies.
#include "chrome_raster.h"
#include "chrome_material.h"

namespace brook {

static constexpr int ChromeIconSourcePx = 24; // LAUNCHER_ICON_PX

int ChromeTextWidth(const FontAtlas& font, const char* text)
{
    int width = 0;
    for (; *text; ++text)
    {
        uint32_t code = static_cast<uint8_t>(*text);
        if (code >= font.firstChar && code < font.firstChar + font.glyphCount)
            width += font.glyphs[code - font.firstChar].advance;
    }
    return width;
}

// Pixel write into the target with bounds check
static inline void ChromePutPixel(const ChromeTarget& t, int x, int y, uint32_t color)
{
    x -= t.originX; y -= t.originY;
    if (x < 0 || y < 0) return;
    if (static_cast<uint32_t>(x) >= t.width) return;
    if (static_cast<uint32_t>(y) >= t.height) return;
    t.pixels[static_cast<uint64_t>(y) * t.stride + x] = color;
}

void ChromeFillRect(const ChromeTarget& t, int x0, int y0, int w, int h, uint32_t color)
{
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            ChromePutPixel(t, x, y, color);
}

int ChromeRenderGlyph(const ChromeTarget& t, const FontAtlas& fa, int penX, int penY, int code,
                      uint32_t fg, int clipRight, const DesktopRect* clip)
{
    if (code < static_cast<int>(fa.firstChar) ||
        code >= static_cast<int>(fa.firstChar + fa.glyphCount))
        return 0;

    const GlyphInfo& gi = fa.glyphs[code - static_cast<int>(fa.firstChar)];
    int gw = gi.atlasX1 - gi.atlasX0;
    int gh = gi.atlasY1 - gi.atlasY0;

    int drawX = penX + gi.bearingX;
    int drawY = penY + fa.ascent - gi.bearingY;

    for (int row = 0; row < gh; ++row)
    {
        for (int col = 0; col < gw; ++col)
        {
            uint8_t cov = fa.pixels[(gi.atlasY0 + row) * static_cast<int>(fa.atlasWidth)
                                     + (gi.atlasX0 + col)];
            if (cov == 0) continue;

            const int px = drawX + col, py = drawY + row;
            if (clip && !clip->contains(px, py)) continue;
            if (clipRight > 0 && px >= clipRight) continue;
            const int tx = px - t.originX, ty = py - t.originY;
            if (tx < 0 || ty < 0) continue;
            if (static_cast<uint32_t>(tx) >= t.width) continue;
            if (static_cast<uint32_t>(ty) >= t.height) continue;

            // Preserve foreground coverage over a translucent material fill.
            // Legacy RGB buffers have alpha zero and remain fully opaque.
            auto& destination = t.pixels[static_cast<uint64_t>(ty) * t.stride + tx];
            destination = ChromePixelOver(destination, (uint32_t(cov) << 24) | (fg & 0xffffff));
        }
    }

    return gi.advance;
}

void ChromeRenderString(const ChromeTarget& t, const FontAtlas& font, int x, int y, const char* str,
                        uint32_t fg, int maxWidth, const DesktopRect* clip)
{
    int penX = x;
    int clipRight = (maxWidth > 0) ? (x + maxWidth) : 0;
    while (*str)
    {
        if (maxWidth > 0 && (penX - x) >= maxWidth) break;
        penX += ChromeRenderGlyph(t, font, penX, y, static_cast<uint8_t>(*str), fg, clipRight, clip);
        ++str;
    }
}

void ChromeDrawIcon(const ChromeTarget& t, int x, int y, int size, const uint32_t* pixels)
{
    if (!pixels) return;
    for (int row = 0; row < size; ++row)
        for (int col = 0; col < size; ++col)
        {
            const int tx = x + col - t.originX, ty = y + row - t.originY;
            if (tx < 0 || ty < 0 || tx >= static_cast<int>(t.width) || ty >= static_cast<int>(t.height)) continue;
            const uint32_t src = pixels[(row * ChromeIconSourcePx / size) * ChromeIconSourcePx +
                                        col * ChromeIconSourcePx / size];
            if (!(src >> 24)) continue;
            auto& destination = t.pixels[static_cast<uint64_t>(ty) * t.stride + tx];
            destination = ChromePixelOver(destination, src);
        }
}

uint32_t ChromeBlendColor(uint32_t fg, uint32_t bg, uint32_t alpha)
{
    uint32_t out = 0;
    for (uint32_t shift = 0; shift <= 16; shift += 8) {
        const uint32_t f = (fg >> shift) & 255, b = (bg >> shift) & 255;
        out |= ((f * alpha + b * (255 - alpha)) / 255) << shift;
    }
    return out;
}

void ChromeExecute(const ChromeDisplayList& list, const ChromeTarget& t, const FontAtlas& font)
{
    if (!t.pixels || !t.width || !t.height) return;
    for (uint32_t i = 0; i < list.Count(); ++i)
    {
        const ChromeCmd& c = list.At(i);
        switch (c.op)
        {
        case ChromeOp::Fill:
            ChromeFillRect(t, c.x, c.y, c.w, c.h, c.color);
            break;
        case ChromeOp::Rows:
            for (int row = 0; row < c.h; ++row)
                ChromeFillRect(t, c.x, c.y + row, c.w, 1, static_cast<const uint32_t*>(c.data)[row]);
            break;
        case ChromeOp::CornerFill:
            if (!c.arg) { ChromeFillRect(t, c.x, c.y, c.w, c.h, c.color); break; }
            for (int row = c.y; row < c.y + c.h; ++row)
            {
                const int r = row - c.clip.y;
                const int right = c.clip.x + c.clip.w -
                    (r >= 0 ? static_cast<int>(ChromeCornerInset(c.arg, static_cast<unsigned>(r))) : 0);
                const int end = c.x + c.w < right ? c.x + c.w : right;
                if (end > c.x) ChromeFillRect(t, c.x, row, end - c.x, 1, c.color);
            }
            break;
        case ChromeOp::MaterialRect:
            for (int y = 0; y < c.h; ++y)
            {
                const unsigned row = y < c.h / 2 ? y : c.h - 1 - y;
                const int inset = ChromeCornerInset(c.arg, row);
                ChromeFillRect(t, c.x + inset, c.y + y, c.w - 2 * inset, 1, c.color);
            }
            break;
        case ChromeOp::Text:
            ChromeRenderString(t, font, c.x, c.y, list.TextAt(c.arg), c.color, c.w, c.dx ? &c.clip : nullptr);
            break;
        case ChromeOp::Icon:
            ChromeDrawIcon(t, c.x, c.y, c.w, static_cast<const uint32_t*>(c.data));
            break;
        case ChromeOp::Squares:
            for (uint32_t n = 0; n < c.arg; ++n)
                ChromeFillRect(t, c.x + static_cast<int>(n) * c.dx, c.y + static_cast<int>(n) * c.dy,
                               c.w, c.h, c.color);
            break;
        }
    }
}

} // namespace brook
