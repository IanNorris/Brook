// CPU raster primitives for kernel-drawn desktop chrome: fills, glyph runs,
// icons and the translucent-material fill rule. Moved unchanged out of
// window.cpp (compositor refactor stage 0.3); the window frame, taskbar,
// launcher and popup painters call these. A later stage turns them into the
// CPU executor of the chrome display list.
#include "chrome_raster.h"
#include "chrome_material.h"
#include "compositor.h"
#include "font_atlas.h"
#include "window.h"

namespace brook {

int WmTextWidth(const char* text)
{
    int width = 0;
    for (; *text; ++text)
    {
        uint32_t code = static_cast<uint8_t>(*text);
        if (code >= WmDesktopFont().firstChar && code < WmDesktopFont().firstChar + WmDesktopFont().glyphCount)
            width += WmDesktopFont().glyphs[code - WmDesktopFont().firstChar].advance;
    }
    return width;
}

// Pixel write into buffer with bounds check
static inline void WmPutPixel(uint32_t* buf, uint32_t stride,
                               uint32_t screenW, uint32_t screenH,
                               int x, int y, uint32_t color)
{
    if (x < 0 || y < 0) return;
    if (static_cast<uint32_t>(x) >= screenW) return;
    if (static_cast<uint32_t>(y) >= screenH) return;
    buf[y * stride + x] = color;
}

// Fill a rectangle
void WmFillRect(uint32_t* buf, uint32_t stride,
                        uint32_t screenW, uint32_t screenH,
                        int x0, int y0, int w, int h, uint32_t color)
{
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            WmPutPixel(buf, stride, screenW, screenH, x, y, color);
}

// Render a single glyph from g_fontAtlas at (penX, penY) into buffer.
// Returns advance width. Clips pixels to [0, clipRight) horizontally.
int WmRenderGlyph(uint32_t* buf, uint32_t stride,
                          uint32_t screenW, uint32_t screenH,
                          int penX, int penY, int code,
                          uint32_t fg, uint32_t bg,
                          int clipRight, const WmRect* clip)
{
    const FontAtlas& fa = WmDesktopFont();
    if (code < static_cast<int>(fa.firstChar) ||
        code >= static_cast<int>(fa.firstChar + fa.glyphCount))
        return 0;

    const GlyphInfo& gi = fa.glyphs[code - static_cast<int>(fa.firstChar)];
    int gw = gi.atlasX1 - gi.atlasX0;
    int gh = gi.atlasY1 - gi.atlasY0;

    int drawX = penX + gi.bearingX;
    int drawY = penY + fa.ascent - gi.bearingY;

    int maxX = (clipRight > 0) ? clipRight : static_cast<int>(screenW);

    for (int row = 0; row < gh; ++row)
    {
        for (int col = 0; col < gw; ++col)
        {
            uint8_t cov = fa.pixels[(gi.atlasY0 + row) * static_cast<int>(fa.atlasWidth)
                                     + (gi.atlasX0 + col)];
            if (cov == 0) continue;

            int px = drawX + col;
            int py = drawY + row;
            if (px < 0 || py < 0) continue;
            if (clip && !clip->contains(px,py)) continue;
            if (px >= maxX) continue;
            if (static_cast<uint32_t>(px) >= screenW) continue;
            if (static_cast<uint32_t>(py) >= screenH) continue;

            // Preserve foreground coverage over a translucent material fill.
            // Legacy RGB buffers have alpha zero and remain fully opaque.
            (void)bg;
            auto& destination=buf[py*stride+px];
            destination=ChromePixelOver(destination,(uint32_t(cov)<<24)|(fg&0xffffff));
        }
    }

    return gi.advance;
}

// Render a string, optionally clipped to maxWidth pixels from starting x.
// maxWidth <= 0 means no clipping.
void WmRenderString(uint32_t* buf, uint32_t stride,
                            uint32_t screenW, uint32_t screenH,
                            int x, int y, const char* str,
                            uint32_t fg, uint32_t bg,
                            int maxWidth, const WmRect* clip)
{
    int penX = x;
    int clipRight = (maxWidth > 0) ? (x + maxWidth) : 0;
    while (*str)
    {
        if (maxWidth > 0 && (penX - x) >= maxWidth) break;
        penX += WmRenderGlyph(buf, stride, screenW, screenH,
                               penX, y, static_cast<uint8_t>(*str), fg, bg,
                               clipRight, clip);
        ++str;
    }
}

// Keep pale launcher labels legible when a low-opacity panel crosses bright
// client content. The narrow outline follows glyph coverage, not a solid box.
void WmRenderMaterialString(uint32_t* buf,uint32_t stride,uint32_t sw,uint32_t sh,
                                  int x,int y,const char* text,uint32_t fg,uint32_t bg,int width)
{
    const unsigned brightness=(((fg>>16)&255)+((fg>>8)&255)+(fg&255))/3;
    if(CompositorGlassActive() && CompositorMaterialOpacity()<180 && brightness>128){
        const int offset=WmMetric(1);
        WmRenderString(buf,stride,sw,sh,x-offset,y,text,WmCurrentPalette().halo,bg,width);
        WmRenderString(buf,stride,sw,sh,x+offset,y,text,WmCurrentPalette().halo,bg,width);
        WmRenderString(buf,stride,sw,sh,x,y-offset,text,WmCurrentPalette().halo,bg,width);
        WmRenderString(buf,stride,sw,sh,x,y+offset,text,WmCurrentPalette().halo,bg,width);
    }
    WmRenderString(buf,stride,sw,sh,x,y,text,fg,bg,width);
}

// Shared straight-alpha ARGB renderer. No VFS access or ownership transfer in
// the compositor path; icons are immutable for the launcher's lifetime.
void WmDrawIcon(uint32_t* buf, uint32_t stride, uint32_t screenW, uint32_t screenH,
                       int x, int y, int size, const uint32_t* pixels)
{
    if (pixels)
    {
        for (int row = 0; row < size; ++row)
            for (int col = 0; col < size; ++col)
            {
                const int px = x + col, py = y + row;
                if (px < 0 || py < 0 || px >= static_cast<int>(screenW) || py >= static_cast<int>(screenH)) continue;
                const uint32_t src = pixels[(row * LAUNCHER_ICON_PX / size) * LAUNCHER_ICON_PX + col * LAUNCHER_ICON_PX / size];
                if(!(src>>24))continue;
                auto& destination=buf[py*stride+px];
                destination=ChromePixelOver(destination,src);
            }
        return;
    }
    // Missing asset: a neutral application-window glyph, never a misleading
    // letter tile or a guessed application logo.
    WmFillRect(buf, stride, screenW, screenH, x + 2, y + 3, size - 4, size - 6, WmCurrentPalette().mutedText);
    WmFillRect(buf, stride, screenW, screenH, x + 3, y + 7, size - 6, size - 11, WmCurrentPalette().itemHover);
    WmFillRect(buf, stride, screenW, screenH, x + 5, y + 5, 2, 1, WmCurrentPalette().titleInactive);
}

uint32_t WmMaterialColor(uint32_t color){
    return (color&0xffffff)|(CompositorGlassActive()?CompositorMaterialOpacity()<<24:0xff000000u);
}

uint32_t WmBlendColor(uint32_t fg, uint32_t bg, uint32_t alpha)
{
    uint32_t out = 0;
    for (uint32_t shift = 0; shift <= 16; shift += 8) {
        const uint32_t f = (fg >> shift) & 255, b = (bg >> shift) & 255;
        out |= ((f * alpha + b * (255 - alpha)) / 255) << shift;
    }
    return out;
}

void WmMaterialRect(uint32_t* pixels,uint32_t stride,uint32_t sw,uint32_t sh,const WmRect& r,uint32_t color){
    const unsigned radius=CompositorGlassActive()?WmMetric(4):0;
    for(int y=0;y<r.h;++y){const unsigned row=y<r.h/2?y:r.h-1-y;
        const int inset=ChromeCornerInset(radius,row);
        WmFillRect(pixels,stride,sw,sh,r.x+inset,r.y+y,r.w-2*inset,1,WmMaterialColor(color));
    }
}

} // namespace brook
