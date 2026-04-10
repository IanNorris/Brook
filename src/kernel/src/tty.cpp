#include "tty.h"
#include "font_atlas.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"

namespace brook {

// ---------------------------------------------------------------------------
// TTY state
// ---------------------------------------------------------------------------

static volatile uint32_t* g_fbPixels  = nullptr; // virtual address of framebuffer
static uint32_t   g_fbWidth           = 0;
static uint32_t   g_fbHeight          = 0;
static uint32_t   g_fbStride          = 0;        // stride in pixels (not bytes)

static int        g_curX              = 0;        // current pen X (pixels)
static int        g_curY              = 0;        // current line top Y (pixels)

static uint32_t   g_fgColor           = 0x00E0E0E0; // light grey
static uint32_t   g_bgColor           = 0x00001A3A; // dark blue (matches kernel bg)

// ---------------------------------------------------------------------------
// Pixel helpers — pure integer arithmetic, no FPU.
// ---------------------------------------------------------------------------

// Alpha-blend a single pixel: out = (fg * cov + bg * (255 - cov)) / 255
// Uses the fast "/ 255 ≈ (x + (x >> 8) + 1) >> 8" approximation.
static inline uint32_t BlendChannel(uint32_t fg, uint32_t bg, uint32_t cov)
{
    uint32_t v = fg * cov + bg * (255u - cov);
    return (v + (v >> 8) + 1u) >> 8;
}

static inline void WritePixel(int x, int y, uint8_t coverage)
{
    if ((uint32_t)x >= g_fbWidth || (uint32_t)y >= g_fbHeight) return;

    uint32_t fg = g_fgColor;
    uint32_t bg = g_bgColor;
    uint32_t cov = coverage;

    uint32_t r = BlendChannel((fg >> 16) & 0xFF, (bg >> 16) & 0xFF, cov);
    uint32_t g = BlendChannel((fg >>  8) & 0xFF, (bg >>  8) & 0xFF, cov);
    uint32_t b = BlendChannel( fg        & 0xFF,  bg        & 0xFF, cov);

    g_fbPixels[y * g_fbStride + x] = (r << 16) | (g << 8) | b;
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

static void ScrollUp()
{
    int lh = g_fontAtlas.lineHeight;
    // Move rows [lh .. height-1] up to [0 .. height-1-lh].
    for (uint32_t row = 0; (int)row + lh < (int)g_fbHeight; row++)
    {
        const volatile uint32_t* src = g_fbPixels + (row + (uint32_t)lh) * g_fbStride;
        volatile uint32_t*       dst = g_fbPixels + row * g_fbStride;
        for (uint32_t col = 0; col < g_fbWidth; col++)
            dst[col] = src[col];
    }
    // Clear the bottom lh rows.
    for (uint32_t row = g_fbHeight - (uint32_t)lh; row < g_fbHeight; row++)
        for (uint32_t col = 0; col < g_fbWidth; col++)
            g_fbPixels[row * g_fbStride + col] = g_bgColor;
}

// Advance cursor to the next line, scrolling if needed.
static void Newline()
{
    g_curX  = 0;
    g_curY += g_fontAtlas.lineHeight;
    if ((uint32_t)(g_curY + g_fontAtlas.lineHeight) > g_fbHeight)
    {
        ScrollUp();
        g_curY = (int)g_fbHeight - g_fontAtlas.lineHeight;
    }
}

// ---------------------------------------------------------------------------
// Glyph rendering
// ---------------------------------------------------------------------------

static void RenderGlyph(int code)
{
    const FontAtlas& fa = g_fontAtlas;

    if (code < (int)fa.firstChar || code >= (int)(fa.firstChar + fa.glyphCount))
        return;

    const GlyphInfo& gi = fa.glyphs[code - (int)fa.firstChar];
    int gw = gi.atlasX1 - gi.atlasX0;
    int gh = gi.atlasY1 - gi.atlasY0;

    // Baseline is at g_curY + ascent.
    int drawX = g_curX + gi.bearingX;
    int drawY = g_curY + fa.ascent - gi.bearingY;

    for (int row = 0; row < gh; row++)
        for (int col = 0; col < gw; col++)
        {
            uint8_t cov = fa.pixels[(gi.atlasY0 + row) * (int)fa.atlasWidth
                                    + (gi.atlasX0 + col)];
            if (cov) WritePixel(drawX + col, drawY + row, cov);
        }

    g_curX += gi.advance;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool TtyInit(const Framebuffer& fb)
{
    uint64_t fbBytes  = static_cast<uint64_t>(fb.stride) * fb.height;
    uint64_t fbPages  = (fbBytes + 4095u) / 4096u;
    uint64_t physBase = fb.physicalBase;

    // Allocate virtual address range for the framebuffer.
    uint64_t fbVirt = VmmAllocPages(fbPages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (fbVirt == 0)
    {
        SerialPuts("TTY: VmmAllocPages failed for framebuffer\n");
        return false;
    }

    // VmmAllocPages backed each page with a fresh PMM page.
    // Replace each with the matching framebuffer physical page.
    for (uint64_t i = 0; i < fbPages; i++)
    {
        uint64_t virt    = fbVirt + i * 4096u;
        uint64_t oldPhys = VmmVirtToPhys(virt);
        VmmUnmapPage(virt);
        if (oldPhys) PmmFreePage(oldPhys);

        if (!VmmMapPage(virt, physBase + i * 4096u,
                        VMM_WRITABLE | VMM_NO_EXEC,
                        MemTag::Device, KernelPid))
        {
            SerialPuts("TTY: VmmMapPage failed for framebuffer page\n");
            return false;
        }
    }

    g_fbPixels = reinterpret_cast<volatile uint32_t*>(fbVirt);
    g_fbWidth  = fb.width;
    g_fbHeight = fb.height;
    g_fbStride = fb.stride / 4u;
    g_curX     = 0;
    g_curY     = 0;

    // Clear framebuffer to background colour.
    for (uint32_t y = 0; y < g_fbHeight; y++)
        for (uint32_t x = 0; x < g_fbWidth; x++)
            g_fbPixels[y * g_fbStride + x] = g_bgColor;

    SerialPrintf("TTY: %ux%u framebuffer mapped at virt 0x%p, "
                 "font %upx (lineHeight=%u ascent=%u)\n",
                 g_fbWidth, g_fbHeight,
                 reinterpret_cast<void*>(fbVirt),
                 static_cast<uint32_t>(g_fontAtlas.lineHeight),
                 static_cast<uint32_t>(g_fontAtlas.lineHeight),
                 static_cast<uint32_t>(g_fontAtlas.ascent));
    return true;
}

void TtyPutChar(char c)
{
    if (!g_fbPixels) return;

    const FontAtlas& fa = g_fontAtlas;
    int code = static_cast<uint8_t>(c);

    switch (c)
    {
    case '\n':
        Newline();
        return;
    case '\r':
        g_curX = 0;
        return;
    case '\t':
    {
        // Align to next multiple of 8 character widths (use space advance).
        int spAdv = fa.glyphCount > 0 ? fa.glyphs[0].advance : 8;
        int tabStop = spAdv * 8;
        g_curX = ((g_curX / tabStop) + 1) * tabStop;
        if ((uint32_t)g_curX >= g_fbWidth) Newline();
        return;
    }
    default:
        break;
    }

    // Word-wrap: if this glyph would exceed the right edge, wrap first.
    if (code >= (int)fa.firstChar && code < (int)(fa.firstChar + fa.glyphCount))
    {
        int adv = fa.glyphs[code - (int)fa.firstChar].advance;
        if ((uint32_t)(g_curX + adv) > g_fbWidth)
            Newline();
    }

    RenderGlyph(code);
}

void TtyPuts(const char* str)
{
    if (!str) return;
    while (*str) TtyPutChar(*str++);
}

// ---------------------------------------------------------------------------
// TtyVPrintf — mirrors SerialPrintf's format specifiers with width/padding.
// ---------------------------------------------------------------------------

static void TtyPrintPtr(unsigned long val)
{
    TtyPuts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        int nib = static_cast<int>((val >> shift) & 0xF);
        TtyPutChar(static_cast<char>(nib < 10 ? '0' + nib : 'a' + nib - 10));
    }
}

void TtyVPrintf(const char* fmt, __builtin_va_list args)
{
    while (*fmt)
    {
        if (*fmt != '%') { TtyPutChar(*fmt++); continue; }
        ++fmt;
        if (*fmt == '\0') break;

        // Parse flags
        bool leftAlign = false;
        bool zeroPad   = false;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') leftAlign = true;
            if (*fmt == '0') zeroPad   = true;
            fmt++;
        }
        if (leftAlign) zeroPad = false;

        // Parse width
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == '\0') break;

        // Parse 'l' length modifier
        bool isLong = false;
        if (*fmt == 'l') { isLong = true; fmt++; }
        if (*fmt == '\0') break;

        char buf[24];
        int len = 0;

        switch (*fmt)
        {
        case 's': {
            const char* s = __builtin_va_arg(args, const char*);
            if (!s) s = "(null)";
            const char* p = s;
            while (*p) { ++p; ++len; }
            if (!leftAlign) { for (int i = len; i < width; ++i) TtyPutChar(' '); }
            TtyPuts(s);
            if (leftAlign) { for (int i = len; i < width; ++i) TtyPutChar(' '); }
            break;
        }
        case 'd': {
            long val;
            if (isLong) val = __builtin_va_arg(args, long);
            else        val = static_cast<long>(__builtin_va_arg(args, int));
            bool neg = val < 0;
            unsigned long uval = neg ? static_cast<unsigned long>(-val) : static_cast<unsigned long>(val);
            len = 0;
            if (uval == 0) { buf[len++] = '0'; }
            else { while (uval > 0) { buf[len++] = static_cast<char>('0' + (uval % 10)); uval /= 10; } }
            int totalLen = len + (neg ? 1 : 0);
            if (!leftAlign && !zeroPad) { for (int i = totalLen; i < width; ++i) TtyPutChar(' '); }
            if (neg) TtyPutChar('-');
            if (!leftAlign && zeroPad) { for (int i = totalLen; i < width; ++i) TtyPutChar('0'); }
            while (len > 0) TtyPutChar(buf[--len]);
            if (leftAlign) { for (int i = totalLen; i < width; ++i) TtyPutChar(' '); }
            break;
        }
        case 'u': {
            unsigned long val;
            if (isLong) val = __builtin_va_arg(args, unsigned long);
            else        val = static_cast<unsigned long>(__builtin_va_arg(args, unsigned int));
            len = 0;
            if (val == 0) { buf[len++] = '0'; }
            else { while (val > 0) { buf[len++] = static_cast<char>('0' + (val % 10)); val /= 10; } }
            char padCh = zeroPad ? '0' : ' ';
            if (!leftAlign) { for (int i = len; i < width; ++i) TtyPutChar(padCh); }
            while (len > 0) TtyPutChar(buf[--len]);
            if (leftAlign) { for (int i = len; i < width; ++i) TtyPutChar(' '); }
            break;
        }
        case 'x': {
            unsigned long val;
            if (isLong) val = __builtin_va_arg(args, unsigned long);
            else        val = static_cast<unsigned long>(__builtin_va_arg(args, unsigned int));
            len = 0;
            if (val == 0) { buf[len++] = '0'; }
            else { while (val > 0) { int n = static_cast<int>(val & 0xF); buf[len++] = static_cast<char>(n < 10 ? '0' + n : 'a' + n - 10); val >>= 4; } }
            char padCh = zeroPad ? '0' : ' ';
            if (!leftAlign) { for (int i = len; i < width; ++i) TtyPutChar(padCh); }
            while (len > 0) TtyPutChar(buf[--len]);
            if (leftAlign) { for (int i = len; i < width; ++i) TtyPutChar(' '); }
            break;
        }
        case 'p': TtyPrintPtr((unsigned long)__builtin_va_arg(args, void*)); break;
        case 'c': TtyPutChar(static_cast<char>(__builtin_va_arg(args, int))); break;
        case '%': TtyPutChar('%'); break;
        default:  TtyPutChar('%'); if (isLong) TtyPutChar('l'); TtyPutChar(*fmt); break;
        }
        ++fmt;
    }
}

void TtyPrintf(const char* fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    TtyVPrintf(fmt, args);
    __builtin_va_end(args);
}

void TtySetColors(uint32_t fg, uint32_t bg)
{
    g_fgColor = fg;
    g_bgColor = bg;
}

void TtyClear()
{
    if (!g_fbPixels) return;
    for (uint32_t y = 0; y < g_fbHeight; y++)
        for (uint32_t x = 0; x < g_fbWidth; x++)
            g_fbPixels[y * g_fbStride + x] = g_bgColor;
    g_curX = 0;
    g_curY = 0;
}

bool TtyReady()
{
    return g_fbPixels != nullptr;
}

} // namespace brook
