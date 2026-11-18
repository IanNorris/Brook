#pragma once
// CPU raster primitives for kernel-drawn desktop chrome (see chrome_raster.cpp).
// Buffers are 32-bit pixels with the given stride; every write is clipped to
// screenW x screenH.
#include <stdint.h>
#include "window.h"

namespace brook {
// Advance width of text in the desktop font.
int WmTextWidth(const char* text);
void WmFillRect(uint32_t* buf, uint32_t stride, uint32_t screenW, uint32_t screenH,
                int x0, int y0, int w, int h, uint32_t color);
// One glyph at the pen position; returns its advance. Pixels at or right of
// clipRight (when > 0) and outside clip (when given) are skipped.
int WmRenderGlyph(uint32_t* buf, uint32_t stride, uint32_t screenW, uint32_t screenH,
                  int penX, int penY, int code, uint32_t fg, uint32_t bg,
                  int clipRight = 0, const WmRect* clip = nullptr);
// A string, stopped at maxWidth pixels from x when maxWidth > 0.
void WmRenderString(uint32_t* buf, uint32_t stride, uint32_t screenW, uint32_t screenH,
                    int x, int y, const char* str, uint32_t fg, uint32_t bg,
                    int maxWidth = 0, const WmRect* clip = nullptr);
// As WmRenderString, with a halo that keeps pale text legible over glass.
void WmRenderMaterialString(uint32_t* buf, uint32_t stride, uint32_t sw, uint32_t sh,
                            int x, int y, const char* text, uint32_t fg, uint32_t bg, int width = 0);
// A straight-alpha LAUNCHER_ICON_PX icon scaled to size, or the neutral
// missing-icon glyph when pixels is null.
void WmDrawIcon(uint32_t* buf, uint32_t stride, uint32_t screenW, uint32_t screenH,
                int x, int y, int size, const uint32_t* pixels);
// A chrome colour with the material alpha (glass) or opaque.
uint32_t WmMaterialColor(uint32_t color);
uint32_t WmBlendColor(uint32_t fg, uint32_t bg, uint32_t alpha);
// A filled rectangle in the material colour, corner-rounded under glass.
void WmMaterialRect(uint32_t* pixels, uint32_t stride, uint32_t sw, uint32_t sh, const WmRect& r, uint32_t color);
} // namespace brook
