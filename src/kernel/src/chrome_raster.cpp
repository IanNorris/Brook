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
    while (*text) width += ChromeGlyphAdvance(font, ChromeReadTextGlyph(text));
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
    // Clip once, then store whole rows (the same pixels as a per-pixel
    // bounds-checked loop, several times faster for panel-sized fills).
    int left = x0 - t.originX, top = y0 - t.originY, right = left + w, bottom = top + h;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > static_cast<int>(t.width)) right = static_cast<int>(t.width);
    if (bottom > static_cast<int>(t.height)) bottom = static_cast<int>(t.height);
    for (int y = top; y < bottom; ++y) {
        uint32_t* row = t.pixels + static_cast<uint64_t>(y) * t.stride;
        for (int x = left; x < right; ++x) row[x] = color;
    }
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
        penX += ChromeRenderGlyph(t, font, penX, y, ChromeReadTextGlyph(str), fg, clipRight, clip);
    }
}

void ChromeDrawIcon(const ChromeTarget& t, int x, int y, int size, const uint32_t* pixels, unsigned opacity)
{
    if (!pixels || size <= 0) return;
    // Bilinear over premultiplied samples (8.8 fixed point): exact at the
    // source size, smooth at 16/20 px and at the 150% and 200% scales.
    const int n = ChromeIconSourcePx;
    for (int row = 0; row < size; ++row)
    {
        int sy = ((2 * row + 1) * n * 128) / size - 128;
        if (sy < 0) sy = 0;
        if (sy > (n - 1) * 256) sy = (n - 1) * 256;
        const int y0 = sy >> 8, y1 = y0 + 1 < n ? y0 + 1 : y0, fy = sy & 255;
        for (int col = 0; col < size; ++col)
        {
            const int tx = x + col - t.originX, ty = y + row - t.originY;
            if (tx < 0 || ty < 0 || tx >= static_cast<int>(t.width) || ty >= static_cast<int>(t.height)) continue;
            int sx = ((2 * col + 1) * n * 128) / size - 128;
            if (sx < 0) sx = 0;
            if (sx > (n - 1) * 256) sx = (n - 1) * 256;
            const int x0 = sx >> 8, x1 = x0 + 1 < n ? x0 + 1 : x0, fx = sx & 255;
            const uint32_t texels[4] = {pixels[y0 * n + x0], pixels[y0 * n + x1], pixels[y1 * n + x0], pixels[y1 * n + x1]};
            const uint32_t weights[4] = {uint32_t((256 - fx) * (256 - fy)), uint32_t(fx * (256 - fy)),
                                         uint32_t((256 - fx) * fy), uint32_t(fx * fy)};
            uint64_t a = 0, c[3] = {0, 0, 0};
            for (int i = 0; i < 4; ++i) {
                const uint64_t wa = uint64_t(texels[i] >> 24) * weights[i];
                a += wa;
                for (int ch = 0; ch < 3; ++ch) c[ch] += ((texels[i] >> (8 * ch)) & 255) * wa;
            }
            if (!a) continue;
            uint32_t src = 0;
            for (int ch = 0; ch < 3; ++ch) src |= static_cast<uint32_t>((c[ch] + a / 2) / a) << (8 * ch);
            const unsigned alpha = static_cast<unsigned>(((a + 32768) >> 16) * opacity + 127) / 255;
            if (!alpha) continue;
            auto& destination = t.pixels[static_cast<uint64_t>(ty) * t.stride + tx];
            destination = ChromePixelOver(destination, (uint32_t(alpha > 255 ? 255 : alpha) << 24) | src);
        }
    }
}

// A material pixel at partial coverage (rounded-corner edges): straight-alpha
// interpolation toward the fill. Alpha 0 on either side is the legacy opaque
// convention and is kept when both sides use it.
static inline void ChromeCoverPixel(const ChromeTarget& t, int x, int y, uint32_t color, unsigned coverage)
{
    if (!coverage) return;
    if (coverage >= 255) { ChromePutPixel(t, x, y, color); return; }
    x -= t.originX; y -= t.originY;
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= t.width || static_cast<uint32_t>(y) >= t.height) return;
    auto& destination = t.pixels[static_cast<uint64_t>(y) * t.stride + x];
    const uint32_t dst = destination;
    const bool legacy = !(dst >> 24) && !(color >> 24);
    const uint32_t da = (dst >> 24) ? dst >> 24 : 255, sa = (color >> 24) ? color >> 24 : 255;
    const uint32_t ws = sa * coverage, wd = da * (255 - coverage), sum = ws + wd;
    if (!sum) return;
    uint32_t out = legacy ? 0 : ((sum + 127) / 255) << 24;
    for (unsigned shift = 0; shift < 24; shift += 8)
        out |= ((((color >> shift) & 255) * ws + ((dst >> shift) & 255) * wd + sum / 2) / sum) << shift;
    destination = out;
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

