#include "tty.h"
#include "font_atlas.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "serial.h"
#include "compositor.h"

namespace brook {

// Set when TTY has been redirected to a compositor backbuffer.
static bool g_ttyUsingBackbuffer = false;

// ---------------------------------------------------------------------------
// TTY state
// ---------------------------------------------------------------------------

static volatile uint32_t* g_fbPixels  = nullptr; // virtual address of framebuffer
static uint64_t  g_fbPhysBase         = 0;        // physical base of framebuffer
static uint32_t  g_fbWidth            = 0;
static uint32_t  g_fbHeight           = 0;
static uint32_t  g_fbStride           = 0;        // stride in pixels (not bytes)

static int        g_curX              = 0;        // current pen X (pixels)
static int        g_curY              = 0;        // current line top Y (pixels)

static uint32_t   g_fgColor           = 0x00E0E0E0; // light grey
static uint32_t   g_bgColor           = 0x00001A3A; // dark blue (matches kernel bg)

static bool       g_displaySuppressed = false;      // when true, skip FB writes

// Region — the sub-rectangle of the framebuffer the TTY renders into.
// Defaults to the entire framebuffer after TtyInit().
static uint32_t  g_regionX           = 0;
static uint32_t  g_regionY           = 0;
static uint32_t  g_regionW           = 0;        // 0 = full width
static uint32_t  g_regionH           = 0;        // 0 = full height

// Effective region bounds (computed from g_region* or full FB).
static uint32_t  g_rx  = 0;  // left edge
static uint32_t  g_ry  = 0;  // top edge
static uint32_t  g_rw  = 0;  // width
static uint32_t  g_rh  = 0;  // height

static void RecalcRegion()
{
    g_rx = g_regionX;
    g_ry = g_regionY;
    g_rw = (g_regionW > 0) ? g_regionW : g_fbWidth;
    g_rh = (g_regionH > 0) ? g_regionH : g_fbHeight;
    // Clamp to framebuffer
    if (g_rx + g_rw > g_fbWidth)  g_rw = g_fbWidth - g_rx;
    if (g_ry + g_rh > g_fbHeight) g_rh = g_fbHeight - g_ry;
}

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
    // x,y are relative to the region
    int absX = static_cast<int>(g_rx) + x;
    int absY = static_cast<int>(g_ry) + y;
    if (absX < 0 || absY < 0) return;
    if ((uint32_t)absX >= g_rx + g_rw || (uint32_t)absY >= g_ry + g_rh) return;

    uint32_t fg = g_fgColor;
    uint32_t bg = g_bgColor;
    uint32_t cov = coverage;

    uint32_t r = BlendChannel((fg >> 16) & 0xFF, (bg >> 16) & 0xFF, cov);
    uint32_t g = BlendChannel((fg >>  8) & 0xFF, (bg >>  8) & 0xFF, cov);
    uint32_t b = BlendChannel( fg        & 0xFF,  bg        & 0xFF, cov);

