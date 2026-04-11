// Boot logo and progress bar display.
//
// Renders the Brook "B" logo centered on screen with a progress bar.
// The logo data is baked at build time from data/brook_logo.png.

#include "boot_logo.h"
#include "boot_logo_data.h"
#include "tty.h"
#include "font_atlas.h"
#include "serial.h"

namespace brook {

// ---------------------------------------------------------------------------
// Framebuffer state
// ---------------------------------------------------------------------------

static uint32_t* g_fb       = nullptr;
static uint32_t  g_fbWidth  = 0;
static uint32_t  g_fbHeight = 0;
static uint32_t  g_fbStride = 0; // in pixels (stride bytes / 4)

// Logo placement
static uint32_t g_logoX = 0;
static uint32_t g_logoY = 0;

// Progress bar placement and dimensions
static constexpr uint32_t BAR_HEIGHT  = 8;
static constexpr uint32_t BAR_MARGIN  = 20;
static constexpr uint32_t BAR_WIDTH_FRACTION = 3; // bar width = logo width * fraction / 2
static uint32_t g_barX = 0;
static uint32_t g_barY = 0;
static uint32_t g_barW = 0;

// Text placement (below bar)
static uint32_t g_textY = 0;

// Colours
static constexpr uint32_t COL_BG       = 0x00000000; // black
static constexpr uint32_t COL_BAR_BG   = 0x00333333; // dark grey
static constexpr uint32_t COL_BAR_FG   = 0x004488CC; // blue
static constexpr uint32_t COL_BAR_ERR  = 0x00CC4444; // red

// ---------------------------------------------------------------------------
// Pixel helpers
// ---------------------------------------------------------------------------

static inline void PutPixel(uint32_t x, uint32_t y, uint32_t colour)
{
    if (x < g_fbWidth && y < g_fbHeight)
        g_fb[y * g_fbStride + x] = colour;
}

static void FillRect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour)
{
    for (uint32_t dy = 0; dy < h; ++dy)
    {
        if (y + dy >= g_fbHeight) break;
        for (uint32_t dx = 0; dx < w; ++dx)
        {
            if (x + dx >= g_fbWidth) break;
            g_fb[(y + dy) * g_fbStride + (x + dx)] = colour;
        }
    }
}

// ---------------------------------------------------------------------------
// Text renderer using the FontAtlas (same approach as TTY)
// ---------------------------------------------------------------------------

static uint32_t RenderGlyph(char c, uint32_t px, uint32_t py, uint32_t colour)
{
    const FontAtlas& fa = g_fontAtlas;
    int code = static_cast<int>(c);
    if (code < static_cast<int>(fa.firstChar) ||
        code >= static_cast<int>(fa.firstChar + fa.glyphCount))
        return 0;

    const GlyphInfo& gi = fa.glyphs[code - static_cast<int>(fa.firstChar)];
    int gw = gi.atlasX1 - gi.atlasX0;
    int gh = gi.atlasY1 - gi.atlasY0;

    int drawX = static_cast<int>(px) + gi.bearingX;
    int drawY = static_cast<int>(py) + fa.ascent - gi.bearingY;

    for (int row = 0; row < gh; ++row)
    {
        for (int col = 0; col < gw; ++col)
        {
            uint8_t cov = fa.pixels[(gi.atlasY0 + row) * static_cast<int>(fa.atlasWidth)
                                     + (gi.atlasX0 + col)];
            if (cov > 0)
            {
                uint32_t r = ((colour >> 16) & 0xFF) * cov / 255;
                uint32_t g = ((colour >> 8) & 0xFF) * cov / 255;
                uint32_t b = (colour & 0xFF) * cov / 255;
                PutPixel(static_cast<uint32_t>(drawX + col),
                         static_cast<uint32_t>(drawY + row),
                         (r << 16) | (g << 8) | b);
            }
        }
    }

    return static_cast<uint32_t>(gi.advance);
}

static void RenderText(const char* text, uint32_t x, uint32_t y, uint32_t colour)
{
    while (*text)
    {
        x += RenderGlyph(*text, x, y, colour);
        ++text;
    }
}

static uint32_t MeasureText(const char* text)
{
    const FontAtlas& fa = g_fontAtlas;
    uint32_t w = 0;
    while (*text)
    {
        int code = static_cast<int>(*text);
        if (code >= static_cast<int>(fa.firstChar) &&
            code < static_cast<int>(fa.firstChar + fa.glyphCount))
        {
            w += static_cast<uint32_t>(fa.glyphs[code - static_cast<int>(fa.firstChar)].advance);
        }
        ++text;
    }
    return w;
}

