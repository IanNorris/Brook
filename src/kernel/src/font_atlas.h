#pragma once

// font_atlas.h — Pre-baked TrueType glyph atlas for kernel TTY rendering.
//
// Generated at build time by scripts/bake_font.py from Hack-Regular.ttf.
// The atlas is a flat greyscale byte array; each byte is a coverage value
// (0 = fully transparent, 255 = fully opaque) to be alpha-blended with
// the foreground and background colours.
//
// No floating-point arithmetic is needed at runtime.

#include <stdint.h>

namespace brook {

// Per-glyph bounding box and pen metrics (all in pixels).
struct GlyphInfo
{
    int16_t atlasX0, atlasY0;  // top-left corner in atlas (inclusive)
    int16_t atlasX1, atlasY1;  // bottom-right corner in atlas (exclusive)
    int16_t bearingX;          // horizontal offset from pen to left edge
    int16_t bearingY;          // vertical offset from baseline to top edge
    int16_t advance;           // pen advance (pixels) after drawing this glyph
};

// The complete font atlas exported from the bake step.
struct FontAtlas
{
    const uint8_t*   pixels;      // greyscale pixel data [atlasWidth * atlasHeight]
    uint32_t         atlasWidth;
    uint32_t         atlasHeight;
    const GlyphInfo* glyphs;      // [glyphCount] entries, indexed by (codepoint - firstChar)
    uint32_t         glyphCount;  // number of glyphs baked (95 for ASCII 32-126)
    uint32_t         firstChar;   // codepoint of glyphs[0]
    int32_t          ascent;      // pixels from baseline to top of cap-height
    int32_t          descender;   // pixels below baseline (positive value)
    int32_t          lineHeight;  // recommended line spacing in pixels
};

// The single font atlas instance, defined in the generated font_atlas.cpp.
extern const FontAtlas g_fontAtlas;

} // namespace brook