    g_fbPixels[absY * g_fbStride + absX] = (r << 16) | (g << 8) | b;
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

static void ScrollUp()
{
    int lh = g_fontAtlas.lineHeight;
    // Move rows within the region up by one line height.
    for (uint32_t row = 0; (int)row + lh < (int)g_rh; row++)
    {
        uint32_t srcRow = g_ry + row + (uint32_t)lh;
        uint32_t dstRow = g_ry + row;
        const volatile uint32_t* src = g_fbPixels + srcRow * g_fbStride + g_rx;
        volatile uint32_t*       dst = g_fbPixels + dstRow * g_fbStride + g_rx;
        for (uint32_t col = 0; col < g_rw; col++)
            dst[col] = src[col];
    }
    // Clear the bottom lh rows within the region.
    for (uint32_t row = g_rh - (uint32_t)lh; row < g_rh; row++)
        for (uint32_t col = 0; col < g_rw; col++)
            g_fbPixels[(g_ry + row) * g_fbStride + (g_rx + col)] = g_bgColor;
}

// Advance cursor to the next line, scrolling if needed.
static void Newline()
{
    g_curX  = 0;
    g_curY += g_fontAtlas.lineHeight;
    if ((uint32_t)(g_curY + g_fontAtlas.lineHeight) > g_rh)
    {
        ScrollUp();
        g_curY = (int)g_rh - g_fontAtlas.lineHeight;
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
    uint64_t fbVirt = VmmAllocPages(fbPages, VMM_WRITABLE, MemTag::Device, KernelPid).raw();
    if (fbVirt == 0)
    {
        SerialPuts("TTY: VmmAllocPages failed for framebuffer\n");
        return false;
    }

    // VmmAllocPages backed each page with a fresh PMM page.
    // Replace each with the matching framebuffer physical page.
    for (uint64_t i = 0; i < fbPages; i++)
    {
        VirtualAddress virt(fbVirt + i * 4096u);
        PhysicalAddress oldPhys = VmmVirtToPhys(KernelPageTable, virt);
        VmmUnmapPage(KernelPageTable, virt);
        if (oldPhys) PmmFreePage(oldPhys);

        if (!VmmMapPage(KernelPageTable, virt, PhysicalAddress(physBase + i * 4096u),
                        VMM_WRITABLE | VMM_NO_EXEC,
                        MemTag::Device, KernelPid))
        {
            SerialPuts("TTY: VmmMapPage failed for framebuffer page\n");
            return false;
        }
    }

    g_fbPixels = reinterpret_cast<volatile uint32_t*>(fbVirt);
    g_fbPhysBase = physBase;
    g_fbWidth  = fb.width;
    g_fbHeight = fb.height;
    g_fbStride = fb.stride / 4u;
    g_curX     = 0;
    g_curY     = 0;

    // Default region = full framebuffer.
    g_regionX = g_regionY = g_regionW = g_regionH = 0;
    RecalcRegion();

    // Clear framebuffer to background colour.
    for (uint32_t y = 0; y < g_fbHeight; y++)
        for (uint32_t x = 0; x < g_fbWidth; x++)
            g_fbPixels[y * g_fbStride + x] = g_bgColor;

    SerialPrintf("TTY: %ux%u framebuffer mapped at virt 0x%p phys 0x%lx, "
                 "font %upx (lineHeight=%u ascent=%u)\n",
                 g_fbWidth, g_fbHeight,
                 reinterpret_cast<void*>(fbVirt),
                 physBase,
                 static_cast<uint32_t>(g_fontAtlas.lineHeight),
                 static_cast<uint32_t>(g_fontAtlas.lineHeight),
                 static_cast<uint32_t>(g_fontAtlas.ascent));
    return true;
}

// ---------------------------------------------------------------------------
// ANSI escape sequence parser (CSI sequences only: ESC [ ... m)
// ---------------------------------------------------------------------------

static enum { TTY_NORMAL, TTY_ESC, TTY_CSI } g_ansiState = TTY_NORMAL;
static int g_ansiParams[8];
static int g_ansiParamCount = 0;

// Map ANSI SGR color code (30-37, 90-97) to 0xRRGGBB
static uint32_t AnsiToRgb(int code)
{
    // Standard colors (30-37)
    static const uint32_t colors[] = {
        0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
        0x5555FF, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    };
    // Bright colors (90-97)
    static const uint32_t bright[] = {
        0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
        0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
    };
    if (code >= 30 && code <= 37) return colors[code - 30];
    if (code >= 90 && code <= 97) return bright[code - 90];
    return 0xE0E0E0; // default
}

static void AnsiApplySgr()
{
    for (int i = 0; i < g_ansiParamCount; ++i)
    {
        int p = g_ansiParams[i];
        if (p == 0) { g_fgColor = 0x00E0E0E0; g_bgColor = 0x00001A3A; } // reset
        else if (p == 1) { /* bold — brighten fg */ }
        else if (p >= 30 && p <= 37) g_fgColor = AnsiToRgb(p);
        else if (p == 39) g_fgColor = 0x00E0E0E0; // default fg
        else if (p >= 40 && p <= 47) g_bgColor = AnsiToRgb(p - 10);
        else if (p == 49) g_bgColor = 0x00001A3A; // default bg
        else if (p >= 90 && p <= 97) g_fgColor = AnsiToRgb(p);
    }
}

void TtyPutChar(char c)
{
    if (!g_fbPixels) return;
    if (g_displaySuppressed) return;   // logo is showing — skip FB writes

    // ANSI escape sequence state machine
    if (g_ansiState == TTY_ESC) {
        if (c == '[') { g_ansiState = TTY_CSI; g_ansiParamCount = 0; g_ansiParams[0] = 0; return; }
        g_ansiState = TTY_NORMAL; // not a CSI sequence, fall through
    }
    if (g_ansiState == TTY_CSI) {
        if (c >= '0' && c <= '9') {
            if (g_ansiParamCount == 0) g_ansiParamCount = 1;
            g_ansiParams[g_ansiParamCount - 1] = g_ansiParams[g_ansiParamCount - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (g_ansiParamCount < 8) { g_ansiParams[g_ansiParamCount] = 0; g_ansiParamCount++; }
            return;
        }
        // Final byte — apply command
        if (c == 'm') AnsiApplySgr();
        else if (c == 'J') {
            // ESC[2J — clear entire screen
            int param = (g_ansiParamCount > 0) ? g_ansiParams[0] : 0;
            if (param == 2) {
                for (uint32_t row = 0; row < g_rh; row++)
                    for (uint32_t col = 0; col < g_rw; col++)
                        g_fbPixels[(g_ry + row) * g_fbStride + (g_rx + col)] = g_bgColor;
                g_curX = 0;
                g_curY = 0;
            }
        }
        else if (c == 'H') {
            // ESC[H — cursor home
            g_curX = 0;
            g_curY = 0;
        }
        g_ansiState = TTY_NORMAL;
        return;
    }
    if (c == '\033') { g_ansiState = TTY_ESC; return; }

    const FontAtlas& fa = g_fontAtlas;
    int code = static_cast<uint8_t>(c);

    switch (c)
    {
    case '\n':
        Newline();
        if (g_ttyUsingBackbuffer) CompositorMarkDirty();
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
        if ((uint32_t)g_curX >= g_rw) Newline();
        return;
    }
    default:
        break;
    }

    // Word-wrap: if this glyph would exceed the right edge, wrap first.
    if (code >= (int)fa.firstChar && code < (int)(fa.firstChar + fa.glyphCount))
    {
        int adv = fa.glyphs[code - (int)fa.firstChar].advance;
        if ((uint32_t)(g_curX + adv) > g_rw)
            Newline();
    }

    RenderGlyph(code);
    if (g_ttyUsingBackbuffer)
        CompositorMarkDirty();
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
    for (uint32_t y = 0; y < g_rh; y++)
        for (uint32_t x = 0; x < g_rw; x++)
            g_fbPixels[(g_ry + y) * g_fbStride + (g_rx + x)] = g_bgColor;
    g_curX = 0;
    g_curY = 0;
    if (g_ttyUsingBackbuffer) CompositorMarkDirty();
}

bool TtyReady()
{
    return g_fbPixels != nullptr;
}

void TtySetRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    g_regionX = x;
    g_regionY = y;
    g_regionW = w;
    g_regionH = h;
    RecalcRegion();
    // Reset cursor to top-left of the new region.
    g_curX = 0;
    g_curY = 0;
    // Clear the region.
    TtyClear();
}

void TtySuppressDisplay(bool suppress)
{
    g_displaySuppressed = suppress;
}

bool TtyGetFramebuffer(uint32_t** outPixels, uint32_t* outWidth,
                       uint32_t* outHeight, uint32_t* outStride)
{
    if (!g_fbPixels) return false;
    *outPixels = const_cast<uint32_t*>(const_cast<volatile uint32_t**>(&g_fbPixels)[0]);
    *outWidth  = g_fbWidth;
    *outHeight = g_fbHeight;
    *outStride = g_fbStride * 4; // convert from pixel stride to byte stride
    return true;
}

bool TtyGetFramebufferPhys(uint64_t* outPhysBase, uint32_t* outWidth,
                           uint32_t* outHeight, uint32_t* outStride)
{
    if (!g_fbPixels) return false;
    *outPhysBase = g_fbPhysBase;
    *outWidth    = g_fbWidth;
    *outHeight   = g_fbHeight;
    *outStride   = g_fbStride * 4; // byte stride
    return true;
}

// ---------------------------------------------------------------------------
// TtyRemap — hot-swap the framebuffer (e.g. when a PCI display driver loads)
// ---------------------------------------------------------------------------

void TtyRemap(uint64_t newPhysBase, uint32_t w, uint32_t h, uint32_t stridePixels)
{
    uint64_t fbBytes = static_cast<uint64_t>(stridePixels) * 4 * h;
    uint64_t fbPages = (fbBytes + 4095u) / 4096u;

    // Allocate virtual address range for the new framebuffer.
    uint64_t fbVirt = VmmAllocPages(fbPages, VMM_WRITABLE, MemTag::Device, KernelPid).raw();
    if (fbVirt == 0) {
        SerialPuts("TTY: TtyRemap VmmAllocPages failed\n");
        return;
    }

    // Replace backing pages with framebuffer physical pages.
    for (uint64_t i = 0; i < fbPages; i++) {
        VirtualAddress virt(fbVirt + i * 4096u);
        PhysicalAddress oldPhys = VmmVirtToPhys(KernelPageTable, virt);
        VmmUnmapPage(KernelPageTable, virt);
        if (oldPhys) PmmFreePage(oldPhys);

        if (!VmmMapPage(KernelPageTable, virt, PhysicalAddress(newPhysBase + i * 4096u),
                        VMM_WRITABLE | VMM_NO_EXEC,
                        MemTag::Device, KernelPid))
        {
            SerialPrintf("TTY: TtyRemap VmmMapPage failed at page %lu\n", i);
            return;
        }
    }

    // Update TTY state atomically (no lock needed — caller ensures serialisation).
    g_fbPixels   = reinterpret_cast<volatile uint32_t*>(fbVirt);
    g_fbPhysBase = newPhysBase;
    g_fbWidth    = w;
    g_fbHeight   = h;
    g_fbStride   = stridePixels;

    // Reset cursor and region.
    g_curX = 0;
    g_curY = 0;
    g_regionX = g_regionY = g_regionW = g_regionH = 0;
    RecalcRegion();

    // Clear to background colour.
    for (uint32_t y2 = 0; y2 < h; y2++)
        for (uint32_t x2 = 0; x2 < w; x2++)
            g_fbPixels[y2 * stridePixels + x2] = g_bgColor;

    SerialPrintf("TTY: remapped to %ux%u at phys 0x%lx, virt 0x%lx\n",
                 w, h, newPhysBase, fbVirt);
}

void TtyRedirectToBackbuffer(uint32_t* backbuffer)
{
    if (backbuffer)
    {
        g_fbPixels = reinterpret_cast<volatile uint32_t*>(backbuffer);
        g_ttyUsingBackbuffer = true;
        SerialPrintf("TTY: redirected to backbuffer 0x%lx\n",
                     reinterpret_cast<uint64_t>(backbuffer));
    }
}

} // namespace brook