static uint32_t FormatUint(char* buf, uint32_t val)
{
    if (val == 0) { buf[0] = '0'; return 1; }
    char tmp[10];
    uint32_t n = 0;
    while (val > 0) { tmp[n++] = static_cast<char>('0' + (val % 10)); val /= 10; }
    for (uint32_t i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    return n;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void BootLogoInit()
{
    uint32_t strideBytes;
    if (!TtyGetFramebuffer(&g_fb, &g_fbWidth, &g_fbHeight, &strideBytes))
        return;

    g_fbStride = strideBytes / 4;

    // Suppress TTY framebuffer writes while the logo is displayed.
    TtySuppressDisplay(true);

    // Clear screen to black
    for (uint32_t y = 0; y < g_fbHeight; ++y)
        for (uint32_t x = 0; x < g_fbWidth; ++x)
            g_fb[y * g_fbStride + x] = COL_BG;

    // Center the logo horizontally, place it 1/4 down from top
    g_logoX = (g_fbWidth - g_bootLogoWidth) / 2;
    g_logoY = (g_fbHeight - g_bootLogoHeight) / 3;

    // Draw the logo — skip black pixels (pre-multiplied alpha on black bg)
    for (uint32_t y = 0; y < g_bootLogoHeight; ++y)
    {
        for (uint32_t x = 0; x < g_bootLogoWidth; ++x)
        {
            uint32_t pixel = g_bootLogoData[y * g_bootLogoWidth + x];
            if (pixel != 0)
                PutPixel(g_logoX + x, g_logoY + y, pixel);
        }
    }

    // Progress bar: centered below logo, wider than logo
    g_barW = g_bootLogoWidth * BAR_WIDTH_FRACTION / 2;
    if (g_barW > g_fbWidth - 40) g_barW = g_fbWidth - 40;
    g_barX = (g_fbWidth - g_barW) / 2;
    g_barY = g_logoY + g_bootLogoHeight + BAR_MARGIN;

    // Text position: below bar
    g_textY = g_barY + BAR_HEIGHT + 8;

    // Draw empty progress bar background
    FillRect(g_barX, g_barY, g_barW, BAR_HEIGHT, COL_BAR_BG);

    SerialPrintf("BOOT LOGO: %ux%u at (%u,%u), bar at (%u,%u) w=%u\n",
                  g_bootLogoWidth, g_bootLogoHeight, g_logoX, g_logoY,
                  g_barX, g_barY, g_barW);
}

void BootLogoProgress(uint8_t percent, const char* text)
{
    if (!g_fb) return;
    if (percent > 100) percent = 100;

    // Fill progress bar
    uint32_t fillW = (g_barW * percent) / 100;
    FillRect(g_barX, g_barY, fillW, BAR_HEIGHT, COL_BAR_FG);

    // Clear text area
    uint32_t textH = static_cast<uint32_t>(g_fontAtlas.lineHeight) + 2;
    FillRect(g_barX, g_textY, g_barW, textH, COL_BG);

    // Format: "42% — Initializing..."
    char buf[64];
    uint32_t pos = FormatUint(buf, percent);
    buf[pos++] = '%';
    buf[pos] = '\0';

    // Render percentage centered above/at bar
    uint32_t pctW = MeasureText(buf);
    uint32_t textCenterX = g_barX + (g_barW - pctW) / 2;

    // If we have a label, show "42% — label" left-aligned
    if (text && text[0])
    {
        buf[pos++] = ' ';
        // Em dash
        buf[pos++] = '-';
        buf[pos++] = '-';
        buf[pos++] = ' ';
        uint32_t ti = 0;
        while (text[ti] && pos < 60) buf[pos++] = text[ti++];
        buf[pos] = '\0';

        uint32_t fullW = MeasureText(buf);
        textCenterX = g_barX + (g_barW - fullW) / 2;
    }

    RenderText(buf, textCenterX, g_textY, 0x00CCCCCC);

    SerialPrintf("BOOT: %s\n", buf);
}

void BootLogoError(uint8_t percentAtError, const char* errorText)
{
    if (!g_fb) return;

    // Fill bar up to error point in red
    uint32_t fillW = (g_barW * percentAtError) / 100;
    FillRect(g_barX, g_barY, fillW, BAR_HEIGHT, COL_BAR_ERR);

    // Clear and show error text in red
    uint32_t errTextH = static_cast<uint32_t>(g_fontAtlas.lineHeight) + 2;
    FillRect(g_barX, g_textY, g_barW, errTextH, COL_BG);

    char buf[64] = "ERROR: ";
    uint32_t pos = 7;
    if (errorText)
    {
        uint32_t ti = 0;
        while (errorText[ti] && pos < 60) buf[pos++] = errorText[ti++];
    }
    buf[pos] = '\0';

    uint32_t textW = MeasureText(buf);
    uint32_t textX = g_barX + (g_barW - textW) / 2;
    RenderText(buf, textX, g_textY, COL_BAR_ERR);
}

void BootLogoClear()
{
    // Clear the logo area to black (TTY will take over)
    if (!g_fb) return;

    for (uint32_t y = g_logoY; y < g_textY + static_cast<uint32_t>(g_fontAtlas.lineHeight) + 4 && y < g_fbHeight; ++y)
        for (uint32_t x = 0; x < g_fbWidth; ++x)
            g_fb[y * g_fbStride + x] = COL_BG;

    // Re-enable TTY framebuffer output and clear to its background.
    TtySuppressDisplay(false);
    TtyClear();
}

} // namespace brook