void ChromeExecute(const ChromeDisplayList& list, const ChromeTarget& base, const ChromeFontSet& fonts)
{
    if (!base.pixels || !base.width || !base.height) return;
    // Every primitive clips to its target, so a Clip is a sub-target.
    ChromeTarget t = base;
    for (uint32_t i = 0; i < list.Count(); ++i)
    {
        const ChromeCmd& c = list.At(i);
        switch (c.op)
        {
        case ChromeOp::Clip: {
            t = base;
            if (c.w <= 0 || c.h <= 0) break;
            const int bx0 = base.originX, by0 = base.originY;
            const int bx1 = bx0 + static_cast<int>(base.width), by1 = by0 + static_cast<int>(base.height);
            const int x0 = c.x > bx0 ? c.x : bx0, y0 = c.y > by0 ? c.y : by0;
            const int x1 = c.x + c.w < bx1 ? c.x + c.w : bx1, y1 = c.y + c.h < by1 ? c.y + c.h : by1;
            if (x1 <= x0 || y1 <= y0) { t.width = t.height = 0; break; }
            t.pixels = base.pixels + static_cast<uint64_t>(y0 - by0) * base.stride + static_cast<uint32_t>(x0 - bx0);
            t.width = static_cast<uint32_t>(x1 - x0);
            t.height = static_cast<uint32_t>(y1 - y0);
            t.originX = x0;
            t.originY = y0;
            break;
        }
        case ChromeOp::FadeX: {
            const int left = c.x - t.originX, top = c.y - t.originY;
            for (int column = 0; column < c.w; ++column) {
                const int x = left + column;
                if (x < 0 || x >= static_cast<int>(t.width)) continue;
                const unsigned alpha = c.w > 1 ? (c.color >> 24) * (c.w - 1 - column) / (c.w - 1) : c.color >> 24;
                for (int row = top; row < top + c.h; ++row) {
                    if (row < 0 || row >= static_cast<int>(t.height)) continue;
                    auto& dst = t.pixels[static_cast<uint64_t>(row) * t.stride + x];
                    dst = ChromePixelOver(dst, (alpha << 24) | (c.color & 0xffffff));
                }
            }
            break;
        }
        case ChromeOp::Fill:
            ChromeFillRect(t, c.x, c.y, c.w, c.h, c.color);
            break;
        case ChromeOp::Rows:
            for (int row = 0; row < c.h; ++row)
                ChromeFillRect(t, c.x, c.y + row, c.w, 1, static_cast<const uint32_t*>(c.data)[row]);
            break;
        case ChromeOp::CornerFill:
            if (!c.arg && !c.data) { ChromeFillRect(t, c.x, c.y, c.w, c.h, c.color); break; }
            for (int row = c.y; row < c.y + c.h; ++row)
            {
                const int r = row - c.clip.y;
                const int right = c.clip.x + c.clip.w -
                    (r >= 0 ? static_cast<int>(ChromeCornerInset(c.arg, static_cast<unsigned>(r))) : 0);
                const int end = c.x + c.w < right ? c.x + c.w : right;
                const uint32_t color = c.data ? 0xff000000u | static_cast<const uint32_t*>(c.data)[row - c.y] : c.color;
                if (end > c.x) ChromeFillRect(t, c.x, row, end - c.x, 1, color);
            }
            break;
        case ChromeOp::MaterialOutline:
        case ChromeOp::MaterialRect: {
            const int half = (c.w < c.h ? c.w : c.h) / 2;
            if (half < 0) break;
            unsigned radius = c.arg > static_cast<unsigned>(half) ? half : c.arg;
            if (radius > ChromeCornerMask::MaxRadius * 2) radius = ChromeCornerMask::MaxRadius * 2;
            const bool outline = c.op == ChromeOp::MaterialOutline;
            const int r = static_cast<int>(radius);
            // Corner coverage: 4x4 samples of the outer circle, less the
            // inner circle (one pixel in) for an outline.
            auto corner = [&](int cx, int cy) -> unsigned {
                const unsigned outer = ChromeCornerMask::Sample(radius, cx, cy);
                if (!outline) return outer;
                const unsigned inner = (cx && cy) ? ChromeCornerMask::Sample(radius - 1, cx - 1, cy - 1) : 0;
                return outer > inner ? outer - inner : 0;
            };
            for (int y = 0; y < c.h; ++y)
            {
                const int cy = y < r ? y : c.h - 1 - y < r ? c.h - 1 - y : -1;
                if (cy >= 0) {
                    for (int cx = 0; cx < r; ++cx) {
                        const unsigned coverage = corner(cx, cy);
                        ChromeCoverPixel(t, c.x + cx, c.y + y, c.color, coverage);
                        ChromeCoverPixel(t, c.x + c.w - 1 - cx, c.y + y, c.color, coverage);
                    }
                }
                const int inset = cy >= 0 ? r : 0;
                if (!outline || y == 0 || y == c.h - 1) {
                    ChromeFillRect(t, c.x + inset, c.y + y, c.w - 2 * inset, 1, c.color);
                } else if (cy < 0) {
                    ChromeFillRect(t, c.x, c.y + y, 1, 1, c.color);
                    ChromeFillRect(t, c.x + c.w - 1, c.y + y, 1, 1, c.color);
                }
            }
            break;
        }
        case ChromeOp::Text:
            ChromeRenderString(t, fonts[static_cast<ChromeFont>(c.dy < static_cast<int>(ChromeFont::Count) && c.dy >= 0 ? c.dy : 0)],
                               c.x, c.y, list.TextAt(c.arg), c.color, c.w, c.dx ? &c.clip : nullptr);
            break;
        case ChromeOp::Icon:
            ChromeDrawIcon(t, c.x, c.y, c.w, static_cast<const uint32_t*>(c.data), c.arg);
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
